/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "td/utils/Timer.h"
#include "td/utils/bits.h"
#include "td/utils/format.h"
#include "ton/ton-types.h"

namespace ton::validator {

// A bounded per-block Space-Saving sketch for locating TVM hot paths without
// creating unbounded code-hash or account-address metric cardinality.
class TvmHotpathStats {
 public:
  static constexpr std::size_t default_capacity = 64;
  static constexpr std::size_t max_accounts_per_code = 10;
  static constexpr std::size_t default_output_limit = 10;
  static constexpr std::size_t exact_capacity = std::numeric_limits<std::size_t>::max();

  struct AccountId {
    WorkchainId workchain = workchainInvalid;
    StdSmcAddress address = StdSmcAddress::zero();

    bool operator<(const AccountId& other) const {
      return workchain < other.workchain || (workchain == other.workchain && address < other.address);
    }
  };

  struct AccountEntry {
    AccountId account;
    td::uint64 executions = 0;
    td::uint64 error = 0;
  };

  struct Entry {
    td::Bits256 code_hash = td::Bits256::zero();
    td::RealCpuTimer::Time observed_time;
    double estimated_wall = 0.0;
    double wall_error = 0.0;
    td::uint64 executions = 0;
    td::uint64 vm_gas_used = 0;
    td::uint64 billed_gas_used = 0;
    td::uint64 vm_steps = 0;
    td::uint64 account_bitmap = 0;
    std::array<AccountEntry, max_accounts_per_code> accounts;
    std::size_t accounts_size = 0;
    bool exact_accounts = false;
    std::map<AccountId, td::uint64> exact_account_executions;

    bool accounts_estimate_saturated() const {
      return account_bitmap == ~td::uint64{0};
    }

    td::uint64 estimated_distinct_accounts() const {
      if (exact_accounts) {
        return exact_account_executions.size();
      }
      const auto occupied = td::count_bits64(account_bitmap);
      if (occupied == 0) {
        return 0;
      }
      if (occupied == 64) {
        return 64;
      }
      const auto estimate = -64.0 * std::log(1.0 - static_cast<double>(occupied) / 64.0);
      return static_cast<td::uint64>(std::llround(estimate));
    }

    std::pair<double, double> top_account_share(std::size_t limit) const {
      if (executions == 0 || limit == 0) {
        return {0.0, 0.0};
      }
      if (exact_accounts) {
        std::vector<td::uint64> counts;
        for (const auto& [_, count] : exact_account_executions) {
          if (counts.size() < limit) {
            counts.push_back(count);
            continue;
          }
          auto smallest = std::min_element(counts.begin(), counts.end());
          if (count > *smallest) {
            *smallest = count;
          }
        }
        td::uint64 sum = 0;
        for (auto count : counts) {
          sum += count;
        }
        const auto share = std::min(1.0, static_cast<double>(sum) / static_cast<double>(executions));
        return {share, share};
      }
      if (accounts_size == 0) {
        return {0.0, 0.0};
      }
      std::array<const AccountEntry*, max_accounts_per_code> ordered{};
      for (std::size_t i = 0; i < accounts_size; ++i) {
        ordered[i] = &accounts[i];
      }
      std::sort(ordered.begin(), ordered.begin() + accounts_size,
                [](const auto* lhs, const auto* rhs) { return lhs->executions > rhs->executions; });
      td::uint64 lower = 0;
      td::uint64 upper = 0;
      for (std::size_t i = 0; i < std::min(limit, accounts_size); ++i) {
        lower += ordered[i]->executions - ordered[i]->error;
        upper += ordered[i]->executions;
      }
      return {std::min(1.0, static_cast<double>(lower) / static_cast<double>(executions)),
              std::min(1.0, static_cast<double>(upper) / static_cast<double>(executions))};
    }

    std::vector<AccountEntry> accounts_by_execution(std::size_t limit) const {
      std::vector<AccountEntry> result;
      if (limit == 0) {
        return result;
      }
      if (exact_accounts) {
        for (const auto& [account, executions] : exact_account_executions) {
          AccountEntry incoming{.account = account, .executions = executions, .error = 0};
          if (result.size() < limit) {
            result.push_back(incoming);
            continue;
          }
          auto smallest = std::min_element(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.executions < rhs.executions;
          });
          if (incoming.executions > smallest->executions) {
            *smallest = incoming;
          }
        }
      } else {
        result.assign(accounts.begin(), accounts.begin() + accounts_size);
      }
      std::sort(result.begin(), result.end(), [](const AccountEntry& lhs, const AccountEntry& rhs) {
        if (lhs.executions != rhs.executions) {
          return lhs.executions > rhs.executions;
        }
        return lhs.account < rhs.account;
      });
      result.resize(std::min(limit, result.size()));
      return result;
    }

   private:
    friend class TvmHotpathStats;

    static td::uint64 account_bit(WorkchainId workchain, const StdSmcAddress& address) {
      td::uint64 hash = 1469598103934665603ULL;
      auto encoded_workchain = static_cast<td::uint32>(workchain);
      for (std::size_t i = 0; i < sizeof(encoded_workchain); ++i) {
        hash = (hash ^ static_cast<td::uint8>(encoded_workchain >> (i * 8))) * 1099511628211ULL;
      }
      for (auto byte : address.as_array()) {
        hash = (hash ^ byte) * 1099511628211ULL;
      }
      return td::uint64{1} << (hash & 63);
    }

    void add_account(WorkchainId workchain, const StdSmcAddress& address, td::uint64 count, td::uint64 error = 0,
                     bool keep_exact = false) {
      AccountId account{workchain, address};
      account_bitmap |= account_bit(workchain, address);
      if (keep_exact || exact_accounts) {
        exact_accounts = true;
        exact_account_executions[account] += count;
        return;
      }
      for (std::size_t i = 0; i < accounts_size; ++i) {
        if (accounts[i].account.workchain == workchain && accounts[i].account.address == address) {
          accounts[i].executions += count;
          accounts[i].error += error;
          return;
        }
      }
      if (accounts_size < accounts.size()) {
        accounts[accounts_size++] = {.account = account, .executions = count, .error = error};
        return;
      }
      auto* smallest = &accounts[0];
      for (auto& account : accounts) {
        if (account.executions < smallest->executions) {
          smallest = &account;
        }
      }
      const auto displaced_count = smallest->executions;
      *smallest = {.account = account, .executions = displaced_count + count, .error = displaced_count + error};
    }

    void merge_accounts(const Entry& other, bool keep_exact) {
      account_bitmap |= other.account_bitmap;
      if (keep_exact) {
        exact_accounts = true;
        if (other.exact_accounts) {
          for (const auto& [account, executions] : other.exact_account_executions) {
            exact_account_executions[account] += executions;
          }
          return;
        }
      }
      for (std::size_t i = 0; i < other.accounts_size; ++i) {
        add_account(other.accounts[i].account.workchain, other.accounts[i].account.address,
                    other.accounts[i].executions, other.accounts[i].error, keep_exact);
      }
    }
  };

  TvmHotpathStats() : TvmHotpathStats(default_capacity) {
  }

  explicit TvmHotpathStats(std::size_t capacity) : capacity_(capacity) {
  }

  void enable_exact() {
    CHECK(total_executions_ == 0);
    CHECK(entries_.empty());
    exact_ = true;
    capacity_ = exact_capacity;
  }

  bool is_exact() const {
    return exact_;
  }

  void record(const td::Bits256& code_hash, WorkchainId workchain, const StdSmcAddress& account,
              td::RealCpuTimer::Time time, td::uint64 vm_gas_used, td::uint64 billed_gas_used, td::uint64 vm_steps) {
    ++total_executions_;
    total_time_ += time;
    total_vm_gas_used_ += vm_gas_used;
    total_billed_gas_used_ += billed_gas_used;
    total_vm_steps_ += vm_steps;

    Entry entry;
    entry.code_hash = code_hash;
    entry.observed_time = time;
    entry.estimated_wall = time.real;
    entry.executions = 1;
    entry.vm_gas_used = vm_gas_used;
    entry.billed_gas_used = billed_gas_used;
    entry.vm_steps = vm_steps;
    entry.add_account(workchain, account, 1, 0, exact_);
    add_entry(std::move(entry));
  }

  void merge(const TvmHotpathStats& other) {
    total_executions_ += other.total_executions_;
    total_time_ += other.total_time_;
    total_vm_gas_used_ += other.total_vm_gas_used_;
    total_billed_gas_used_ += other.total_billed_gas_used_;
    total_vm_steps_ += other.total_vm_steps_;
    replacements_ += other.replacements_;
    exact_complete_ = exact_complete_ && (!exact_ || (other.exact_ && other.exact_complete_));
    for (const auto& entry : other.entries_) {
      add_entry(entry);
    }
  }

  void scale(double factor) {
    total_time_ *= factor;
    total_executions_ = scale_u64(total_executions_, factor);
    total_vm_gas_used_ = scale_u64(total_vm_gas_used_, factor);
    total_billed_gas_used_ = scale_u64(total_billed_gas_used_, factor);
    total_vm_steps_ = scale_u64(total_vm_steps_, factor);
    replacements_ = scale_u64(replacements_, factor);
    for (auto& entry : entries_) {
      entry.observed_time *= factor;
      entry.estimated_wall *= factor;
      entry.wall_error *= factor;
      entry.executions = scale_u64(entry.executions, factor);
      entry.vm_gas_used = scale_u64(entry.vm_gas_used, factor);
      entry.billed_gas_used = scale_u64(entry.billed_gas_used, factor);
      entry.vm_steps = scale_u64(entry.vm_steps, factor);
      for (std::size_t i = 0; i < entry.accounts_size; ++i) {
        entry.accounts[i].executions = scale_u64(entry.accounts[i].executions, factor);
        entry.accounts[i].error = scale_u64(entry.accounts[i].error, factor);
      }
      for (auto& [_, executions] : entry.exact_account_executions) {
        executions = scale_u64(executions, factor);
      }
    }
  }

  std::vector<Entry> entries_by_wall(std::size_t limit = default_output_limit) const {
    const auto ordered = ordered_entries(false);
    std::vector<Entry> result;
    result.reserve(std::min(limit, ordered.size()));
    for (std::size_t i = 0; i < std::min(limit, ordered.size()); ++i) {
      result.push_back(*ordered[i]);
    }
    return result;
  }

  std::string to_str(bool is_cpu, std::size_t limit = default_output_limit) const {
    td::StringBuilder out;
    const auto total = total_time_.get(is_cpu);
    double observed = 0.0;
    for (const auto& entry : entries_) {
      observed += entry.observed_time.get(is_cpu);
    }
    out << "{exec=" << total_executions_ << " " << (is_cpu ? "cpu" : "wall") << "_s=" << total
        << " vm_gas=" << total_vm_gas_used_ << " billed_gas=" << total_billed_gas_used_
        << " vm_steps=" << total_vm_steps_ << " retained_coverage=" << (total > 0.0 ? observed / total : 1.0)
        << " capacity=";
    if (exact_) {
      out << "exact";
    } else {
      out << capacity_;
    }
    out << " replacements=" << replacements_ << " exact_complete=" << (exact_ && exact_complete_) << " entries=[";
    bool first = true;
    const auto ordered = ordered_entries(is_cpu);
    for (std::size_t i = 0; i < std::min(limit, ordered.size()); ++i) {
      const auto& entry = *ordered[i];
      if (!first) {
        out << ",";
      }
      first = false;
      const auto top1 = entry.top_account_share(1);
      const auto top10 = entry.top_account_share(max_accounts_per_code);
      out << "{code_hash=" << entry.code_hash.to_hex() << " exec=" << entry.executions << " "
          << (is_cpu ? "cpu" : "wall") << "_s=" << entry.observed_time.get(is_cpu)
          << " wall_rank_estimate_s=" << entry.estimated_wall << " wall_rank_error_s=" << entry.wall_error
          << " vm_gas=" << entry.vm_gas_used << " billed_gas=" << entry.billed_gas_used
          << " vm_steps=" << entry.vm_steps << " distinct_accounts_est=" << entry.estimated_distinct_accounts()
          << (entry.accounts_estimate_saturated() ? "+" : "") << " top1_share=[" << top1.first << "," << top1.second
          << "] top10_share=[" << top10.first << "," << top10.second
          << "] account_distribution_exact=" << entry.exact_accounts << "}";
    }
    out << "]}";
    return out.as_cslice().str();
  }

  std::string to_json(bool is_cpu, std::size_t offset = 0, std::size_t limit = 100) const {
    td::StringBuilder out;
    const auto total = total_time_.get(is_cpu);
    const auto ordered = ordered_entries(is_cpu);
    const auto begin = std::min(offset, ordered.size());
    const auto end = begin + std::min(limit, ordered.size() - begin);
    double observed = 0.0;
    for (const auto& entry : entries_) {
      observed += entry.observed_time.get(is_cpu);
    }
    out << "{\"exact\":" << exact_ << ",\"exact_complete\":" << (exact_ && exact_complete_) << ",\"metric\":\""
        << (is_cpu ? "cpu" : "wall") << "\",\"total_entries\":" << ordered.size() << ",\"offset\":" << begin
        << ",\"returned\":" << end - begin << ",\"total_executions\":" << total_executions_
        << ",\"total_seconds\":" << total << ",\"retained_coverage\":" << (total > 0.0 ? observed / total : 1.0)
        << ",\"total_vm_gas\":" << total_vm_gas_used_ << ",\"total_billed_gas\":" << total_billed_gas_used_
        << ",\"total_vm_steps\":" << total_vm_steps_ << ",\"replacements\":" << replacements_ << ",\"entries\":[";
    bool first_entry = true;
    for (std::size_t i = begin; i < end; ++i) {
      const auto& entry = *ordered[i];
      if (!first_entry) {
        out << ",";
      }
      first_entry = false;
      const auto top1 = entry.top_account_share(1);
      const auto top10 = entry.top_account_share(max_accounts_per_code);
      out << "{\"code_hash\":\"" << entry.code_hash.to_hex() << "\",\"executions\":" << entry.executions
          << ",\"seconds\":" << entry.observed_time.get(is_cpu)
          << ",\"wall_rank_estimate_seconds\":" << entry.estimated_wall
          << ",\"wall_rank_error_seconds\":" << entry.wall_error << ",\"vm_gas\":" << entry.vm_gas_used
          << ",\"billed_gas\":" << entry.billed_gas_used << ",\"vm_steps\":" << entry.vm_steps
          << ",\"distinct_accounts\":" << entry.estimated_distinct_accounts()
          << ",\"account_distribution_exact\":" << entry.exact_accounts << ",\"top1_share_lower\":" << top1.first
          << ",\"top1_share_upper\":" << top1.second << ",\"top10_share_lower\":" << top10.first
          << ",\"top10_share_upper\":" << top10.second << ",\"top_accounts\":[";
      bool first_account = true;
      for (const auto& account : entry.accounts_by_execution(max_accounts_per_code)) {
        if (!first_account) {
          out << ",";
        }
        first_account = false;
        out << "{\"workchain\":" << account.account.workchain << ",\"address\":\"" << account.account.address.to_hex()
            << "\",\"executions\":" << account.executions << ",\"error\":" << account.error << "}";
      }
      out << "]}";
    }
    out << "]}";
    return out.as_cslice().str();
  }

  std::size_t size() const {
    return entries_.size();
  }

  std::size_t capacity() const {
    return capacity_;
  }

  td::uint64 replacements() const {
    return replacements_;
  }

  td::uint64 total_executions() const {
    return total_executions_;
  }

  td::RealCpuTimer::Time total_time() const {
    return total_time_;
  }

 private:
  static td::uint64 scale_u64(td::uint64 value, double factor) {
    return static_cast<td::uint64>(std::llround(static_cast<double>(value) * factor));
  }

  std::vector<const Entry*> ordered_entries(bool is_cpu) const {
    std::vector<const Entry*> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
      result.push_back(&entry);
    }
    std::sort(result.begin(), result.end(), [is_cpu](const Entry* lhs, const Entry* rhs) {
      const auto lhs_time = is_cpu ? lhs->observed_time.cpu : lhs->estimated_wall;
      const auto rhs_time = is_cpu ? rhs->observed_time.cpu : rhs->estimated_wall;
      if (lhs_time != rhs_time) {
        return lhs_time > rhs_time;
      }
      return lhs->code_hash < rhs->code_hash;
    });
    return result;
  }

  void add_entry(Entry incoming) {
    if (exact_) {
      auto it = exact_entry_index_.find(incoming.code_hash);
      if (it == exact_entry_index_.end()) {
        exact_entry_index_.emplace(incoming.code_hash, entries_.size());
        entries_.push_back(std::move(incoming));
        return;
      }
      merge_entry(entries_[it->second], incoming);
      return;
    }
    for (auto& entry : entries_) {
      if (entry.code_hash == incoming.code_hash) {
        merge_entry(entry, incoming);
        return;
      }
    }
    if (capacity_ == 0) {
      return;
    }
    if (entries_.size() < capacity_) {
      entries_.push_back(std::move(incoming));
      return;
    }
    auto smallest = std::min_element(entries_.begin(), entries_.end(), [](const Entry& lhs, const Entry& rhs) {
      return lhs.estimated_wall < rhs.estimated_wall;
    });
    const auto displaced_estimate = smallest->estimated_wall;
    incoming.estimated_wall += displaced_estimate;
    incoming.wall_error += displaced_estimate;
    *smallest = std::move(incoming);
    ++replacements_;
  }

  void merge_entry(Entry& entry, const Entry& incoming) {
    entry.observed_time += incoming.observed_time;
    entry.estimated_wall += incoming.estimated_wall;
    entry.wall_error += incoming.wall_error;
    entry.executions += incoming.executions;
    entry.vm_gas_used += incoming.vm_gas_used;
    entry.billed_gas_used += incoming.billed_gas_used;
    entry.vm_steps += incoming.vm_steps;
    entry.merge_accounts(incoming, exact_);
  }

  std::size_t capacity_;
  std::vector<Entry> entries_;
  std::map<td::Bits256, std::size_t> exact_entry_index_;
  bool exact_ = false;
  bool exact_complete_ = true;
  td::uint64 replacements_ = 0;
  td::uint64 total_executions_ = 0;
  td::RealCpuTimer::Time total_time_;
  td::uint64 total_vm_gas_used_ = 0;
  td::uint64 total_billed_gas_used_ = 0;
  td::uint64 total_vm_steps_ = 0;
};

}  // namespace ton::validator
