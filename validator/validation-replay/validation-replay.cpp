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

#include "impl/parallel-inbound-scheduler.h"
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
      std::vector<std::string> params;
      while (!eoln()) {
        std::string token = CO_TRY(next());
        if (token == "--mode") {
          mode = CO_TRY(parse_mode(CO_TRY(next())));
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
      if (params.empty()) {
        co_return td::Status::Error("Expected at least one block id");
      }
      std::vector<BlockId> block_ids;
      for (const std::string& s : params) {
        block_ids.push_back(CO_TRY(BlockId::from_str(s)));
      }
      command_run(std::move(block_ids), mode, log_work_time, exact_tvm_hotpaths).start().detach_silent();
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
                                bool exact_tvm_hotpaths) {
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
    co_await run_start(description);
    auto result = co_await command_run_inner(std::move(block_ids), mode, log_work_time, exact_tvm_hotpaths).wrap();
    if (result.is_error()) {
      LOG(ERROR) << "ERROR run #" << current_run_.idx << ": " << result.error();
      current_run_.status = PSTRING() << "ERROR: " << result.error().to_string()
                                      << "\nLast status: " << current_run_.status;
    }
    run_end();
    co_return {};
  }

  td::actor::Task<> command_run_inner(std::vector<BlockId> block_ids, ReplayMode mode, bool log_work_time,
                                      bool exact_tvm_hotpaths) {
    auto cancellation_token = cancellation_.get_cancellation_token();
    ProcessBlockResult total;
    size_t processed_ok = 0;
    for (size_t i = 0; i < block_ids.size(); ++i) {
      BlockId block_id = block_ids[i];
      auto handle = co_await get_block_by_id(manager_, block_id);
      current_run_.status = "Processing block " + block_id.to_str();
      auto R = co_await process_block(handle, mode, exact_tvm_hotpaths).wrap();
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
      auto R = co_await self->process_block(handle, mode, exact_tvm_hotpaths).wrap();
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
  td::actor::Task<ProcessBlockResult> process_block(ConstBlockHandle handle, ReplayMode mode, bool exact_tvm_hotpaths) {
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

    BlockCandidate candidate;
    if (mode == ReplayMode::collate || mode == ReplayMode::both) {
      auto [task, promise] = td::actor::StartedTask<BlockCandidate>::make_bridge();
      auto [stats_task, stats_promise] = td::actor::StartedTask<CollationStats>::make_bridge();
      LOG(WARNING) << "Collating block " << block_id.id;
      td::Timer timer;
      run_collate_query(
          CollateParams{
              .shard = block_id.shard_full(),
              .min_masterchain_block_id = min_mc_block_id,
              .prev = unpacked.prev,
              .creator = unpacked.creator,
              .validator_set = validator_set,
              .collator_opts = opts_->get_collator_options(),
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
      candidate = co_await std::move(task).trace("collate " + block_id.id.to_str());
      LOG(WARNING) << "Collating block " << block_id.id << ": done, size=" << candidate.data.size() << "/"
                   << block->data().size() << ", cdata_size=" << candidate.collated_data.size()
                   << ", time=" << timer.elapsed();
      auto stats = co_await std::move(stats_task);
      result.collate = ProcessBlockResult::Collate{
          .new_block_size = (double)candidate.data.size(),
          .new_collated_data_size = (double)candidate.collated_data.size(),
          .time = timer.elapsed(),
          .work_time = stats.work_time,
      };
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
      candidate = BlockCandidate{unpacked.creator, block_id, td::sha256_bits256(collated_data), block->data(),
                                 collated_data.clone()};
    }

    if (mode == ReplayMode::validate || mode == ReplayMode::both) {
      LOG(WARNING) << "Validating block " << block_id.id;
      auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
      auto [stats_task, stats_promise] = td::actor::StartedTask<ValidationStats>::make_bridge();
      td::Timer timer;
      run_validate_query(std::move(candidate),
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
