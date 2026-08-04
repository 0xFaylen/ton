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
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "impl/parallel-inbound-scheduler.h"
#include "td/utils/format.h"
#include "td/utils/port/FileFd.h"
#include "ton/ton-io.hpp"

#include "block-auto.h"
#include "block-parse.h"
#include "checksum.h"
#include "fabric.h"
#include "top-shard-descr.hpp"
#include "utils.h"
#include "validation-replay.h"

namespace ton::validator {

namespace {

using AccountWorkMap = std::map<TvmHotpathStats::AccountId, double>;

td::Status write_new_file(td::CSlice path, td::Slice data) {
  TRY_RESULT(file, td::FileFd::open(path, td::FileFd::Write | td::FileFd::CreateNew, 0600));
  TRY_STATUS(file.write_all(data));
  TRY_STATUS(file.sync());
  file.close();
  return td::Status::OK();
}

td::Ref<CollatorOptions> clone_collator_options(const td::Ref<CollatorOptions>& source,
                                                td::uint32 replay_parallel_account_workers,
                                                const td::Bits256& replay_watch_account,
                                                bool replay_log_limit_deltas) {
  auto target = td::Ref<CollatorOptions>{true};
  auto& mutable_target = target.write();
  if (source.not_null()) {
    mutable_target.deferring_enabled = source->deferring_enabled;
    mutable_target.defer_messages_after = source->defer_messages_after;
    mutable_target.defer_out_queue_size_limit = source->defer_out_queue_size_limit;
    mutable_target.dispatch_phase_2_max_total = source->dispatch_phase_2_max_total;
    mutable_target.dispatch_phase_3_max_total = source->dispatch_phase_3_max_total;
    mutable_target.dispatch_phase_2_max_per_initiator = source->dispatch_phase_2_max_per_initiator;
    mutable_target.dispatch_phase_3_max_per_initiator = source->dispatch_phase_3_max_per_initiator;
    mutable_target.whitelist = source->whitelist;
    mutable_target.prioritylist = source->prioritylist;
    mutable_target.force_full_collated_data = source->force_full_collated_data;
    mutable_target.ignore_collated_data_limits = source->ignore_collated_data_limits;
  }
  mutable_target.replay_parallel_account_workers = replay_parallel_account_workers;
  mutable_target.replay_watch_account = replay_watch_account;
  mutable_target.replay_log_limit_deltas = replay_log_limit_deltas;
  return target;
}

std::string account_lane_scope_json(const char* scope, const AccountWorkMap& work_by_account,
                                    std::optional<double> full_collation_wall) {
  td::StringBuilder out;
  if (work_by_account.empty()) {
    out << "{\"available\":false,\"scope\":\"" << scope << "\",\"reason\":\"no_transactions_in_scope\"}";
    return out.as_cslice().str();
  }

  std::vector<double> account_work;
  account_work.reserve(work_by_account.size());
  double total_account_work = 0.0;
  double max_account_work = 0.0;
  for (const auto& [_, work] : work_by_account) {
    account_work.push_back(work);
    total_account_work += work;
    max_account_work = std::max(max_account_work, work);
  }

  out << "{\"available\":true,\"scope\":\"" << scope << "\",\"method\":\"greedy_lpt_account_totals\""
      << ",\"distinct_accounts\":" << work_by_account.size() << ",\"account_serial_seconds\":" << total_account_work
      << ",\"max_account_share\":" << (total_account_work > 0.0 ? max_account_work / total_account_work : 0.0)
      << ",\"full_collation_wall_seconds\":";
  if (full_collation_wall) {
    out << *full_collation_wall;
  } else {
    out << "null";
  }
  out << ",\"workers\":[";
  bool first_worker = true;
  for (std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
    if (!first_worker) {
      out << ",";
    }
    first_worker = false;
    const auto lane = parallel_inbound::estimate_account_lane_ceiling(account_work, workers);
    out << "{\"workers\":" << workers << ",\"account_critical_path_seconds\":" << lane.critical_path
        << ",\"account_ideal_speedup\":" << lane.ideal_speedup();
    if (full_collation_wall) {
      const auto full = parallel_inbound::estimate_full_path_ceiling(*full_collation_wall, account_work, workers);
      out << ",\"measurement_consistent\":" << full.measurement_consistent
          << ",\"serial_residue_seconds\":" << full.serial_residue
          << ",\"projected_full_critical_path_seconds\":" << full.projected_critical_path
          << ",\"projected_full_ideal_speedup\":" << full.ideal_speedup();
    } else {
      out << ",\"measurement_consistent\":null,\"serial_residue_seconds\":null"
             ",\"projected_full_critical_path_seconds\":null,\"projected_full_ideal_speedup\":null";
    }
    out << "}";
  }
  out << "]}";
  return out.as_cslice().str();
}

std::string account_lane_ceiling_json(const TvmHotpathStats& stats, bool is_cpu,
                                      std::optional<double> full_collation_wall) {
  td::StringBuilder out;
  if (!stats.is_exact()) {
    out << "{\"available\":false,\"reason\":\"exact_replay_required\"}";
    return out.as_cslice().str();
  }
  if (!stats.account_work_complete()) {
    out << "{\"available\":false,\"reason\":\"account_work_incomplete\"}";
    return out.as_cslice().str();
  }

  AccountWorkMap all;
  AccountWorkMap inbound_internal;
  AccountWorkMap external;
  AccountWorkMap new_or_deferred;
  AccountWorkMap special;
  for (const auto& entry : stats.account_work_entries()) {
    const auto work = entry.observed_time.get(is_cpu);
    all[entry.account] += work;
    switch (entry.phase) {
      case TvmHotpathStats::AccountWorkPhase::inbound_internal:
        inbound_internal[entry.account] += work;
        break;
      case TvmHotpathStats::AccountWorkPhase::external:
        external[entry.account] += work;
        break;
      case TvmHotpathStats::AccountWorkPhase::new_or_deferred:
        new_or_deferred[entry.account] += work;
        break;
      case TvmHotpathStats::AccountWorkPhase::special:
        special[entry.account] += work;
        break;
      case TvmHotpathStats::AccountWorkPhase::unspecified:
        break;
    }
  }
  if (all.empty()) {
    out << "{\"available\":false,\"reason\":\"no_successful_ordinary_transactions\"}";
    return out.as_cslice().str();
  }

  out << "{\"available\":true,\"metric\":\"" << (is_cpu ? "cpu" : "wall") << "\",\"scopes\":["
      << account_lane_scope_json("all_ordinary", all, full_collation_wall) << ","
      << account_lane_scope_json("inbound_internal", inbound_internal, full_collation_wall) << ","
      << account_lane_scope_json("external", external, full_collation_wall) << ","
      << account_lane_scope_json("new_or_deferred", new_or_deferred, full_collation_wall) << ","
      << account_lane_scope_json("special", special, full_collation_wall)
      << "],\"excludes\":[\"worker_contention\",\"receipt_merge_overhead\"]}";
  return out.as_cslice().str();
}

struct RunInfo {
  size_t idx = 0;
  std::string description;
  std::string status;
  std::optional<TvmHotpathStats> collate_hotpaths;
  std::optional<TvmHotpathStats> validate_hotpaths;
  std::optional<double> collate_wall_seconds;
};

struct StateUpdateFootprint {
  struct MaterializedCell {
    std::string description;
    Ref<vm::Cell> cell;
  };
  // Virtual (level-0) hash of every materialized cell, with a short shape hint.
  std::map<td::Bits256, MaterializedCell> materialized;
  // Virtual hashes referenced by pruned branches, with the first path at which
  // the pruned branch occurs.
  std::map<td::Bits256, std::string> pruned;
};

// A cell under the accounts subtree whose single reference unpacks as an
// Account is a ShardAccounts leaf: label + DepthBalance augmentation +
// ShardAccount. Extracting its address identifies the divergent access.
std::string try_account_leaf_address(const Ref<vm::Cell>& cell) {
  vm::CellSlice cs{vm::NoVm{}, cell};
  if (cs.is_special() || cs.size_refs() != 1) {
    return {};
  }
  auto account_cell = cs.prefetch_ref();
  block::gen::Account::Record_account record;
  if (!tlb::unpack_cell(account_cell, record)) {
    return {};
  }
  WorkchainId workchain = workchainInvalid;
  StdSmcAddress address = StdSmcAddress::zero();
  if (!block::tlb::t_MsgAddressInt.extract_std_address(record.addr, workchain, address)) {
    return {};
  }
  return PSTRING() << workchain << ":" << address.to_hex();
}

// Probing the block's AccountBlocks shows whether the diverged account also
// has a committed transaction or was only read.
std::string try_describe_account_leaf(const Ref<vm::Cell>& cell, const Ref<vm::Cell>& account_blocks) {
  auto address_text = try_account_leaf_address(cell);
  if (address_text.empty()) {
    return {};
  }
  std::string transactions = "lookup_failed";
  const auto colon = address_text.find(':');
  if (account_blocks.not_null() && colon != std::string::npos) {
    StdSmcAddress address = StdSmcAddress::zero();
    if (address.from_hex(address_text.substr(colon + 1)) == 256) {
      vm::AugmentedDictionary blocks_dict{vm::load_cell_slice_ref(account_blocks), 256,
                                          block::tlb::aug_ShardAccountBlocks};
      auto entry = blocks_dict.lookup(address.cbits(), 256);
      transactions = entry.not_null() ? "yes" : "no";
    }
  }
  return PSTRING() << " account=" << address_text << " has_transaction=" << transactions;
}

StateUpdateFootprint collect_state_update_footprint(Ref<vm::Cell> root) {
  StateUpdateFootprint result;
  std::set<vm::Cell::Hash> visited;
  // The ref-index path from the Merkle-update root identifies the state
  // subsystem: "0/..." is the pruned old state, "1/..." the new state; within
  // a ShardStateUnsplit, ref 0 is OutMsgQueueInfo and ref 1 is ShardAccounts.
  // Cells below a ShardAccounts leaf additionally carry the owning account.
  struct PendingCell {
    Ref<vm::Cell> cell;
    std::string path;
    std::string account;
  };
  std::vector<PendingCell> stack;
  stack.push_back({std::move(root), "", ""});
  while (!stack.empty()) {
    auto [cell, path, account] = std::move(stack.back());
    stack.pop_back();
    if (cell.is_null() || !visited.insert(cell->get_hash()).second) {
      continue;
    }
    vm::CellSlice cs{vm::NoVm{}, cell};
    const td::Bits256 virtual_hash = cell->get_hash(0).bits();
    if (cs.is_special() && cs.special_type() == vm::Cell::SpecialType::PrunnedBranch) {
      result.pruned.emplace(virtual_hash, path.empty() ? "root" : path);
      continue;
    }
    if (account.empty()) {
      auto leaf_address = try_account_leaf_address(cell);
      if (!leaf_address.empty()) {
        account = std::move(leaf_address);
      }
    }
    const unsigned tag_bits = std::min<unsigned>(8, cs.size());
    const auto tag = tag_bits != 0 ? static_cast<unsigned>(cs.prefetch_ulong(tag_bits) << (8 - tag_bits)) : 0u;
    result.materialized.emplace(
        virtual_hash,
        StateUpdateFootprint::MaterializedCell{
            PSTRING() << "path=" << (path.empty() ? "root" : path) << " bits=" << cs.size()
                      << " refs=" << cs.size_refs() << " tag=0x" << td::format::as_hex(td::uint8(tag))
                      << (account.empty() ? "" : PSTRING() << " in_account=" << account),
            cell});
    for (unsigned i = 0; i < cs.size_refs(); ++i) {
      constexpr std::size_t max_path_length = 48;
      std::string child_path = path.size() >= max_path_length
                                   ? path
                                   : (path.empty() ? PSTRING() << i : PSTRING() << path << "/" << i);
      stack.push_back({cs.prefetch_ref(i), std::move(child_path), account});
    }
  }
  return result;
}

// Reports which cells one collation pass loaded from the previous state while
// the other pass pruned them. This is the exact cell-usage divergence that
// makes two otherwise identical candidates serialize different Merkle updates.
void append_state_update_divergence(td::StringBuilder& out, const StateUpdateFootprint& serial,
                                    const StateUpdateFootprint& parallel, const Ref<vm::Cell>& account_blocks) {
  const auto describe = [&](const char* label, const StateUpdateFootprint& from, const StateUpdateFootprint& other) {
    constexpr std::size_t max_samples = 8;
    std::size_t usage_diverged = 0;
    std::size_t absent_from_other = 0;
    std::size_t shown = 0;
    for (const auto& [hash, entry] : from.materialized) {
      if (other.materialized.count(hash) != 0) {
        continue;
      }
      auto pruned_it = other.pruned.find(hash);
      if (pruned_it != other.pruned.end()) {
        ++usage_diverged;
        if (shown < max_samples) {
          out << "\n  " << label << " " << hash.to_hex() << " " << entry.description
              << " other_pruned_at=" << pruned_it->second << try_describe_account_leaf(entry.cell, account_blocks);
          ++shown;
        }
      } else {
        ++absent_from_other;
      }
    }
    out << "\n  " << label << " totals: usage_diverged=" << usage_diverged
        << ", absent_from_other=" << absent_from_other;
  };
  describe("serial_loaded_parallel_pruned", serial, parallel);
  describe("parallel_loaded_serial_pruned", parallel, serial);
}

class ValidationReplayerImpl : public ValidationReplayer {
 public:
  static constexpr std::size_t max_stored_runs = 16;

  ValidationReplayerImpl(td::actor::ActorId<ValidatorManager> manager, Ref<ValidatorManagerOptions> opts)
      : manager_(std::move(manager)), opts_(std::move(opts)) {
  }

  td::actor::Task<std::string> run_command(std::string command) override {
    LOG(INFO) << "Command: " << command;
    std::vector<std::string> tokens = tokenize(command);

    if (tokens.empty()) {
      co_return "Validation replayer. `vrp help` for more info";
    }
    size_t idx = 1;
    auto eoln = [&]() -> bool { return idx == tokens.size(); };
    auto next = [&]() -> td::Result<std::string> {
      if (eoln()) {
        return td::Status::Error("unexpected eoln");
      }
      return tokens[idx++];
    };
    auto check_eoln = [&]() -> td::Status {
      if (!eoln()) {
        return td::Status::Error("extra data in command");
      }
      return td::Status::OK();
    };
    if (tokens[0] == "help") {
      CO_TRY(check_eoln());
      co_return command_help();
    }
    if (tokens[0] == "show") {
      CO_TRY(check_eoln());
      co_return command_show();
    }
    if (tokens[0] == "cancel") {
      CO_TRY(check_eoln());
      co_return command_cancel();
    }
    if (tokens[0] == "forget") {
      if (eoln()) {
        co_return td::Status::Error("expected run id");
      }
      auto run_idx = CO_TRY(td::to_integer_safe<td::uint64>(CO_TRY(next())));
      CO_TRY(check_eoln());
      co_return command_forget(run_idx);
    }
    if (tokens[0] == "hotpaths") {
      if (eoln()) {
        co_return td::Status::Error("expected run id");
      }
      auto run_idx = CO_TRY(td::to_integer_safe<td::uint64>(CO_TRY(next())));
      std::string source = "validate";
      bool is_cpu = false;
      std::size_t offset = 0;
      std::size_t limit = 100;
      while (!eoln()) {
        std::string token = CO_TRY(next());
        if (token == "--source") {
          source = CO_TRY(next());
        } else if (token == "--metric") {
          auto metric = CO_TRY(next());
          if (metric == "cpu") {
            is_cpu = true;
          } else if (metric != "wall") {
            co_return td::Status::Error(PSTRING() << "invalid metric " << metric);
          }
        } else if (token == "--offset") {
          offset = CO_TRY(td::to_integer_safe<std::size_t>(CO_TRY(next())));
        } else if (token == "--limit") {
          limit = CO_TRY(td::to_integer_safe<std::size_t>(CO_TRY(next())));
        } else {
          co_return td::Status::Error(PSTRING() << "unknown flag " << token);
        }
      }
      if (limit == 0 || limit > 1000) {
        co_return td::Status::Error("limit must be between 1 and 1000");
      }
      co_return command_hotpaths(run_idx, source, is_cpu, offset, limit);
    }
    if (tokens[0] == "run") {
      ReplayMode mode = ReplayMode::validate;
      bool log_work_time = false;
      bool exact_tvm_hotpaths = false;
      td::uint32 parallel_account_workers = 0;
      bool parallel_first = false;
      td::Bits256 watch_account = td::Bits256::zero();
      std::optional<std::string> collated_data_output;
      std::vector<std::string> params;
      while (!eoln()) {
        std::string token = CO_TRY(next());
        if (token == "--mode") {
          mode = CO_TRY(parse_mode(CO_TRY(next())));
        } else if (token == "--log-work-time") {
          log_work_time = true;
        } else if (token == "--exact-tvm-hotpaths") {
          exact_tvm_hotpaths = true;
        } else if (token == "--parallel-account-workers") {
          parallel_account_workers = CO_TRY(td::to_integer_safe<td::uint32>(CO_TRY(next())));
        } else if (token == "--parallel-first") {
          parallel_first = true;
        } else if (token == "--watch-account") {
          auto hex = CO_TRY(next());
          if (watch_account.from_hex(hex) != 256 || watch_account.is_zero()) {
            co_return td::Status::Error("--watch-account requires 64 non-zero hex digits");
          }
        } else if (token == "--export-collated-data") {
          if (collated_data_output) {
            co_return td::Status::Error("--export-collated-data may be specified only once");
          }
          collated_data_output = CO_TRY(next());
          if (collated_data_output->empty()) {
            co_return td::Status::Error("--export-collated-data path is empty");
          }
        } else if (token[0] == '-') {
          co_return td::Status::Error(PSTRING() << "unknown flag " << token);
        } else {
          params.push_back(std::move(token));
        }
      }
      if (params.empty()) {
        co_return td::Status::Error("Expected at least one block id");
      }
      if (collated_data_output && params.size() != 1) {
        co_return td::Status::Error("--export-collated-data requires exactly one block id");
      }
      if (collated_data_output && mode == ReplayMode::validate) {
        co_return td::Status::Error("--export-collated-data requires collate or both mode");
      }
      if (parallel_account_workers == 1 || parallel_account_workers > 64) {
        co_return td::Status::Error("parallel-account-workers must be zero or between 2 and 64");
      }
      if (parallel_account_workers != 0 && mode != ReplayMode::both) {
        co_return td::Status::Error("--parallel-account-workers requires both mode so ValidateQuery is mandatory");
      }
      if (parallel_first && parallel_account_workers == 0) {
        co_return td::Status::Error("--parallel-first requires --parallel-account-workers");
      }
      std::vector<BlockId> block_ids;
      for (const std::string& s : params) {
        block_ids.push_back(CO_TRY(BlockId::from_str(s)));
      }
      command_run(std::move(block_ids), mode, log_work_time, exact_tvm_hotpaths, parallel_account_workers,
                  parallel_first, watch_account, std::move(collated_data_output))
          .start()
          .detach_silent();
      co_return "Started. `vrp show` to see results.";
    }
    if (tokens[0] == "run-range") {
      ReplayMode mode = ReplayMode::validate;
      bool log_work_time = false;
      bool exact_tvm_hotpaths = false;
      td::uint32 max_jobs = 1;
      std::optional<WorkchainId> wc;
      std::vector<std::string> params;
      while (!eoln()) {
        std::string token = CO_TRY(next());
        if (token == "--mode") {
          mode = CO_TRY(parse_mode(CO_TRY(next())));
        } else if (token == "--wc") {
          wc = CO_TRY(td::to_integer_safe<WorkchainId>(CO_TRY(next())));
        } else if (token == "--max-jobs") {
          max_jobs = CO_TRY(td::to_integer_safe<td::uint32>(CO_TRY(next())));
        } else if (token == "--log-work-time") {
          log_work_time = true;
        } else if (token == "--exact-tvm-hotpaths") {
          exact_tvm_hotpaths = true;
        } else if (token[0] == '-') {
          co_return td::Status::Error(PSTRING() << "unknown flag " << token);
        } else {
          params.push_back(std::move(token));
        }
      }
      if (params.size() != 2) {
        co_return td::Status::Error("Expected two seqnos");
      }
      BlockSeqno start = CO_TRY(td::to_integer_safe<BlockSeqno>(params[0]));
      BlockSeqno end = CO_TRY(td::to_integer_safe<BlockSeqno>(params[1]));
      if (end <= start) {
        co_return td::Status::Error("Invalid range, start should be earlier than end");
      }
      if (max_jobs == 0) {
        co_return td::Status::Error("max-jobs is 0");
      }
      if (max_jobs > 64) {
        co_return td::Status::Error("max-jobs must not exceed 64");
      }
      if (exact_tvm_hotpaths && max_jobs != 1) {
        co_return td::Status::Error("exact TVM hotpaths require max-jobs 1 for reproducible timing");
      }
      command_run_range(start, end, mode, log_work_time, exact_tvm_hotpaths, wc, max_jobs).start().detach_silent();
      co_return "Started. `vrp show` to see results.";
    }
    co_return td::Status::Error("Unknown command " + tokens[0]);
  }

  void update_options(Ref<ValidatorManagerOptions> opts) override {
    opts_ = opts;
  }

 private:
  td::actor::ActorId<ValidatorManager> manager_;
  Ref<ValidatorManagerOptions> opts_;

  std::deque<RunInfo> past_runs_;
  bool busy_ = false;
  RunInfo current_run_;
  std::queue<td::Promise<>> run_queue_;
  size_t next_run_idx_ = 0;
  td::CancellationTokenSource cancellation_;

  std::string command_help() {
    return "Validation replayer - replays collation and validation for old blocks\n"
           "vrp help\tshow help\n"
           "vrp show\tshow runs\n"
           "vrp forget <run_id>\tforget a completed run and release its retained results\n"
           "vrp hotpaths <run_id> [--source validate|collate] [--metric wall|cpu] [--offset n] [--limit n]\n"
           "\treturn a paginated JSON result; limit is 1..1000\n"
           "vrp run [--mode mode] <block_id> ...\tprocess given blocks. Id format: (0,8000000000000000,123456) (no "
           "hashes)\n"
           "\t--mode mode\tcollate/validate/both (default: validate)\n"
           "\t--log-work-time\tshow detailed work time stats\n"
           "\t--exact-tvm-hotpaths\tretain every executed code hash and exact per-hash account counts\n"
           "\t--parallel-account-workers <n>\toffline exact-candidate plus ValidateQuery gate, 2..64; mode=both\n"
           "\t--parallel-first\trun the parallel pass before the serial reference to expose warm-cache bias\n"
           "\t--export-collated-data <path>\twrite one newly collated candidate artifact; refuses overwrite\n"
           "vrp run-range [--mode mode] <start> <end>\tprocess all blocks between mc seqnos <start> and <end>\n"
           "\t--mode mode\tcollate/validate/both (default: validate)\n"
           "\t--log-work-time\tshow detailed work time stats\n"
           "\t--exact-tvm-hotpaths\tretain every executed code hash and exact per-hash account counts\n"
           "\t--wc <wc>\tprocess blocks from <wc> - 0 or -1 (default: both)\n"
           "\t--max-jobs <n>\tmaximum number of blocks processed in parallel, 1..64 (default: 1; exact mode: 1)\n"
           "vrp cancel\tcancel current run\n";
  }

  std::string command_show() {
    td::StringBuilder sb;
    if (past_runs_.empty()) {
      sb << "No past runs\n";
    } else {
      sb << "Past runs (" << past_runs_.size() << "):\n";
      for (const auto& run : past_runs_) {
        sb << "  #" << run.idx << ": " << run.description << "\n";
        sb << "    ";
        for (char c : run.status) {
          sb << c << (c == '\n' ? "    " : "");
        }
        sb << "\n";
      }
    }
    if (busy_) {
      sb << "\nCurrent run #" << current_run_.idx << ": " << current_run_.description << "\n";
      sb << "  ";
      for (char c : current_run_.status) {
        sb << c << (c == '\n' ? "  " : "");
      }
      sb << "\n";
    }
    if (!run_queue_.empty()) {
      sb << "\nRuns in queue: " << run_queue_.size() << "\n";
    }
    return sb.as_cslice().str();
  }

  std::string command_cancel() {
    if (!busy_) {
      return "Nothing to cancel";
    }
    cancellation_.cancel();
    return "Cancelled";
  }

  td::Result<std::string> command_forget(td::uint64 run_idx) {
    for (auto it = past_runs_.begin(); it != past_runs_.end(); ++it) {
      if (it->idx == run_idx) {
        past_runs_.erase(it);
        return PSTRING() << "Forgot run #" << run_idx;
      }
    }
    return td::Status::Error(PSTRING() << "completed run #" << run_idx << " not found");
  }

  td::Result<std::string> command_hotpaths(td::uint64 run_idx, const std::string& source, bool is_cpu,
                                           std::size_t offset, std::size_t limit) const {
    const RunInfo* run = nullptr;
    for (const auto& candidate : past_runs_) {
      if (candidate.idx == run_idx) {
        run = &candidate;
        break;
      }
    }
    if (run == nullptr) {
      return td::Status::Error(PSTRING() << "run #" << run_idx << " not found or still active");
    }
    const std::optional<TvmHotpathStats>* hotpaths = nullptr;
    if (source == "validate") {
      hotpaths = &run->validate_hotpaths;
    } else if (source == "collate") {
      hotpaths = &run->collate_hotpaths;
    } else {
      return td::Status::Error(PSTRING() << "invalid source " << source);
    }
    if (!*hotpaths) {
      return td::Status::Error(PSTRING() << "run #" << run_idx << " has no " << source << " hotpath data");
    }
    const auto full_collation_wall = source == "collate" && !is_cpu ? run->collate_wall_seconds : std::nullopt;
    return PSTRING() << "{\"run\":" << run_idx << ",\"source\":\"" << source
                     << "\",\"data\":" << (*hotpaths)->to_json(is_cpu, offset, limit) << ",\"account_lane_ceiling\":"
                     << account_lane_ceiling_json(**hotpaths, is_cpu, full_collation_wall) << "}";
  }

  td::actor::Task<> command_run(std::vector<BlockId> block_ids, ReplayMode mode, bool log_work_time,
                                bool exact_tvm_hotpaths, td::uint32 parallel_account_workers, bool parallel_first,
                                td::Bits256 watch_account, std::optional<std::string> collated_data_output) {
    std::string description;
    CHECK(!block_ids.empty());
    if (block_ids.size() == 1) {
      description = PSTRING() << "Block " << block_ids[0] << ", mode=" << mode_to_str(mode);
    } else {
      description = PSTRING() << block_ids.size() << " blocks, mode=" << mode_to_str(mode);
    }
    if (exact_tvm_hotpaths) {
      description += ", tvm_hotpaths=exact";
    }
    if (collated_data_output) {
      description += ", collated_data_export=enabled";
    }
    if (parallel_account_workers != 0) {
      description += PSTRING() << ", parallel_account_workers=" << parallel_account_workers
                               << ", pass_order=" << (parallel_first ? "parallel-first" : "serial-first");
    }
    if (!watch_account.is_zero()) {
      description += PSTRING() << ", watch_account=" << watch_account.to_hex();
    }
    co_await run_start(description);
    auto result = co_await command_run_inner(std::move(block_ids), mode, log_work_time, exact_tvm_hotpaths,
                                             parallel_account_workers, parallel_first, watch_account,
                                             std::move(collated_data_output))
                      .wrap();
    if (result.is_error()) {
      LOG(ERROR) << "ERROR run #" << current_run_.idx << ": " << result.error();
      current_run_.status = PSTRING() << "ERROR: " << result.error().to_string()
                                      << "\nLast status: " << current_run_.status;
    }
    run_end();
    co_return {};
  }

  td::actor::Task<> command_run_inner(std::vector<BlockId> block_ids, ReplayMode mode, bool log_work_time,
                                      bool exact_tvm_hotpaths, td::uint32 parallel_account_workers, bool parallel_first,
                                      td::Bits256 watch_account, std::optional<std::string> collated_data_output) {
    auto cancellation_token = cancellation_.get_cancellation_token();
    ProcessBlockResult total;
    size_t processed_ok = 0;
    for (size_t i = 0; i < block_ids.size(); ++i) {
      BlockId block_id = block_ids[i];
      auto handle = co_await get_block_by_id(manager_, block_id);
      current_run_.status = "Processing block " + block_id.to_str();
      auto R = co_await process_block(handle, mode, exact_tvm_hotpaths, collated_data_output, parallel_account_workers,
                                      parallel_first, watch_account)
                   .wrap();
      if (R.is_ok()) {
        total += R.ok();
        ++processed_ok;
      } else {
        LOG(ERROR) << "ERROR run #" << current_run_.idx << " " << handle->id().id << ": " << R.error();
      }
      td::StringBuilder sb;
      if (block_ids.size() == 1) {
        if (R.is_error()) {
          sb << "ERROR: " << R.error().message();
        } else {
          sb << total.to_str(1.0, log_work_time);
        }
      } else {
        sb << "Processed " << i + 1 << "/" << block_ids.size() << " blocks, " << processed_ok << " ok, "
           << i + 1 - processed_ok << " errors";
        if (processed_ok > 0) {
          sb << "\n" << total.to_str((double)processed_ok, log_work_time);
        }
      }
      current_run_.status = sb.as_cslice().str();
      CO_TRY(cancellation_token.check());
    }
    if (total.collate) {
      current_run_.collate_wall_seconds = total.collate->time;
      current_run_.collate_hotpaths = std::move(total.collate->work_time.tvm_hotpath);
    }
    if (total.validate) {
      current_run_.validate_hotpaths = std::move(total.validate->work_time.tvm_hotpath);
    }

    co_return {};
  }

  td::actor::Task<> command_run_range(BlockSeqno mc_seqno_start, BlockSeqno mc_seqno_end, ReplayMode mode,
                                      bool log_work_time, bool exact_tvm_hotpaths, std::optional<WorkchainId> wc,
                                      td::uint32 max_jobs) {
    std::string description = PSTRING() << "MC range " << mc_seqno_start << " to " << mc_seqno_end
                                        << ", mode=" << mode_to_str(mode);
    if (wc) {
      description += PSTRING() << ", wc=" << *wc;
    }
    if (exact_tvm_hotpaths) {
      description += ", tvm_hotpaths=exact";
    }
    co_await run_start(std::move(description));
    auto result = co_await command_run_range_inner(mc_seqno_start, mc_seqno_end, mode, log_work_time,
                                                   exact_tvm_hotpaths, wc, max_jobs)
                      .wrap();
    if (result.is_error()) {
      LOG(ERROR) << "ERROR run #" << current_run_.idx << ": " << result.error();
      current_run_.status = PSTRING() << "ERROR: " << result.error().to_string()
                                      << "\nLast status: " << current_run_.status;
    }
    run_end();
    co_return {};
  }

  td::actor::Task<> command_run_range_inner(BlockSeqno mc_seqno_start, BlockSeqno mc_seqno_end, ReplayMode mode,
                                            bool log_work_time, bool exact_tvm_hotpaths, std::optional<WorkchainId> wc,
                                            td::uint32 max_jobs) {
    auto cancellation_token = cancellation_.get_cancellation_token();
    struct State {
      size_t processed_total = 0;
      size_t processed_ok = 0;
      ProcessBlockResult total;
      td::uint32 running_jobs = 0;
      td::Promise<> wait_for_job_finish;
      td::Promise<> wait_for_all_jobs;
    };
    auto state = std::make_shared<State>();
    BlockSeqno current_mc_seqno = mc_seqno_start;
    td::Timer timer;

    auto update_status = [&] {
      td::StringBuilder sb;
      sb << "mc_seqno=" << current_mc_seqno << " ("
         << td::StringBuilder::FixedDouble(
                (double)(current_mc_seqno - mc_seqno_start) / (double)(mc_seqno_end - mc_seqno_start) * 100.0, 1)
         << "%), processed " << state->processed_total << " blocks, " << state->processed_ok << " ok, "
         << state->processed_total - state->processed_ok << " errors";
      if (state->running_jobs > 0) {
        sb << "; " << state->running_jobs << " in progress";
      }
      sb << "; running for " << td::StringBuilder::FixedDouble(timer.elapsed(), 1) << "s";
      if (state->processed_ok > 0) {
        sb << "\n" << state->total.to_str((double)state->processed_ok, log_work_time);
      }
      current_run_.status = sb.as_cslice().str();
    };

    auto process_block_outer = [](ValidationReplayerImpl* self, size_t run_idx, std::shared_ptr<State> state,
                                  ReplayMode mode, bool exact_tvm_hotpaths,
                                  ConstBlockHandle handle) -> td::actor::Task<> {
      auto R = co_await self->process_block(handle, mode, exact_tvm_hotpaths, std::nullopt).wrap();
      ++state->processed_total;
      if (R.is_ok()) {
        ++state->processed_ok;
        state->total += R.move_as_ok();
      } else {
        LOG(ERROR) << "ERROR run #" << run_idx << " " << handle->id().id << ": " << R.error();
      }
      --state->running_jobs;
      if (state->wait_for_job_finish) {
        state->wait_for_job_finish.set_value(td::Unit{});
      }
      if (state->running_jobs == 0 && state->wait_for_all_jobs) {
        state->wait_for_all_jobs.set_value(td::Unit{});
      }
      co_return {};
    };

    auto process_block = [&](ConstBlockHandle handle, BlockSeqno cur_mc_seqno) -> td::actor::Task<> {
      current_mc_seqno = cur_mc_seqno;
      if (!wc || handle->id().id.workchain == *wc) {
        if (state->running_jobs == max_jobs) {
          auto [task, promise] = td::actor::StartedTask<>::make_bridge();
          state->wait_for_job_finish = std::move(promise);
          co_await std::move(task);
          CHECK(state->running_jobs < max_jobs);
        }
        ++state->running_jobs;
        process_block_outer(this, current_run_.idx, state, mode, exact_tvm_hotpaths, handle).start().detach_silent();
      }
      update_status();
      CO_TRY(cancellation_token.check());
      co_return {};
    };

    co_await process_all_blocks(manager_, mc_seqno_start, mc_seqno_end, !wc || *wc == basechainId,
                                std::move(process_block));

    if (state->running_jobs > 0) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      state->wait_for_all_jobs = std::move(promise);
      co_await std::move(task);
    }
    update_status();
    if (state->total.collate) {
      current_run_.collate_wall_seconds = state->total.collate->time;
      current_run_.collate_hotpaths = std::move(state->total.collate->work_time.tvm_hotpath);
    }
    if (state->total.validate) {
      current_run_.validate_hotpaths = std::move(state->total.validate->work_time.tvm_hotpath);
    }

    co_return {};
  }

  td::actor::Task<> run_start(std::string desc) {
    if (busy_) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      run_queue_.push(std::move(promise));
      co_await std::move(task);
    }
    CHECK(!busy_);
    busy_ = true;
    current_run_ = RunInfo{};
    current_run_.idx = next_run_idx_++;
    current_run_.description = std::move(desc);
    current_run_.status = "Started";
    co_return {};
  }

  void run_end() {
    CHECK(busy_);
    busy_ = false;
    past_runs_.push_back(std::move(current_run_));
    if (past_runs_.size() > max_stored_runs) {
      past_runs_.pop_front();
    }
    if (!run_queue_.empty()) {
      auto promise = std::move(run_queue_.front());
      run_queue_.pop();
      promise.set_value(td::Unit{});
    }
  }

  struct ProcessBlockResult {
    double block_size = 0;
    struct Collate {
      double new_block_size = 0;
      double new_collated_data_size = 0;
      double time = 0.0;
      double serial_time = 0.0;
      td::uint32 parallel_account_workers = 0;
      bool parallel_first = false;
      bool exact_candidate_match = false;
      td::uint64 transactions = 0;
      td::uint64 estimated_bytes = 0;
      td::uint64 gas = 0;
      td::uint64 lt_delta = 0;
      double load_fraction_internals = 0.0;
      int peak_block_limit_class = 0;
      CollationStats::ReplayParallelAccountStats replay_parallel_accounts;
      CollationStats::WorkTimeStats work_time;
    };
    std::optional<Collate> collate;
    struct Validate {
      double time = 0.0;
      ValidationStats::WorkTimeStats work_time;
    };
    std::optional<Validate> validate;

    ProcessBlockResult& operator+=(const ProcessBlockResult& r) {
      block_size += r.block_size;
      if (!collate) {
        collate = r.collate;
      } else if (r.collate) {
        collate->new_block_size += r.collate->new_block_size;
        collate->new_collated_data_size += r.collate->new_collated_data_size;
        collate->time += r.collate->time;
        collate->serial_time += r.collate->serial_time;
        collate->parallel_account_workers = collate->parallel_account_workers == r.collate->parallel_account_workers
                                                ? collate->parallel_account_workers
                                                : 0;
        collate->parallel_first = collate->parallel_first == r.collate->parallel_first && collate->parallel_first;
        collate->exact_candidate_match = collate->exact_candidate_match && r.collate->exact_candidate_match;
        collate->transactions += r.collate->transactions;
        collate->estimated_bytes += r.collate->estimated_bytes;
        collate->gas += r.collate->gas;
        collate->lt_delta += r.collate->lt_delta;
        collate->load_fraction_internals += r.collate->load_fraction_internals;
        collate->peak_block_limit_class = std::max(collate->peak_block_limit_class, r.collate->peak_block_limit_class);
        collate->replay_parallel_accounts += r.collate->replay_parallel_accounts;
        collate->work_time += r.collate->work_time;
      }
      if (!validate) {
        validate = r.validate;
      } else if (r.validate) {
        validate->time += r.validate->time;
        validate->work_time += r.validate->work_time;
      }
      return *this;
    }

    std::string to_str(double n, bool log_work_time) const {
      using Fixed = td::StringBuilder::FixedDouble;
      td::StringBuilder sb;
      if (collate) {
        sb << (n == 1.0 ? "" : "Avg ") << "Collate: size=" << Fixed(collate->new_block_size / n, 0) << "/"
           << Fixed(block_size / n, 0) << ", cdata_size=" << Fixed(collate->new_collated_data_size / n, 0)
           << ", time=" << Fixed(collate->time / n, 6);
        sb << "\n  Block workload: transactions=" << Fixed((double)collate->transactions / n, 2)
           << ", estimated_bytes=" << Fixed((double)collate->estimated_bytes / n, 2)
           << ", gas=" << Fixed((double)collate->gas / n, 2) << ", lt_delta=" << Fixed((double)collate->lt_delta / n, 2)
           << ", internal_load=" << Fixed(collate->load_fraction_internals / n, 6)
           << ", peak_limit_class=" << collate->peak_block_limit_class;
        if (collate->parallel_account_workers != 0) {
          const auto serial_time = collate->serial_time / n;
          const auto parallel_time = collate->time / n;
          sb << "\n  Parallel account replay: workers=" << collate->parallel_account_workers
             << ", pass_order=" << (collate->parallel_first ? "parallel-first" : "serial-first")
             << ", serial_time=" << Fixed(serial_time, 6) << ", parallel_time=" << Fixed(parallel_time, 6)
             << ", speedup=" << Fixed(parallel_time > 0.0 ? serial_time / parallel_time : 0.0, 3)
             << ", exact_candidate_match=" << collate->exact_candidate_match;
          const auto& actual = collate->replay_parallel_accounts;
          sb << "\n  Parallel account actual: attempts=" << Fixed((double)actual.attempts / n, 2)
             << ", batches=" << Fixed((double)actual.batches / n, 2)
             << ", transactions=" << Fixed((double)actual.transactions / n, 2)
             << ", serial_fallbacks=" << Fixed((double)actual.serial_fallbacks / n, 2)
             << ", empty_root_fallbacks=" << Fixed((double)actual.empty_root_fallbacks / n, 2)
             << ", boundary_stops=" << Fixed((double)actual.boundary_stops / n, 2)
             << ", discarded_prepared=" << Fixed((double)actual.discarded_prepared / n, 2)
             << ", prepare_time=" << Fixed(actual.prepare_time.real / n, 6)
             << ", worker_time=" << Fixed(actual.worker_time.real / n, 6)
             << ", commit_time=" << Fixed(actual.commit_time.real / n, 6);
        }
        if (log_work_time) {
          auto wt = collate->work_time;
          wt *= 1.0 / n;
          auto s1 = tokenize(wt.to_str(false));
          auto s2 = tokenize(wt.to_str(true));
          for (size_t i = 0; i < s1.size(); ++i) {
            sb << "\n  " << "real_" << s1[i] << " / " << "cpu_" << s2[i];
          }
        }
      } else {
        sb << (n == 1.0 ? "" : "Avg ") << "Block size = " << Fixed(block_size / n, 0);
      }
      if (validate) {
        sb << "\n" << (n == 1.0 ? "" : "Avg ") << "Validate: time=" << Fixed(validate->time / n, 6);
        if (log_work_time) {
          auto wt = validate->work_time;
          wt *= 1.0 / n;
          auto s1 = tokenize(wt.to_str(false));
          auto s2 = tokenize(wt.to_str(true));
          for (size_t i = 0; i < s1.size(); ++i) {
            sb << "\n  " << "real_" << s1[i] << " / " << "cpu_" << s2[i];
          }
        }
      }
      return sb.as_cslice().str();
    }
  };
  td::actor::Task<ProcessBlockResult> process_block(ConstBlockHandle handle, ReplayMode mode, bool exact_tvm_hotpaths,
                                                    const std::optional<std::string>& collated_data_output,
                                                    td::uint32 parallel_account_workers = 0,
                                                    bool parallel_first = false,
                                                    td::Bits256 watch_account = td::Bits256::zero()) {
    Ref<BlockData> block = co_await td::actor::ask(manager_, &ValidatorManager::get_block_data_from_db, handle);
    ProcessBlockResult result;
    result.block_size = (double)block->data().size();

    BlockIdExt block_id = block->block_id();
    UnpackedBlock unpacked = unpack_block(block);
    BlockIdExt min_mc_block_id =
        (co_await get_block_by_id(manager_, BlockId{masterchainId, shardIdAll, unpacked.min_mc_ref_seqno}))->id();

    Ref<MasterchainState> prev_mc_state{
        co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, unpacked.mc_block_id)};
    auto validator_set = prev_mc_state->get_validator_set(block_id.shard_full());

    std::vector<Ref<ShardTopBlockDescription>> shard_blocks;
    if (block_id.is_masterchain()) {
      Ref<MasterchainState> new_mc_state{
          co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, block_id)};
      shard_blocks = co_await get_shard_block_descriptions(new_mc_state, prev_mc_state, manager_);
    }

    std::unique_ptr<BlockCandidate> candidate;
    if (mode == ReplayMode::collate || mode == ReplayMode::both) {
      struct CollatePass {
        BlockCandidate candidate;
        CollationStats stats;
        double elapsed = 0.0;
      };
      auto run_collate_pass = [&](td::uint32 workers) -> td::actor::Task<CollatePass> {
        auto [task, promise] = td::actor::StartedTask<BlockCandidate>::make_bridge();
        auto [stats_task, stats_promise] = td::actor::StartedTask<CollationStats>::make_bridge();
        td::Timer timer;
        run_collate_query(
            CollateParams{
                .shard = block_id.shard_full(),
                .min_masterchain_block_id = min_mc_block_id,
                .prev = unpacked.prev,
                .creator = unpacked.creator,
                .validator_set = validator_set,
                .collator_opts = clone_collator_options(opts_->get_collator_options(), workers, watch_account,
                                                        parallel_account_workers != 0),
                .utime = (double)unpacked.gen_utime,
                .hard_timeout = td::Timestamp::in(10.0),
                .is_replay = true,
                .in_top_mc_block_id = unpacked.mc_block_id,
                .in_external_messages = unpacked.ext_msgs,
                .in_shard_blocks = shard_blocks,
                .in_rand_seed = unpacked.rand_seed,
                .exact_tvm_hotpaths = exact_tvm_hotpaths,
                .store_stats_to = std::move(stats_promise),
            },
            manager_, {}, std::move(promise));
        auto pass_candidate = co_await std::move(task).trace("collate " + block_id.id.to_str());
        auto elapsed = timer.elapsed();
        auto pass_stats = co_await std::move(stats_task);
        co_return CollatePass{std::move(pass_candidate), std::move(pass_stats), elapsed};
      };

      LOG(WARNING) << "Collating block " << block_id.id;
      std::unique_ptr<CollatePass> selected;
      double serial_time = 0.0;
      bool exact_candidate_match = false;
      if (parallel_account_workers != 0) {
        std::unique_ptr<CollatePass> serial;
        std::unique_ptr<CollatePass> parallel;
        if (parallel_first) {
          parallel = std::make_unique<CollatePass>(co_await run_collate_pass(parallel_account_workers));
          serial = std::make_unique<CollatePass>(co_await run_collate_pass(0));
        } else {
          serial = std::make_unique<CollatePass>(co_await run_collate_pass(0));
          parallel = std::make_unique<CollatePass>(co_await run_collate_pass(parallel_account_workers));
        }
        serial_time = serial->elapsed;
        const bool id_match = serial->candidate.id == parallel->candidate.id;
        const bool collated_hash_match = serial->candidate.collated_file_hash == parallel->candidate.collated_file_hash;
        const bool block_bytes_match = serial->candidate.data.as_slice() == parallel->candidate.data.as_slice();
        const bool collated_bytes_match =
            serial->candidate.collated_data.as_slice() == parallel->candidate.collated_data.as_slice();
        if (!id_match || !collated_hash_match || !block_bytes_match || !collated_bytes_match) {
          auto serial_root = vm::std_boc_deserialize(serial->candidate.data.as_slice()).ensure().move_as_ok();
          auto parallel_root = vm::std_boc_deserialize(parallel->candidate.data.as_slice()).ensure().move_as_ok();
          block::gen::Block::Record serial_block;
          block::gen::Block::Record parallel_block;
          CHECK(block::gen::unpack_cell(serial_root, serial_block));
          CHECK(block::gen::unpack_cell(parallel_root, parallel_block));
          block::gen::BlockExtra::Record serial_extra;
          block::gen::BlockExtra::Record parallel_extra;
          CHECK(block::gen::unpack_cell(serial_block.extra, serial_extra));
          CHECK(block::gen::unpack_cell(parallel_block.extra, parallel_extra));
          td::StringBuilder divergence;
          if (serial_block.state_update->get_hash() != parallel_block.state_update->get_hash()) {
            append_state_update_divergence(divergence, collect_state_update_footprint(serial_block.state_update),
                                           collect_state_update_footprint(parallel_block.state_update),
                                           serial_extra.account_blocks);
          }
          co_return td::Status::Error(
              PSTRING()
              << "serial/parallel candidate mismatch for " << block_id.id << ": id=" << id_match
              << ", collated_hash=" << collated_hash_match << ", block_bytes=" << block_bytes_match << " (serial="
              << serial->candidate.data.size() << "/" << td::sha256_bits256(serial->candidate.data.as_slice()).to_hex()
              << ", parallel=" << parallel->candidate.data.size() << "/"
              << td::sha256_bits256(parallel->candidate.data.as_slice()).to_hex()
              << "), collated_bytes=" << collated_bytes_match << " (serial=" << serial->candidate.collated_data.size()
              << "/" << td::sha256_bits256(serial->candidate.collated_data.as_slice()).to_hex()
              << ", parallel=" << parallel->candidate.collated_data.size() << "/"
              << td::sha256_bits256(parallel->candidate.collated_data.as_slice()).to_hex()
              << "), component_match={info:" << (serial_block.info->get_hash() == parallel_block.info->get_hash())
              << ", value_flow:" << (serial_block.value_flow->get_hash() == parallel_block.value_flow->get_hash())
              << ", state_update:" << (serial_block.state_update->get_hash() == parallel_block.state_update->get_hash())
              << ", extra:" << (serial_block.extra->get_hash() == parallel_block.extra->get_hash())
              << ", in_msg_descr:" << (serial_extra.in_msg_descr->get_hash() == parallel_extra.in_msg_descr->get_hash())
              << ", out_msg_descr:"
              << (serial_extra.out_msg_descr->get_hash() == parallel_extra.out_msg_descr->get_hash())
              << ", account_blocks:"
              << (serial_extra.account_blocks->get_hash() == parallel_extra.account_blocks->get_hash()) << "}"
              << divergence.as_cslice());
        }
        exact_candidate_match = true;
        selected = std::move(parallel);
      } else {
        selected = std::make_unique<CollatePass>(co_await run_collate_pass(0));
      }
      candidate = std::make_unique<BlockCandidate>(std::move(selected->candidate));
      LOG(WARNING) << "Collating block " << block_id.id << ": done, size=" << candidate->data.size() << "/"
                   << block->data().size() << ", cdata_size=" << candidate->collated_data.size()
                   << ", time=" << selected->elapsed;
      result.collate = ProcessBlockResult::Collate{
          .new_block_size = (double)candidate->data.size(),
          .new_collated_data_size = (double)candidate->collated_data.size(),
          .time = selected->elapsed,
          .serial_time = serial_time,
          .parallel_account_workers = parallel_account_workers,
          .parallel_first = parallel_first,
          .exact_candidate_match = exact_candidate_match,
          .transactions = selected->stats.transactions,
          .estimated_bytes = selected->stats.estimated_bytes,
          .gas = selected->stats.gas,
          .lt_delta = selected->stats.lt_delta,
          .load_fraction_internals = selected->stats.load_fraction_internals,
          .peak_block_limit_class = selected->stats.peak_block_limit_class,
          .replay_parallel_accounts = selected->stats.replay_parallel_accounts,
          .work_time = selected->stats.work_time,
      };
      if (collated_data_output) {
        CO_TRY(write_new_file(*collated_data_output, candidate->collated_data.as_slice()));
      }
    } else {
      block::gen::ConsensusExtraData::Record rec;
      rec.flags = 0;
      rec.gen_utime_ms = (td::uint64)unpacked.gen_utime * 1000;
      Ref<vm::Cell> cell;
      CHECK(block::gen::pack_cell(cell, rec));
      std::vector<Ref<vm::Cell>> roots = {cell};
      if (!shard_blocks.empty()) {
        roots.push_back(create_collated_data_shard_block_descr(shard_blocks));
      }
      td::BufferSlice collated_data = vm::std_boc_serialize_multi(std::move(roots), 2).ensure().move_as_ok();
      candidate = std::make_unique<BlockCandidate>(BlockCandidate{
          unpacked.creator, block_id, td::sha256_bits256(collated_data), block->data(), collated_data.clone()});
    }

    if (mode == ReplayMode::validate || mode == ReplayMode::both) {
      LOG(WARNING) << "Validating block " << block_id.id;
      auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
      auto [stats_task, stats_promise] = td::actor::StartedTask<ValidationStats>::make_bridge();
      td::Timer timer;
      run_validate_query(std::move(*candidate),
                         ValidateParams{
                             .shard = block_id.shard_full(),
                             .min_masterchain_block_id = min_mc_block_id,
                             .prev = unpacked.prev,
                             .validator_set = validator_set,
                             .is_replay = true,
                             .exact_tvm_hotpaths = exact_tvm_hotpaths,
                             .store_stats_to = std::move(stats_promise),
                         },
                         manager_, td::Timestamp::in(30.0), std::move(promise));
      auto verdict = co_await std::move(task).trace("validate " + block_id.id.to_str());
      if (verdict.has<CandidateReject>()) {
        co_return td::Status::Error(PSTRING()
                                    << "REJECT " << block_id.id << ": " << verdict.get<CandidateReject>().reason);
      }
      LOG(WARNING) << "Validating block " << block_id.id << ": done, time=" << timer.elapsed();
      auto stats = co_await std::move(stats_task);
      result.validate = ProcessBlockResult::Validate{
          .time = timer.elapsed(),
          .work_time = stats.work_time,
      };
    }
    co_return result;
  }
};

}  // namespace

td::actor::ActorOwn<ValidationReplayer> ValidationReplayer::create(td::actor::ActorId<ValidatorManager> manager,
                                                                   Ref<ValidatorManagerOptions> opts) {
  return td::actor::create_actor<ValidationReplayerImpl>("VRP", manager, opts);
}

}  // namespace ton::validator
