// SPDX-License-Identifier: LGPL-2.0-or-later

#include <algorithm>
#include <map>
#include <set>

#include "parallel-inbound-scheduler.h"

namespace ton::validator::parallel_inbound {

bool MessageKey::operator<(const MessageKey& other) const {
  return lt < other.lt || (lt == other.lt && hash < other.hash);
}

bool MessageKey::operator==(const MessageKey& other) const {
  return lt == other.lt && hash == other.hash;
}

PlanResult build_lane_plan(const std::vector<WorkItem>& items, std::size_t worker_count) {
  if (worker_count == 0) {
    return {LanePlan{}, PlanError::no_workers};
  }
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (!(items[i - 1].key < items[i].key)) {
      return {LanePlan{}, PlanError::non_canonical_input};
    }
  }

  LanePlan plan;
  plan.account_lanes.resize(worker_count);
  std::map<Hash256, std::size_t> account_work;
  std::optional<Hash256> frontier_account;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!items[i].account) {
      plan.coordinator_items.push_back(i);
      continue;
    }
    if (!frontier_account) {
      frontier_account = items[i].account;
    }
    ++account_work[*items[i].account];
  }

  std::vector<std::size_t> lane_load(worker_count, 0);
  std::map<Hash256, std::size_t> account_to_lane;
  if (frontier_account) {
    account_to_lane[*frontier_account] = 0;
    lane_load[0] = account_work[*frontier_account];
    account_work.erase(*frontier_account);
  }

  std::vector<std::pair<Hash256, std::size_t>> remaining_accounts(account_work.begin(), account_work.end());
  std::sort(remaining_accounts.begin(), remaining_accounts.end(), [](const auto& left, const auto& right) {
    return left.second > right.second || (left.second == right.second && left.first < right.first);
  });
  for (const auto& [account, work] : remaining_accounts) {
    const auto lane =
        static_cast<std::size_t>(std::min_element(lane_load.begin(), lane_load.end()) - lane_load.begin());
    account_to_lane[account] = lane;
    lane_load[lane] += work;
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!items[i].account) {
      continue;
    }
    plan.account_lanes[account_to_lane.at(*items[i].account)].push_back(i);
  }
  return {std::move(plan), PlanError::none};
}

ReceiptValidationResult validate_receipt_set(const std::vector<WorkItem>& items,
                                             const std::vector<std::optional<WorkerReceipt>>& receipts,
                                             const std::map<Hash256, AccountCheckpoint>& initial_checkpoints) {
  ReceiptValidationResult result;
  result.checkpoints = initial_checkpoints;
  if (items.size() != receipts.size()) {
    result.error = ReceiptError::size_mismatch;
    return result;
  }
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (!(items[i - 1].key < items[i].key)) {
      result.error = ReceiptError::non_canonical_input;
      result.item_index = i;
      return result;
    }
  }

  std::set<Hash256> accounts_with_holes;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    const auto& receipt = receipts[i];
    if (!item.account) {
      if (receipt) {
        result.error = ReceiptError::unexpected_coordinator_receipt;
        result.item_index = i;
        return result;
      }
      continue;
    }
    if (!receipt) {
      accounts_with_holes.insert(*item.account);
      continue;
    }
    if (accounts_with_holes.count(*item.account) != 0) {
      result.error = ReceiptError::missing_account_predecessor;
      result.item_index = i;
      return result;
    }
    if (!(receipt->input == item.key)) {
      result.error = ReceiptError::input_key_mismatch;
      result.item_index = i;
      return result;
    }
    if (receipt->account != *item.account) {
      result.error = ReceiptError::account_mismatch;
      result.item_index = i;
      return result;
    }
    auto checkpoint = result.checkpoints.find(*item.account);
    if (checkpoint == result.checkpoints.end()) {
      result.error = ReceiptError::missing_initial_checkpoint;
      result.item_index = i;
      return result;
    }
    if (receipt->account_sequence != checkpoint->second.next_sequence) {
      result.error = ReceiptError::account_sequence_mismatch;
      result.item_index = i;
      return result;
    }
    if (receipt->pre_account_state_hash != checkpoint->second.state_hash) {
      result.error = ReceiptError::pre_state_mismatch;
      result.item_index = i;
      return result;
    }
    if (receipt->transaction_start_lt <= checkpoint->second.last_transaction_end_lt ||
        receipt->transaction_end_lt < receipt->transaction_start_lt) {
      result.error = ReceiptError::invalid_logical_time;
      result.item_index = i;
      return result;
    }

    checkpoint->second.state_hash = receipt->post_account_state_hash;
    ++checkpoint->second.next_sequence;
    checkpoint->second.last_transaction_end_lt = receipt->transaction_end_lt;
    ++result.verified_receipts;
  }
  return result;
}

const char* to_string(PlanError error) {
  switch (error) {
    case PlanError::none:
      return "none";
    case PlanError::no_workers:
      return "no_workers";
    case PlanError::non_canonical_input:
      return "non_canonical_input";
  }
  return "unknown";
}

const char* to_string(PrefixStopReason reason) {
  switch (reason) {
    case PrefixStopReason::end_of_input:
      return "end_of_input";
    case PrefixStopReason::pending:
      return "pending";
    case PrefixStopReason::worker_failure:
      return "worker_failure";
    case PrefixStopReason::block_limit:
      return "block_limit";
    case PrefixStopReason::commit_failure:
      return "commit_failure";
  }
  return "unknown";
}

const char* to_string(ReceiptError error) {
  switch (error) {
    case ReceiptError::none:
      return "none";
    case ReceiptError::size_mismatch:
      return "size_mismatch";
    case ReceiptError::non_canonical_input:
      return "non_canonical_input";
    case ReceiptError::unexpected_coordinator_receipt:
      return "unexpected_coordinator_receipt";
    case ReceiptError::missing_initial_checkpoint:
      return "missing_initial_checkpoint";
    case ReceiptError::missing_account_predecessor:
      return "missing_account_predecessor";
    case ReceiptError::input_key_mismatch:
      return "input_key_mismatch";
    case ReceiptError::account_mismatch:
      return "account_mismatch";
    case ReceiptError::account_sequence_mismatch:
      return "account_sequence_mismatch";
    case ReceiptError::pre_state_mismatch:
      return "pre_state_mismatch";
    case ReceiptError::invalid_logical_time:
      return "invalid_logical_time";
  }
  return "unknown";
}

}  // namespace ton::validator::parallel_inbound
