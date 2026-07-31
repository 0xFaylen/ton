// SPDX-License-Identifier: LGPL-2.0-or-later

#include <algorithm>
#include <map>

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

}  // namespace ton::validator::parallel_inbound
