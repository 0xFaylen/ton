// SPDX-License-Identifier: LGPL-2.0-or-later

#include <algorithm>
#include <map>
#include <random>
#include <set>

#include "impl/parallel-inbound-scheduler.h"
#include "td/utils/tests.h"

namespace ton::validator::parallel_inbound::test {
namespace {

Hash256 hash(std::uint64_t value) {
  Hash256 result{};
  for (std::size_t i = 0; i < 8; ++i) {
    result[result.size() - 1 - i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
  return result;
}

WorkItem item(std::uint64_t lt, std::uint64_t message_hash, std::optional<std::uint64_t> account) {
  return {{lt, hash(message_hash)}, account ? std::optional<Hash256>{hash(*account)} : std::nullopt};
}

WorkerReceipt receipt(const WorkItem& work, std::uint64_t account, std::uint64_t sequence, std::uint64_t pre_state,
                      std::uint64_t post_state, std::uint64_t start_lt, std::uint64_t end_lt) {
  return {.input = work.key,
          .account = hash(account),
          .account_sequence = sequence,
          .pre_account_state_hash = hash(pre_state),
          .transaction_hash = hash(1000 + sequence),
          .post_account_state_hash = hash(post_state),
          .effects_hash = hash(2000 + sequence),
          .proof_journal_hash = hash(3000 + sequence),
          .transaction_start_lt = start_lt,
          .transaction_end_lt = end_lt,
          .gas_used = 100 + sequence};
}

TEST(ParallelInboundScheduler, RejectsNonCanonicalInputAndZeroWorkers) {
  const std::vector<WorkItem> ordered{item(1, 1, 10), item(1, 2, 20)};
  ASSERT_EQ(build_lane_plan(ordered, 0).error, PlanError::no_workers);

  const std::vector<WorkItem> reversed{item(1, 2, 10), item(1, 1, 20)};
  ASSERT_EQ(build_lane_plan(reversed, 2).error, PlanError::non_canonical_input);

  const std::vector<WorkItem> duplicate{item(1, 1, 10), item(1, 1, 20)};
  ASSERT_EQ(build_lane_plan(duplicate, 2).error, PlanError::non_canonical_input);
}

TEST(ParallelInboundScheduler, KeepsEachAccountSerialAndFrontierAccountOnFirstLane) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 10),
                                    item(1, 4, 30), item(1, 5, 20), item(1, 6, std::nullopt)};
  auto result = build_lane_plan(items, 3);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.plan.account_lanes[0], (std::vector<std::size_t>{0, 2}));
  ASSERT_EQ(result.plan.account_lanes[1], (std::vector<std::size_t>{1, 4}));
  ASSERT_EQ(result.plan.account_lanes[2], (std::vector<std::size_t>{3}));
  ASSERT_EQ(result.plan.coordinator_items, (std::vector<std::size_t>{5}));
}

TEST(ParallelInboundScheduler, BalancesKnownAccountChainsWithoutSplittingThem) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 30), item(1, 4, 40), item(1, 5, 10),
                                    item(1, 6, 20), item(1, 7, 30), item(1, 8, 10), item(1, 9, 20), item(1, 10, 10)};
  auto result = build_lane_plan(items, 3);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.plan.account_lanes[0], (std::vector<std::size_t>{0, 4, 7, 9}));
  ASSERT_EQ(result.plan.account_lanes[1], (std::vector<std::size_t>{1, 5, 8}));
  ASSERT_EQ(result.plan.account_lanes[2], (std::vector<std::size_t>{2, 3, 6}));
}

TEST(ParallelInboundScheduler, DoesNotAdvanceAcrossCompletionHole) {
  const std::vector<CompletionStatus> completions{CompletionStatus::succeeded, CompletionStatus::pending,
                                                  CompletionStatus::succeeded};
  std::vector<std::size_t> committed;
  auto decision = commit_ready_prefix(
      completions, [](std::size_t) { return true; },
      [&](std::size_t index) {
        committed.push_back(index);
        return true;
      });

  ASSERT_EQ(decision.committed_count, 1u);
  ASSERT_EQ(decision.stop_reason, PrefixStopReason::pending);
  ASSERT_EQ(decision.first_uncommitted(), std::optional<std::size_t>{1});
  ASSERT_EQ(committed, (std::vector<std::size_t>{0}));
}

TEST(ParallelInboundScheduler, MirrorsSerialPreItemLimitCheck) {
  const std::vector<CompletionStatus> completions(3, CompletionStatus::succeeded);
  const std::vector<unsigned> cost{5, 4, 1};
  unsigned used = 8;
  auto decision = commit_ready_prefix(
      completions, [&](std::size_t) { return used < 10; },
      [&](std::size_t index) {
        used += cost[index];
        return true;
      });

  // The serial collator starts the first item while the block still fits. That
  // item may cross the normal limit; the following item must not start.
  ASSERT_EQ(decision.committed_count, 1u);
  ASSERT_EQ(decision.stop_reason, PrefixStopReason::block_limit);
  ASSERT_EQ(used, 13u);
}

TEST(ParallelInboundScheduler, WorkerFailureLeavesAUsefulCommittedPrefix) {
  const std::vector<CompletionStatus> completions{CompletionStatus::succeeded, CompletionStatus::succeeded,
                                                  CompletionStatus::failed, CompletionStatus::succeeded};
  auto decision = commit_ready_prefix(completions, [](std::size_t) { return true; }, [](std::size_t) { return true; });

  ASSERT_EQ(decision.committed_count, 2u);
  ASSERT_EQ(decision.stop_reason, PrefixStopReason::worker_failure);
  ASSERT_EQ(decision.first_uncommitted(), std::optional<std::size_t>{2});
}

TEST(ParallelInboundScheduler, CompletionArrivalOrderCannotChangeCommittedPrefix) {
  std::mt19937 rng(0x544f4e);
  for (unsigned round = 0; round < 100; ++round) {
    std::vector<std::size_t> arrival{0, 1, 2, 3, 4, 5, 6, 7};
    std::shuffle(arrival.begin(), arrival.end(), rng);
    std::vector<CompletionStatus> completions(arrival.size(), CompletionStatus::pending);
    for (auto index : arrival) {
      completions[index] = CompletionStatus::succeeded;
      std::size_t expected_prefix = 0;
      while (expected_prefix < completions.size() && completions[expected_prefix] == CompletionStatus::succeeded) {
        ++expected_prefix;
      }
      auto partial_decision =
          commit_ready_prefix(completions, [](std::size_t) { return true; }, [](std::size_t) { return true; });
      ASSERT_EQ(partial_decision.committed_count, expected_prefix);
      ASSERT_EQ(partial_decision.stop_reason,
                expected_prefix == completions.size() ? PrefixStopReason::end_of_input : PrefixStopReason::pending);
    }
    auto decision =
        commit_ready_prefix(completions, [](std::size_t) { return true; }, [](std::size_t) { return true; });
    ASSERT_EQ(decision.committed_count, completions.size());
    ASSERT_EQ(decision.stop_reason, PrefixStopReason::end_of_input);
  }
}

TEST(ParallelInboundScheduler, CommitFailureCannotExposeACompletedSuffix) {
  const std::vector<CompletionStatus> completions(4, CompletionStatus::succeeded);
  std::vector<std::size_t> committed;
  auto decision = commit_ready_prefix(
      completions, [](std::size_t) { return true; },
      [&](std::size_t index) {
        if (index == 2) {
          return false;
        }
        committed.push_back(index);
        return true;
      });

  ASSERT_EQ(decision.committed_count, 2u);
  ASSERT_EQ(decision.stop_reason, PrefixStopReason::commit_failure);
  ASSERT_EQ(decision.first_uncommitted(), std::optional<std::size_t>{2});
  ASSERT_EQ(committed, (std::vector<std::size_t>{0, 1}));
}

TEST(ParallelInboundScheduler, PublishesProcessedUptoOnlyForContinuousCommittedPrefix) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 30)};
  const std::vector<CompletionStatus> completions{CompletionStatus::succeeded, CompletionStatus::pending,
                                                  CompletionStatus::succeeded};
  ProcessedUptoFrontier frontier;
  std::vector<std::size_t> committed;
  auto result = commit_ready_prefix_and_publish_frontier(
      items, completions, frontier, [](std::size_t) { return true; },
      [&](std::size_t index) {
        committed.push_back(index);
        return true;
      });

  ASSERT_TRUE(result);
  ASSERT_EQ(result.decision.committed_count, 1u);
  ASSERT_EQ(result.decision.stop_reason, PrefixStopReason::pending);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{items[0].key});
  ASSERT_EQ(committed, (std::vector<std::size_t>{0}));

  const std::vector<WorkItem> tail{items[1], items[2]};
  result = commit_ready_prefix_and_publish_frontier(
      tail, {CompletionStatus::succeeded, CompletionStatus::succeeded}, frontier, [](std::size_t) { return true; },
      [](std::size_t) { return true; });
  ASSERT_TRUE(result);
  ASSERT_EQ(result.decision.committed_count, 2u);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{items[2].key});
}

TEST(ParallelInboundScheduler, FailedOrUncommittedItemNeverAdvancesProcessedUpto) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 30)};
  ProcessedUptoFrontier frontier{items[0].key};
  const std::vector<WorkItem> tail{items[1], items[2]};

  auto failed = commit_ready_prefix_and_publish_frontier(
      tail, {CompletionStatus::failed, CompletionStatus::succeeded}, frontier, [](std::size_t) { return true; },
      [](std::size_t) { return true; });
  ASSERT_TRUE(failed);
  ASSERT_EQ(failed.decision.stop_reason, PrefixStopReason::worker_failure);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{items[0].key});

  auto limited = commit_ready_prefix_and_publish_frontier(
      tail, {CompletionStatus::succeeded, CompletionStatus::succeeded}, frontier, [](std::size_t) { return false; },
      [](std::size_t) { return true; });
  ASSERT_TRUE(limited);
  ASSERT_EQ(limited.decision.stop_reason, PrefixStopReason::block_limit);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{items[0].key});

  auto commit_failed = commit_ready_prefix_and_publish_frontier(
      tail, {CompletionStatus::succeeded, CompletionStatus::succeeded}, frontier, [](std::size_t) { return true; },
      [](std::size_t) { return false; });
  ASSERT_TRUE(commit_failed);
  ASSERT_EQ(commit_failed.decision.stop_reason, PrefixStopReason::commit_failure);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{items[0].key});
}

TEST(ParallelInboundScheduler, RejectsMalformedFrontierSliceBeforeCoordinatorMutation) {
  const auto first = item(1, 1, 10);
  ProcessedUptoFrontier frontier{first.key};
  std::size_t callbacks = 0;
  const std::vector<WorkItem> stale{first, item(1, 2, 20)};
  auto result = commit_ready_prefix_and_publish_frontier(
      stale, {CompletionStatus::succeeded, CompletionStatus::succeeded}, frontier,
      [&](std::size_t) {
        ++callbacks;
        return true;
      },
      [&](std::size_t) {
        ++callbacks;
        return true;
      });
  ASSERT_EQ(result.error, FrontierError::input_not_after_frontier);
  ASSERT_EQ(result.item_index, std::optional<std::size_t>{0});
  ASSERT_EQ(callbacks, 0u);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{first.key});

  const std::vector<WorkItem> reversed{item(2, 2, 10), item(2, 1, 20)};
  result = commit_ready_prefix_and_publish_frontier(
      reversed, {CompletionStatus::succeeded, CompletionStatus::succeeded}, frontier,
      [&](std::size_t) {
        ++callbacks;
        return true;
      },
      [&](std::size_t) {
        ++callbacks;
        return true;
      });
  ASSERT_EQ(result.error, FrontierError::non_canonical_input);
  ASSERT_EQ(callbacks, 0u);

  result = commit_ready_prefix_and_publish_frontier(
      std::vector<WorkItem>{item(2, 1, 10)}, {}, frontier, [](std::size_t) { return true; },
      [](std::size_t) { return true; });
  ASSERT_EQ(result.error, FrontierError::size_mismatch);
  ASSERT_EQ(frontier.last_processed, std::optional<MessageKey>{first.key});
}

TEST(ParallelInboundScheduler, AccountLanesAreEquivalentToSerialAccountState) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 10), item(1, 4, 30),
                                    item(1, 5, 20), item(1, 6, 10), item(1, 7, 30), item(1, 8, 40)};
  const std::vector<std::uint64_t> values{3, 5, 7, 11, 13, 17, 19, 23};
  std::map<Hash256, std::uint64_t> serial_state;
  for (std::size_t i = 0; i < items.size(); ++i) {
    serial_state[*items[i].account] = serial_state[*items[i].account] * 131 + values[i];
  }

  auto result = build_lane_plan(items, 4);
  ASSERT_TRUE(result);
  std::map<Hash256, std::uint64_t> lane_state;
  std::set<std::size_t> seen;
  for (const auto& lane : result.plan.account_lanes) {
    for (auto index : lane) {
      seen.insert(index);
      const auto account = *items[index].account;
      lane_state[account] = lane_state[account] * 131 + values[index];
    }
  }
  ASSERT_EQ(seen.size(), items.size());
  ASSERT_EQ(lane_state, serial_state);
}

TEST(ParallelInboundScheduler, ValidatesInterleavedReceiptChainsPerAccount) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 10), item(1, 4, std::nullopt),
                                    item(1, 5, 20)};
  const std::vector<std::optional<WorkerReceipt>> receipts{
      receipt(items[0], 10, 0, 100, 101, 11, 15), receipt(items[1], 20, 0, 200, 201, 21, 25),
      receipt(items[2], 10, 1, 101, 102, 16, 20), std::nullopt, receipt(items[4], 20, 1, 201, 202, 26, 30)};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}},
      {hash(20), {.state_hash = hash(200), .next_sequence = 0, .last_transaction_end_lt = 20}}};

  auto result = validate_receipt_set(items, receipts, initial);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.verified_receipts, 4u);
  ASSERT_EQ(result.checkpoints.at(hash(10)).state_hash, hash(102));
  ASSERT_EQ(result.checkpoints.at(hash(10)).next_sequence, 2u);
  ASSERT_EQ(result.checkpoints.at(hash(20)).state_hash, hash(202));
  ASSERT_EQ(result.checkpoints.at(hash(20)).last_transaction_end_lt, 30u);
}

TEST(ParallelInboundScheduler, AllowsOtherAccountsToProgressAcrossAPendingAccount) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 10), item(1, 4, 20)};
  const std::vector<std::optional<WorkerReceipt>> receipts{std::nullopt, receipt(items[1], 20, 0, 200, 201, 21, 25),
                                                           std::nullopt, receipt(items[3], 20, 1, 201, 202, 26, 30)};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}},
      {hash(20), {.state_hash = hash(200), .next_sequence = 0, .last_transaction_end_lt = 20}}};

  auto result = validate_receipt_set(items, receipts, initial);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.verified_receipts, 2u);
  ASSERT_EQ(result.checkpoints.at(hash(10)).state_hash, hash(100));
  ASSERT_EQ(result.checkpoints.at(hash(20)).state_hash, hash(202));
}

TEST(ParallelInboundScheduler, RejectsReceiptAfterAnAccountLocalHole) {
  const std::vector<WorkItem> items{item(1, 1, 10), item(1, 2, 20), item(1, 3, 10)};
  const std::vector<std::optional<WorkerReceipt>> receipts{std::nullopt, receipt(items[1], 20, 0, 200, 201, 21, 25),
                                                           receipt(items[2], 10, 0, 100, 101, 11, 15)};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}},
      {hash(20), {.state_hash = hash(200), .next_sequence = 0, .last_transaction_end_lt = 20}}};

  auto result = validate_receipt_set(items, receipts, initial);
  ASSERT_EQ(result.error, ReceiptError::missing_account_predecessor);
  ASSERT_EQ(result.item_index, std::optional<std::size_t>{2});
}

TEST(ParallelInboundScheduler, RejectsTamperedReceiptHeaderBeforeCommit) {
  const std::vector<WorkItem> items{item(1, 1, 10)};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}}};

  auto tampered = receipt(items[0], 10, 0, 999, 101, 11, 15);
  auto result = validate_receipt_set(items, {tampered}, initial);
  ASSERT_EQ(result.error, ReceiptError::pre_state_mismatch);
  ASSERT_EQ(result.verified_receipts, 0u);

  tampered = receipt(items[0], 10, 0, 100, 101, 9, 15);
  result = validate_receipt_set(items, {tampered}, initial);
  ASSERT_EQ(result.error, ReceiptError::invalid_logical_time);

  tampered = receipt(items[0], 20, 0, 100, 101, 11, 15);
  result = validate_receipt_set(items, {tampered}, initial);
  ASSERT_EQ(result.error, ReceiptError::account_mismatch);
}

TEST(ParallelInboundScheduler, RejectsReceiptForCoordinatorOnlyWork) {
  const std::vector<WorkItem> items{item(1, 1, std::nullopt)};
  auto unexpected = receipt(item(1, 1, 10), 10, 0, 100, 101, 11, 15);
  auto result = validate_receipt_set(items, {unexpected}, {});
  ASSERT_EQ(result.error, ReceiptError::unexpected_coordinator_receipt);
  ASSERT_EQ(result.item_index, std::optional<std::size_t>{0});
}

TEST(ParallelInboundScheduler, RejectsReceiptsOutsideCanonicalQueueOrder) {
  const std::vector<WorkItem> items{item(1, 2, 10), item(1, 1, 10)};
  const std::vector<std::optional<WorkerReceipt>> receipts{receipt(items[0], 10, 0, 100, 101, 11, 15),
                                                           receipt(items[1], 10, 1, 101, 102, 16, 20)};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}}};

  auto result = validate_receipt_set(items, receipts, initial);
  ASSERT_EQ(result.error, ReceiptError::non_canonical_input);
  ASSERT_EQ(result.item_index, std::optional<std::size_t>{1});
  ASSERT_EQ(result.verified_receipts, 0u);

  const std::vector<WorkItem> duplicate{item(1, 1, 10), item(1, 1, 10)};
  result = validate_receipt_set(duplicate, {std::nullopt, std::nullopt}, initial);
  ASSERT_EQ(result.error, ReceiptError::non_canonical_input);
}

TEST(ParallelInboundScheduler, RejectsMalformedReceiptBindingsAndChains) {
  const std::vector<WorkItem> items{item(1, 1, 10)};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}}};
  auto valid = receipt(items[0], 10, 0, 100, 101, 11, 15);

  ASSERT_EQ(validate_receipt_set(items, {}, initial).error, ReceiptError::size_mismatch);
  ASSERT_EQ(validate_receipt_set(items, {valid}, {}).error, ReceiptError::missing_initial_checkpoint);

  auto malformed = valid;
  malformed.input = item(1, 2, 10).key;
  ASSERT_EQ(validate_receipt_set(items, {malformed}, initial).error, ReceiptError::input_key_mismatch);

  malformed = valid;
  malformed.account_sequence = 1;
  ASSERT_EQ(validate_receipt_set(items, {malformed}, initial).error, ReceiptError::account_sequence_mismatch);

  malformed = valid;
  malformed.transaction_end_lt = 10;
  ASSERT_EQ(validate_receipt_set(items, {malformed}, initial).error, ReceiptError::invalid_logical_time);

  malformed = valid;
  malformed.transaction_start_lt = 10;
  malformed.transaction_end_lt = 11;
  ASSERT_TRUE(validate_receipt_set(items, {malformed}, initial));

  malformed = valid;
  malformed.transaction_end_lt = malformed.transaction_start_lt;
  ASSERT_EQ(validate_receipt_set(items, {malformed}, initial).error, ReceiptError::invalid_logical_time);
}

TEST(ParallelInboundScheduler, ReportsOnlyAnIdealOfflineAccountLaneCeiling) {
  const std::vector<double> work{8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0};
  const auto one = estimate_account_lane_ceiling(work, 1);
  const auto four = estimate_account_lane_ceiling(work, 4);
  ASSERT_EQ(one.serial_work, 36.0);
  ASSERT_EQ(one.critical_path, 36.0);
  ASSERT_EQ(one.ideal_speedup(), 1.0);
  ASSERT_EQ(four.serial_work, 36.0);
  ASSERT_EQ(four.critical_path, 9.0);
  ASSERT_EQ(four.ideal_speedup(), 4.0);

  const auto hotspot = estimate_account_lane_ceiling({90.0, 10.0}, 8);
  ASSERT_EQ(hotspot.critical_path, 90.0);
  ASSERT_TRUE(hotspot.ideal_speedup() < 1.12);
  ASSERT_EQ(estimate_account_lane_ceiling({}, 8).ideal_speedup(), 1.0);
}

TEST(ParallelInboundScheduler, ProjectsOnlyMeasuredAccountWorkOutOfTheFullWallPath) {
  const std::vector<double> account_work{0.3, 0.2, 0.1};
  const auto one = estimate_full_path_ceiling(1.0, account_work, 1);
  const auto two = estimate_full_path_ceiling(1.0, account_work, 2);
  ASSERT_TRUE(one.measurement_consistent);
  ASSERT_TRUE(std::abs(one.serial_residue - 0.4) < 1e-12);
  ASSERT_TRUE(std::abs(one.projected_critical_path - 1.0) < 1e-12);
  ASSERT_TRUE(std::abs(one.ideal_speedup() - 1.0) < 1e-12);
  ASSERT_TRUE(std::abs(two.account_critical_path - 0.3) < 1e-12);
  ASSERT_TRUE(std::abs(two.projected_critical_path - 0.7) < 1e-12);
  ASSERT_TRUE(std::abs(two.ideal_speedup() - (1.0 / 0.7)) < 1e-12);

  const auto inconsistent = estimate_full_path_ceiling(0.5, account_work, 2);
  ASSERT_TRUE(!inconsistent.measurement_consistent);
  ASSERT_EQ(inconsistent.serial_residue, 0.0);
}

}  // namespace
}  // namespace ton::validator::parallel_inbound::test
