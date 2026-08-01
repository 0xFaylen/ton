// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace ton::validator::parallel_inbound {

using Hash256 = std::array<std::uint8_t, 32>;

struct MessageKey {
  std::uint64_t lt{0};
  Hash256 hash{};

  bool operator<(const MessageKey& other) const;
  bool operator==(const MessageKey& other) const;
};

struct WorkItem {
  MessageKey key;
  std::optional<Hash256> account;
};

// Immutable-by-convention commitment header returned by an account worker.
// The future payload contains the referenced cells and global deltas; the
// coordinator must materialize and hash that payload before committing it.
// A header alone is never sufficient to mutate block state.
struct WorkerReceipt {
  MessageKey input;
  Hash256 account{};
  std::uint64_t account_sequence{0};
  Hash256 pre_account_state_hash{};
  Hash256 transaction_hash{};
  Hash256 post_account_state_hash{};
  Hash256 effects_hash{};
  Hash256 proof_journal_hash{};
  std::uint64_t transaction_start_lt{0};
  std::uint64_t transaction_end_lt{0};
  std::uint64_t gas_used{0};
};

struct AccountCheckpoint {
  Hash256 state_hash{};
  std::uint64_t next_sequence{0};
  std::uint64_t last_transaction_end_lt{0};
};

enum class ReceiptError {
  none,
  size_mismatch,
  non_canonical_input,
  unexpected_coordinator_receipt,
  missing_initial_checkpoint,
  missing_account_predecessor,
  input_key_mismatch,
  account_mismatch,
  account_sequence_mismatch,
  pre_state_mismatch,
  invalid_logical_time,
};

struct ReceiptValidationResult {
  ReceiptError error{ReceiptError::none};
  std::optional<std::size_t> item_index;
  std::size_t verified_receipts{0};
  std::map<Hash256, AccountCheckpoint> checkpoints;

  explicit operator bool() const {
    return error == ReceiptError::none;
  }
};

// Validates completed receipt headers without changing block state. Missing
// receipts are allowed, but a later receipt for the same account cannot cross
// that account-local hole. Global commit order remains the separate
// commit_ready_prefix() invariant.
ReceiptValidationResult validate_receipt_set(const std::vector<WorkItem>& items,
                                             const std::vector<std::optional<WorkerReceipt>>& receipts,
                                             const std::map<Hash256, AccountCheckpoint>& initial_checkpoints);

struct LanePlan {
  std::vector<std::vector<std::size_t>> account_lanes;
  std::vector<std::size_t> coordinator_items;
};

enum class PlanError {
  none,
  no_workers,
  non_canonical_input,
};

struct PlanResult {
  LanePlan plan;
  PlanError error{PlanError::none};

  explicit operator bool() const {
    return error == PlanError::none;
  }
};

// The input order must be the exact OutputQueueMerger order: (lt, message hash).
// Account work is kept serial inside a lane. Items without a local destination
// account stay on the coordinator path (for example, transit messages). The
// first local account is pinned to lane zero for frontier progress; remaining
// account chains are placed largest-first by known item count.
PlanResult build_lane_plan(const std::vector<WorkItem>& items, std::size_t worker_count);

enum class CompletionStatus {
  pending,
  succeeded,
  failed,
};

enum class PrefixStopReason {
  end_of_input,
  pending,
  worker_failure,
  block_limit,
  commit_failure,
};

struct PrefixDecision {
  std::size_t committed_count{0};
  PrefixStopReason stop_reason{PrefixStopReason::end_of_input};

  std::optional<std::size_t> first_uncommitted() const {
    if (stop_reason == PrefixStopReason::end_of_input) {
      return std::nullopt;
    }
    return committed_count;
  }
};

struct LaneCeiling {
  double serial_work{0.0};
  double critical_path{0.0};

  double ideal_speedup() const {
    return critical_path > 0.0 ? serial_work / critical_path : 1.0;
  }
};

struct FullPathCeiling {
  double full_serial_work{0.0};
  double account_serial_work{0.0};
  double serial_residue{0.0};
  double account_critical_path{0.0};
  double projected_critical_path{0.0};
  bool measurement_consistent{true};

  double ideal_speedup() const {
    return projected_critical_path > 0.0 ? full_serial_work / projected_critical_path : 1.0;
  }
};

// Greedy longest-processing-time placement is used only as an offline ceiling
// estimate from measured per-account work. It is not a live scheduling policy
// and excludes merge, proof, storage, and worker-contention overhead.
inline LaneCeiling estimate_account_lane_ceiling(std::vector<double> account_work, std::size_t worker_count) {
  LaneCeiling result;
  result.serial_work = std::accumulate(account_work.begin(), account_work.end(), 0.0);
  if (account_work.empty() || worker_count == 0) {
    return result;
  }
  std::sort(account_work.begin(), account_work.end(), std::greater<>());
  std::vector<double> lanes(std::min(worker_count, account_work.size()), 0.0);
  for (double work : account_work) {
    auto lane = std::min_element(lanes.begin(), lanes.end());
    *lane += work;
  }
  result.critical_path = *std::max_element(lanes.begin(), lanes.end());
  return result;
}

// Amdahl-style shadow projection over a measured full wall path. Only the
// measured account-local portion is replaced by its ideal lane critical path;
// all remaining wall time stays serial. Worker and receipt-merge overhead are
// intentionally absent and must be measured by the next gate.
inline FullPathCeiling estimate_full_path_ceiling(double full_serial_work, std::vector<double> account_work,
                                                  std::size_t worker_count) {
  const auto accounts = estimate_account_lane_ceiling(std::move(account_work), worker_count);
  FullPathCeiling result;
  result.full_serial_work = full_serial_work;
  result.account_serial_work = accounts.serial_work;
  result.measurement_consistent = accounts.serial_work <= full_serial_work + 1e-9;
  result.serial_residue = std::max(0.0, full_serial_work - accounts.serial_work);
  result.account_critical_path = accounts.critical_path;
  result.projected_critical_path = result.serial_residue + accounts.critical_path;
  return result;
}

// Commits only a continuous prefix of the canonical input order. can_start()
// must query the real BlockLimitStatus before the item, exactly as the serial
// collator does. commit() performs the serial mutation of global block state.
// A completed suffix after a hole is deliberately not committed: advancing
// ProcessedUpto over that hole would make the suffix look processed forever.
// completions must start at the current uncommitted frontier; remove an already
// committed prefix before calling this function again.
template <class CanStart, class Commit>
PrefixDecision commit_ready_prefix(const std::vector<CompletionStatus>& completions, CanStart&& can_start,
                                   Commit&& commit) {
  PrefixDecision result;
  for (std::size_t i = 0; i < completions.size(); ++i) {
    if (!can_start(i)) {
      result.stop_reason = PrefixStopReason::block_limit;
      return result;
    }
    if (completions[i] == CompletionStatus::pending) {
      result.stop_reason = PrefixStopReason::pending;
      return result;
    }
    if (completions[i] == CompletionStatus::failed) {
      result.stop_reason = PrefixStopReason::worker_failure;
      return result;
    }
    if (!commit(i)) {
      result.stop_reason = PrefixStopReason::commit_failure;
      return result;
    }
    ++result.committed_count;
  }
  result.stop_reason = PrefixStopReason::end_of_input;
  return result;
}

const char* to_string(PlanError error);
const char* to_string(PrefixStopReason reason);
const char* to_string(ReceiptError error);

}  // namespace ton::validator::parallel_inbound
