/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "auto/tl/lite_api.hpp"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/check-proof.h"
#include "block/mc-config.h"
#include "common/checksum.h"
#include "emulator/transaction-emulator.h"
#include "td/db/utils/BlobView.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Timer.h"
#include "td/utils/filesystem.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/FileFd.h"
#include "ton/lite-tl.hpp"
#include "validator/db/fileref.hpp"
#include "validator/db/package.hpp"
#include "validator/impl/parallel-coordinator-shadow.h"
#include "validator/impl/parallel-inbound-scheduler.h"
#include "validator/impl/parallel-merkle-proof-merge.h"
#include "validator/impl/parallel-transaction-payload.h"
#include "validator/impl/parallel-worker-pool.h"
#include "validator/impl/selective-split-state.h"
#include "validator/interfaces/tvm-hotpath-stats.h"
#include "vm/boc.h"
#include "vm/cells/CellUsageTree.h"
#include "vm/cells/DataCell.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/db/StaticBagOfCellsDb.h"

namespace {

using td::Ref;
using ton::BlockId;
using ton::BlockIdExt;
using ton::StdSmcAddress;
using ton::validator::TvmHotpathStats;
using ton::validator::parallel_inbound::CanonicalTransactionEffects;
using ton::validator::parallel_inbound::CanonicalTransactionPayload;
using ton::validator::parallel_inbound::CoordinatorCommitContext;
using ton::validator::parallel_inbound::Hash256;
using ton::validator::parallel_inbound::InboundDescriptorContext;
using ton::validator::parallel_inbound::inspect_transaction_payload;
using ton::validator::parallel_inbound::OutboundQueueKey;
using ton::validator::parallel_inbound::WorkItem;

Hash256 as_hash256(const td::Bits256& value) {
  Hash256 result{};
  std::memcpy(result.data(), value.as_slice().data(), result.size());
  return result;
}

td::BitArray<256> as_dictionary_key(const Hash256& value) {
  td::BitArray<256> result;
  std::memcpy(result.data(), value.data(), value.size());
  return result;
}

// Core's persistent-state serializer requires split_depth <= 63. This CLI uses
// whole hexadecimal digits, so 60 is the largest representable supported depth.
constexpr int kMaxHexSplitDepth = 60;

struct BlockContext {
  Ref<vm::Cell> root;
  BlockIdExt id;
  std::vector<BlockIdExt> prev;
  BlockIdExt mc_id;
  td::Bits256 rand_seed = td::Bits256::zero();
  ton::UnixTime gen_utime = 0;
  ton::LogicalTime start_lt = 0;
  ton::LogicalTime end_lt = 0;
  int global_id = 0;
  bool after_split = false;
  bool after_merge = false;
  bool before_split = false;
  Ref<vm::Cell> in_msg_descr;
  Ref<vm::Cell> out_msg_descr;
  Ref<vm::Cell> account_blocks;
  Ref<vm::Cell> state_update;
  std::size_t file_bytes = 0;
};

using HistoryBlocks = std::map<ton::BlockSeqno, BlockContext>;

struct LoadedBlock {
  BlockIdExt id;
  Ref<vm::Cell> root;
  std::size_t file_bytes = 0;
};

struct LoadedState {
  std::shared_ptr<vm::StaticBagOfCellsDb> boc;
  Ref<vm::Cell> serialized_root;
  Ref<vm::Cell> root;
  block::gen::ShardStateUnsplit::Record record;
  BlockId id;
  bool split_header = false;
};

struct LoadedAccountPart {
  std::string prefix_hex;
  int prefix_len = 0;
  td::Bits256 prefix = td::Bits256::zero();
  std::shared_ptr<vm::StaticBagOfCellsDb> boc;
  Ref<vm::Cell> root;
  std::unique_ptr<vm::AugmentedDictionary> accounts;
};

struct LoadedAccountProof {
  StdSmcAddress address;
  BlockIdExt shard_block;
  Ref<vm::Cell> shard_account;
  Ref<vm::Cell> state_proof;
};

struct LoadedLibraryBodies {
  Ref<vm::Cell> root;
  std::set<td::Bits256> hashes;
};

struct ShadowCoordinatorCandidate {
  WorkItem work;
  CanonicalTransactionEffects effects;
  CoordinatorCommitContext context;
  Ref<vm::Cell> pre_account_state;
};

struct ReplayAccountArtifacts {
  std::vector<CanonicalTransactionEffects> limit_effects;
  std::vector<ton::validator::parallel_inbound::BasechainLimitContext> limit_contexts;
  std::vector<ShadowCoordinatorCandidate> coordinator_candidates;
  std::vector<ton::validator::parallel_inbound::AccountDictionaryDelta> account_dictionary_deltas;
  std::map<Hash256, Hash256> inbound_message_transactions;
  std::map<Hash256, Hash256> outbound_message_transactions;
};

struct TransactionKindCounts {
  std::size_t ordinary{0};
  std::size_t tick{0};
  std::size_t tock{0};
  std::size_t storage{0};
  std::size_t split_prepare{0};
  std::size_t split_install{0};
  std::size_t merge_prepare{0};
  std::size_t merge_install{0};

  void add(const TransactionKindCounts& other) {
    ordinary += other.ordinary;
    tick += other.tick;
    tock += other.tock;
    storage += other.storage;
    split_prepare += other.split_prepare;
    split_install += other.split_install;
    merge_prepare += other.merge_prepare;
    merge_install += other.merge_install;
  }

  std::size_t total() const {
    return ordinary + tick + tock + storage + split_prepare + split_install + merge_prepare + merge_install;
  }
};

struct ReplayResult {
  struct LimitTriplet {
    td::uint32 underload = 0;
    td::uint32 soft = 0;
    td::uint32 hard = 0;

    bool operator==(const LimitTriplet&) const = default;
  };

  struct AccountWork {
    std::size_t transactions = 0;
    double transaction_seconds = 0.0;
    double tvm_seconds = 0.0;
  };

  std::size_t target_accounts = 0;
  std::size_t accounts = 0;
  std::size_t skipped_accounts = 0;
  std::size_t transactions = 0;
  std::size_t tvm_transactions = 0;
  TransactionKindCounts transaction_kinds;
  std::size_t canonical_payloads_validated = 0;
  std::size_t canonical_payload_out_messages = 0;
  std::size_t canonical_outbound_registrations = 0;
  std::size_t canonical_inbound_fin_descriptors = 0;
  std::size_t canonical_outbound_deq_imm_descriptors = 0;
  std::size_t canonical_fee_augmentations_validated = 0;
  std::size_t in_msg_descriptors_bound = 0;
  std::size_t out_msg_descriptors_bound = 0;
  bool complete_message_descriptor_coverage = false;
  std::size_t basechain_limit_effects_applied = 0;
  std::size_t basechain_limit_accounts = 0;
  td::uint64 basechain_limit_gas = 0;
  ton::LogicalTime basechain_limit_max_end_lt = 0;
  std::size_t shadow_coordinator_commits = 0;
  std::size_t shadow_coordinator_accounts = 0;
  std::size_t shadow_coordinator_in_descriptors = 0;
  std::size_t shadow_coordinator_out_descriptors = 0;
  std::size_t shadow_coordinator_queue_deletions = 0;
  std::size_t shadow_coordinator_new_messages = 0;
  std::size_t shard_account_proof_values_bound = 0;
  bool predecessor_state_witness_loaded = false;
  bool collated_predecessor_witness_loaded = false;
  std::string predecessor_state_witness_source = "none";
  bool shard_accounts_predecessor_root_bound = false;
  bool shard_accounts_transition_validated = false;
  std::string shard_accounts_transition_status = "not_run";
  std::size_t out_msg_queue_diff_additions = 0;
  std::size_t out_msg_queue_diff_deletions = 0;
  std::size_t out_msg_queue_diff_replacements = 0;
  std::size_t out_msg_queue_additions_bound = 0;
  std::size_t out_msg_queue_deletions_bound = 0;
  bool out_msg_queue_unchanged_root_commitment = false;
  std::string out_msg_queue_transition_status = "not_run";
  std::size_t augmented_dictionary_roots_validated = 0;
  td::uint32 consensus_max_block_bytes = 0;
  td::uint32 consensus_max_collated_bytes = 0;
  td::uint32 consensus_protocol_version = 0;
  td::uint32 consensus_slots_per_leader_window = 0;
  td::uint64 consensus_target_rate_ms = 0;
  td::uint64 consensus_min_block_interval_ms = 0;
  LimitTriplet block_limit_bytes;
  LimitTriplet block_limit_gas;
  LimitTriplet block_limit_lt_delta;
  LimitTriplet block_limit_collated_bytes;
  double phase_setup_seconds = 0.0;
  double phase_account_replay_seconds = 0.0;
  double phase_artifact_export_seconds = 0.0;
  double phase_block_limits_seconds = 0.0;
  double phase_coordinator_seconds = 0.0;
  double phase_augmented_roots_seconds = 0.0;
  double phase_root_inputs_seconds = 0.0;
  double phase_root_predecessor_proof_seconds = 0.0;
  double phase_root_descriptor_baseline_seconds = 0.0;
  double phase_root_account_binding_seconds = 0.0;
  double phase_root_state_transition_seconds = 0.0;
  double phase_root_commit_seconds = 0.0;
  double replay_total_seconds = 0.0;
  TvmHotpathStats hotpaths;
  std::map<StdSmcAddress, AccountWork> account_work;

  ReplayResult() {
    hotpaths.enable_exact();
  }
};

struct PreparedAccountReplay {
  ReplayResult summary;
  ReplayAccountArtifacts artifacts;
};

enum class ReplayExecutionMode {
  full,
  account_effects_only,
  prepared_effects,
};

struct ParallelAccountReplayProbe {
  std::size_t requested_workers{0};
  std::size_t workers{0};
  std::size_t samples{0};
  double worker_pool_startup_seconds{0.0};
  double serial_wall_seconds{0.0};
  double parallel_wall_seconds{0.0};
  std::size_t transactions{0};
  std::size_t accounts{0};
  std::vector<double> serial_wall_samples;
  std::vector<double> parallel_wall_samples;
  std::vector<double> wall_speedup_samples;
  std::vector<std::size_t> lane_accounts;
  std::vector<double> lane_planned_account_seconds;
  std::vector<double> lane_wall_seconds;

  double speedup() const {
    if (wall_speedup_samples.empty()) {
      return 0.0;
    }
    auto values = wall_speedup_samples;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  }
};

struct OfflineCollatorReplayProbe {
  std::size_t requested_workers{0};
  std::size_t workers{0};
  std::size_t transactions{0};
  std::size_t accounts{0};
  double lane_planning_seconds{0.0};
  double worker_pool_startup_seconds{0.0};
  double account_execution_seconds{0.0};
  double artifact_merge_seconds{0.0};
  double equivalence_check_seconds{0.0};
  double serial_commit_seconds{0.0};
  double total_seconds{0.0};
  double serial_reference_seconds{0.0};
  std::size_t augmented_dictionary_roots_validated{0};
  bool exact_artifact_set{false};
  bool exact_replay_result{false};

  double speedup() const {
    return total_seconds > 0.0 ? serial_reference_seconds / total_seconds : 0.0;
  }
};

td::Result<BlockContext> unpack_block_context(LoadedBlock block_data);

td::Status collect_library_refs(Ref<vm::Cell> cell, std::set<vm::Cell::Hash>& visited, std::set<td::Bits256>& libraries,
                                int depth = 1024);
std::string join_library_hashes(const std::set<td::Bits256>& libraries);

struct BlockWorkloadSummary {
  std::size_t distinct_accounts{0};
  std::size_t raw_transactions{0};
  std::size_t max_account_transactions{0};
  TransactionKindCounts transaction_kinds;
};

td::Result<BlockWorkloadSummary> summarize_account_blocks(const BlockContext& block_context);
td::Result<TvmHotpathStats::ExecutionKind> classify_and_count_transaction(Ref<vm::Cell> transaction,
                                                                          TransactionKindCounts& counts);
std::string transaction_kinds_json(const TransactionKindCounts& counts);

td::Status write_new_file(td::CSlice path, td::Slice data) {
  TRY_RESULT(file, td::FileFd::open(path, td::FileFd::Write | td::FileFd::CreateNew, 0600));
  TRY_STATUS(file.write_all(data));
  TRY_STATUS(file.sync());
  file.close();
  return td::Status::OK();
}

td::Result<LoadedBlock> verify_block_data(const BlockIdExt& expected_id, td::Slice data, td::Slice source) {
  if (td::sha256_bits256(data) != expected_id.file_hash) {
    return td::Status::Error(PSLICE() << source << " file hash does not match the expected block id");
  }
  TRY_RESULT_PREFIX(root, vm::std_boc_deserialize(data), PSLICE() << "cannot deserialize " << source << ": ");
  if (td::Bits256(root->get_hash().bits()) != expected_id.root_hash) {
    return td::Status::Error(PSLICE() << source << " root hash does not match the expected block id");
  }
  return LoadedBlock{expected_id, std::move(root), data.size()};
}

struct ArchivedBlockFile {
  BlockIdExt id;
  td::BufferSlice data;
};

td::Result<ArchivedBlockFile> load_block_file_from_archive(const std::string& archive, const BlockId& requested_id) {
  TRY_RESULT(package, ton::Package::open(archive, true, false));
  ArchivedBlockFile found;
  td::Status scan_status = td::Status::OK();
  std::size_t matches = 0;

  TRY_STATUS(package.iterate([&](std::string filename, td::BufferSlice data, td::uint64) {
    auto file_ref = ton::validator::FileReference::create(std::move(filename));
    if (file_ref.is_error()) {
      return true;
    }
    auto parsed = file_ref.move_as_ok();
    parsed.ref().visit(td::overloaded(
        [&](const ton::validator::fileref::Block& block_ref) {
          if (block_ref.block_id.id != requested_id) {
            return;
          }
          ++matches;
          if (matches > 1) {
            scan_status = td::Status::Error("archive contains more than one block file for the requested block id");
            return;
          }
          found.id = block_ref.block_id;
          found.data = std::move(data);
        },
        [&](const auto&) {}));
    return scan_status.is_ok();
  }));

  TRY_STATUS(std::move(scan_status));
  if (found.data.empty()) {
    return td::Status::Error(PSTRING() << "block " << requested_id.to_str() << " was not found in archive");
  }
  return found;
}

td::Result<LoadedBlock> load_block_from_archive(const std::string& archive, const BlockId& requested_id) {
  TRY_RESULT(file, load_block_file_from_archive(archive, requested_id));
  return verify_block_data(file.id, file.data.as_slice(), "archive block");
}

td::Result<LoadedBlock> load_block_from_boc(const std::string& path, const BlockIdExt& expected_id) {
  TRY_RESULT(data, td::read_file(path));
  return verify_block_data(expected_id, data.as_slice(), "block BOC");
}

td::Result<LoadedBlock> load_unanchored_block_boc(const std::string& path) {
  TRY_RESULT(data, td::read_file(path));
  TRY_RESULT_PREFIX(root, vm::std_boc_deserialize(data.as_slice()), "cannot deserialize history block BOC: ");

  block::gen::Block::Record block_record;
  block::gen::BlockInfo::Record info;
  ton::ShardIdFull shard;
  if (!tlb::unpack_cell(root, block_record) || !tlb::unpack_cell(block_record.info, info) || info.version ||
      !block::tlb::t_ShardIdent.unpack(info.shard.write(), shard)) {
    return td::Status::Error("cannot derive block id from history block BOC header");
  }
  BlockIdExt derived_id{BlockId{shard, static_cast<unsigned>(info.seq_no)}, root->get_hash().bits(),
                        td::sha256_bits256(data.as_slice())};
  return LoadedBlock{derived_id, std::move(root), data.size()};
}

td::Result<std::string> derive_block_boc_id_json(const std::string& path) {
  TRY_RESULT(block, load_unanchored_block_boc(path));
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"derive_block_boc_id\",\"block_id\":\"" << block.id.to_str()
      << "\",\"file_bytes\":" << block.file_bytes
      << ",\"anchored\":false,\"warning\":\"self_derived_identity_not_an_external_trust_anchor\"}";
  return out.as_cslice().str();
}

td::Result<std::string> export_block_boc(const std::string& archive, const BlockId& requested_id,
                                         const std::string& output_path) {
  TRY_RESULT(file, load_block_file_from_archive(archive, requested_id));
  TRY_RESULT(block, verify_block_data(file.id, file.data.as_slice(), "archive block"));
  TRY_STATUS(write_new_file(output_path, file.data.as_slice()));
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"block_boc_export\",\"block_id\":\"" << block.id.to_str()
      << "\",\"file_bytes\":" << block.file_bytes << "}";
  return out.as_cslice().str();
}

td::Result<std::string> list_archive_blocks(const std::string& archive) {
  TRY_RESULT(package, ton::Package::open(archive, true, false));
  struct Summary {
    BlockIdExt id;
    std::size_t file_bytes;
    BlockIdExt masterchain_ref;
    td::uint32 gen_utime;
    BlockWorkloadSummary workload;
  };
  std::vector<Summary> blocks;
  td::Status scan_status = td::Status::OK();

  TRY_STATUS(package.iterate([&](std::string filename, td::BufferSlice data, td::uint64) {
    auto file_ref = ton::validator::FileReference::create(std::move(filename));
    if (file_ref.is_error()) {
      return true;
    }
    auto parsed = file_ref.move_as_ok();
    parsed.ref().visit(td::overloaded(
        [&](const ton::validator::fileref::Block& block_ref) {
          if (td::sha256_bits256(data) != block_ref.block_id.file_hash) {
            scan_status = td::Status::Error("block file hash does not match its archive filename");
            return;
          }
          auto root = vm::std_boc_deserialize(data.as_slice());
          if (root.is_error()) {
            scan_status = root.move_as_error_prefix("cannot deserialize archive block: ");
            return;
          }
          auto block_root = root.move_as_ok();
          if (td::Bits256(block_root->get_hash().bits()) != block_ref.block_id.root_hash) {
            scan_status = td::Status::Error("block root hash does not match its archive filename");
            return;
          }
          auto context = unpack_block_context(LoadedBlock{block_ref.block_id, std::move(block_root), data.size()});
          if (context.is_error()) {
            scan_status = context.move_as_error_prefix("cannot inspect archive block: ");
            return;
          }
          auto loaded = context.move_as_ok();
          auto workload = summarize_account_blocks(loaded);
          if (workload.is_error()) {
            scan_status = workload.move_as_error_prefix("cannot summarize archive block: ");
            return;
          }
          blocks.push_back(
              Summary{loaded.id, loaded.file_bytes, loaded.mc_id, loaded.gen_utime, workload.move_as_ok()});
        },
        [&](const auto&) {}));
    return scan_status.is_ok();
  }));
  TRY_STATUS(std::move(scan_status));

  std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) { return left.id.id < right.id.id; });
  std::size_t nonempty_blocks = 0;
  std::size_t total_file_bytes = 0;
  std::size_t total_raw_transactions = 0;
  TransactionKindCounts total_transaction_kinds;
  for (const auto& block : blocks) {
    nonempty_blocks += block.workload.raw_transactions != 0;
    total_file_bytes += block.file_bytes;
    total_raw_transactions += block.workload.raw_transactions;
    total_transaction_kinds.add(block.workload.transaction_kinds);
  }
  struct SizeFit {
    std::size_t samples{0};
    double fixed_bytes{0};
    double marginal_bytes_per_raw_transaction{0};
    double r_squared{0};
    bool valid{false};
  };
  const auto fit_serialized_size = [&](std::size_t minimum_transactions) {
    double sum_x = 0;
    double sum_y = 0;
    double sum_xx = 0;
    double sum_xy = 0;
    SizeFit fit;
    for (const auto& block : blocks) {
      if (block.workload.raw_transactions < minimum_transactions) {
        continue;
      }
      const double x = static_cast<double>(block.workload.raw_transactions);
      const double y = static_cast<double>(block.file_bytes);
      ++fit.samples;
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
    }
    const double sample_count = static_cast<double>(fit.samples);
    const double denominator = sample_count * sum_xx - sum_x * sum_x;
    if (fit.samples < 2 || denominator == 0) {
      return fit;
    }
    fit.marginal_bytes_per_raw_transaction = (sample_count * sum_xy - sum_x * sum_y) / denominator;
    fit.fixed_bytes = (sum_y - fit.marginal_bytes_per_raw_transaction * sum_x) / sample_count;
    const double mean_y = sum_y / sample_count;
    double total_square = 0;
    double residual_square = 0;
    for (const auto& block : blocks) {
      if (block.workload.raw_transactions < minimum_transactions) {
        continue;
      }
      const double x = static_cast<double>(block.workload.raw_transactions);
      const double y = static_cast<double>(block.file_bytes);
      const double predicted = fit.fixed_bytes + fit.marginal_bytes_per_raw_transaction * x;
      total_square += (y - mean_y) * (y - mean_y);
      residual_square += (y - predicted) * (y - predicted);
    }
    if (total_square == 0) {
      return fit;
    }
    fit.r_squared = 1 - residual_square / total_square;
    fit.valid = true;
    return fit;
  };
  const auto write_fit = [](td::StringBuilder& builder, const SizeFit& fit) {
    if (!fit.valid) {
      builder << "null";
      return;
    }
    builder << "{\"samples\":" << fit.samples << ",\"fixed_bytes\":" << fit.fixed_bytes
            << ",\"marginal_bytes_per_raw_transaction\":" << fit.marginal_bytes_per_raw_transaction
            << ",\"r_squared\":" << fit.r_squared << "}";
  };
  const auto all_blocks_fit = fit_serialized_size(0);
  const auto nonempty_blocks_fit = fit_serialized_size(1);
  const auto forty_transaction_blocks_fit = fit_serialized_size(40);
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"archive_block_list\",\"block_count\":" << blocks.size()
      << ",\"nonempty_block_count\":" << nonempty_blocks << ",\"total_file_bytes\":" << total_file_bytes
      << ",\"total_raw_transactions\":" << total_raw_transactions
      << ",\"transaction_kinds\":" << transaction_kinds_json(total_transaction_kinds)
      << ",\"aggregate_bytes_per_raw_transaction\":";
  if (total_raw_transactions == 0) {
    out << "null";
  } else {
    out << static_cast<double>(total_file_bytes) / static_cast<double>(total_raw_transactions);
  }
  out << ",\"diagnostic_serialized_size_ols\":{\"measurement_domain\":\"serialized_block_boc\""
         ",\"capacity_measurement\":false,\"all_blocks\":";
  write_fit(out, all_blocks_fit);
  out << ",\"nonempty_blocks\":";
  write_fit(out, nonempty_blocks_fit);
  out << ",\"blocks_with_at_least_40_transactions\":";
  write_fit(out, forty_transaction_blocks_fit);
  out << "}";
  out << ",\"blocks\":[";
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "{\"block_id\":\"" << blocks[i].id.to_str() << "\",\"file_bytes\":" << blocks[i].file_bytes
        << ",\"masterchain_ref\":\"" << blocks[i].masterchain_ref.to_str() << "\",\"gen_utime\":" << blocks[i].gen_utime
        << ",\"distinct_accounts\":" << blocks[i].workload.distinct_accounts
        << ",\"raw_transactions\":" << blocks[i].workload.raw_transactions
        << ",\"transaction_kinds\":" << transaction_kinds_json(blocks[i].workload.transaction_kinds)
        << ",\"max_account_transactions\":" << blocks[i].workload.max_account_transactions
        << ",\"bytes_per_raw_transaction\":";
    if (blocks[i].workload.raw_transactions == 0) {
      out << "null";
    } else {
      out << static_cast<double>(blocks[i].file_bytes) / static_cast<double>(blocks[i].workload.raw_transactions);
    }
    out << "}";
  }
  out << "]}";
  return out.as_cslice().str();
}

td::Result<BlockContext> unpack_block_context(LoadedBlock block_data) {
  BlockContext result;
  result.id = block_data.id;
  result.root = std::move(block_data.root);
  result.file_bytes = block_data.file_bytes;
  TRY_STATUS(block::unpack_block_prev_blk_try(result.root, result.id, result.prev, result.mc_id, result.after_split));

  block::gen::Block::Record block_record;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  if (!tlb::unpack_cell(result.root, block_record) || !tlb::unpack_cell(block_record.info, info) ||
      !tlb::unpack_cell(block_record.extra, extra)) {
    return td::Status::Error("cannot unpack block header and extra");
  }
  result.global_id = block_record.global_id;
  result.gen_utime = info.gen_utime;
  result.start_lt = info.start_lt;
  result.end_lt = info.end_lt;
  result.after_merge = info.after_merge;
  result.before_split = info.before_split;
  result.rand_seed = extra.rand_seed;
  result.in_msg_descr = std::move(extra.in_msg_descr);
  result.out_msg_descr = std::move(extra.out_msg_descr);
  result.account_blocks = std::move(extra.account_blocks);
  result.state_update = std::move(block_record.state_update);
  return result;
}

td::Result<HistoryBlocks> load_history_block_bocs(const std::vector<std::string>& paths, const BlockContext& target) {
  HistoryBlocks result;
  for (const auto& path : paths) {
    TRY_RESULT(block_data, load_unanchored_block_boc(path));
    TRY_RESULT(block_context, unpack_block_context(std::move(block_data)));
    if (block_context.id.shard_full() != target.id.shard_full()) {
      return td::Status::Error(PSLICE() << "history block " << block_context.id.to_str()
                                        << " belongs to a different shard");
    }
    if (block_context.id.seqno() >= target.id.seqno()) {
      return td::Status::Error(PSLICE() << "history block " << block_context.id.to_str()
                                        << " is not older than the target block");
    }
    const auto seqno = block_context.id.seqno();
    if (!result.emplace(seqno, std::move(block_context)).second) {
      return td::Status::Error(PSLICE() << "duplicate history block at seqno " << seqno);
    }
  }
  return result;
}

td::Result<BlockContext> load_intermediate_block(const std::string& archive, const HistoryBlocks& history,
                                                 const BlockContext& target, ton::BlockSeqno seqno) {
  if (!archive.empty()) {
    BlockId intermediate_id{target.id.shard_full(), seqno};
    TRY_RESULT(intermediate_data, load_block_from_archive(archive, intermediate_id));
    return unpack_block_context(std::move(intermediate_data));
  }
  auto it = history.find(seqno);
  if (it == history.end()) {
    return td::Status::Error(PSLICE() << "missing --history-block-boc for intermediate block at seqno " << seqno);
  }
  return it->second;
}

td::Result<TvmHotpathStats::ExecutionKind> classify_and_count_transaction(Ref<vm::Cell> transaction,
                                                                          TransactionKindCounts& counts) {
  if (transaction.is_null()) {
    return td::Status::Error("cannot classify a null transaction");
  }
  block::gen::Transaction::Record record;
  if (!tlb::unpack_cell(transaction, record)) {
    return td::Status::Error("cannot unpack transaction while classifying its kind");
  }
  const auto tag = block::gen::t_TransactionDescr.get_tag(vm::load_cell_slice(record.description));
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord:
      ++counts.ordinary;
      return TvmHotpathStats::ExecutionKind::ordinary;
    case block::gen::TransactionDescr::trans_tick_tock: {
      block::gen::TransactionDescr::Record_trans_tick_tock tick_tock;
      if (!tlb::unpack_cell(record.description, tick_tock)) {
        return td::Status::Error("cannot unpack tick-tock transaction description while classifying its kind");
      }
      if (tick_tock.is_tock) {
        ++counts.tock;
      } else {
        ++counts.tick;
      }
      return TvmHotpathStats::ExecutionKind::tick_tock;
    }
    case block::gen::TransactionDescr::trans_storage:
      ++counts.storage;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_split_prepare:
      ++counts.split_prepare;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_split_install:
      ++counts.split_install;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_merge_prepare:
      ++counts.merge_prepare;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_merge_install:
      ++counts.merge_install;
      return TvmHotpathStats::ExecutionKind::other;
    default:
      return td::Status::Error("unknown transaction description tag while classifying its kind");
  }
}

std::string transaction_kinds_json(const TransactionKindCounts& counts) {
  td::StringBuilder out;
  out << "{\"total\":" << counts.total() << ",\"ordinary\":" << counts.ordinary << ",\"tick\":" << counts.tick
      << ",\"tock\":" << counts.tock << ",\"storage\":" << counts.storage
      << ",\"split_prepare\":" << counts.split_prepare << ",\"split_install\":" << counts.split_install
      << ",\"merge_prepare\":" << counts.merge_prepare << ",\"merge_install\":" << counts.merge_install << "}";
  return out.as_cslice().str();
}

td::Result<BlockWorkloadSummary> summarize_account_blocks(const BlockContext& block_context) {
  vm::AugmentedDictionary account_blocks{vm::load_cell_slice_ref(block_context.account_blocks), 256,
                                         block::tlb::aug_ShardAccountBlocks};
  BlockWorkloadSummary result;
  td::Status scan_status = td::Status::OK();
  const bool accounts_ok = account_blocks.check_for_each_extra(
      [&](Ref<vm::CellSlice> account_block_slice, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
        if (key_len != 256) {
          scan_status = td::Status::Error("invalid account block key length");
          return false;
        }
        const StdSmcAddress address = key;
        block::gen::AccountBlock::Record account_block;
        if (!tlb::csr_unpack(std::move(account_block_slice), account_block) || account_block.account_addr != address) {
          scan_status = td::Status::Error("cannot unpack AccountBlock");
          return false;
        }

        std::size_t account_transactions = 0;
        vm::AugmentedDictionary transactions{vm::DictNonEmpty(), std::move(account_block.transactions), 64,
                                             block::tlb::aug_AccountTransactions};
        const bool transactions_ok = transactions.check_for_each_extra(
            [&](Ref<vm::CellSlice> transaction_slice, Ref<vm::CellSlice>, td::ConstBitPtr, int tx_key_len) {
              auto transaction = transaction_slice->prefetch_ref();
              if (tx_key_len != 64 || transaction.is_null()) {
                scan_status = td::Status::Error("invalid transaction entry in AccountBlock");
                return false;
              }
              auto kind = classify_and_count_transaction(std::move(transaction), result.transaction_kinds);
              if (kind.is_error()) {
                scan_status = kind.move_as_error();
                return false;
              }
              ++account_transactions;
              return true;
            });
        if (!transactions_ok) {
          if (scan_status.is_ok()) {
            scan_status = td::Status::Error("cannot scan AccountBlock transactions");
          }
          return false;
        }
        ++result.distinct_accounts;
        result.raw_transactions += account_transactions;
        result.max_account_transactions = std::max(result.max_account_transactions, account_transactions);
        return true;
      });
  if (!accounts_ok) {
    if (scan_status.is_ok()) {
      scan_status = td::Status::Error("cannot scan AccountBlocks dictionary");
    }
    return scan_status;
  }
  if (result.transaction_kinds.total() != result.raw_transactions) {
    return td::Status::Error("transaction kind counts do not cover the complete block workload");
  }
  return result;
}

td::Result<LoadedState> load_state_boc_unchecked(const std::string& path, td::Slice description) {
  TRY_RESULT(blob, td::FileBlobView::create(path));
  TRY_RESULT(boc, vm::StaticBagOfCellsDbLazy::create(std::move(blob)));
  TRY_RESULT(root_count, boc->get_root_count());
  if (root_count != 1) {
    return td::Status::Error(PSLICE() << description << " BOC must contain exactly one root, found " << root_count);
  }
  TRY_RESULT(root, boc->get_root_cell(0));
  Ref<vm::Cell> serialized_root = root;

  block::gen::ShardStateUnsplit::Record state;
  bool split_header = false;
  if (!tlb::unpack_cell(root, state)) {
    TRY_RESULT(virtual_root, vm::MerkleProof::virtualize(root));
    block::gen::ShardStateUnsplit::Record virtual_state;
    if (!tlb::unpack_cell(virtual_root, virtual_state)) {
      return td::Status::Error(PSLICE() << "cannot unpack " << description
                                        << " as ShardStateUnsplit or a split-state Merkle header");
    }
    root = std::move(virtual_root);
    state = std::move(virtual_state);
    split_header = true;
  }
  BlockId actual_id{ton::ShardIdFull(block::ShardId{state.shard_id}), static_cast<ton::BlockSeqno>(state.seq_no)};
  return LoadedState{std::move(boc), std::move(serialized_root), std::move(root), std::move(state), actual_id,
                     split_header};
}

td::Result<LoadedState> load_state_boc(const std::string& path, const BlockIdExt& expected_id, td::Slice description) {
  TRY_RESULT(state, load_state_boc_unchecked(path, description));
  const auto& actual_id = state.id;
  if (actual_id != expected_id.id) {
    return td::Status::Error(PSLICE() << description << " id mismatch: expected " << expected_id.id.to_str()
                                      << ", found " << actual_id.to_str());
  }
  return state;
}

td::Result<td::Bits256> parse_hex_prefix(td::Slice prefix) {
  if (prefix.empty() || prefix.size() > kMaxHexSplitDepth / 4) {
    return td::Status::Error("account-part prefix must contain 1..15 hexadecimal digits");
  }
  td::Bits256 result = td::Bits256::zero();
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    char c = prefix[i];
    int digit = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
    if (digit < 0) {
      return td::Status::Error("account-part prefix is not hexadecimal");
    }
    (result.bits() + static_cast<int>(i * 4)).store_uint(static_cast<td::uint64>(digit), 4);
  }
  return result;
}

td::Result<LoadedAccountPart> load_account_part(const std::string& spec, const LoadedState& split_state) {
  auto separator = spec.find('=');
  if (separator == std::string::npos || separator == 0 || separator + 1 == spec.size()) {
    return td::Status::Error("account-part must use HEX_PREFIX=PATH");
  }
  auto prefix_hex = spec.substr(0, separator);
  auto path = spec.substr(separator + 1);
  TRY_RESULT(prefix, parse_hex_prefix(prefix_hex));
  int prefix_len = static_cast<int>(prefix_hex.size() * 4);

  TRY_RESULT(blob, td::FileBlobView::create(path));
  TRY_RESULT(boc, vm::StaticBagOfCellsDbLazy::create(std::move(blob)));
  TRY_RESULT(root_count, boc->get_root_count());
  if (root_count != 1) {
    return td::Status::Error(PSLICE() << "account part " << prefix_hex << " must contain exactly one BOC root");
  }
  TRY_RESULT(root, boc->get_root_cell(0));
  auto accounts = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(root), 256,
                                                            block::tlb::aug_ShardAccounts, false);
  if (!accounts->is_valid() || !accounts->has_common_prefix(prefix.bits(), prefix_len)) {
    return td::Status::Error(PSLICE() << "account part " << prefix_hex << " has an invalid dictionary prefix");
  }

  vm::AugmentedDictionary expected{vm::load_cell_slice_ref(split_state.record.accounts), 256,
                                   block::tlb::aug_ShardAccounts, false};
  if (!expected.cut_prefix_subdict(prefix.bits(), prefix_len) || expected.is_empty()) {
    return td::Status::Error(PSLICE() << "split-state header has no account part for prefix " << prefix_hex);
  }
  auto expected_root = expected.get_wrapped_dict_root();
  if (expected_root.is_null() ||
      td::Bits256(expected_root->get_hash().bits()) != td::Bits256(root->get_hash().bits())) {
    return td::Status::Error(PSLICE() << "account part " << prefix_hex << " hash does not match split-state header");
  }

  return LoadedAccountPart{std::move(prefix_hex), prefix_len,      prefix,
                           std::move(boc),        std::move(root), std::move(accounts)};
}

td::Result<LoadedState> load_config_proof(const std::string& path, const BlockIdExt& expected_id) {
  TRY_RESULT(data, td::read_file(path));
  TRY_RESULT(response, ton::fetch_tl_object<ton::lite_api::liteServer_configInfo>(std::move(data), true));
  auto actual_id = ton::create_block_id(response->id_);
  if (actual_id != expected_id) {
    return td::Status::Error(PSLICE() << "config proof id mismatch: expected " << expected_id.to_str() << ", found "
                                      << actual_id.to_str());
  }
  TRY_RESULT(root, block::check_extract_state_proof(actual_id, response->state_proof_.as_slice(),
                                                    response->config_proof_.as_slice()));
  block::gen::ShardStateUnsplit::Record state;
  if (!tlb::unpack_cell(root, state)) {
    return td::Status::Error("cannot unpack masterchain state from config proof");
  }
  BlockId state_id{ton::ShardIdFull(block::ShardId{state.shard_id}), static_cast<ton::BlockSeqno>(state.seq_no)};
  if (state_id != expected_id.id) {
    return td::Status::Error("masterchain state in config proof has the wrong block id");
  }
  Ref<vm::Cell> serialized_root = root;
  return LoadedState{nullptr, std::move(serialized_root), std::move(root), std::move(state), state_id, false};
}

td::Result<LoadedAccountProof> load_account_proof(const std::string& spec, const BlockContext& target) {
  auto separator = spec.find('=');
  if (separator == std::string::npos || separator == 0 || separator + 1 == spec.size()) {
    return td::Status::Error("account-proof must use ACCOUNT_HEX=PATH");
  }
  auto address_text = spec.substr(0, separator);
  auto path = spec.substr(separator + 1);
  StdSmcAddress address;
  if (address.from_hex(address_text) != 256) {
    return td::Status::Error("account-proof address must contain exactly 64 hexadecimal digits");
  }

  TRY_RESULT(data, td::read_file(path));
  TRY_RESULT(response, ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data), true));
  block::AccountState proof;
  proof.blk = ton::create_block_id(response->id_);
  proof.shard_blk = ton::create_block_id(response->shardblk_);
  proof.shard_proof = std::move(response->shard_proof_);
  proof.proof = std::move(response->proof_);
  proof.state = std::move(response->state_);
  const bool masterchain_bound = proof.blk == target.mc_id;
  const bool predecessor_bound =
      target.prev.size() == 1 && proof.blk == target.prev[0] && proof.shard_blk == target.prev[0];
  if (!masterchain_bound && !predecessor_bound) {
    return td::Status::Error(PSLICE() << "account proof reference must equal the target masterchain reference or exact "
                                         "predecessor shard block: "
                                      << proof.blk.to_str());
  }
  TRY_RESULT(info, proof.validate(proof.blk, block::StdAddress(ton::basechainId, address)));
  TRY_RESULT(proof_roots, vm::std_boc_deserialize_multi(proof.proof.as_slice()));
  if (proof_roots.size() != 2 || proof_roots[1].is_null()) {
    return td::Status::Error("verified account proof has no shard-state proof root");
  }

  Ref<vm::Cell> shard_account;
  if (info.root.not_null()) {
    block::gen::ShardAccount::Record record;
    record.account = std::move(info.root);
    record.last_trans_hash = info.last_trans_hash;
    record.last_trans_lt = info.last_trans_lt;
    if (!block::gen::t_ShardAccount.cell_pack(shard_account, record)) {
      return td::Status::Error("cannot reconstruct ShardAccount from verified account proof");
    }
  }
  return LoadedAccountProof{address, proof.shard_blk, std::move(shard_account), std::move(proof_roots[1])};
}

td::Result<Ref<vm::Cell>> load_out_msg_queue_proof(const std::string& path, const BlockIdExt& expected_id) {
  TRY_RESULT(data, td::read_file(path));
  TRY_RESULT(response, ton::fetch_tl_object<ton::lite_api::liteServer_blockOutMsgQueueSize>(std::move(data), true));
  if (ton::create_block_id(response->id_) != expected_id || !(response->mode_ & 1) || response->proof_.empty()) {
    return td::Status::Error("OutMsgQueueInfo proof is not bound to the expected predecessor block");
  }
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(response->proof_.as_slice()));
  if (roots.size() != 2) {
    return td::Status::Error("OutMsgQueueInfo proof bundle must contain exactly two roots");
  }
  TRY_RESULT(state_proof, vm::std_boc_serialize(roots[0]));
  TRY_RESULT(data_proof, vm::std_boc_serialize(roots[1]));
  TRY_RESULT(state, block::check_extract_state_proof(expected_id, state_proof.as_slice(), data_proof.as_slice()));

  block::gen::ShardStateUnsplit::Record shard_state;
  block::gen::OutMsgQueueInfo::Record queue_info;
  if (!tlb::unpack_cell(state, shard_state) || !tlb::unpack_cell(shard_state.out_msg_queue_info, queue_info)) {
    return td::Status::Error("cannot unpack verified OutMsgQueueInfo proof");
  }
  auto& extra = queue_info.extra.write();
  if (!extra.fetch_long(1)) {
    return td::Status::Error("verified OutMsgQueueInfo proof has no queue-size commitment");
  }
  block::gen::OutMsgQueueExtra::Record queue_extra;
  if (!tlb::unpack(extra, queue_extra)) {
    return td::Status::Error("cannot unpack verified OutMsgQueueExtra");
  }
  auto& size = queue_extra.out_queue_size.write();
  if (!size.fetch_long(1) || size.prefetch_ulong(48) != static_cast<td::uint64>(response->size_)) {
    return td::Status::Error("OutMsgQueueInfo proof size disagrees with the response");
  }
  return state;
}

td::Result<LoadedLibraryBodies> load_library_bodies(const std::vector<std::string>& paths) {
  vm::Dictionary dictionary{256};
  std::set<td::Bits256> hashes;
  for (const auto& path : paths) {
    TRY_RESULT(data, td::read_file(path));
    TRY_RESULT(response, ton::fetch_tl_object<ton::lite_api::liteServer_libraryResult>(std::move(data), true));
    if (response->result_.empty() || response->result_.size() > 256) {
      return td::Status::Error("library body bundle must contain 1..256 entries");
    }
    for (auto& entry : response->result_) {
      TRY_RESULT(cell, vm::std_boc_deserialize(entry->data_.as_slice()));
      if (cell.is_null() || !cell->get_hash().bits().equals(entry->hash_.cbits(), 256)) {
        return td::Status::Error(PSLICE() << "library body hash mismatch for " << entry->hash_.to_hex());
      }
      if (cell->get_depth() > 512) {
        return td::Status::Error(PSLICE() << "library body exceeds VM depth limit: " << entry->hash_.to_hex());
      }
      if (!hashes.insert(entry->hash_).second) {
        continue;
      }
      vm::CellBuilder value;
      if (!value.store_ref_bool(std::move(cell)) ||
          !dictionary.set_builder(entry->hash_.bits(), 256, value, vm::Dictionary::SetMode::Add)) {
        return td::Status::Error(PSLICE() << "cannot register library body " << entry->hash_.to_hex());
      }
    }
  }
  std::set<vm::Cell::Hash> visited;
  std::set<td::Bits256> referenced;
  td::Status scan_status = td::Status::OK();
  const bool valid = dictionary.check_for_each([&](Ref<vm::CellSlice> value, td::ConstBitPtr, int key_len) {
    if (key_len != 256 || value.is_null() || !value->have_refs()) {
      scan_status = td::Status::Error("invalid library body dictionary entry");
      return false;
    }
    scan_status = collect_library_refs(value->prefetch_ref(), visited, referenced);
    return scan_status.is_ok();
  });
  if (!valid) {
    TRY_STATUS(std::move(scan_status));
    return td::Status::Error("cannot scan library body dictionary");
  }
  std::set<td::Bits256> missing;
  for (const auto& hash : referenced) {
    if (hashes.count(hash) == 0) {
      missing.insert(hash);
    }
  }
  if (!missing.empty()) {
    return td::Status::Error(PSLICE() << "library body bundle is missing transitive references: "
                                      << join_library_hashes(missing));
  }
  return LoadedLibraryBodies{dictionary.get_root_cell(), std::move(hashes)};
}

td::Status verify_replay_scope(const BlockContext& target) {
  if (target.id.is_masterchain()) {
    return td::Status::Error("masterchain replay is not supported by this transaction-equivalence tool");
  }
  if (target.id.id.workchain != ton::basechainId) {
    return td::Status::Error("only basechain blocks are supported");
  }
  if (target.prev.size() != 1 || target.after_merge || target.after_split || target.before_split) {
    return td::Status::Error("split/merge boundary blocks are not supported");
  }
  if (!target.mc_id.is_valid() || !target.mc_id.is_masterchain()) {
    return td::Status::Error("block does not contain a valid masterchain reference");
  }
  return td::Status::OK();
}

td::Result<std::set<std::string>> collect_account_prefixes(const BlockContext& target, int split_depth) {
  if (split_depth <= 0 || split_depth > kMaxHexSplitDepth || split_depth % 4 != 0) {
    return td::Status::Error("split depth must be a positive multiple of 4 and at most 60");
  }
  vm::AugmentedDictionary account_blocks{vm::load_cell_slice_ref(target.account_blocks), 256,
                                         block::tlb::aug_ShardAccountBlocks};
  std::set<std::string> result;
  bool valid = account_blocks.check_for_each_extra(
      [&](Ref<vm::CellSlice>, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
        if (key_len != 256) {
          return false;
        }
        result.insert(key.to_hex(split_depth));
        return true;
      });
  if (!valid) {
    return td::Status::Error("cannot enumerate account prefixes from target block");
  }
  return result;
}

td::Result<std::set<StdSmcAddress>> collect_accounts(const BlockContext& block_context) {
  vm::AugmentedDictionary account_blocks{vm::load_cell_slice_ref(block_context.account_blocks), 256,
                                         block::tlb::aug_ShardAccountBlocks};
  std::set<StdSmcAddress> result;
  bool valid = account_blocks.check_for_each_extra(
      [&](Ref<vm::CellSlice>, Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
        if (key_len != 256) {
          return false;
        }
        result.emplace(key);
        return true;
      });
  if (!valid) {
    return td::Status::Error("cannot enumerate accounts from block");
  }
  return result;
}

td::Result<td::Bits256> get_new_state_hash(const BlockContext& block_context) {
  if (block_context.state_update.is_null()) {
    return td::Status::Error("cannot unpack block state update");
  }
  vm::CellSlice update{vm::NoVm(), block_context.state_update};
  if (update.special_type() != vm::Cell::SpecialType::MerkleUpdate || update.size_refs() != 2) {
    return td::Status::Error("cannot unpack block state update");
  }
  return td::Bits256(update.prefetch_ref(1)->get_hash(0).bits());
}

td::Status verify_masterchain_state(const std::string& mc_archive, const BlockIdExt& expected_id,
                                    const LoadedState& mc_state) {
  TRY_RESULT(block_data, load_block_from_archive(mc_archive, expected_id.id));
  TRY_RESULT(block_context, unpack_block_context(std::move(block_data)));
  if (block_context.id != expected_id) {
    return td::Status::Error("masterchain archive block does not match the shard block reference");
  }
  TRY_RESULT(state_hash, get_new_state_hash(block_context));
  if (state_hash != td::Bits256(mc_state.root->get_hash().bits())) {
    return td::Status::Error("masterchain state root hash does not match its producing block");
  }
  return td::Status::OK();
}

td::Status verify_account_state_base(const std::string& archive, const BlockContext& target,
                                     const LoadedState& account_state) {
  if (account_state.id.shard_full() != target.id.shard_full()) {
    return td::Status::Error("account state shard does not match target block shard");
  }
  if (account_state.id.seqno > target.prev[0].seqno()) {
    return td::Status::Error("account state is newer than target predecessor");
  }

  TRY_RESULT(base_data, load_block_from_archive(archive, account_state.id));
  TRY_RESULT(base_block, unpack_block_context(std::move(base_data)));
  TRY_RESULT(base_state_hash, get_new_state_hash(base_block));
  if (base_state_hash != td::Bits256(account_state.root->get_hash().bits())) {
    return td::Status::Error("account state root hash does not match its producing block");
  }

  TRY_RESULT(target_accounts, collect_accounts(target));
  BlockIdExt current = base_block.id;
  for (ton::BlockSeqno seqno = account_state.id.seqno + 1; seqno <= target.prev[0].seqno(); ++seqno) {
    BlockId intermediate_id{target.id.shard_full(), seqno};
    TRY_RESULT(intermediate_data, load_block_from_archive(archive, intermediate_id));
    TRY_RESULT(intermediate, unpack_block_context(std::move(intermediate_data)));
    if (intermediate.prev.size() != 1 || intermediate.prev[0] != current) {
      return td::Status::Error(PSLICE() << "non-linear block history at " << intermediate.id.to_str());
    }
    TRY_RESULT(changed_accounts, collect_accounts(intermediate));
    for (const auto& account : target_accounts) {
      if (changed_accounts.count(account) != 0) {
        return td::Status::Error(PSLICE() << "account state is stale for " << account.to_hex()
                                          << ": changed in intermediate block " << intermediate.id.to_str());
      }
    }
    current = intermediate.id;
  }
  if (current != target.prev[0]) {
    return td::Status::Error("account-state history does not reach target predecessor");
  }
  return td::Status::OK();
}

td::Status verify_account_proof_base(const std::string& archive, const HistoryBlocks& history,
                                     const BlockContext& target, const LoadedAccountProof& proof) {
  if (proof.shard_block.shard_full() != target.id.shard_full()) {
    return td::Status::Error("account proof shard does not match target block shard");
  }
  if (proof.shard_block.seqno() > target.prev[0].seqno()) {
    return td::Status::Error("account proof is newer than target predecessor");
  }

  BlockIdExt current = proof.shard_block;
  for (ton::BlockSeqno seqno = proof.shard_block.seqno() + 1; seqno <= target.prev[0].seqno(); ++seqno) {
    TRY_RESULT(intermediate, load_intermediate_block(archive, history, target, seqno));
    if (intermediate.prev.size() != 1 || intermediate.prev[0] != current) {
      return td::Status::Error(PSLICE() << "non-linear block history at " << intermediate.id.to_str());
    }
    TRY_RESULT(changed_accounts, collect_accounts(intermediate));
    if (changed_accounts.count(proof.address) != 0) {
      return td::Status::Error(PSLICE() << "account proof is stale for " << proof.address.to_hex()
                                        << ": changed in intermediate block " << intermediate.id.to_str());
    }
    current = intermediate.id;
  }
  if (current != target.prev[0]) {
    return td::Status::Error("account-proof history does not reach target predecessor");
  }
  return td::Status::OK();
}

td::Result<Ref<vm::Cell>> extract_partial_shard_accounts_root(const Ref<vm::Cell>& state_root) {
  if (state_root.is_null()) {
    return td::Status::Error("cannot unpack partial ShardAccounts root");
  }
  vm::CellSlice state{vm::NoVm(), state_root};
  if (state.fetch_ulong(32) != 0x9023afe2U || !state.advance(328) || !state.advance_refs(1) || !state.advance(1)) {
    return td::Status::Error("partial state is not ShardStateUnsplit");
  }
  auto accounts = state.fetch_ref();
  if (accounts.is_null()) {
    return td::Status::Error("partial ShardState has no ShardAccounts");
  }
  auto accounts_root = vm::load_cell_slice(accounts).prefetch_ref();
  if (accounts_root.is_null()) {
    return td::Status::Error("partial ShardAccounts root is empty");
  }
  return accounts_root;
}

td::Result<Ref<vm::Cell>> serialize_slice(Ref<vm::CellSlice> slice) {
  if (slice.is_null()) {
    return td::Status::Error("cannot serialize a null cell slice");
  }
  vm::CellBuilder builder;
  if (!builder.append_cellslice_bool(slice->clone())) {
    return td::Status::Error("cannot serialize cell slice");
  }
  return builder.finalize_novm();
}

struct PartialStateDictionaries {
  Ref<vm::Cell> shard_accounts_root;
  Ref<vm::Cell> out_msg_queue_root;
};

struct PartialStateReferences {
  Ref<vm::Cell> out_msg_queue_info;
  Ref<vm::Cell> shard_accounts;
};

td::Result<PartialStateReferences> extract_partial_state_references(const Ref<vm::Cell>& state_root) {
  if (state_root.is_null()) {
    return td::Status::Error("cannot unpack partial ShardState references");
  }
  vm::CellSlice state{vm::NoVm(), state_root};
  if (state.fetch_ulong(32) != 0x9023afe2U || !state.advance(328)) {
    return td::Status::Error("partial state is not ShardStateUnsplit");
  }
  auto out_msg_queue_info = state.fetch_ref();
  bool before_split = false;
  if (out_msg_queue_info.is_null() || !state.fetch_bool_to(before_split)) {
    return td::Status::Error("partial ShardState has no OutMsgQueueInfo");
  }
  auto shard_accounts = state.fetch_ref();
  if (shard_accounts.is_null()) {
    return td::Status::Error("partial ShardState has no ShardAccounts");
  }
  return PartialStateReferences{std::move(out_msg_queue_info), std::move(shard_accounts)};
}

td::Result<Ref<vm::Cell>> extract_out_msg_queue_wrapper(const Ref<vm::Cell>& out_msg_queue_info) {
  if (out_msg_queue_info.is_null()) {
    return td::Status::Error("cannot unpack partial OutMsgQueueInfo");
  }
  vm::CellSlice queue_info{vm::NoVm(), out_msg_queue_info};
  bool queue_non_empty = false;
  if (!queue_info.fetch_bool_to(queue_non_empty)) {
    return td::Status::Error("partial OutMsgQueueInfo has no OutMsgQueue");
  }
  vm::CellBuilder out_queue;
  if (!out_queue.store_bool_bool(queue_non_empty)) {
    return td::Status::Error("cannot reconstruct partial OutMsgQueue wrapper");
  }
  if (queue_non_empty) {
    auto dictionary_root = queue_info.fetch_ref();
    if (dictionary_root.is_null() || !out_queue.store_ref_bool(std::move(dictionary_root))) {
      return td::Status::Error("partial OutMsgQueue has no dictionary root");
    }
  }
  return out_queue.finalize_novm();
}

td::Result<PartialStateDictionaries> extract_partial_state_dictionaries(const Ref<vm::Cell>& state_root) {
  if (state_root.is_null()) {
    return td::Status::Error("cannot unpack partial ShardState dictionaries");
  }
  vm::CellSlice state{vm::NoVm(), state_root};
  if (state.fetch_ulong(32) != 0x9023afe2U || !state.advance(328)) {
    return td::Status::Error("partial state is not ShardStateUnsplit");
  }
  auto out_msg_queue_info = state.fetch_ref();
  bool before_split = false;
  if (out_msg_queue_info.is_null() || !state.fetch_bool_to(before_split)) {
    return td::Status::Error("partial ShardState has no OutMsgQueueInfo");
  }
  auto shard_accounts_root = state.fetch_ref();
  if (shard_accounts_root.is_null()) {
    return td::Status::Error("partial ShardState has no ShardAccounts");
  }

  TRY_RESULT(out_msg_queue_root, extract_out_msg_queue_wrapper(out_msg_queue_info));
  return PartialStateDictionaries{std::move(shard_accounts_root), std::move(out_msg_queue_root)};
}

// HashmapAug nodes carry augmentation bits after their references. Structural
// proof diffing must parse labels without applying the plain-Hashmap fork shape
// check; typed value extraction remains delegated to AugmentedDictionary.
class StructuralAugmentedDictionary final : public vm::DictionaryFixed {
 public:
  StructuralAugmentedDictionary(Ref<vm::Cell> root, int key_bits)
      : vm::DictionaryFixed(std::move(root), key_bits, false) {
  }

 protected:
  int label_mode() const override {
    return vm::dict::LabelParser::chk_size;
  }
};

td::Result<Ref<vm::Cell>> validate_inbound_descriptor(Ref<vm::CellSlice> descriptor, const Hash256& message_hash,
                                                      const Hash256& transaction_hash) {
  TRY_RESULT(descriptor_cell, serialize_slice(std::move(descriptor)));
  auto descriptor_slice = vm::load_cell_slice_ref(descriptor_cell);
  const auto tag = block::gen::t_InMsg.get_tag(*descriptor_slice);
  Ref<vm::Cell> message;
  Ref<vm::Cell> transaction;
  if (tag == block::gen::InMsg::msg_import_ext) {
    auto unpack_slice = descriptor_slice->clone();
    if (!block::gen::t_InMsg.unpack_msg_import_ext(unpack_slice, message, transaction)) {
      return td::Status::Error("cannot unpack canonical external inbound descriptor");
    }
  } else {
    Ref<vm::Cell> envelope;
    if (tag == block::gen::InMsg::msg_import_imm) {
      block::gen::InMsg::Record_msg_import_imm in;
      if (!tlb::csr_unpack(td::make_ref<vm::CellSlice>(descriptor_slice->clone()), in)) {
        return td::Status::Error("cannot unpack canonical immediate inbound descriptor");
      }
      envelope = std::move(in.in_msg);
      transaction = std::move(in.transaction);
    } else if (tag == block::gen::InMsg::msg_import_fin) {
      block::gen::InMsg::Record_msg_import_fin in;
      if (!tlb::csr_unpack(td::make_ref<vm::CellSlice>(descriptor_slice->clone()), in)) {
        return td::Status::Error("cannot unpack canonical final inbound descriptor");
      }
      envelope = std::move(in.in_msg);
      transaction = std::move(in.transaction);
    } else if (tag == block::gen::InMsg::msg_import_deferred_fin) {
      block::gen::InMsg::Record_msg_import_deferred_fin in;
      if (!tlb::csr_unpack(td::make_ref<vm::CellSlice>(descriptor_slice->clone()), in)) {
        return td::Status::Error("cannot unpack canonical deferred inbound descriptor");
      }
      envelope = std::move(in.in_msg);
      transaction = std::move(in.transaction);
    } else if (tag == block::gen::InMsg::msg_import_ihr) {
      block::gen::InMsg::Record_msg_import_ihr in;
      if (!tlb::csr_unpack(td::make_ref<vm::CellSlice>(descriptor_slice->clone()), in)) {
        return td::Status::Error("cannot unpack canonical IHR inbound descriptor");
      }
      message = std::move(in.msg);
      transaction = std::move(in.transaction);
    } else {
      return td::Status::Error("canonical transaction refers to an unsupported inbound descriptor tag");
    }
    if (message.is_null()) {
      block::tlb::MsgEnvelope::Record_std envelope_record;
      if (envelope.is_null() || !tlb::unpack_cell(envelope, envelope_record)) {
        return td::Status::Error("cannot unpack canonical inbound message envelope");
      }
      message = std::move(envelope_record.msg);
    }
  }
  if (message.is_null() || transaction.is_null() || as_hash256(message->get_hash().as_bits256()) != message_hash ||
      as_hash256(transaction->get_hash().as_bits256()) != transaction_hash) {
    return td::Status::Error("canonical inbound descriptor binding failed");
  }
  return descriptor_cell;
}

td::Result<Ref<vm::Cell>> validate_generated_outbound_descriptor(Ref<vm::CellSlice> descriptor,
                                                                 const Hash256& message_hash,
                                                                 const Hash256& transaction_hash) {
  TRY_RESULT(descriptor_cell, serialize_slice(std::move(descriptor)));
  auto descriptor_slice = vm::load_cell_slice_ref(descriptor_cell);
  const auto tag = block::gen::t_OutMsg.get_tag(*descriptor_slice);
  Ref<vm::Cell> message;
  Ref<vm::Cell> transaction;
  if (tag == block::gen::OutMsg::msg_export_ext) {
    auto unpack_slice = descriptor_slice->clone();
    if (!block::gen::t_OutMsg.unpack_msg_export_ext(unpack_slice, message, transaction)) {
      return td::Status::Error("cannot unpack canonical external outbound descriptor");
    }
  } else {
    Ref<vm::Cell> envelope;
    if (tag == block::gen::OutMsg::msg_export_new) {
      block::gen::OutMsg::Record_msg_export_new out;
      auto unpack_slice = td::make_ref<vm::CellSlice>(descriptor_slice->clone());
      if (!tlb::csr_unpack(unpack_slice, out)) {
        return td::Status::Error("cannot unpack canonical queued outbound descriptor");
      }
      envelope = std::move(out.out_msg);
      transaction = std::move(out.transaction);
    } else if (tag == block::gen::OutMsg::msg_export_imm) {
      block::gen::OutMsg::Record_msg_export_imm out;
      auto unpack_slice = td::make_ref<vm::CellSlice>(descriptor_slice->clone());
      if (!tlb::csr_unpack(unpack_slice, out)) {
        return td::Status::Error("cannot unpack canonical immediate outbound descriptor");
      }
      envelope = std::move(out.out_msg);
      transaction = std::move(out.transaction);
    } else if (tag == block::gen::OutMsg::msg_export_new_defer) {
      block::gen::OutMsg::Record_msg_export_new_defer out;
      auto unpack_slice = td::make_ref<vm::CellSlice>(descriptor_slice->clone());
      if (!tlb::csr_unpack(unpack_slice, out)) {
        return td::Status::Error("cannot unpack canonical deferred outbound descriptor");
      }
      envelope = std::move(out.out_msg);
      transaction = std::move(out.transaction);
    } else {
      return td::Status::Error("canonical transaction refers to an unsupported outbound descriptor tag");
    }
    block::tlb::MsgEnvelope::Record_std envelope_record;
    if (envelope.is_null() || !tlb::unpack_cell(envelope, envelope_record)) {
      return td::Status::Error("cannot unpack canonical outbound message envelope");
    }
    message = std::move(envelope_record.msg);
  }
  if (message.is_null() || transaction.is_null() || as_hash256(message->get_hash().as_bits256()) != message_hash ||
      as_hash256(transaction->get_hash().as_bits256()) != transaction_hash) {
    return td::Status::Error("canonical outbound descriptor binding failed");
  }
  return descriptor_cell;
}

td::Result<std::pair<Ref<vm::Cell>, Ref<vm::Cell>>> extract_state_update_raw_views(const BlockContext& block_context) {
  if (block_context.state_update.is_null()) {
    return td::Status::Error("cannot unpack block Merkle-update views");
  }
  vm::CellSlice update_slice{vm::NoVm(), block_context.state_update};
  if (!update_slice.is_special() || update_slice.prefetch_long(8) != 4 || update_slice.size_ext() != 0x20228) {
    return td::Status::Error("cannot unpack block Merkle-update views");
  }
  auto old_raw = update_slice.prefetch_ref(0);
  auto new_raw = update_slice.prefetch_ref(1);
  return std::make_pair(std::move(old_raw), std::move(new_raw));
}

td::Result<std::pair<Ref<vm::Cell>, Ref<vm::Cell>>> extract_state_update_views(const BlockContext& block_context) {
  TRY_RESULT(raw_views, extract_state_update_raw_views(block_context));
  auto& [old_raw, new_raw] = raw_views;
  auto old_state = vm::MerkleProof::virtualize_raw(old_raw, 0);
  auto new_state = vm::MerkleProof::virtualize_raw(new_raw, 0);
  if (old_state.is_null() || new_state.is_null() || old_state->get_hash() != old_raw->get_hash(0) ||
      new_state->get_hash() != new_raw->get_hash(0)) {
    return td::Status::Error("block Merkle-update view hash mismatch");
  }
  return std::make_pair(std::move(old_state), std::move(new_state));
}

td::Result<Ref<vm::Cell>> load_collated_predecessor_witness(const std::string& path,
                                                            const td::Bits256& predecessor_state_hash) {
  TRY_RESULT(data, td::read_file(path));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(data.as_slice()));
  Ref<vm::Cell> result;
  for (const auto& root : roots) {
    vm::CellSlice slice{vm::NoVm(), root};
    if (slice.special_type() != vm::Cell::SpecialType::MerkleProof) {
      continue;
    }
    auto virtualized = vm::MerkleProof::virtualize(root);
    if (virtualized.is_error()) {
      return virtualized.move_as_error_prefix("invalid Merkle proof in collated data: ");
    }
    auto candidate = virtualized.move_as_ok();
    if (td::Bits256(candidate->get_hash().bits()) != predecessor_state_hash) {
      continue;
    }
    if (result.not_null()) {
      return td::Status::Error("collated data contains duplicate predecessor-state witnesses");
    }
    result = std::move(candidate);
  }
  if (result.is_null()) {
    return td::Status::Error("collated data has no Merkle proof for the target predecessor state");
  }
  return result;
}

td::Result<Ref<vm::Cell>> build_predecessor_accounts_proof(const std::string& archive, const HistoryBlocks& history,
                                                           const BlockContext& target,
                                                           const std::vector<LoadedAccountProof>& proofs) {
  if (proofs.empty()) {
    return td::Status::Error("cannot build ShardAccounts proof without account proofs");
  }
  const auto base_block = proofs.front().shard_block;
  std::vector<Ref<vm::Cell>> state_proofs;
  state_proofs.reserve(proofs.size());
  for (const auto& proof : proofs) {
    if (proof.shard_block != base_block || proof.state_proof.is_null()) {
      return td::Status::Error("account proofs do not share one shard-state root");
    }
    state_proofs.push_back(proof.state_proof);
  }
  TRY_RESULT(combined, ton::validator::parallel_inbound::merge_merkle_proofs_fast(state_proofs));

  vm::CellSlice combined_proof{vm::NoVm(), combined};
  if (combined_proof.special_type() != vm::Cell::SpecialType::MerkleProof || combined_proof.size_refs() != 1) {
    return td::Status::Error("combined account proof is not a Merkle proof");
  }
  auto base_state_raw = combined_proof.prefetch_ref();
  TRY_RESULT(base_accounts_raw, extract_partial_shard_accounts_root(base_state_raw));
  Ref<vm::Cell> accounts_root = vm::MerkleProof::virtualize_raw(std::move(base_accounts_raw), 0);
  if (accounts_root.is_null()) {
    return td::Status::Error("cannot virtualize combined ShardAccounts proof");
  }

  BlockIdExt current = base_block;
  for (ton::BlockSeqno seqno = base_block.seqno() + 1; seqno <= target.prev[0].seqno(); ++seqno) {
    TRY_RESULT(intermediate, load_intermediate_block(archive, history, target, seqno));
    if (intermediate.prev.size() != 1 || intermediate.prev[0] != current) {
      return td::Status::Error(PSLICE() << "non-linear account-proof history at " << intermediate.id.to_str());
    }
    TRY_RESULT(raw_views, extract_state_update_raw_views(intermediate));
    TRY_RESULT(old_accounts, extract_partial_shard_accounts_root(raw_views.first));
    TRY_RESULT(new_accounts, extract_partial_shard_accounts_root(raw_views.second));
    if (accounts_root->get_hash(0) != old_accounts->get_hash(0)) {
      return td::Status::Error("combined account proof does not match an intermediate ShardAccounts root");
    }
    TRY_RESULT(next_accounts, vm::MerkleUpdate::apply_raw(accounts_root, old_accounts, new_accounts, 0, 0));
    if (next_accounts->get_hash(0) != new_accounts->get_hash(0)) {
      return td::Status::Error("advanced ShardAccounts proof hash mismatch");
    }
    accounts_root = std::move(next_accounts);
    current = intermediate.id;
  }
  if (current != target.prev[0]) {
    return td::Status::Error("account-proof history does not reach target predecessor");
  }
  return accounts_root;
}

td::Result<Ref<vm::Cell>> build_target_update_predecessor_witness(const std::string& archive,
                                                                  const HistoryBlocks& history,
                                                                  const BlockContext& target,
                                                                  const std::vector<LoadedAccountProof>& account_proofs,
                                                                  const LoadedState* base_state,
                                                                  const Ref<vm::Cell>& out_msg_queue_state_proof) {
  if (account_proofs.empty()) {
    return td::Status::Error("target-update witness requires predecessor-bound account proofs");
  }
  TRY_RESULT(update_views, extract_state_update_views(target));
  const auto expected_root = update_views.first->get_hash();

  if (out_msg_queue_state_proof.is_null()) {
    return td::Status::Error("target-update witness requires an exact predecessor OutMsgQueueInfo proof");
  }
  if (out_msg_queue_state_proof->get_hash() != expected_root) {
    return td::Status::Error("OutMsgQueueInfo proof does not match the target predecessor state root");
  }
  if (base_state != nullptr) {
    if (base_state->id.shard_full() != target.id.shard_full() || base_state->id.seqno > target.prev[0].seqno()) {
      return td::Status::Error("target-update witness base state is outside the target predecessor history");
    }
    TRY_RESULT(base_block, load_intermediate_block(archive, history, target, base_state->id.seqno));
    if (base_block.id.id != base_state->id) {
      return td::Status::Error("target-update witness base state does not match its producing block");
    }
    TRY_RESULT(base_state_hash, get_new_state_hash(base_block));
    Ref<vm::Cell> current_root = base_state->root;
    if (current_root.is_null() || td::Bits256(current_root->get_hash().bits()) != base_state_hash) {
      return td::Status::Error("target-update witness base state root does not match its producing block");
    }

    BlockIdExt current_id = base_block.id;
    for (ton::BlockSeqno seqno = base_state->id.seqno + 1; seqno <= target.prev[0].seqno(); ++seqno) {
      TRY_RESULT(intermediate, load_intermediate_block(archive, history, target, seqno));
      if (intermediate.prev.size() != 1 || intermediate.prev[0] != current_id) {
        return td::Status::Error(PSLICE()
                                 << "non-linear target-update witness history at " << intermediate.id.to_str());
      }
      TRY_RESULT(raw_views, extract_state_update_raw_views(intermediate));
      if (current_root->get_hash() != raw_views.first->get_hash(0)) {
        return td::Status::Error("target-update witness history does not match the current sparse state root");
      }
      TRY_RESULT(next_root, vm::MerkleUpdate::apply(current_root, intermediate.state_update));
      if (next_root->get_hash() != raw_views.second->get_hash(0)) {
        return td::Status::Error("target-update witness sparse state advance changed the committed root");
      }
      current_root = std::move(next_root);
      current_id = intermediate.id;
    }
    if (current_id != target.prev[0] || current_root->get_hash() != expected_root) {
      return td::Status::Error("target-update witness history does not reach the exact target predecessor");
    }
  }
  for (const auto& account_proof : account_proofs) {
    if (account_proof.shard_block != target.prev[0] || account_proof.state_proof.is_null()) {
      return td::Status::Error("target-update witness requires exact predecessor account proofs");
    }
  }
  std::vector<Ref<vm::Cell>> queue_proofs;
  queue_proofs.reserve(2);
  queue_proofs.push_back(vm::CellBuilder::create_merkle_proof(update_views.first));
  queue_proofs.push_back(vm::CellBuilder::create_merkle_proof(out_msg_queue_state_proof));
  TRY_RESULT(combined_queue_proof, ton::validator::parallel_inbound::merge_merkle_proofs_fast(queue_proofs));
  TRY_RESULT(queue_witness, vm::MerkleProof::virtualize(std::move(combined_queue_proof)));
  if (queue_witness->get_hash() != expected_root) {
    return td::Status::Error("combined OutMsgQueueInfo witness changed the committed predecessor root");
  }
  return queue_witness;
}

td::Status collect_library_refs(Ref<vm::Cell> cell, std::set<vm::Cell::Hash>& visited, std::set<td::Bits256>& libraries,
                                int depth) {
  if (cell.is_null()) {
    return td::Status::OK();
  }
  if (depth <= 0 || visited.size() >= 4096 || libraries.size() >= 256) {
    return td::Status::Error("library-reference scan exceeded its safety bound");
  }
  if (!visited.insert(cell->get_hash()).second) {
    return td::Status::OK();
  }
  TRY_RESULT(loaded, cell->load_cell());
  if (loaded.data_cell->is_special()) {
    if (loaded.data_cell->special_type() == vm::DataCell::SpecialType::Library) {
      vm::CellSlice slice(std::move(loaded));
      if (slice.size() != vm::Cell::hash_bits + 8) {
        return td::Status::Error("invalid library-reference cell");
      }
      libraries.emplace(slice.data_bits() + 8);
    }
    return td::Status::OK();
  }
  for (unsigned i = 0; i < loaded.data_cell->get_refs_cnt(); ++i) {
    TRY_STATUS(collect_library_refs(loaded.data_cell->get_ref(i), visited, libraries, depth - 1));
  }
  return td::Status::OK();
}

td::Result<std::set<td::Bits256>> required_libraries(const block::Account& account, Ref<vm::Cell> transaction) {
  block::gen::Transaction::Record transaction_record;
  if (!tlb::unpack_cell(transaction, transaction_record)) {
    return td::Status::Error("cannot unpack transaction while scanning library references");
  }

  std::set<vm::Cell::Hash> visited;
  std::set<td::Bits256> result;
  TRY_STATUS(collect_library_refs(account.code, visited, result));
  TRY_STATUS(collect_library_refs(transaction_record.r1.in_msg->prefetch_ref(), visited, result));
  return result;
}

std::string join_library_hashes(const std::set<td::Bits256>& libraries) {
  td::StringBuilder out;
  bool first = true;
  for (const auto& hash : libraries) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << hash.to_hex();
  }
  return out.as_cslice().str();
}

td::Result<ReplayResult> replay_transactions(
    const std::string& archive, const HistoryBlocks& history, const BlockContext& target,
    const Ref<vm::Cell>& predecessor_state_witness, const LoadedState* prev_state, const LoadedState& mc_state,
    const std::vector<LoadedAccountPart>& account_parts, const std::vector<LoadedAccountProof>& account_proofs,
    const LoadedLibraryBodies* library_bodies, bool profile_ed25519, bool validate_augmented_roots,
    ReplayExecutionMode execution_mode = ReplayExecutionMode::full,
    ReplayAccountArtifacts* collected_artifacts = nullptr, const PreparedAccountReplay* prepared = nullptr,
    td::Slice predecessor_state_witness_source = {}) {
  td::Timer replay_timer;
  ReplayResult result;
  if ((execution_mode == ReplayExecutionMode::prepared_effects) != (prepared != nullptr)) {
    return td::Status::Error("prepared replay mode requires exactly one prepared account replay");
  }
  if (predecessor_state_witness.not_null() == predecessor_state_witness_source.empty()) {
    return td::Status::Error("predecessor-state witness and provenance must be supplied together");
  }
  TRY_STATUS(verify_replay_scope(target));
  if ((prev_state != nullptr && prev_state->record.global_id != target.global_id) ||
      mc_state.record.global_id != target.global_id) {
    return td::Status::Error("block, predecessor state, and masterchain state have different global ids");
  }
  if (prev_state == nullptr && account_proofs.empty()) {
    return td::Status::Error("replay requires a predecessor state or at least one account proof");
  }
  if (prev_state != nullptr && !account_proofs.empty()) {
    return td::Status::Error("predecessor-state and account-proof replay modes cannot be mixed");
  }
  if (prev_state == nullptr && !account_parts.empty()) {
    return td::Status::Error("account parts require a split predecessor-state header");
  }
  if (prev_state != nullptr && prev_state->split_header && account_parts.empty()) {
    return td::Status::Error("split predecessor state requires at least one --account-part");
  }
  if (prev_state != nullptr && !prev_state->split_header && !account_parts.empty()) {
    return td::Status::Error("--account-part is only valid with a split-state predecessor header");
  }

  constexpr int config_mode = block::ConfigInfo::needLibraries | block::ConfigInfo::needCapabilities |
                              block::ConfigInfo::needPrevBlocks | block::ConfigInfo::needWorkchainInfo |
                              block::ConfigInfo::needSpecialSmc;
  TRY_RESULT(config_unique, block::ConfigInfo::extract_config(mc_state.root, target.mc_id, config_mode));
  auto config = std::shared_ptr<block::ConfigInfo>(std::move(config_unique));
  if (config->get_global_blockchain_id() != target.global_id) {
    return td::Status::Error("masterchain configuration global id does not match the target block");
  }
  const auto consensus_config = config->get_new_consensus_config(target.id.id.workchain);
  result.consensus_max_block_bytes = consensus_config.max_block_size;
  result.consensus_max_collated_bytes = consensus_config.max_collated_data_size;
  result.consensus_protocol_version = consensus_config.protocol_version;
  result.consensus_slots_per_leader_window = consensus_config.slots_per_leader_window;
  result.consensus_target_rate_ms = static_cast<td::uint64>(consensus_config.noncritical_params.target_rate.count());
  result.consensus_min_block_interval_ms =
      static_cast<td::uint64>(consensus_config.noncritical_params.min_block_interval.count());
  TRY_RESULT(configured_block_limits, config->get_block_limits(target.id.id.workchain == ton::masterchainId));
  const auto capture_limits = [](const block::ParamLimits& limits) {
    return ReplayResult::LimitTriplet{limits.underload(), limits.soft(), limits.hard()};
  };
  result.block_limit_bytes = capture_limits(configured_block_limits->bytes);
  result.block_limit_gas = capture_limits(configured_block_limits->gas);
  result.block_limit_lt_delta = capture_limits(configured_block_limits->lt_delta);
  result.block_limit_collated_bytes = capture_limits(configured_block_limits->collated_data);
  TRY_RESULT(prev_blocks_info, config->get_prev_blocks_info());

  block::tlb::Aug_InMsgDescr in_msg_augmentation{config->get_global_version()};
  block::tlb::Aug_OutMsgDescr out_msg_augmentation{config->get_global_version()};
  vm::AugmentedDictionary in_msg_descr{vm::load_cell_slice_ref(target.in_msg_descr), 256, in_msg_augmentation};
  vm::AugmentedDictionary out_msg_descr{vm::load_cell_slice_ref(target.out_msg_descr), 256, out_msg_augmentation};
  if (!in_msg_descr.is_valid() || !out_msg_descr.is_valid()) {
    return td::Status::Error("target block has invalid message descriptor dictionaries");
  }

  emulator::TransactionEmulator emulator(config);
  emulator.set_profile_ed25519(profile_ed25519);
  emulator.set_block_lt(target.start_lt);
  auto rand_seed = target.rand_seed;
  emulator.set_rand_seed(rand_seed);
  emulator.set_prev_blocks_info(std::move(prev_blocks_info));
  emulator.set_libs(
      vm::Dictionary(library_bodies == nullptr ? config->get_libraries_root() : library_bodies->root, 256));

  std::unique_ptr<vm::AugmentedDictionary> accounts;
  if (prev_state != nullptr && !prev_state->split_header) {
    accounts = std::make_unique<vm::AugmentedDictionary>(
        vm::load_cell_slice(prev_state->record.accounts).prefetch_ref(), 256, block::tlb::aug_ShardAccounts);
  }
  vm::AugmentedDictionary account_blocks{vm::load_cell_slice_ref(target.account_blocks), 256,
                                         block::tlb::aug_ShardAccountBlocks};

  result.phase_setup_seconds = replay_timer.elapsed();
  double phase_checkpoint = result.phase_setup_seconds;
  ReplayAccountArtifacts artifacts;
  if (prepared != nullptr) {
    const auto setup = result;
    result = prepared->summary;
    result.consensus_max_block_bytes = setup.consensus_max_block_bytes;
    result.consensus_max_collated_bytes = setup.consensus_max_collated_bytes;
    result.consensus_protocol_version = setup.consensus_protocol_version;
    result.consensus_slots_per_leader_window = setup.consensus_slots_per_leader_window;
    result.consensus_target_rate_ms = setup.consensus_target_rate_ms;
    result.consensus_min_block_interval_ms = setup.consensus_min_block_interval_ms;
    result.block_limit_bytes = setup.block_limit_bytes;
    result.block_limit_gas = setup.block_limit_gas;
    result.block_limit_lt_delta = setup.block_limit_lt_delta;
    result.block_limit_collated_bytes = setup.block_limit_collated_bytes;
    result.phase_setup_seconds = phase_checkpoint;
    artifacts = prepared->artifacts;
  }
  auto& canonical_limit_effects = artifacts.limit_effects;
  auto& canonical_limit_contexts = artifacts.limit_contexts;
  auto& shadow_coordinator_candidates = artifacts.coordinator_candidates;
  auto& account_dictionary_deltas = artifacts.account_dictionary_deltas;
  auto& inbound_message_transactions = artifacts.inbound_message_transactions;
  auto& outbound_message_transactions = artifacts.outbound_message_transactions;
  td::Status replay_status = td::Status::OK();
  std::set<td::Bits256> missing_libraries;
  bool accounts_ok = prepared != nullptr || account_blocks.check_for_each_extra([&](Ref<vm::CellSlice>
                                                                                        account_block_slice,
                                                                                    Ref<vm::CellSlice>,
                                                                                    td::ConstBitPtr key, int key_len) {
    if (key_len != 256) {
      replay_status = td::Status::Error("invalid account block key length");
      return false;
    }
    ++result.target_accounts;
    StdSmcAddress address = key;

    const LoadedAccountPart* matching_part = nullptr;
    const LoadedAccountProof* matching_proof = nullptr;
    if (prev_state == nullptr) {
      for (const auto& proof : account_proofs) {
        if (proof.address == address) {
          matching_proof = &proof;
          break;
        }
      }
      if (matching_proof == nullptr) {
        ++result.skipped_accounts;
        return true;
      }
    } else if (prev_state->split_header) {
      for (const auto& part : account_parts) {
        if (key.equals(part.prefix.bits(), part.prefix_len) &&
            (matching_part == nullptr || part.prefix_len > matching_part->prefix_len)) {
          matching_part = &part;
        }
      }
      if (matching_part == nullptr) {
        ++result.skipped_accounts;
        return true;
      }
    }
    ++result.accounts;

    block::gen::AccountBlock::Record account_block;
    if (!tlb::csr_unpack(std::move(account_block_slice), account_block) || account_block.account_addr != address) {
      replay_status = td::Status::Error(PSTRING() << "cannot unpack AccountBlock for " << address.to_hex());
      return false;
    }

    block::Account account(target.id.id.workchain, address.bits());
    Ref<vm::CellSlice> old_account;
    if (matching_proof != nullptr && matching_proof->shard_account.not_null()) {
      old_account = vm::load_cell_slice_ref(matching_proof->shard_account);
    } else if (prev_state != nullptr && prev_state->split_header) {
      old_account = matching_part->accounts->lookup_extra(key, 256).first;
    } else if (prev_state != nullptr) {
      old_account = accounts->lookup_extra(key, 256).first;
    }
    const bool account_existed_before = old_account.not_null();
    if (old_account.is_null()) {
      if (!account.init_new(target.gen_utime)) {
        replay_status = td::Status::Error(PSTRING() << "cannot initialize missing account " << address.to_hex());
        return false;
      }
    } else if (!account.unpack(std::move(old_account), target.gen_utime, false)) {
      replay_status = td::Status::Error(PSTRING() << "cannot unpack predecessor account " << address.to_hex());
      return false;
    }
    if (!account.belongs_to_shard(target.id.shard_full())) {
      replay_status = td::Status::Error(PSTRING() << "account " << address.to_hex() << " is outside the target shard");
      return false;
    }

    vm::AugmentedDictionary transactions{vm::DictNonEmpty(), std::move(account_block.transactions), 64,
                                         block::tlb::aug_AccountTransactions};
    block::CurrencyCollection declared_account_fees;
    if (!declared_account_fees.validate_unpack(transactions.get_root_extra())) {
      replay_status =
          td::Status::Error(PSTRING() << "cannot unpack transaction fee augmentation for " << address.to_hex());
      return false;
    }
    block::CurrencyCollection derived_account_fees{0};
    bool first_account_transaction = true;
    bool account_missing_libraries = false;
    bool transactions_ok = transactions.check_for_each_extra([&](Ref<vm::CellSlice> transaction_slice,
                                                                 Ref<vm::CellSlice>, td::ConstBitPtr tx_key,
                                                                 int tx_key_len) {
      if (tx_key_len != 64) {
        replay_status = td::Status::Error("invalid transaction key length");
        return false;
      }
      auto transaction = transaction_slice->prefetch_ref();
      if (transaction.is_null()) {
        replay_status = td::Status::Error("transaction dictionary contains a null transaction");
        return false;
      }
      TransactionKindCounts transaction_kind;
      auto classified_kind = classify_and_count_transaction(transaction, transaction_kind);
      if (classified_kind.is_error()) {
        replay_status = classified_kind.move_as_error_prefix(PSTRING() << "transaction " << tx_key.get_uint(64)
                                                                       << " of " << address.to_hex() << ": ");
        return false;
      }
      const auto execution_kind = classified_kind.move_as_ok();
      auto required = required_libraries(account, transaction);
      if (required.is_error()) {
        replay_status = required.move_as_error_prefix(PSTRING() << "transaction " << tx_key.get_uint(64) << " of "
                                                                << address.to_hex() << ": ");
        return false;
      }
      if (mc_state.boc == nullptr) {
        std::set<td::Bits256> missing;
        for (const auto& hash : required.ok()) {
          if (library_bodies == nullptr || library_bodies->hashes.count(hash) == 0) {
            missing.insert(hash);
            missing_libraries.insert(hash);
          }
        }
        if (!missing.empty()) {
          account_missing_libraries = true;
          return false;
        }
      }
      const auto pre_account_state = account.total_state;
      const auto pre_account_state_hash = pre_account_state->get_hash().as_bits256();
      auto emulation = emulator.emulate_transaction(std::move(account), transaction);
      if (emulation.is_error()) {
        replay_status = emulation.move_as_error_prefix(PSTRING() << "transaction " << tx_key.get_uint(64) << " of "
                                                                 << address.to_hex() << ": ");
        return false;
      }
      auto emulated = emulation.move_as_ok();
      CanonicalTransactionPayload canonical_payload{.transaction_root = emulated.transaction,
                                                    .post_account_state = emulated.account.total_state,
                                                    .proof_journals = {}};
      auto payload_result = inspect_transaction_payload(canonical_payload);
      if (!payload_result) {
        replay_status = td::Status::Error(
            PSTRING() << "canonical PSAE payload validation failed for transaction " << tx_key.get_uint(64) << " of "
                      << address.to_hex() << ": " << ton::validator::parallel_inbound::to_string(payload_result.error));
        return false;
      }
      const auto& payload_effects = payload_result.effects.value();
      if (payload_effects.account != as_hash256(address) ||
          payload_effects.pre_account_state_hash != as_hash256(pre_account_state_hash) ||
          payload_effects.transaction_hash != as_hash256(transaction->get_hash().as_bits256()) ||
          payload_effects.post_account_state_hash !=
              as_hash256(emulated.account.total_state->get_hash().as_bits256()) ||
          payload_effects.gas_used != emulated.vm.billed_gas_used) {
        replay_status =
            td::Status::Error(PSTRING() << "canonical PSAE payload fields disagree with replay for transaction "
                                        << tx_key.get_uint(64) << " of " << address.to_hex());
        return false;
      }

      td::optional<block::MsgMetadata> inbound_metadata;
      std::optional<ton::validator::parallel_inbound::MessageKey> coordinator_message_key;
      std::optional<InboundDescriptorContext> coordinator_inbound_descriptor;
      std::optional<OutboundQueueKey> coordinator_queue_deletion;
      if (payload_effects.inbound_message.not_null()) {
        auto message_key = payload_effects.inbound_message->get_hash().bits();
        auto declared_in_slice = in_msg_descr.lookup(message_key, 256);
        if (declared_in_slice.is_null()) {
          replay_status = td::Status::Error(PSTRING() << "canonical inbound message is absent from InMsgDescr for "
                                                      << address.to_hex() << " at " << tx_key.get_uint(64));
          return false;
        }
        const auto in_tag = block::gen::t_InMsg.get_tag(*declared_in_slice);
        const bool has_envelope = in_tag == block::gen::InMsg::msg_import_imm ||
                                  in_tag == block::gen::InMsg::msg_import_fin ||
                                  in_tag == block::gen::InMsg::msg_import_deferred_fin;
        if (has_envelope) {
          auto envelope_cell = declared_in_slice->prefetch_ref();
          block::tlb::MsgEnvelope::Record_std envelope;
          if (envelope_cell.is_null() || !block::tlb::unpack_cell(envelope_cell, envelope) || envelope.msg.is_null() ||
              envelope.msg->get_hash() != payload_effects.inbound_message->get_hash()) {
            replay_status = td::Status::Error(PSTRING() << "canonical inbound envelope mismatch for "
                                                        << address.to_hex() << " at " << tx_key.get_uint(64));
            return false;
          }
          inbound_metadata = envelope.metadata;
        }

        if (in_tag == block::gen::InMsg::msg_import_fin) {
          auto declared_in_cell = vm::CellBuilder().append_cellslice(declared_in_slice->clone()).finalize_novm();
          block::gen::InMsg::Record_msg_import_fin declared_in;
          if (declared_in_cell.is_null() || !block::gen::t_InMsg.cell_unpack(declared_in_cell, declared_in) ||
              declared_in.transaction.is_null() || declared_in.transaction->get_hash() != transaction->get_hash()) {
            replay_status = td::Status::Error(PSTRING() << "invalid msg_import_fin transaction binding for "
                                                        << address.to_hex() << " at " << tx_key.get_uint(64));
            return false;
          }

          bool dequeued_from_current_shard = false;
          td::Ref<vm::Cell> declared_out_cell;
          auto declared_out_slice = out_msg_descr.lookup(message_key, 256);
          if (declared_out_slice.not_null() &&
              block::gen::t_OutMsg.get_tag(*declared_out_slice) == block::gen::OutMsg::msg_export_deq_imm) {
            dequeued_from_current_shard = true;
            declared_out_cell = vm::CellBuilder().append_cellslice(declared_out_slice->clone()).finalize_novm();
          }

          auto descriptors = ton::validator::parallel_inbound::materialize_inbound_internal_descriptors(
              payload_effects,
              {.message_envelope = declared_in.in_msg, .dequeued_from_current_shard = dequeued_from_current_shard});
          if (!descriptors || descriptors.delta->in_msg_descriptor->get_hash() != declared_in_cell->get_hash() ||
              (dequeued_from_current_shard &&
               (declared_out_cell.is_null() || descriptors.delta->out_msg_descriptor.is_null() ||
                descriptors.delta->out_msg_descriptor->get_hash() != declared_out_cell->get_hash())) ||
              (!dequeued_from_current_shard && descriptors.delta->out_msg_descriptor.not_null())) {
            replay_status = td::Status::Error(PSTRING() << "canonical inbound descriptor reconstruction failed for "
                                                        << address.to_hex() << " at " << tx_key.get_uint(64));
            return false;
          }

          block::tlb::MsgEnvelope::Record_std inbound_envelope;
          block::gen::CommonMsgInfo::Record_int_msg_info inbound_info;
          if (!block::tlb::unpack_cell(declared_in.in_msg, inbound_envelope) ||
              !block::tlb::unpack_cell_inexact(inbound_envelope.msg, inbound_info)) {
            replay_status = td::Status::Error(PSTRING() << "cannot derive canonical inbound queue order for "
                                                        << address.to_hex() << " at " << tx_key.get_uint(64));
            return false;
          }
          coordinator_message_key = ton::validator::parallel_inbound::MessageKey{
              inbound_envelope.emitted_lt ? inbound_envelope.emitted_lt.value() : inbound_info.created_lt,
              payload_effects.inbound_message_hash.value()};
          coordinator_inbound_descriptor = InboundDescriptorContext{
              .message_envelope = declared_in.in_msg, .dequeued_from_current_shard = dequeued_from_current_shard};
          if (dequeued_from_current_shard) {
            OutboundQueueKey queue_key;
            if (!block::compute_out_msg_queue_key(declared_in.in_msg, queue_key)) {
              replay_status = td::Status::Error(PSTRING() << "cannot derive outbound queue key for " << address.to_hex()
                                                          << " at " << tx_key.get_uint(64));
              return false;
            }
            coordinator_queue_deletion = queue_key;
          }
          ++result.canonical_inbound_fin_descriptors;
          result.canonical_outbound_deq_imm_descriptors += dequeued_from_current_shard;
        }
      }

      auto outbound_metadata = inbound_metadata;
      if (outbound_metadata) {
        ++outbound_metadata.value().depth;
      }
      auto registrations = ton::validator::parallel_inbound::materialize_outbound_registrations(
          payload_effects,
          {.metadata_enabled = config->has_capability(ton::capMsgMetadata), .metadata = outbound_metadata});
      if (!registrations || registrations.batch->messages.size() != payload_effects.outbound_messages.size()) {
        replay_status = td::Status::Error(PSTRING() << "canonical outbound registration failed for transaction "
                                                    << tx_key.get_uint(64) << " of " << address.to_hex());
        return false;
      }
      if (payload_effects.inbound_message_hash &&
          !inbound_message_transactions
               .emplace(payload_effects.inbound_message_hash.value(), payload_effects.transaction_hash)
               .second) {
        replay_status = td::Status::Error(PSTRING() << "duplicate canonical inbound message for transaction "
                                                    << tx_key.get_uint(64) << " of " << address.to_hex());
        return false;
      }
      for (const auto& outbound : payload_effects.outbound_messages) {
        if (!outbound_message_transactions.emplace(outbound.message_hash, payload_effects.transaction_hash).second) {
          replay_status = td::Status::Error(PSTRING() << "duplicate canonical outbound message for transaction "
                                                      << tx_key.get_uint(64) << " of " << address.to_hex());
          return false;
        }
      }
      result.canonical_outbound_registrations += registrations.batch->messages.size();
      ++result.canonical_payloads_validated;
      result.canonical_payload_out_messages += payload_effects.outbound_messages.size();
      derived_account_fees += payload_effects.total_fees;
      if (!derived_account_fees.is_valid()) {
        replay_status = td::Status::Error(PSTRING() << "canonical fee accumulation failed for " << address.to_hex());
        return false;
      }
      canonical_limit_effects.push_back(payload_effects);
      canonical_limit_contexts.push_back({.account_is_first = first_account_transaction, .charge_gas = true});
      if (coordinator_message_key) {
        CHECK(coordinator_inbound_descriptor);
        shadow_coordinator_candidates.push_back(
            {.work = {.key = coordinator_message_key.value(),
                      .account = std::optional<Hash256>{payload_effects.account}},
             .effects = payload_effects,
             .context = {.limit = {.account_is_first = first_account_transaction, .charge_gas = true},
                         .outbound_registration = {.metadata_enabled = config->has_capability(ton::capMsgMetadata),
                                                   .metadata = outbound_metadata},
                         .inbound_descriptor = coordinator_inbound_descriptor,
                         .outbound_queue_deletion = coordinator_queue_deletion},
             .pre_account_state = pre_account_state});
      }
      first_account_transaction = false;
      ++result.transactions;
      result.transaction_kinds.add(transaction_kind);
      auto& account_work = result.account_work[address];
      ++account_work.transactions;
      account_work.transaction_seconds += emulated.elapsed_time;
      if (emulated.vm.executed) {
        ++result.tvm_transactions;
        account_work.tvm_seconds += emulated.vm.time.real;
        result.hotpaths.record(emulated.vm.code_hash, target.id.id.workchain, address, emulated.vm.time,
                               emulated.vm.vm_gas_used, emulated.vm.billed_gas_used, emulated.vm.vm_steps,
                               emulated.vm.ed25519_verifications, emulated.vm.ed25519_time, execution_kind);
      }
      account = std::move(emulated.account);
      return true;
    });
    if (!transactions_ok && account_missing_libraries) {
      return true;
    }
    if (!transactions_ok && replay_status.is_ok()) {
      replay_status = td::Status::Error(PSTRING() << "invalid transaction dictionary for " << address.to_hex());
    }
    if (transactions_ok && !(derived_account_fees == declared_account_fees)) {
      replay_status = td::Status::Error(PSTRING() << "canonical transaction fees disagree with AccountBlock "
                                                  << "augmentation for " << address.to_hex());
      return false;
    }
    if (transactions_ok) {
      ++result.canonical_fee_augmentations_validated;
      account_dictionary_deltas.push_back({.account = as_hash256(address),
                                           .post_account_state = account.total_state,
                                           .last_transaction_hash = as_hash256(account.last_trans_hash_),
                                           .last_transaction_lt = account.last_trans_lt_,
                                           .existed_before = account_existed_before,
                                           .exists_after = account.status != block::Account::acc_nonexist});
    }
    return transactions_ok;
  });

  if (!accounts_ok && replay_status.is_ok()) {
    replay_status = td::Status::Error("invalid account-block dictionary");
  }
  TRY_STATUS(std::move(replay_status));
  if (!missing_libraries.empty()) {
    return td::Status::Error(PSTRING() << "replay requires public library bodies absent from --library-bodies: "
                                       << join_library_hashes(missing_libraries));
  }
  if (result.transaction_kinds.total() != result.transactions) {
    return td::Status::Error("transaction kind counts do not cover the complete replay scope");
  }
  auto phase_elapsed = replay_timer.elapsed();
  if (prepared == nullptr) {
    result.phase_account_replay_seconds = phase_elapsed - phase_checkpoint;
  }
  phase_checkpoint = phase_elapsed;

  if (collected_artifacts != nullptr) {
    const auto artifact_export_started = replay_timer.elapsed();
    *collected_artifacts = artifacts;
    const auto artifact_export_finished = replay_timer.elapsed();
    result.phase_artifact_export_seconds = artifact_export_finished - artifact_export_started;
    phase_checkpoint = artifact_export_finished;
  }
  if (execution_mode == ReplayExecutionMode::account_effects_only) {
    result.replay_total_seconds = result.phase_setup_seconds + result.phase_account_replay_seconds;
    return result;
  }
  block::BlockLimits shadow_limits = *configured_block_limits;
  auto synthetic_usage_tree = std::make_shared<vm::CellUsageTree>();
  shadow_limits.usage_tree = synthetic_usage_tree.get();
  block::BlockLimitStatus shadow_limit_status{shadow_limits};
  auto limit_result = ton::validator::parallel_inbound::apply_basechain_block_limits_atomic(
      shadow_limit_status, canonical_limit_effects, canonical_limit_contexts);
  if (!limit_result) {
    return td::Status::Error(PSTRING() << "canonical basechain block-limit effect application failed: "
                                       << ton::validator::parallel_inbound::to_string(limit_result.error));
  }
  if (shadow_limit_status.transactions != result.transactions || shadow_limit_status.accounts != result.accounts) {
    return td::Status::Error("canonical basechain block-limit counters disagree with replay scope");
  }
  result.basechain_limit_effects_applied = limit_result.applied_transactions;
  result.basechain_limit_accounts = shadow_limit_status.accounts;
  result.basechain_limit_gas = shadow_limit_status.gas_used;
  result.basechain_limit_max_end_lt = shadow_limit_status.cur_lt;
  phase_elapsed = replay_timer.elapsed();
  result.phase_block_limits_seconds = phase_elapsed - phase_checkpoint;
  phase_checkpoint = phase_elapsed;

  std::sort(shadow_coordinator_candidates.begin(), shadow_coordinator_candidates.end(),
            [](const auto& left, const auto& right) { return left.work.key < right.work.key; });
  block::BlockLimits coordinator_limits = *configured_block_limits;
  auto coordinator_usage_tree = std::make_shared<vm::CellUsageTree>();
  coordinator_limits.usage_tree = coordinator_usage_tree.get();
  ton::validator::parallel_inbound::ShadowCoordinatorState coordinator_state{coordinator_limits};
  std::map<Hash256, Hash256> expected_final_states;
  std::size_t expected_accounts = 0;
  std::size_t expected_queue_deletions = 0;
  std::size_t expected_new_messages = 0;
  std::vector<WorkItem> coordinator_items;
  std::vector<ton::validator::parallel_inbound::CompletionStatus> coordinator_completions;
  std::vector<std::optional<CanonicalTransactionEffects>> coordinator_effects;
  std::vector<CoordinatorCommitContext> coordinator_contexts;
  for (const auto& candidate : shadow_coordinator_candidates) {
    auto [account, inserted] = coordinator_state.accounts.emplace(
        candidate.effects.account, ton::validator::parallel_inbound::ShadowAccountState{
                                       .state_hash = as_hash256(candidate.pre_account_state->get_hash().as_bits256()),
                                       .state = candidate.pre_account_state});
    if (!inserted && account->second.state_hash != candidate.effects.pre_account_state_hash &&
        expected_final_states.at(candidate.effects.account) != candidate.effects.pre_account_state_hash) {
      return td::Status::Error("canonical coordinator candidate account chain is discontinuous");
    }
    expected_final_states[candidate.effects.account] = candidate.effects.post_account_state_hash;
    expected_accounts += candidate.context.limit.account_is_first;
    expected_new_messages += candidate.effects.outbound_messages.size();
    if (candidate.context.outbound_queue_deletion) {
      if (!coordinator_state.outbound_queue_entries.insert(candidate.context.outbound_queue_deletion.value()).second) {
        return td::Status::Error("canonical coordinator candidate contains a duplicate outbound queue deletion");
      }
      ++expected_queue_deletions;
    }
    coordinator_items.push_back(candidate.work);
    coordinator_completions.push_back(ton::validator::parallel_inbound::CompletionStatus::succeeded);
    coordinator_effects.push_back(candidate.effects);
    coordinator_contexts.push_back(candidate.context);
  }
  auto coordinator_result = ton::validator::parallel_inbound::apply_ready_coordinator_prefix_atomic(
      coordinator_state, coordinator_items, coordinator_completions, coordinator_effects, coordinator_contexts);
  if (!coordinator_result) {
    return td::Status::Error(
        PSTRING() << "canonical atomic shadow coordinator failed at item "
                  << (coordinator_result.item_index ? td::to_string(coordinator_result.item_index.value())
                                                    : std::string("none"))
                  << ": " << ton::validator::parallel_inbound::to_string(coordinator_result.error)
                  << ", outbound=" << ton::validator::parallel_inbound::to_string(coordinator_result.outbound_error)
                  << ", descriptor=" << ton::validator::parallel_inbound::to_string(coordinator_result.descriptor_error)
                  << ", limits=" << ton::validator::parallel_inbound::to_string(coordinator_result.limit_error));
  }
  if (coordinator_result.decision.committed_count != shadow_coordinator_candidates.size() ||
      coordinator_state.block_limits.transactions != shadow_coordinator_candidates.size() ||
      coordinator_state.block_limits.accounts != expected_accounts ||
      coordinator_state.in_msg_descriptors.size() != shadow_coordinator_candidates.size() ||
      coordinator_state.out_msg_descriptors.size() != expected_queue_deletions ||
      !coordinator_state.outbound_queue_entries.empty() ||
      coordinator_state.new_messages.size() != expected_new_messages) {
    return td::Status::Error("canonical atomic shadow coordinator counters disagree with replay scope");
  }
  if (!coordinator_items.empty() &&
      coordinator_state.processed_upto.last_processed !=
          std::optional<ton::validator::parallel_inbound::MessageKey>{coordinator_items.back().key}) {
    return td::Status::Error("canonical atomic shadow coordinator frontier disagrees with replay scope");
  }
  for (const auto& [address, expected_state] : expected_final_states) {
    auto actual = coordinator_state.accounts.find(address);
    if (actual == coordinator_state.accounts.end() || actual->second.state_hash != expected_state) {
      return td::Status::Error("canonical atomic shadow coordinator account state disagrees with replay scope");
    }
  }
  result.shadow_coordinator_commits = coordinator_result.decision.committed_count;
  result.shadow_coordinator_accounts = coordinator_state.block_limits.accounts;
  result.shadow_coordinator_in_descriptors = coordinator_state.in_msg_descriptors.size();
  result.shadow_coordinator_out_descriptors = coordinator_state.out_msg_descriptors.size();
  result.shadow_coordinator_queue_deletions = expected_queue_deletions;
  result.shadow_coordinator_new_messages = coordinator_state.new_messages.size();
  phase_elapsed = replay_timer.elapsed();
  result.phase_coordinator_seconds = phase_elapsed - phase_checkpoint;
  phase_checkpoint = phase_elapsed;

  if (validate_augmented_roots && result.skipped_accounts == 0 && !account_proofs.empty()) {
    std::string root_stage = "extract_update_views";
    td::Timer augmented_root_timer;
    double augmented_root_checkpoint = 0.0;
    const auto record_root_phase = [&](double& destination) {
      const auto now = augmented_root_timer.elapsed();
      destination += now - augmented_root_checkpoint;
      augmented_root_checkpoint = now;
    };
    try {
      TRY_RESULT(update_views, extract_state_update_views(target));
      TRY_RESULT(raw_update_views, extract_state_update_raw_views(target));
      TRY_RESULT(old_state_references, extract_partial_state_references(raw_update_views.first));
      TRY_RESULT(new_state_references, extract_partial_state_references(raw_update_views.second));
      bool queue_root_commitment_unchanged =
          old_state_references.out_msg_queue_info->get_hash(0) == new_state_references.out_msg_queue_info->get_hash(0);
      Ref<vm::Cell> old_raw_out_msg_queue_wrapper;
      Ref<vm::Cell> new_raw_out_msg_queue_wrapper;
      if (!queue_root_commitment_unchanged) {
        TRY_RESULT_ASSIGN(old_raw_out_msg_queue_wrapper,
                          extract_out_msg_queue_wrapper(old_state_references.out_msg_queue_info));
        TRY_RESULT_ASSIGN(new_raw_out_msg_queue_wrapper,
                          extract_out_msg_queue_wrapper(new_state_references.out_msg_queue_info));
        queue_root_commitment_unchanged =
            old_raw_out_msg_queue_wrapper->get_hash() == new_raw_out_msg_queue_wrapper->get_hash();
      }
      result.predecessor_state_witness_loaded = predecessor_state_witness.not_null();
      result.collated_predecessor_witness_loaded = predecessor_state_witness_source == "collated_data";
      PartialStateDictionaries old_state_dictionaries;
      PartialStateDictionaries new_state_dictionaries;
      result.predecessor_state_witness_source =
          predecessor_state_witness.not_null() ? predecessor_state_witness_source.str() : "not_available";
      if (predecessor_state_witness.not_null()) {
        if (predecessor_state_witness_source == "target_block_state_update_hindsight" &&
            queue_root_commitment_unchanged) {
          vm::AugmentedDictionary unchanged_queue_placeholder{352, block::tlb::aug_OutMsgQueue};
          old_state_dictionaries.out_msg_queue_root = unchanged_queue_placeholder.get_wrapped_dict_root();
          new_state_dictionaries.out_msg_queue_root = unchanged_queue_placeholder.get_wrapped_dict_root();
          old_state_dictionaries.shard_accounts_root = old_state_references.shard_accounts;
          new_state_dictionaries.shard_accounts_root = new_state_references.shard_accounts;
          result.out_msg_queue_unchanged_root_commitment = true;
        } else if (predecessor_state_witness_source == "target_block_state_update_hindsight") {
          if (old_raw_out_msg_queue_wrapper.is_null() || new_raw_out_msg_queue_wrapper.is_null()) {
            return td::Status::Error("target MerkleUpdate has no structural OutMsgQueue transition");
          }
          old_state_dictionaries.out_msg_queue_root = old_raw_out_msg_queue_wrapper;
          new_state_dictionaries.out_msg_queue_root = new_raw_out_msg_queue_wrapper;
          old_state_dictionaries.shard_accounts_root = old_state_references.shard_accounts;
          new_state_dictionaries.shard_accounts_root = new_state_references.shard_accounts;
        } else {
          root_stage = "extract_state_dictionaries";
          TRY_RESULT_ASSIGN(old_state_dictionaries, extract_partial_state_dictionaries(predecessor_state_witness));
          root_stage = "apply_target_state_update";
          TRY_RESULT(next_state_witness, vm::MerkleUpdate::apply(predecessor_state_witness, target.state_update));
          if (next_state_witness->get_hash() != update_views.second->get_hash()) {
            return td::Status::Error("target state update changed the committed result root");
          }
          root_stage = "extract_new_state_dictionaries";
          TRY_RESULT_ASSIGN(new_state_dictionaries, extract_partial_state_dictionaries(next_state_witness));
        }
      }
      root_stage = "extract_predecessor_accounts_root";
      TRY_RESULT(target_predecessor_accounts_root, extract_partial_shard_accounts_root(update_views.first));
      record_root_phase(result.phase_root_inputs_seconds);
      root_stage = "build_predecessor_accounts_proof";
      TRY_RESULT(predecessor_accounts_root, build_predecessor_accounts_proof(archive, history, target, account_proofs));
      if (predecessor_accounts_root->get_hash() != target_predecessor_accounts_root->get_hash()) {
        return td::Status::Error("combined account proof disagrees with target Merkle-update predecessor root");
      }
      result.shard_accounts_predecessor_root_bound = true;
      record_root_phase(result.phase_root_predecessor_proof_seconds);

      root_stage = "strip_target_descriptors";
      block::tlb::Aug_InMsgDescr in_augmentation{config->get_global_version()};
      block::tlb::Aug_OutMsgDescr out_augmentation{config->get_global_version()};
      auto in_baseline = std::make_unique<vm::AugmentedDictionary>(256, in_augmentation);
      auto out_baseline = std::make_unique<vm::AugmentedDictionary>(256, out_augmentation);
      std::map<Hash256, Ref<vm::Cell>> root_in_descriptors = coordinator_state.in_msg_descriptors;
      std::map<Hash256, Ref<vm::Cell>> root_out_descriptors = coordinator_state.out_msg_descriptors;
      if (predecessor_state_witness.not_null()) {
        root_stage = "bind_complete_target_descriptors";
        vm::AugmentedDictionary target_in_descriptors{vm::load_cell_slice_ref(target.in_msg_descr), 256,
                                                      in_augmentation};
        vm::AugmentedDictionary target_out_descriptors{vm::load_cell_slice_ref(target.out_msg_descr), 256,
                                                       out_augmentation};
        std::size_t target_in_count = 0;
        std::size_t target_out_count = 0;
        if (!target_in_descriptors.check_for_each([&](Ref<vm::CellSlice>, td::ConstBitPtr, int key_len) {
              ++target_in_count;
              return key_len == 256;
            }) ||
            !target_out_descriptors.check_for_each([&](Ref<vm::CellSlice>, td::ConstBitPtr, int key_len) {
              ++target_out_count;
              return key_len == 256;
            })) {
          return td::Status::Error("cannot enumerate target message descriptors");
        }
        if (target_in_count != inbound_message_transactions.size() ||
            target_out_count != outbound_message_transactions.size()) {
          return td::Status::Error("canonical transaction messages do not cover the target descriptor roots");
        }
        root_in_descriptors.clear();
        for (const auto& [message_hash, transaction_hash] : inbound_message_transactions) {
          auto descriptor = target_in_descriptors.lookup(as_dictionary_key(message_hash));
          if (descriptor.is_null()) {
            return td::Status::Error("canonical inbound message is absent from target InMsgDescr");
          }
          TRY_RESULT(descriptor_cell,
                     validate_inbound_descriptor(std::move(descriptor), message_hash, transaction_hash));
          root_in_descriptors.emplace(message_hash, std::move(descriptor_cell));
          ++result.in_msg_descriptors_bound;
        }
        root_out_descriptors.clear();
        for (const auto& [message_hash, transaction_hash] : outbound_message_transactions) {
          auto descriptor = target_out_descriptors.lookup(as_dictionary_key(message_hash));
          if (descriptor.is_null()) {
            return td::Status::Error("canonical outbound message is absent from target OutMsgDescr");
          }
          TRY_RESULT(descriptor_cell,
                     validate_generated_outbound_descriptor(std::move(descriptor), message_hash, transaction_hash));
          root_out_descriptors.emplace(message_hash, std::move(descriptor_cell));
          ++result.out_msg_descriptors_bound;
        }
        result.complete_message_descriptor_coverage = true;
      } else {
        in_baseline = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(target.in_msg_descr), 256,
                                                                in_augmentation);
        out_baseline = std::make_unique<vm::AugmentedDictionary>(vm::load_cell_slice_ref(target.out_msg_descr), 256,
                                                                 out_augmentation);
        for (const auto& [hash, descriptor] : coordinator_state.in_msg_descriptors) {
          auto removed = in_baseline->lookup_delete(as_dictionary_key(hash));
          if (removed.is_null()) {
            return td::Status::Error("canonical InMsg descriptor is absent from target root");
          }
          TRY_RESULT(removed_cell, serialize_slice(std::move(removed)));
          if (removed_cell->get_hash() != descriptor->get_hash()) {
            return td::Status::Error("canonical InMsg descriptor value disagrees with target root");
          }
        }
        for (const auto& [hash, descriptor] : coordinator_state.out_msg_descriptors) {
          auto removed = out_baseline->lookup_delete(as_dictionary_key(hash));
          if (removed.is_null()) {
            return td::Status::Error("canonical OutMsg descriptor is absent from target root");
          }
          TRY_RESULT(removed_cell, serialize_slice(std::move(removed)));
          if (removed_cell->get_hash() != descriptor->get_hash()) {
            return td::Status::Error("canonical OutMsg descriptor value disagrees with target root");
          }
        }
      }
      record_root_phase(result.phase_root_descriptor_baseline_seconds);

      root_stage = "bind_account_proofs";
      vm::AugmentedDictionary predecessor_accounts{
          vm::DictNonEmpty(), vm::load_cell_slice_ref(predecessor_accounts_root), 256, block::tlb::aug_ShardAccounts};
      for (const auto& proof : account_proofs) {
        auto value = predecessor_accounts.lookup(proof.address);
        if (proof.shard_account.is_null() != value.is_null()) {
          return td::Status::Error("account proof existence disagrees with target Merkle-update old root");
        }
        ++result.shard_account_proof_values_bound;
        if (proof.shard_account.not_null()) {
          TRY_RESULT(value_cell, serialize_slice(std::move(value)));
          if (value_cell->get_hash() != proof.shard_account->get_hash()) {
            return td::Status::Error("account proof value disagrees with target Merkle-update old root");
          }
        }
      }
      for (const auto& delta : account_dictionary_deltas) {
        const bool root_contains_account = predecessor_accounts.lookup(as_dictionary_key(delta.account)).not_null();
        if (root_contains_account != delta.existed_before) {
          return td::Status::Error("replayed account existence disagrees with predecessor ShardAccounts root");
        }
      }
      record_root_phase(result.phase_root_account_binding_seconds);

      if (predecessor_state_witness.not_null()) {
        root_stage = "bind_new_shard_account_values";
        TRY_RESULT(target_new_accounts_root, extract_partial_shard_accounts_root(update_views.second));
        vm::AugmentedDictionary target_new_accounts{
            vm::DictNonEmpty(), vm::load_cell_slice_ref(target_new_accounts_root), 256, block::tlb::aug_ShardAccounts};
        for (std::size_t i = 0; i < account_dictionary_deltas.size(); ++i) {
          const auto& delta = account_dictionary_deltas[i];
          auto target_value = target_new_accounts.lookup(as_dictionary_key(delta.account));
          if (target_value.is_null() != !delta.exists_after) {
            return td::Status::Error(PSLICE() << "new ShardAccounts existence disagrees at item " << i);
          }
          if (!delta.exists_after) {
            continue;
          }
          if (delta.post_account_state.is_null()) {
            return td::Status::Error(PSLICE() << "ShardAccounts delta has no post-state at item " << i);
          }
          vm::CellBuilder builder;
          if (!(builder.store_ref_bool(delta.post_account_state) &&
                builder.store_bytes_bool(delta.last_transaction_hash.data(), delta.last_transaction_hash.size()) &&
                builder.store_long_bool(delta.last_transaction_lt, 64))) {
            return td::Status::Error(PSLICE() << "cannot serialize ShardAccounts delta at item " << i);
          }
          TRY_RESULT(target_value_cell, serialize_slice(std::move(target_value)));
          if (target_value_cell->get_hash() != builder.finalize_novm()->get_hash()) {
            return td::Status::Error(PSLICE() << "new ShardAccount value disagrees at item " << i);
          }
        }

        root_stage = "scan_shard_accounts_transition";
        StructuralAugmentedDictionary old_accounts_structure{predecessor_accounts_root, 256};
        StructuralAugmentedDictionary new_accounts_structure{target_new_accounts_root, 256};
        std::map<StdSmcAddress, std::pair<bool, bool>> account_diff;
        const bool account_diff_ok = old_accounts_structure.scan_diff(
            new_accounts_structure,
            [&](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_value, Ref<vm::CellSlice> new_value) -> bool {
              if (key_len != 256) {
                return false;
              }
              StdSmcAddress account;
              account.bits().copy_from(key, 256);
              return account_diff.emplace(account, std::make_pair(old_value.not_null(), new_value.not_null())).second;
            },
            0);
        if (!account_diff_ok || account_diff.size() != account_dictionary_deltas.size()) {
          return td::Status::Error("target ShardAccounts diff is incomplete or has unexpected keys");
        }
        for (const auto& delta : account_dictionary_deltas) {
          const auto entry = account_diff.find(delta.account);
          if (entry == account_diff.end() || entry->second.first != delta.existed_before ||
              entry->second.second != delta.exists_after) {
            return td::Status::Error("target ShardAccounts diff disagrees with canonical account deltas");
          }
        }
        result.shard_accounts_transition_validated = true;
        result.shard_accounts_transition_status = "exact_diff_and_value_binding";

        if (result.out_msg_queue_unchanged_root_commitment) {
          for (const auto& context : coordinator_contexts) {
            if (context.outbound_queue_deletion) {
              return td::Status::Error(
                  "canonical queue deletion contradicts the unchanged OutMsgQueue root commitment");
            }
          }
          result.out_msg_queue_transition_status = "exact_unchanged_root_commitment";
        } else {
          root_stage = "scan_out_msg_queue_transition";
          auto old_out_queue_raw = vm::load_cell_slice(old_state_dictionaries.out_msg_queue_root).prefetch_ref();
          auto new_out_queue_raw = vm::load_cell_slice(new_state_dictionaries.out_msg_queue_root).prefetch_ref();
          StructuralAugmentedDictionary old_out_queue_structure{std::move(old_out_queue_raw), 352};
          StructuralAugmentedDictionary new_out_queue_structure{std::move(new_out_queue_raw), 352};
          std::map<OutboundQueueKey, std::pair<Ref<vm::Cell>, Ref<vm::Cell>>> queue_diff;
          td::Status queue_diff_status = td::Status::OK();
          bool queue_diff_ok = false;
          try {
            queue_diff_ok = old_out_queue_structure.scan_diff(
                new_out_queue_structure,
                [&](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_value,
                    Ref<vm::CellSlice> new_value) -> bool {
                  if (key_len != 352) {
                    queue_diff_status = td::Status::Error("OutMsgQueue diff has a non-canonical key length");
                    return false;
                  }
                  OutboundQueueKey queue_key;
                  queue_key.bits().copy_from(key, 352);
                  Ref<vm::Cell> old_cell;
                  Ref<vm::Cell> new_cell;
                  if (old_value.not_null()) {
                    if (!old_value.write().advance(64)) {
                      queue_diff_status = td::Status::Error("old OutMsgQueue leaf has no augmentation");
                      return false;
                    }
                    auto serialized = serialize_slice(std::move(old_value));
                    if (serialized.is_error()) {
                      queue_diff_status = serialized.move_as_error_prefix("cannot serialize old OutMsgQueue value: ");
                      return false;
                    }
                    old_cell = serialized.move_as_ok();
                  }
                  if (new_value.not_null()) {
                    if (!new_value.write().advance(64)) {
                      queue_diff_status = td::Status::Error("new OutMsgQueue leaf has no augmentation");
                      return false;
                    }
                    auto serialized = serialize_slice(std::move(new_value));
                    if (serialized.is_error()) {
                      queue_diff_status = serialized.move_as_error_prefix("cannot serialize new OutMsgQueue value: ");
                      return false;
                    }
                    new_cell = serialized.move_as_ok();
                  }
                  queue_diff.emplace(queue_key, std::make_pair(std::move(old_cell), std::move(new_cell)));
                  return true;
                },
                0);
          } catch (vm::VmVirtError& error) {
            if (std::string(error.get_msg()) != "prunned branch") {
              throw;
            }
            queue_diff_status =
                td::Status::Error("predecessor witness is incomplete for the canonical OutMsgQueue transition");
          }
          if (!queue_diff_ok) {
            if (queue_diff_status.is_error()) {
              return queue_diff_status;
            }
            if (result.out_msg_queue_transition_status == "not_run") {
              return td::Status::Error("cannot scan target OutMsgQueue Merkle diff");
            }
          } else {
            std::map<OutboundQueueKey, Ref<vm::Cell>> expected_deletions;
            for (const auto& context : coordinator_contexts) {
              if (context.outbound_queue_deletion) {
                if (!context.inbound_descriptor ||
                    !expected_deletions
                         .emplace(context.outbound_queue_deletion.value(), context.inbound_descriptor->message_envelope)
                         .second) {
                  return td::Status::Error("invalid canonical OutMsgQueue deletion set");
                }
              }
            }
            for (const auto& [key, values] : queue_diff) {
              if (values.first.is_null()) {
                ++result.out_msg_queue_diff_additions;
                if (values.second.is_null()) {
                  return td::Status::Error("OutMsgQueue addition has no value");
                }
                td::Bits256 message_hash_bits;
                message_hash_bits.bits().copy_from(key.bits() + 96, 256);
                const auto message_hash = as_hash256(message_hash_bits);
                auto canonical_message = outbound_message_transactions.find(message_hash);
                auto canonical_descriptor = root_out_descriptors.find(message_hash);
                if (canonical_message == outbound_message_transactions.end() ||
                    canonical_descriptor == root_out_descriptors.end()) {
                  return td::Status::Error("OutMsgQueue addition is not bound to a canonical outbound message");
                }
                vm::CellSlice enqueued{vm::NoVm(), values.second};
                auto envelope = enqueued.advance(64) ? enqueued.fetch_ref() : Ref<vm::Cell>{};
                block::tlb::MsgEnvelope::Record_std envelope_record;
                if (envelope.is_null() || !enqueued.empty_ext() || !tlb::unpack_cell(envelope, envelope_record) ||
                    envelope_record.msg.is_null() ||
                    as_hash256(envelope_record.msg->get_hash().as_bits256()) != message_hash) {
                  return td::Status::Error("OutMsgQueue addition has an invalid canonical envelope");
                }
                auto descriptor_slice = vm::load_cell_slice_ref(canonical_descriptor->second);
                block::gen::OutMsg::Record_msg_export_new out;
                if (!tlb::csr_unpack(descriptor_slice, out) || out.out_msg.is_null() ||
                    out.out_msg->get_hash() != envelope->get_hash()) {
                  return td::Status::Error("OutMsgQueue addition disagrees with msg_export_new");
                }
                ++result.out_msg_queue_additions_bound;
              } else if (values.second.is_null()) {
                ++result.out_msg_queue_diff_deletions;
                const auto expected = expected_deletions.find(key);
                if (expected == expected_deletions.end() || values.first.is_null()) {
                  return td::Status::Error("OutMsgQueue deletion is not bound to a canonical inbound message");
                }
                vm::CellSlice enqueued{vm::NoVm(), values.first};
                auto envelope = enqueued.advance(64) ? enqueued.fetch_ref() : Ref<vm::Cell>{};
                if (envelope.is_null() || !enqueued.empty_ext() ||
                    envelope->get_hash() != expected->second->get_hash()) {
                  return td::Status::Error("OutMsgQueue deletion disagrees with the canonical inbound envelope");
                }
                ++result.out_msg_queue_deletions_bound;
              } else {
                ++result.out_msg_queue_diff_replacements;
                return td::Status::Error("OutMsgQueue replacement is outside the canonical replay model");
              }
            }
            if (result.out_msg_queue_deletions_bound != expected_deletions.size()) {
              return td::Status::Error("canonical OutMsgQueue deletions do not cover the target diff");
            }
            result.out_msg_queue_transition_status = "exact_diff_and_value_binding";
          }
        }

      } else {
        result.shard_accounts_transition_status = "requires_collated_data_predecessor_witness";
        result.out_msg_queue_transition_status = "requires_collated_data_predecessor_witness";
      }
      record_root_phase(result.phase_root_state_transition_seconds);

      root_stage = "apply_augmented_deltas";
      const bool exact_queue_transition = result.out_msg_queue_transition_status == "exact_unchanged_root_commitment" ||
                                          result.out_msg_queue_transition_status == "exact_diff_and_value_binding";
      const bool complete_state_transition = predecessor_state_witness.not_null() &&
                                             result.shard_accounts_transition_validated && exact_queue_transition &&
                                             result.complete_message_descriptor_coverage;
      vm::AugmentedDictionary accounts_out_of_scope{256, block::tlb::aug_ShardAccounts};
      vm::AugmentedDictionary queue_out_of_scope{352, block::tlb::aug_OutMsgQueue};
      ton::validator::parallel_inbound::AugmentedDictionarySeed seed{
          .global_version = config->get_global_version(),
          .shard_accounts_root = accounts_out_of_scope.get_wrapped_dict_root(),
          .in_msg_descr_root = in_baseline->get_wrapped_dict_root(),
          .out_msg_descr_root = out_baseline->get_wrapped_dict_root(),
          .out_msg_queue_root = queue_out_of_scope.get_wrapped_dict_root()};
      const std::vector<ton::validator::parallel_inbound::AccountDictionaryDelta> account_deltas_out_of_scope;
      auto root_result = ton::validator::parallel_inbound::apply_augmented_dictionary_deltas_atomic(
          seed, account_deltas_out_of_scope, root_in_descriptors, root_out_descriptors,
          std::vector<ton::validator::parallel_inbound::QueueDictionaryDelta>{});
      if (!root_result) {
        return td::Status::Error(
            PSTRING() << "canonical augmented dictionary commit failed at item "
                      << (root_result.item_index ? td::to_string(root_result.item_index.value()) : std::string("none"))
                      << ": " << ton::validator::parallel_inbound::to_string(root_result.error)
                      << (root_result.error_detail.empty() ? std::string() : ": " + root_result.error_detail));
      }
      const auto& roots = root_result.roots.value();
      if (roots.in_msg_descr_root->get_hash() != target.in_msg_descr->get_hash() ||
          roots.out_msg_descr_root->get_hash() != target.out_msg_descr->get_hash()) {
        return td::Status::Error("canonical augmented dictionary roots disagree with copied block artifacts");
      }
      if (complete_state_transition) {
        result.augmented_dictionary_roots_validated = 4;
      } else {
        result.augmented_dictionary_roots_validated = 2;
      }
      record_root_phase(result.phase_root_commit_seconds);
    } catch (vm::VmVirtError& error) {
      return td::Status::Error(PSTRING() << "augmented root gate virtualization error at " << root_stage << ": "
                                         << error.get_msg());
    } catch (vm::VmError& error) {
      return td::Status::Error(PSTRING() << "augmented root gate VM error at " << root_stage << ": "
                                         << error.get_msg());
    }
  }
  if (predecessor_state_witness.not_null() && result.augmented_dictionary_roots_validated != 4) {
    return td::Status::Error(
        "predecessor-state witness requires complete account-proof replay scope and an exact four-root transition");
  }
  phase_elapsed = replay_timer.elapsed();
  result.phase_augmented_roots_seconds = phase_elapsed - phase_checkpoint;
  result.replay_total_seconds = result.phase_setup_seconds + result.phase_account_replay_seconds +
                                result.phase_block_limits_seconds + result.phase_coordinator_seconds +
                                result.phase_augmented_roots_seconds;
  return result;
}

struct AccountReplayBatchMeasurement {
  std::size_t workers{0};
  double wall_seconds{0.0};
  std::vector<std::size_t> lane_accounts;
  std::vector<double> lane_planned_account_seconds;
  std::vector<double> lane_wall_seconds;
};

struct AccountLanePlan {
  std::vector<std::vector<LoadedAccountProof>> proofs;
  std::vector<double> planned_account_seconds;
};

double median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  if (values.size() % 2 != 0) {
    return values[middle];
  }
  return (values[middle - 1] + values[middle]) / 2.0;
}

td::Result<AccountLanePlan> build_account_lane_plan(const std::vector<LoadedAccountProof>& account_proofs,
                                                    const ReplayResult& reference, std::size_t requested_workers) {
  if (requested_workers == 0 || requested_workers > 64) {
    return td::Status::Error("parallel account replay worker count must be between 1 and 64");
  }
  if (account_proofs.empty() || reference.skipped_accounts != 0 || reference.accounts != account_proofs.size() ||
      reference.account_work.size() != account_proofs.size()) {
    return td::Status::Error("parallel account replay requires a complete account-proof block replay");
  }

  const std::size_t workers = std::min(requested_workers, account_proofs.size());
  std::vector<const LoadedAccountProof*> ordered;
  ordered.reserve(account_proofs.size());
  for (const auto& proof : account_proofs) {
    if (reference.account_work.find(proof.address) == reference.account_work.end()) {
      return td::Status::Error(PSTRING() << "parallel account replay has no measured work for "
                                         << proof.address.to_hex());
    }
    ordered.push_back(&proof);
  }
  std::sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) {
    const double left_work = reference.account_work.at(left->address).transaction_seconds;
    const double right_work = reference.account_work.at(right->address).transaction_seconds;
    if (left_work != right_work) {
      return left_work > right_work;
    }
    return left->address < right->address;
  });

  AccountLanePlan plan;
  plan.proofs.resize(workers);
  plan.planned_account_seconds.resize(workers, 0.0);
  for (const auto* proof : ordered) {
    const auto lane = static_cast<std::size_t>(
        std::min_element(plan.planned_account_seconds.begin(), plan.planned_account_seconds.end()) -
        plan.planned_account_seconds.begin());
    plan.proofs[lane].push_back(*proof);
    plan.planned_account_seconds[lane] += reference.account_work.at(proof->address).transaction_seconds;
  }
  return plan;
}

td::Status validate_account_replay_batch(const ReplayResult& reference,
                                         const std::vector<std::unique_ptr<td::Result<ReplayResult>>>& lanes,
                                         bool expect_global_effects = true) {
  auto check_sum = [&](auto member, td::Slice name) -> td::Status {
    using Value = std::decay_t<decltype(reference.*member)>;
    Value total{};
    for (const auto& lane : lanes) {
      total += lane->ok().*member;
    }
    if (total != reference.*member) {
      return td::Status::Error(PSTRING() << "parallel account replay counter mismatch for " << name);
    }
    return td::Status::OK();
  };

  TRY_STATUS(check_sum(&ReplayResult::accounts, "accounts"));
  TRY_STATUS(check_sum(&ReplayResult::transactions, "transactions"));
  TRY_STATUS(check_sum(&ReplayResult::tvm_transactions, "tvm_transactions"));
  TRY_STATUS(check_sum(&ReplayResult::canonical_payloads_validated, "canonical_payloads_validated"));
  TRY_STATUS(check_sum(&ReplayResult::canonical_payload_out_messages, "canonical_payload_out_messages"));
  TRY_STATUS(check_sum(&ReplayResult::canonical_outbound_registrations, "canonical_outbound_registrations"));
  TRY_STATUS(check_sum(&ReplayResult::canonical_inbound_fin_descriptors, "canonical_inbound_fin_descriptors"));
  TRY_STATUS(
      check_sum(&ReplayResult::canonical_outbound_deq_imm_descriptors, "canonical_outbound_deq_imm_descriptors"));
  TRY_STATUS(check_sum(&ReplayResult::canonical_fee_augmentations_validated, "canonical_fee_augmentations_validated"));
  if (expect_global_effects) {
    TRY_STATUS(check_sum(&ReplayResult::basechain_limit_effects_applied, "basechain_limit_effects_applied"));
    TRY_STATUS(check_sum(&ReplayResult::basechain_limit_accounts, "basechain_limit_accounts"));
    TRY_STATUS(check_sum(&ReplayResult::basechain_limit_gas, "basechain_limit_gas"));
    TRY_STATUS(check_sum(&ReplayResult::shadow_coordinator_commits, "shadow_coordinator_commits"));
    TRY_STATUS(check_sum(&ReplayResult::shadow_coordinator_accounts, "shadow_coordinator_accounts"));
    TRY_STATUS(check_sum(&ReplayResult::shadow_coordinator_in_descriptors, "shadow_coordinator_in_descriptors"));
    TRY_STATUS(check_sum(&ReplayResult::shadow_coordinator_out_descriptors, "shadow_coordinator_out_descriptors"));
    TRY_STATUS(check_sum(&ReplayResult::shadow_coordinator_queue_deletions, "shadow_coordinator_queue_deletions"));
    TRY_STATUS(check_sum(&ReplayResult::shadow_coordinator_new_messages, "shadow_coordinator_new_messages"));
  }

  TransactionKindCounts transaction_kinds;
  for (const auto& lane : lanes) {
    transaction_kinds.add(lane->ok().transaction_kinds);
  }
  if (transaction_kinds.ordinary != reference.transaction_kinds.ordinary ||
      transaction_kinds.tick != reference.transaction_kinds.tick ||
      transaction_kinds.tock != reference.transaction_kinds.tock ||
      transaction_kinds.storage != reference.transaction_kinds.storage ||
      transaction_kinds.split_prepare != reference.transaction_kinds.split_prepare ||
      transaction_kinds.split_install != reference.transaction_kinds.split_install ||
      transaction_kinds.merge_prepare != reference.transaction_kinds.merge_prepare ||
      transaction_kinds.merge_install != reference.transaction_kinds.merge_install) {
    return td::Status::Error("parallel account replay transaction-kind mismatch");
  }

  std::map<StdSmcAddress, std::size_t> account_transactions;
  for (const auto& lane : lanes) {
    const auto& replay = lane->ok();
    if (replay.target_accounts != reference.target_accounts || replay.augmented_dictionary_roots_validated != 0 ||
        replay.consensus_max_block_bytes != reference.consensus_max_block_bytes ||
        replay.consensus_max_collated_bytes != reference.consensus_max_collated_bytes ||
        replay.consensus_target_rate_ms != reference.consensus_target_rate_ms ||
        replay.block_limit_bytes != reference.block_limit_bytes ||
        replay.block_limit_gas != reference.block_limit_gas ||
        replay.block_limit_lt_delta != reference.block_limit_lt_delta ||
        replay.block_limit_collated_bytes != reference.block_limit_collated_bytes) {
      return td::Status::Error("parallel account replay lane escaped its isolated-account scope");
    }
    for (const auto& [address, work] : replay.account_work) {
      if (!account_transactions.emplace(address, work.transactions).second) {
        return td::Status::Error(PSTRING() << "parallel account replay duplicated account " << address.to_hex());
      }
    }
  }
  if (account_transactions.size() != reference.account_work.size()) {
    return td::Status::Error("parallel account replay account coverage mismatch");
  }
  for (const auto& [address, work] : reference.account_work) {
    auto actual = account_transactions.find(address);
    if (actual == account_transactions.end() || actual->second != work.transactions) {
      return td::Status::Error(PSTRING() << "parallel account replay transaction-chain mismatch for "
                                         << address.to_hex());
    }
  }
  return td::Status::OK();
}

td::Status normalize_account_artifacts(ReplayAccountArtifacts& artifacts) {
  if (artifacts.limit_effects.size() != artifacts.limit_contexts.size()) {
    return td::Status::Error("account artifact limit effect/context size mismatch");
  }

  std::vector<std::size_t> order(artifacts.limit_effects.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
    const auto& lhs = artifacts.limit_effects[left];
    const auto& rhs = artifacts.limit_effects[right];
    return lhs.transaction_start_lt < rhs.transaction_start_lt ||
           (lhs.transaction_start_lt == rhs.transaction_start_lt && lhs.transaction_hash < rhs.transaction_hash);
  });
  std::vector<CanonicalTransactionEffects> effects;
  std::vector<ton::validator::parallel_inbound::BasechainLimitContext> contexts;
  effects.reserve(order.size());
  contexts.reserve(order.size());
  std::set<Hash256> transaction_hashes;
  for (auto index : order) {
    if (!transaction_hashes.insert(artifacts.limit_effects[index].transaction_hash).second) {
      return td::Status::Error("account artifacts contain a duplicate transaction hash");
    }
    effects.push_back(std::move(artifacts.limit_effects[index]));
    contexts.push_back(artifacts.limit_contexts[index]);
  }
  artifacts.limit_effects = std::move(effects);
  artifacts.limit_contexts = std::move(contexts);

  std::sort(artifacts.coordinator_candidates.begin(), artifacts.coordinator_candidates.end(),
            [](const auto& left, const auto& right) { return left.work.key < right.work.key; });
  for (std::size_t i = 1; i < artifacts.coordinator_candidates.size(); ++i) {
    if (!(artifacts.coordinator_candidates[i - 1].work.key < artifacts.coordinator_candidates[i].work.key)) {
      return td::Status::Error("account artifacts contain a duplicate coordinator key");
    }
  }

  std::sort(artifacts.account_dictionary_deltas.begin(), artifacts.account_dictionary_deltas.end(),
            [](const auto& left, const auto& right) { return left.account < right.account; });
  for (std::size_t i = 1; i < artifacts.account_dictionary_deltas.size(); ++i) {
    if (!(artifacts.account_dictionary_deltas[i - 1].account < artifacts.account_dictionary_deltas[i].account)) {
      return td::Status::Error("account artifacts contain a duplicate account delta");
    }
  }
  return td::Status::OK();
}

bool equal_optional_metadata(const td::optional<block::MsgMetadata>& left,
                             const td::optional<block::MsgMetadata>& right) {
  return static_cast<bool>(left) == static_cast<bool>(right) && (!left || left.value() == right.value());
}

bool equal_coordinator_context(const CoordinatorCommitContext& left, const CoordinatorCommitContext& right) {
  if (left.limit.account_is_first != right.limit.account_is_first || left.limit.charge_gas != right.limit.charge_gas ||
      left.outbound_registration.metadata_enabled != right.outbound_registration.metadata_enabled ||
      !equal_optional_metadata(left.outbound_registration.metadata, right.outbound_registration.metadata) ||
      left.inbound_descriptor.has_value() != right.inbound_descriptor.has_value() ||
      left.outbound_queue_deletion.has_value() != right.outbound_queue_deletion.has_value()) {
    return false;
  }
  if (left.inbound_descriptor &&
      (left.inbound_descriptor->dequeued_from_current_shard != right.inbound_descriptor->dequeued_from_current_shard ||
       left.inbound_descriptor->message_envelope.is_null() || right.inbound_descriptor->message_envelope.is_null() ||
       left.inbound_descriptor->message_envelope->get_hash() !=
           right.inbound_descriptor->message_envelope->get_hash())) {
    return false;
  }
  return !left.outbound_queue_deletion || left.outbound_queue_deletion.value() == right.outbound_queue_deletion.value();
}

td::Status validate_account_artifact_equivalence(const ReplayAccountArtifacts& reference,
                                                 const ReplayAccountArtifacts& actual) {
  if (reference.limit_effects.size() != actual.limit_effects.size() ||
      reference.limit_contexts.size() != actual.limit_contexts.size() ||
      reference.coordinator_candidates.size() != actual.coordinator_candidates.size() ||
      reference.account_dictionary_deltas.size() != actual.account_dictionary_deltas.size() ||
      reference.inbound_message_transactions != actual.inbound_message_transactions ||
      reference.outbound_message_transactions != actual.outbound_message_transactions) {
    return td::Status::Error("offline collator artifact cardinality mismatch");
  }
  for (std::size_t i = 0; i < reference.limit_effects.size(); ++i) {
    const auto& expected = reference.limit_effects[i];
    const auto& observed = actual.limit_effects[i];
    if (expected.account != observed.account || expected.transaction_hash != observed.transaction_hash ||
        expected.pre_account_state_hash != observed.pre_account_state_hash ||
        expected.post_account_state_hash != observed.post_account_state_hash ||
        expected.effects_hash != observed.effects_hash || expected.proof_journal_hash != observed.proof_journal_hash ||
        expected.transaction_start_lt != observed.transaction_start_lt ||
        expected.transaction_end_lt != observed.transaction_end_lt || expected.transaction_root.is_null() ||
        observed.transaction_root.is_null() ||
        expected.transaction_root->get_hash() != observed.transaction_root->get_hash() ||
        expected.post_account_state.is_null() || observed.post_account_state.is_null() ||
        expected.post_account_state->get_hash() != observed.post_account_state->get_hash() ||
        expected.total_fees_hash != observed.total_fees_hash ||
        reference.limit_contexts[i].account_is_first != actual.limit_contexts[i].account_is_first ||
        reference.limit_contexts[i].charge_gas != actual.limit_contexts[i].charge_gas) {
      return td::Status::Error(PSTRING() << "offline collator transaction artifact mismatch at item " << i);
    }
  }
  for (std::size_t i = 0; i < reference.coordinator_candidates.size(); ++i) {
    const auto& expected = reference.coordinator_candidates[i];
    const auto& observed = actual.coordinator_candidates[i];
    if (!(expected.work.key == observed.work.key) || expected.work.account != observed.work.account ||
        expected.effects.effects_hash != observed.effects.effects_hash || expected.pre_account_state.is_null() ||
        observed.pre_account_state.is_null() ||
        expected.pre_account_state->get_hash() != observed.pre_account_state->get_hash() ||
        !equal_coordinator_context(expected.context, observed.context)) {
      return td::Status::Error(PSTRING() << "offline collator coordinator artifact mismatch at item " << i);
    }
  }
  for (std::size_t i = 0; i < reference.account_dictionary_deltas.size(); ++i) {
    const auto& expected = reference.account_dictionary_deltas[i];
    const auto& observed = actual.account_dictionary_deltas[i];
    if (expected.account != observed.account || expected.last_transaction_hash != observed.last_transaction_hash ||
        expected.last_transaction_lt != observed.last_transaction_lt ||
        expected.existed_before != observed.existed_before || expected.exists_after != observed.exists_after ||
        expected.post_account_state.is_null() != observed.post_account_state.is_null() ||
        (expected.post_account_state.not_null() &&
         expected.post_account_state->get_hash() != observed.post_account_state->get_hash())) {
      return td::Status::Error(PSTRING() << "offline collator account delta mismatch at item " << i);
    }
  }
  return td::Status::OK();
}

td::Status validate_offline_replay_result(const ReplayResult& reference, const ReplayResult& actual) {
  if (actual.target_accounts != reference.target_accounts || actual.accounts != reference.accounts ||
      actual.skipped_accounts != reference.skipped_accounts || actual.transactions != reference.transactions ||
      actual.tvm_transactions != reference.tvm_transactions ||
      actual.transaction_kinds.ordinary != reference.transaction_kinds.ordinary ||
      actual.transaction_kinds.tick != reference.transaction_kinds.tick ||
      actual.transaction_kinds.tock != reference.transaction_kinds.tock ||
      actual.transaction_kinds.storage != reference.transaction_kinds.storage ||
      actual.transaction_kinds.split_prepare != reference.transaction_kinds.split_prepare ||
      actual.transaction_kinds.split_install != reference.transaction_kinds.split_install ||
      actual.transaction_kinds.merge_prepare != reference.transaction_kinds.merge_prepare ||
      actual.transaction_kinds.merge_install != reference.transaction_kinds.merge_install ||
      actual.canonical_payloads_validated != reference.canonical_payloads_validated ||
      actual.canonical_payload_out_messages != reference.canonical_payload_out_messages ||
      actual.canonical_outbound_registrations != reference.canonical_outbound_registrations ||
      actual.canonical_inbound_fin_descriptors != reference.canonical_inbound_fin_descriptors ||
      actual.canonical_outbound_deq_imm_descriptors != reference.canonical_outbound_deq_imm_descriptors ||
      actual.canonical_fee_augmentations_validated != reference.canonical_fee_augmentations_validated ||
      actual.in_msg_descriptors_bound != reference.in_msg_descriptors_bound ||
      actual.out_msg_descriptors_bound != reference.out_msg_descriptors_bound ||
      actual.complete_message_descriptor_coverage != reference.complete_message_descriptor_coverage ||
      actual.basechain_limit_effects_applied != reference.basechain_limit_effects_applied ||
      actual.basechain_limit_accounts != reference.basechain_limit_accounts ||
      actual.basechain_limit_gas != reference.basechain_limit_gas ||
      actual.basechain_limit_max_end_lt != reference.basechain_limit_max_end_lt ||
      actual.shadow_coordinator_commits != reference.shadow_coordinator_commits ||
      actual.shadow_coordinator_accounts != reference.shadow_coordinator_accounts ||
      actual.shadow_coordinator_in_descriptors != reference.shadow_coordinator_in_descriptors ||
      actual.shadow_coordinator_out_descriptors != reference.shadow_coordinator_out_descriptors ||
      actual.shadow_coordinator_queue_deletions != reference.shadow_coordinator_queue_deletions ||
      actual.shadow_coordinator_new_messages != reference.shadow_coordinator_new_messages ||
      actual.shard_account_proof_values_bound != reference.shard_account_proof_values_bound ||
      actual.predecessor_state_witness_loaded != reference.predecessor_state_witness_loaded ||
      actual.collated_predecessor_witness_loaded != reference.collated_predecessor_witness_loaded ||
      actual.predecessor_state_witness_source != reference.predecessor_state_witness_source ||
      actual.shard_accounts_predecessor_root_bound != reference.shard_accounts_predecessor_root_bound ||
      actual.shard_accounts_transition_validated != reference.shard_accounts_transition_validated ||
      actual.shard_accounts_transition_status != reference.shard_accounts_transition_status ||
      actual.out_msg_queue_diff_additions != reference.out_msg_queue_diff_additions ||
      actual.out_msg_queue_diff_deletions != reference.out_msg_queue_diff_deletions ||
      actual.out_msg_queue_diff_replacements != reference.out_msg_queue_diff_replacements ||
      actual.out_msg_queue_additions_bound != reference.out_msg_queue_additions_bound ||
      actual.out_msg_queue_deletions_bound != reference.out_msg_queue_deletions_bound ||
      actual.out_msg_queue_unchanged_root_commitment != reference.out_msg_queue_unchanged_root_commitment ||
      actual.out_msg_queue_transition_status != reference.out_msg_queue_transition_status ||
      actual.augmented_dictionary_roots_validated != reference.augmented_dictionary_roots_validated) {
    return td::Status::Error("offline collator replay result disagrees with serial reference");
  }
  return td::Status::OK();
}

td::Result<AccountReplayBatchMeasurement> run_account_replay_batch(
    const std::string& archive, const HistoryBlocks& history, const BlockContext& target, const LoadedState& mc_state,
    const std::vector<LoadedAccountProof>& account_proofs, const LoadedLibraryBodies* library_bodies,
    bool profile_ed25519, const ReplayResult& reference,
    ton::validator::parallel_inbound::ReusableWorkerPool& worker_pool, std::size_t requested_workers) {
  TRY_RESULT(plan, build_account_lane_plan(account_proofs, reference, requested_workers));
  const auto workers = plan.proofs.size();

  std::vector<std::unique_ptr<td::Result<ReplayResult>>> lane_results(workers);
  std::vector<double> lane_wall(workers, 0.0);
  std::vector<ton::validator::parallel_inbound::ReusableWorkerPool::Task> tasks;
  tasks.reserve(workers);
  td::Timer batch_timer;
  for (std::size_t lane = 0; lane < workers; ++lane) {
    tasks.push_back([&, lane]() {
      td::Timer lane_timer;
      try {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(
            replay_transactions(archive, history, target, {}, nullptr, mc_state, {}, plan.proofs[lane], library_bodies,
                                profile_ed25519, false));
      } catch (const vm::VmVirtError& error) {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(
            td::Status::Error(PSTRING() << "parallel replay virtualization error: " << error.get_msg()));
      } catch (const vm::VmError& error) {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(
            td::Status::Error(PSTRING() << "parallel replay VM error: " << error.get_msg()));
      } catch (const std::exception& error) {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(td::Status::Error(PSLICE() << error.what()));
      } catch (...) {
        lane_results[lane] =
            std::make_unique<td::Result<ReplayResult>>(td::Status::Error("unknown parallel replay error"));
      }
      lane_wall[lane] = lane_timer.elapsed();
    });
  }
  TRY_STATUS(worker_pool.run_batch(std::move(tasks)));
  const double wall_seconds = batch_timer.elapsed();
  for (std::size_t lane = 0; lane < workers; ++lane) {
    if (lane_results[lane] == nullptr) {
      return td::Status::Error("parallel replay worker returned no result");
    }
    if (lane_results[lane]->is_error()) {
      return lane_results[lane]->move_as_error_prefix(PSTRING() << "parallel replay lane " << lane << ": ");
    }
  }
  TRY_STATUS(validate_account_replay_batch(reference, lane_results));

  AccountReplayBatchMeasurement measurement;
  measurement.workers = workers;
  measurement.wall_seconds = wall_seconds;
  measurement.lane_planned_account_seconds = std::move(plan.planned_account_seconds);
  measurement.lane_wall_seconds = std::move(lane_wall);
  measurement.lane_accounts.reserve(workers);
  for (const auto& proofs : plan.proofs) {
    measurement.lane_accounts.push_back(proofs.size());
  }
  return measurement;
}

td::Result<PreparedAccountReplay> merge_account_replay_lanes(
    const ReplayResult& reference, const std::vector<std::unique_ptr<td::Result<ReplayResult>>>& lane_results,
    std::vector<ReplayAccountArtifacts> lane_artifacts, double account_wall_seconds) {
  PreparedAccountReplay merged;
  auto& summary = merged.summary;
  summary.target_accounts = reference.target_accounts;
  summary.consensus_max_block_bytes = reference.consensus_max_block_bytes;
  summary.consensus_max_collated_bytes = reference.consensus_max_collated_bytes;
  summary.consensus_protocol_version = reference.consensus_protocol_version;
  summary.consensus_slots_per_leader_window = reference.consensus_slots_per_leader_window;
  summary.consensus_target_rate_ms = reference.consensus_target_rate_ms;
  summary.consensus_min_block_interval_ms = reference.consensus_min_block_interval_ms;
  summary.block_limit_bytes = reference.block_limit_bytes;
  summary.block_limit_gas = reference.block_limit_gas;
  summary.block_limit_lt_delta = reference.block_limit_lt_delta;
  summary.block_limit_collated_bytes = reference.block_limit_collated_bytes;
  summary.phase_account_replay_seconds = account_wall_seconds;
  summary.replay_total_seconds = account_wall_seconds;

  if (lane_results.size() != lane_artifacts.size()) {
    return td::Status::Error("offline collator lane result/artifact size mismatch");
  }
  for (std::size_t lane = 0; lane < lane_results.size(); ++lane) {
    const auto& replay = lane_results[lane]->ok();
    summary.accounts += replay.accounts;
    summary.transactions += replay.transactions;
    summary.tvm_transactions += replay.tvm_transactions;
    summary.transaction_kinds.add(replay.transaction_kinds);
    summary.canonical_payloads_validated += replay.canonical_payloads_validated;
    summary.canonical_payload_out_messages += replay.canonical_payload_out_messages;
    summary.canonical_outbound_registrations += replay.canonical_outbound_registrations;
    summary.canonical_inbound_fin_descriptors += replay.canonical_inbound_fin_descriptors;
    summary.canonical_outbound_deq_imm_descriptors += replay.canonical_outbound_deq_imm_descriptors;
    summary.canonical_fee_augmentations_validated += replay.canonical_fee_augmentations_validated;
    summary.hotpaths.merge(replay.hotpaths);
    for (const auto& [address, work] : replay.account_work) {
      if (!summary.account_work.emplace(address, work).second) {
        return td::Status::Error(PSTRING() << "offline collator duplicated account summary " << address.to_hex());
      }
    }

    auto& artifacts = lane_artifacts[lane];
    merged.artifacts.limit_effects.insert(merged.artifacts.limit_effects.end(),
                                          std::make_move_iterator(artifacts.limit_effects.begin()),
                                          std::make_move_iterator(artifacts.limit_effects.end()));
    merged.artifacts.limit_contexts.insert(merged.artifacts.limit_contexts.end(), artifacts.limit_contexts.begin(),
                                           artifacts.limit_contexts.end());
    merged.artifacts.coordinator_candidates.insert(merged.artifacts.coordinator_candidates.end(),
                                                   std::make_move_iterator(artifacts.coordinator_candidates.begin()),
                                                   std::make_move_iterator(artifacts.coordinator_candidates.end()));
    merged.artifacts.account_dictionary_deltas.insert(
        merged.artifacts.account_dictionary_deltas.end(),
        std::make_move_iterator(artifacts.account_dictionary_deltas.begin()),
        std::make_move_iterator(artifacts.account_dictionary_deltas.end()));
    for (const auto& [message, transaction] : artifacts.inbound_message_transactions) {
      if (!merged.artifacts.inbound_message_transactions.emplace(message, transaction).second) {
        return td::Status::Error("offline collator duplicated an inbound message artifact");
      }
    }
    for (const auto& [message, transaction] : artifacts.outbound_message_transactions) {
      if (!merged.artifacts.outbound_message_transactions.emplace(message, transaction).second) {
        return td::Status::Error("offline collator duplicated an outbound message artifact");
      }
    }
  }
  summary.skipped_accounts = summary.target_accounts - summary.accounts;
  TRY_STATUS(normalize_account_artifacts(merged.artifacts));
  return merged;
}

td::Result<OfflineCollatorReplayProbe> run_offline_collator_replay(
    const std::string& archive, const HistoryBlocks& history, const BlockContext& target,
    const Ref<vm::Cell>& predecessor_state_witness, td::Slice predecessor_state_witness_source,
    const LoadedState& mc_state, const std::vector<LoadedAccountProof>& account_proofs,
    const LoadedLibraryBodies* library_bodies, bool profile_ed25519, const ReplayResult& reference,
    ReplayAccountArtifacts reference_artifacts, std::size_t requested_workers) {
  if (reference.transaction_kinds.total() != reference.transaction_kinds.ordinary) {
    return td::Status::Error(
        "offline collator probe rejects blocks containing special transactions; serial separation is not implemented");
  }
  td::Timer planning_timer;
  TRY_RESULT(plan, build_account_lane_plan(account_proofs, reference, requested_workers));
  const double planning_seconds = planning_timer.elapsed();
  const auto workers = plan.proofs.size();

  td::Timer startup_timer;
  TRY_RESULT(worker_pool, ton::validator::parallel_inbound::ReusableWorkerPool::create(workers));
  const double startup_seconds = startup_timer.elapsed();

  std::vector<std::unique_ptr<td::Result<ReplayResult>>> lane_results(workers);
  std::vector<ReplayAccountArtifacts> lane_artifacts(workers);
  std::vector<ton::validator::parallel_inbound::ReusableWorkerPool::Task> tasks;
  tasks.reserve(workers);
  td::Timer account_timer;
  for (std::size_t lane = 0; lane < workers; ++lane) {
    tasks.push_back([&, lane]() {
      try {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(replay_transactions(
            archive, history, target, {}, nullptr, mc_state, {}, plan.proofs[lane], library_bodies, profile_ed25519,
            false, ReplayExecutionMode::account_effects_only, &lane_artifacts[lane]));
      } catch (const vm::VmVirtError& error) {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(
            td::Status::Error(PSTRING() << "offline collator virtualization error: " << error.get_msg()));
      } catch (const vm::VmError& error) {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(
            td::Status::Error(PSTRING() << "offline collator VM error: " << error.get_msg()));
      } catch (const std::exception& error) {
        lane_results[lane] = std::make_unique<td::Result<ReplayResult>>(td::Status::Error(PSLICE() << error.what()));
      } catch (...) {
        lane_results[lane] =
            std::make_unique<td::Result<ReplayResult>>(td::Status::Error("unknown offline collator worker error"));
      }
    });
  }
  TRY_STATUS(worker_pool->run_batch(std::move(tasks)));
  const double account_seconds = account_timer.elapsed();
  for (std::size_t lane = 0; lane < workers; ++lane) {
    if (lane_results[lane] == nullptr) {
      return td::Status::Error("offline collator worker returned no result");
    }
    if (lane_results[lane]->is_error()) {
      return lane_results[lane]->move_as_error_prefix(PSTRING() << "offline collator lane " << lane << ": ");
    }
  }
  td::Timer equivalence_timer;
  TRY_STATUS(validate_account_replay_batch(reference, lane_results, false));
  double equivalence_seconds = equivalence_timer.elapsed();
  td::Timer merge_timer;
  TRY_RESULT(prepared, merge_account_replay_lanes(reference, lane_results, std::move(lane_artifacts), account_seconds));
  const double merge_seconds = merge_timer.elapsed();
  td::Timer artifact_equivalence_timer;
  TRY_STATUS(normalize_account_artifacts(reference_artifacts));
  TRY_STATUS(validate_account_artifact_equivalence(reference_artifacts, prepared.artifacts));
  equivalence_seconds += artifact_equivalence_timer.elapsed();

  td::Timer commit_timer;
  TRY_RESULT(committed, replay_transactions(archive, history, target, predecessor_state_witness, nullptr, mc_state, {},
                                            account_proofs, library_bodies, profile_ed25519, true,
                                            ReplayExecutionMode::prepared_effects, nullptr, &prepared,
                                            predecessor_state_witness_source));
  const double commit_seconds = commit_timer.elapsed();
  td::Timer result_equivalence_timer;
  TRY_STATUS(validate_offline_replay_result(reference, committed));
  equivalence_seconds += result_equivalence_timer.elapsed();

  OfflineCollatorReplayProbe probe;
  probe.requested_workers = requested_workers;
  probe.workers = workers;
  probe.transactions = committed.transactions;
  probe.accounts = committed.accounts;
  probe.lane_planning_seconds = planning_seconds;
  probe.worker_pool_startup_seconds = startup_seconds;
  probe.account_execution_seconds = account_seconds;
  probe.artifact_merge_seconds = merge_seconds;
  probe.equivalence_check_seconds = equivalence_seconds;
  probe.serial_commit_seconds = commit_seconds;
  probe.total_seconds = planning_seconds + account_seconds + merge_seconds + commit_seconds;
  probe.serial_reference_seconds = reference.replay_total_seconds;
  probe.augmented_dictionary_roots_validated = committed.augmented_dictionary_roots_validated;
  probe.exact_artifact_set = true;
  probe.exact_replay_result = true;
  return probe;
}

td::Result<ParallelAccountReplayProbe> run_parallel_account_replay_probe(
    const std::string& archive, const HistoryBlocks& history, const BlockContext& target, const LoadedState& mc_state,
    const std::vector<LoadedAccountProof>& account_proofs, const LoadedLibraryBodies* library_bodies,
    bool profile_ed25519, const ReplayResult& reference, std::size_t requested_workers, std::size_t samples) {
  if (samples == 0 || samples > 31 || samples % 2 == 0) {
    return td::Status::Error("parallel account replay sample count must be odd and between 1 and 31");
  }
  if (account_proofs.empty()) {
    return td::Status::Error("parallel account replay requires at least one account proof");
  }
  const auto workers = std::min(requested_workers, account_proofs.size());
  td::Timer startup_timer;
  TRY_RESULT(worker_pool, ton::validator::parallel_inbound::ReusableWorkerPool::create(workers));
  const double startup_seconds = startup_timer.elapsed();

  std::vector<AccountReplayBatchMeasurement> serial_runs;
  std::vector<AccountReplayBatchMeasurement> parallel_runs;
  serial_runs.reserve(samples);
  parallel_runs.reserve(samples);
  auto run_serial = [&]() -> td::Status {
    TRY_RESULT(measurement, run_account_replay_batch(archive, history, target, mc_state, account_proofs, library_bodies,
                                                     profile_ed25519, reference, *worker_pool, 1));
    serial_runs.push_back(std::move(measurement));
    return td::Status::OK();
  };
  auto run_parallel = [&]() -> td::Status {
    TRY_RESULT(measurement, run_account_replay_batch(archive, history, target, mc_state, account_proofs, library_bodies,
                                                     profile_ed25519, reference, *worker_pool, requested_workers));
    parallel_runs.push_back(std::move(measurement));
    return td::Status::OK();
  };
  for (std::size_t sample = 0; sample < samples; ++sample) {
    if (sample % 2 == 0) {
      TRY_STATUS(run_serial());
      TRY_STATUS(run_parallel());
    } else {
      TRY_STATUS(run_parallel());
      TRY_STATUS(run_serial());
    }
  }

  ParallelAccountReplayProbe probe;
  probe.requested_workers = requested_workers;
  probe.workers = parallel_runs.front().workers;
  probe.samples = samples;
  probe.worker_pool_startup_seconds = startup_seconds;
  probe.transactions = reference.transactions;
  probe.accounts = reference.accounts;
  probe.lane_accounts = parallel_runs.front().lane_accounts;
  probe.lane_planned_account_seconds = parallel_runs.front().lane_planned_account_seconds;
  std::vector<std::vector<double>> lane_wall_samples(probe.workers);
  for (std::size_t sample = 0; sample < samples; ++sample) {
    probe.serial_wall_samples.push_back(serial_runs[sample].wall_seconds);
    probe.parallel_wall_samples.push_back(parallel_runs[sample].wall_seconds);
    probe.wall_speedup_samples.push_back(serial_runs[sample].wall_seconds / parallel_runs[sample].wall_seconds);
    for (std::size_t lane = 0; lane < probe.workers; ++lane) {
      lane_wall_samples[lane].push_back(parallel_runs[sample].lane_wall_seconds[lane]);
    }
  }
  probe.serial_wall_seconds = median(probe.serial_wall_samples);
  probe.parallel_wall_seconds = median(probe.parallel_wall_samples);
  probe.lane_wall_seconds.reserve(probe.workers);
  for (auto& lane_samples : lane_wall_samples) {
    probe.lane_wall_seconds.push_back(median(std::move(lane_samples)));
  }
  return probe;
}

td::Result<std::string> inspect_json(const BlockContext& target, int split_depth) {
  TRY_RESULT(prefixes, collect_account_prefixes(target, split_depth));
  TRY_RESULT(accounts, collect_accounts(target));
  TRY_RESULT(workload, summarize_account_blocks(target));
  TRY_RESULT(state_update_views, extract_state_update_raw_views(target));
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"inspect\",\"block_id\":\"" << target.id.to_str()
      << "\",\"global_id\":" << target.global_id << ",\"gen_utime\":" << target.gen_utime
      << ",\"start_lt\":" << target.start_lt << ",\"end_lt\":" << target.end_lt
      << ",\"after_split\":" << target.after_split << ",\"after_merge\":" << target.after_merge
      << ",\"before_split\":" << target.before_split << ",\"file_bytes\":" << target.file_bytes
      << ",\"predecessor_state_root\":\"" << state_update_views.first->get_hash(0).to_hex()
      << "\",\"result_state_root\":\"" << state_update_views.second->get_hash(0).to_hex() << "\""
      << ",\"distinct_accounts\":" << workload.distinct_accounts
      << ",\"raw_transactions\":" << workload.raw_transactions
      << ",\"transaction_kinds\":" << transaction_kinds_json(workload.transaction_kinds)
      << ",\"max_account_transactions\":" << workload.max_account_transactions << ",\"predecessors\":[";
  for (std::size_t i = 0; i < target.prev.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "\"" << target.prev[i].to_str() << "\"";
  }
  out << "],\"masterchain_ref\":\"" << target.mc_id.to_str() << "\",\"split_depth\":" << split_depth
      << ",\"recommended_account_proof_reference\":\""
      << (target.prev.size() == 1 ? target.prev[0].to_str() : "unavailable_at_split_or_merge") << "\""
      << ",\"required_account_prefixes\":[";
  bool first_prefix = true;
  for (const auto& prefix : prefixes) {
    if (!first_prefix) {
      out << ",";
    }
    first_prefix = false;
    out << "\"" << prefix << "\"";
  }
  out << "],\"required_accounts\":[";
  bool first_account = true;
  for (const auto& account : accounts) {
    if (!first_account) {
      out << ",";
    }
    first_account = false;
    out << "\"" << account.to_hex() << "\"";
  }
  out << "],\"replay_supported\":" << verify_replay_scope(target).is_ok() << "}";
  return out.as_cslice().str();
}

td::Result<std::string> inspect_state_json(const LoadedState& state, int split_depth) {
  std::vector<ton::validator::SelectiveSplitStatePartDescriptor> parts;
  if (state.split_header) {
    TRY_RESULT(assembler, ton::validator::SelectiveSplitStateAssembler::create(
                              state.id.shard_full(), ton::RootHash{state.root->get_hash().bits()},
                              state.serialized_root, static_cast<td::uint32>(split_depth)));
    parts = assembler->parts();
  }
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"inspect_state\",\"state_id\":\"" << state.id.to_str()
      << "\",\"global_id\":" << state.record.global_id << ",\"gen_utime\":" << state.record.gen_utime
      << ",\"gen_lt\":" << state.record.gen_lt << ",\"root_hash\":\"" << state.root->get_hash().to_hex()
      << "\",\"split_header\":" << state.split_header << ",\"split_depth\":" << split_depth
      << ",\"account_part_count\":" << parts.size() << ",\"account_parts\":[";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "{\"effective_shard\":\"" << ton::shard_to_str(parts[i].effective_shard) << "\",\"root_hash\":\""
        << parts[i].wrapped_root_hash.to_hex() << "\"}";
  }
  out << "]}";
  return out.as_cslice().str();
}

std::string account_lane_ceiling_json(const ReplayResult& replay) {
  std::vector<double> transaction_work;
  std::vector<double> tvm_work;
  transaction_work.reserve(replay.account_work.size());
  tvm_work.reserve(replay.account_work.size());
  double max_transaction_work = 0.0;
  double total_transaction_work = 0.0;
  for (const auto& [_, work] : replay.account_work) {
    transaction_work.push_back(work.transaction_seconds);
    tvm_work.push_back(work.tvm_seconds);
    max_transaction_work = std::max(max_transaction_work, work.transaction_seconds);
    total_transaction_work += work.transaction_seconds;
  }

  td::StringBuilder out;
  out << "{\"scope\":\"transaction_replay_only\",\"method\":\"greedy_lpt_account_totals\""
      << ",\"distinct_accounts\":" << replay.account_work.size() << ",\"max_account_transaction_share\":"
      << (total_transaction_work > 0.0 ? max_transaction_work / total_transaction_work : 0.0) << ",\"workers\":[";
  bool first_worker = true;
  for (std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
    if (!first_worker) {
      out << ",";
    }
    first_worker = false;
    const auto transaction_ceiling =
        ton::validator::parallel_inbound::estimate_account_lane_ceiling(transaction_work, workers);
    const auto tvm_ceiling = ton::validator::parallel_inbound::estimate_account_lane_ceiling(tvm_work, workers);
    out << "{\"workers\":" << workers << ",\"transaction_serial_seconds\":" << transaction_ceiling.serial_work
        << ",\"transaction_critical_path_seconds\":" << transaction_ceiling.critical_path
        << ",\"transaction_ideal_speedup\":" << transaction_ceiling.ideal_speedup()
        << ",\"tvm_serial_seconds\":" << tvm_ceiling.serial_work
        << ",\"tvm_critical_path_seconds\":" << tvm_ceiling.critical_path
        << ",\"tvm_ideal_speedup\":" << tvm_ceiling.ideal_speedup() << "}";
  }
  out << "],\"excludes\":[\"worker_contention\",\"serial_commit\",\"block_limits\",\"cell_proof_merge\","
         "\"state_merge\",\"network\",\"consensus\"]}";
  return out.as_cslice().str();
}

std::string single_shard_capacity_json(const BlockContext& target, const ReplayResult& replay,
                                       const ParallelAccountReplayProbe* parallel_probe,
                                       const OfflineCollatorReplayProbe* offline_collator_probe) {
  const bool full_block = replay.skipped_accounts == 0 && replay.accounts == replay.target_accounts;
  const double target_rate_seconds = static_cast<double>(replay.consensus_target_rate_ms) / 1000.0;
  const double bytes_per_raw_transaction =
      full_block && replay.transactions > 0
          ? static_cast<double>(target.file_bytes) / static_cast<double>(replay.transactions)
          : 0.0;
  const bool byte_projection_available = full_block && target_rate_seconds > 0.0 && target.file_bytes > 0 &&
                                         replay.transactions > 0 && replay.consensus_max_block_bytes > 0;
  const double byte_ceiling_raw_tps = byte_projection_available
                                          ? static_cast<double>(replay.transactions) *
                                                static_cast<double>(replay.consensus_max_block_bytes) /
                                                static_cast<double>(target.file_bytes) / target_rate_seconds
                                          : 0.0;

  td::StringBuilder out;
  out << "{\"status\":\"incomplete_requires_collator_validate_query_and_saturated_workload\""
      << ",\"metric\":\"raw_transactions_per_second\""
      << ",\"operation_tps\":null"
      << ",\"config_source_masterchain_id\":\"" << target.mc_id.to_str() << "\""
      << ",\"config29\":{\"measurement_domain\":\"serialized_candidate_data\",\"max_block_bytes\":"
      << replay.consensus_max_block_bytes << ",\"max_collated_bytes\":" << replay.consensus_max_collated_bytes << "}"
      << ",\"config30\":{\"protocol_version\":" << replay.consensus_protocol_version
      << ",\"slots_per_leader_window\":" << replay.consensus_slots_per_leader_window
      << ",\"target_rate_ms\":" << replay.consensus_target_rate_ms
      << ",\"min_block_interval_ms\":" << replay.consensus_min_block_interval_ms << "}"
      << ",\"config23\":{\"measurement_domain\":\"block_limit_status_estimates\",\"bytes\":{\"underload\":"
      << replay.block_limit_bytes.underload << ",\"soft\":" << replay.block_limit_bytes.soft
      << ",\"hard\":" << replay.block_limit_bytes.hard
      << "},\"gas\":{\"underload\":" << replay.block_limit_gas.underload << ",\"soft\":" << replay.block_limit_gas.soft
      << ",\"hard\":" << replay.block_limit_gas.hard
      << "},\"lt_delta\":{\"underload\":" << replay.block_limit_lt_delta.underload
      << ",\"soft\":" << replay.block_limit_lt_delta.soft << ",\"hard\":" << replay.block_limit_lt_delta.hard
      << "},\"collated_bytes\":{\"underload\":" << replay.block_limit_collated_bytes.underload
      << ",\"soft\":" << replay.block_limit_collated_bytes.soft
      << ",\"hard\":" << replay.block_limit_collated_bytes.hard << "}}"
      << ",\"sample\":{\"scope\":\"" << (full_block ? "full_block" : "account_subset")
      << "\",\"block_file_bytes\":" << target.file_bytes << ",\"raw_transactions\":" << replay.transactions
      << ",\"transaction_kinds\":" << transaction_kinds_json(replay.transaction_kinds)
      << ",\"distinct_accounts\":" << replay.accounts << ",\"billed_gas_sum\":" << replay.basechain_limit_gas
      << ",\"bytes_per_raw_transaction\":";
  if (full_block && replay.transactions > 0) {
    out << bytes_per_raw_transaction;
  } else {
    out << "null";
  }
  out << ",\"max_block_fill_ratio\":"
      << (replay.consensus_max_block_bytes > 0
              ? static_cast<double>(target.file_bytes) / replay.consensus_max_block_bytes
              : 0.0)
      << "}"
      << ",\"workload_byte_projection_raw_tps\":";
  if (byte_projection_available) {
    out << byte_ceiling_raw_tps;
  } else {
    out << "null";
  }
  out << ",\"three_raw_transactions_per_operation_byte_projection_tps\":";
  if (byte_projection_available) {
    out << byte_ceiling_raw_tps / 3.0;
  } else {
    out << "null";
  }
  out << ",\"active_config23_limit_projection_raw_tps\":null";
  out << ",\"serial_replay_raw_tps\":";
  if (parallel_probe != nullptr && parallel_probe->serial_wall_seconds > 0.0) {
    out << static_cast<double>(replay.transactions) / parallel_probe->serial_wall_seconds;
  } else {
    out << "null";
  }
  out << ",\"parallel_replay_raw_tps\":";
  if (parallel_probe != nullptr && parallel_probe->parallel_wall_seconds > 0.0) {
    out << static_cast<double>(replay.transactions) / parallel_probe->parallel_wall_seconds;
  } else {
    out << "null";
  }
  out << ",\"serial_full_replay_raw_tps\":";
  if (full_block && replay.replay_total_seconds > 0.0) {
    out << static_cast<double>(replay.transactions) / replay.replay_total_seconds;
  } else {
    out << "null";
  }
  out << ",\"offline_collator_full_replay_raw_tps\":";
  if (full_block && offline_collator_probe != nullptr && offline_collator_probe->total_seconds > 0.0) {
    out << static_cast<double>(offline_collator_probe->transactions) / offline_collator_probe->total_seconds;
  } else {
    out << "null";
  }
  out << ",\"offline_collator_full_replay_speedup\":";
  if (offline_collator_probe != nullptr) {
    out << offline_collator_probe->speedup();
  } else {
    out << "null";
  }
  out << ",\"mainnet_sustainable_raw_tps\":null"
      << ",\"missing_gates\":[\"live_collator_integration\",\"validate_query_wall\","
         "\"live_four_root_commit_wall\",\"active_config23_dimension_under_saturation\","
         "\"multi_block_saturated_workload\","
         "\"network_candidate_delivery\"]"
      << ",\"warnings\":[\"linear_block_boc_density_projection\",\"mixed_raw_transaction_workload\","
         "\"isolated_replay_is_not_collation\",\"offline_root_probe_is_not_live_collator_commit\","
         "\"offline_full_replay_rate_is_not_sustainable_mainnet_tps\","
         "\"three_transaction_operation_is_not_workload_classification\","
         "\"billed_gas_sum_excludes_unreconstructed_special_context\","
         "\"target_rate_is_not_observed_block_interval\",\"max_block_bytes_may_not_be_active_limit\","
         "\"collated_bytes_are_not_block_bytes\"";
  if (replay.predecessor_state_witness_source == "target_block_state_update_hindsight") {
    out << ",\"target_block_update_witness_is_hindsight_only\"";
  }
  out << "]}";
  return out.as_cslice().str();
}

std::string replay_json(const BlockContext& target, const LoadedState* account_state,
                        const std::vector<LoadedAccountProof>& account_proofs, const LoadedState& mc_state,
                        const LoadedLibraryBodies* library_bodies, const ReplayResult& replay, bool profile_ed25519,
                        const ParallelAccountReplayProbe* parallel_probe,
                        const OfflineCollatorReplayProbe* offline_collator_probe) {
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"transaction_equivalence_replay\",\"block_id\":\"" << target.id.to_str()
      << "\",\"predecessor_id\":\"" << target.prev[0].to_str() << "\",\"account_source\":\""
      << (account_state == nullptr ? "lite_server_proofs" : "persistent_state") << "\"";
  if (account_state != nullptr) {
    out << ",\"account_state_base_id\":\"" << account_state->id.to_str() << "\",\"account_state_root_hash\":\""
        << account_state->root->get_hash().to_hex() << "\"";
  } else {
    out << ",\"account_proof_bases\":[";
    for (std::size_t i = 0; i < account_proofs.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << "{\"address\":\"" << account_proofs[i].address.to_hex() << "\",\"block_id\":\""
          << account_proofs[i].shard_block.to_str() << "\"}";
    }
    out << "]";
  }
  out << ",\"masterchain_state_id\":\"" << mc_state.id.to_str() << "\",\"masterchain_state_root_hash\":\""
      << mc_state.root->get_hash().to_hex() << "\",\"masterchain_ref\":\"" << target.mc_id.to_str()
      << "\",\"library_source\":\""
      << (library_bodies != nullptr ? "content_hash_bundle"
          : mc_state.boc != nullptr ? "masterchain_state"
                                    : "config_proof")
      << "\",\"library_membership_at_target_proven\":" << (library_bodies == nullptr && mc_state.boc != nullptr)
      << ",\"scope\":\""
      << (replay.skipped_accounts == 0 ? "full_block"
          : account_proofs.empty()     ? "account_prefix_subset"
                                       : "account_subset")
      << "\",\"target_accounts\":" << replay.target_accounts << ",\"accounts\":" << replay.accounts
      << ",\"skipped_accounts\":" << replay.skipped_accounts << ",\"transactions\":" << replay.transactions
      << ",\"transaction_kinds\":" << transaction_kinds_json(replay.transaction_kinds)
      << ",\"tvm_transactions\":" << replay.tvm_transactions << ",\"ed25519_profiled\":" << profile_ed25519
      << ",\"equivalence\":\"transaction_hash_and_account_state_hash\""
      << ",\"psae_payload_validation\":{\"canonical_payloads\":" << replay.canonical_payloads_validated
      << ",\"ordered_out_messages\":" << replay.canonical_payload_out_messages
      << ",\"outbound_registrations\":" << replay.canonical_outbound_registrations
      << ",\"inbound_fin_descriptors\":" << replay.canonical_inbound_fin_descriptors
      << ",\"outbound_deq_imm_descriptors\":" << replay.canonical_outbound_deq_imm_descriptors
      << ",\"fee_augmentations\":" << replay.canonical_fee_augmentations_validated
      << ",\"in_msg_descriptors_bound\":" << replay.in_msg_descriptors_bound
      << ",\"out_msg_descriptors_bound\":" << replay.out_msg_descriptors_bound
      << ",\"complete_message_descriptor_coverage\":"
      << (replay.complete_message_descriptor_coverage ? "true" : "false")
      << ",\"basechain_limit_effects\":" << replay.basechain_limit_effects_applied
      << ",\"basechain_limit_accounts\":" << replay.basechain_limit_accounts
      << ",\"basechain_limit_gas\":" << replay.basechain_limit_gas
      << ",\"basechain_limit_gas_status\":\"billed_gas_sum_special_context_not_reconstructed\""
      << ",\"basechain_limit_max_end_lt\":" << replay.basechain_limit_max_end_lt
      << ",\"shadow_coordinator_commits\":" << replay.shadow_coordinator_commits
      << ",\"shadow_coordinator_accounts\":" << replay.shadow_coordinator_accounts
      << ",\"shadow_coordinator_in_descriptors\":" << replay.shadow_coordinator_in_descriptors
      << ",\"shadow_coordinator_out_descriptors\":" << replay.shadow_coordinator_out_descriptors
      << ",\"shadow_coordinator_queue_deletions\":" << replay.shadow_coordinator_queue_deletions
      << ",\"shadow_coordinator_new_messages\":" << replay.shadow_coordinator_new_messages
      << ",\"shadow_coordinator_queue_scope\":\"descriptor_derived_deletion_subset\""
      << ",\"predecessor_state_witness_loaded\":" << (replay.predecessor_state_witness_loaded ? "true" : "false")
      << ",\"collated_predecessor_witness_loaded\":" << (replay.collated_predecessor_witness_loaded ? "true" : "false")
      << ",\"predecessor_state_witness_source\":\"" << replay.predecessor_state_witness_source << "\""
      << ",\"predecessor_proof_merge_algorithm\":";
  if (replay.shard_accounts_predecessor_root_bound) {
    out << "\"ton_merkle_proof_combine_fast_incremental\"";
  } else {
    out << "null";
  }
  out << ",\"shard_account_proof_values_bound\":" << replay.shard_account_proof_values_bound
      << ",\"shard_accounts_predecessor_root_bound\":"
      << (replay.shard_accounts_predecessor_root_bound ? "true" : "false")
      << ",\"shard_accounts_transition_validated\":" << (replay.shard_accounts_transition_validated ? "true" : "false")
      << ",\"shard_accounts_transition_status\":\"" << replay.shard_accounts_transition_status << "\""
      << ",\"out_msg_queue_diff_additions\":" << replay.out_msg_queue_diff_additions
      << ",\"out_msg_queue_diff_deletions\":" << replay.out_msg_queue_diff_deletions
      << ",\"out_msg_queue_diff_replacements\":" << replay.out_msg_queue_diff_replacements
      << ",\"out_msg_queue_additions_bound\":" << replay.out_msg_queue_additions_bound
      << ",\"out_msg_queue_deletions_bound\":" << replay.out_msg_queue_deletions_bound
      << ",\"out_msg_queue_unchanged_root_commitment\":"
      << (replay.out_msg_queue_unchanged_root_commitment ? "true" : "false")
      << ",\"out_msg_queue_transition_status\":\"" << replay.out_msg_queue_transition_status << "\""
      << ",\"augmented_dictionary_roots_validated\":" << replay.augmented_dictionary_roots_validated
      << ",\"augmented_dictionary_root_scope\":\""
      << (replay.augmented_dictionary_roots_validated == 4 ? "ShardAccounts_InMsgDescr_OutMsgDescr_OutMsgQueue"
                                                           : "InMsgDescr_OutMsgDescr")
      << "\""
      << ",\"augmented_dictionary_baseline_source\":\"target_minus_validated_deltas\""
      << ",\"augmented_dictionary_historical_transition_proven\":"
      << (replay.collated_predecessor_witness_loaded ? "true" : "false") << ",\"shard_accounts_root_status\":\""
      << (replay.shard_accounts_transition_validated ? "exact_transition_validated"
          : replay.predecessor_state_witness_loaded  ? "predecessor_witness_loaded_transition_incomplete"
                                                     : "requires_predecessor_state_witness")
      << "\""
      << ",\"out_msg_queue_root_status\":\""
      << ((replay.out_msg_queue_transition_status == "exact_unchanged_root_commitment" ||
           replay.out_msg_queue_transition_status == "exact_diff_and_value_binding")
              ? "exact_transition_validated"
          : replay.out_msg_queue_transition_status == "complete_diff_enumerated"
              ? "complete_diff_enumerated_root_commit_pending"
          : replay.predecessor_state_witness_loaded ? "predecessor_witness_loaded_transition_incomplete"
                                                    : "requires_predecessor_state_witness")
      << "\""
      << ",\"block_size_status\":\"not_claimed_without_collator_usage_tree\""
      << ",\"proof_journals\":\"empty_in_transaction_replay\""
      << ",\"processed_upto\":\"atomic_shadow_prefix_applied_for_inbound_fin_subset\""
      << ",\"global_effects\":\"atomic_shadow_applied_not_live_collator_dictionaries\"}"
      << ",\"replay_phases_wall\":{\"setup_seconds\":" << replay.phase_setup_seconds
      << ",\"account_replay_seconds\":" << replay.phase_account_replay_seconds
      << ",\"artifact_export_seconds_excluded_from_total\":" << replay.phase_artifact_export_seconds
      << ",\"block_limits_seconds\":" << replay.phase_block_limits_seconds
      << ",\"coordinator_seconds\":" << replay.phase_coordinator_seconds
      << ",\"augmented_roots_seconds\":" << replay.phase_augmented_roots_seconds
      << ",\"augmented_root_subphases\":{\"inputs_seconds\":" << replay.phase_root_inputs_seconds
      << ",\"predecessor_proof_seconds\":" << replay.phase_root_predecessor_proof_seconds
      << ",\"descriptor_baseline_seconds\":" << replay.phase_root_descriptor_baseline_seconds
      << ",\"account_binding_seconds\":" << replay.phase_root_account_binding_seconds
      << ",\"state_transition_seconds\":" << replay.phase_root_state_transition_seconds
      << ",\"root_commit_seconds\":" << replay.phase_root_commit_seconds << "}"
      << ",\"augmented_roots_validated\":" << replay.augmented_dictionary_roots_validated
      << ",\"total_seconds\":" << replay.replay_total_seconds << "}"
      << ",\"hotpaths_wall\":" << replay.hotpaths.to_json(false, 0, replay.hotpaths.size());
#if TD_WINDOWS
  out << ",\"hotpaths_cpu\":null,\"cpu_metric_status\":\"unsupported_windows_timer_resolution\"";
#else
  out << ",\"hotpaths_cpu\":" << replay.hotpaths.to_json(true, 0, replay.hotpaths.size())
      << ",\"cpu_metric_status\":\"available\"";
#endif
  out << ",\"account_lane_ceiling\":" << account_lane_ceiling_json(replay) << ",\"parallel_account_replay\":";
  if (parallel_probe == nullptr) {
    out << "null";
  } else {
    out << "{\"scope\":\"isolated_account_proof_probe\",\"requested_workers\":" << parallel_probe->requested_workers
        << ",\"workers\":" << parallel_probe->workers << ",\"samples\":" << parallel_probe->samples
        << ",\"sample_order\":\""
        << (parallel_probe->samples == 1 ? "serial_then_parallel" : "alternating_serial_parallel_pairs") << "\""
        << ",\"summary_statistic\":\"median\""
        << ",\"worker_pool_startup_seconds\":" << parallel_probe->worker_pool_startup_seconds
        << ",\"transactions\":" << parallel_probe->transactions << ",\"accounts\":" << parallel_probe->accounts
        << ",\"serial_wall_seconds\":" << parallel_probe->serial_wall_seconds
        << ",\"parallel_wall_seconds\":" << parallel_probe->parallel_wall_seconds
        << ",\"wall_speedup\":" << parallel_probe->speedup() << ",\"serial_wall_samples\":[";
    for (std::size_t i = 0; i < parallel_probe->serial_wall_samples.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << parallel_probe->serial_wall_samples[i];
    }
    out << "],\"parallel_wall_samples\":[";
    for (std::size_t i = 0; i < parallel_probe->parallel_wall_samples.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << parallel_probe->parallel_wall_samples[i];
    }
    out << "],\"wall_speedup_samples\":[";
    for (std::size_t i = 0; i < parallel_probe->wall_speedup_samples.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << parallel_probe->wall_speedup_samples[i];
    }
    out << "],\"lane_accounts\":[";
    for (std::size_t i = 0; i < parallel_probe->lane_accounts.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << parallel_probe->lane_accounts[i];
    }
    out << "],\"lane_planned_account_seconds\":[";
    for (std::size_t i = 0; i < parallel_probe->lane_planned_account_seconds.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << parallel_probe->lane_planned_account_seconds[i];
    }
    out << "],\"lane_wall_seconds\":[";
    for (std::size_t i = 0; i < parallel_probe->lane_wall_seconds.size(); ++i) {
      if (i != 0) {
        out << ",";
      }
      out << parallel_probe->lane_wall_seconds[i];
    }
    out << "],\"equivalence_gate\":\"historical_transaction_and_account_state_hashes_plus_effect_counters\""
        << ",\"scheduler\":\"greedy_lpt_from_canonical_account_wall\""
        << ",\"includes\":[\"reusable_worker_pool_batch_barrier\",\"per_lane_config_extract\","
           "\"per_lane_full_account_block_scan\",\"per_lane_shadow_effect_checks\"]"
        << ",\"known_biases\":[\"shared_process_cache_state\""
        << (parallel_probe->samples == 1 ? ",\"single_pair_serial_precedes_parallel\"" : "") << "]"
        << ",\"excludes\":[\"worker_pool_startup\",\"augmented_root_commit\",\"collator_integration\","
           "\"network\",\"consensus\"]}";
  }
  out << ",\"offline_collator_replay\":";
  if (offline_collator_probe == nullptr) {
    out << "null";
  } else {
    out << "{\"scope\":\"offline_immutable_account_effects_plus_serial_shadow_commit\""
        << ",\"requested_workers\":" << offline_collator_probe->requested_workers
        << ",\"workers\":" << offline_collator_probe->workers
        << ",\"transactions\":" << offline_collator_probe->transactions
        << ",\"accounts\":" << offline_collator_probe->accounts
        << ",\"lane_planning_seconds\":" << offline_collator_probe->lane_planning_seconds
        << ",\"worker_pool_startup_seconds\":" << offline_collator_probe->worker_pool_startup_seconds
        << ",\"account_execution_seconds\":" << offline_collator_probe->account_execution_seconds
        << ",\"artifact_merge_seconds\":" << offline_collator_probe->artifact_merge_seconds
        << ",\"equivalence_check_seconds_excluded_from_total\":" << offline_collator_probe->equivalence_check_seconds
        << ",\"serial_commit_seconds\":" << offline_collator_probe->serial_commit_seconds
        << ",\"total_seconds\":" << offline_collator_probe->total_seconds
        << ",\"serial_reference_seconds\":" << offline_collator_probe->serial_reference_seconds
        << ",\"wall_speedup\":" << offline_collator_probe->speedup()
        << ",\"augmented_dictionary_roots_validated\":" << offline_collator_probe->augmented_dictionary_roots_validated
        << ",\"exact_artifact_set\":" << (offline_collator_probe->exact_artifact_set ? "true" : "false")
        << ",\"exact_replay_result\":" << (offline_collator_probe->exact_replay_result ? "true" : "false")
        << ",\"special_transaction_policy\":\"probe_rejects_any_non_ordinary_transaction\""
        << ",\"failure_policy\":\"discard_entire_offline_batch_before_global_publish\""
        << ",\"sample_order\":\"serial_reference_then_parallel_path\""
        << ",\"known_biases\":[\"single_sample\",\"shared_process_cache_state\","
           "\"serial_reference_precedes_parallel_path\"]"
        << ",\"includes\":[\"parallel_account_execution\",\"deterministic_artifact_merge\","
           "\"serial_block_limits\",\"serial_shadow_coordinator\",\"available_augmented_root_gates\"]"
        << ",\"excludes\":[\"worker_pool_startup\",\"live_collator_mutation\",\"block_candidate_serialization\","
           "\"validate_query\",\"network\",\"consensus\"]}";
  }
  out << ",\"single_shard_capacity\":"
      << single_shard_capacity_json(target, replay, parallel_probe, offline_collator_probe) << "}";
  return out.as_cslice().str();
}

td::Result<std::string> run(const std::string& archive, const std::string& block_boc, const std::string& mc_archive,
                            const std::string& block_id_text, bool inspect, const std::string& collated_data_path,
                            bool use_block_update_witness, const std::string& block_update_witness_base_state_path,
                            const std::string& out_msg_queue_proof_path, const std::string& prev_state_path,
                            const std::string& mc_state_path, const std::string& mc_proof_path,
                            const std::vector<std::string>& history_block_boc_paths,
                            const std::vector<std::string>& library_body_paths,
                            const std::vector<std::string>& account_part_specs,
                            const std::vector<std::string>& account_proof_specs, int split_depth, bool profile_ed25519,
                            std::size_t account_workers, std::size_t account_samples,
                            std::size_t offline_collator_workers) {
  const bool block_boc_mode = !block_boc.empty();
  td::Result<LoadedBlock> loaded_block = td::Status::Error("block source was not selected");
  if (block_boc_mode) {
    TRY_RESULT(expected_id, BlockIdExt::from_str(block_id_text));
    loaded_block = load_block_from_boc(block_boc, expected_id);
  } else {
    TRY_RESULT(requested_id, BlockId::from_str(block_id_text));
    loaded_block = load_block_from_archive(archive, requested_id);
  }
  TRY_RESULT(block_data, std::move(loaded_block));
  TRY_RESULT(target, unpack_block_context(std::move(block_data)));
  if (inspect) {
    if (account_workers != 0 || offline_collator_workers != 0) {
      return td::Status::Error("worker probes are only valid in replay mode");
    }
    return inspect_json(target, split_depth);
  }

  Ref<vm::Cell> predecessor_state_witness;
  std::string predecessor_state_witness_source;
  if (!collated_data_path.empty()) {
    TRY_RESULT(raw_views, extract_state_update_raw_views(target));
    TRY_RESULT(witness,
               load_collated_predecessor_witness(collated_data_path, td::Bits256(raw_views.first->get_hash(0).bits())));
    predecessor_state_witness = std::move(witness);
    predecessor_state_witness_source = "collated_data";
  } else if (use_block_update_witness) {
    predecessor_state_witness_source = "target_block_state_update_hindsight";
  }

  TRY_STATUS(verify_replay_scope(target));
  TRY_RESULT(history, load_history_block_bocs(history_block_boc_paths, target));
  if (block_boc_mode &&
      (!prev_state_path.empty() || !account_part_specs.empty() || !mc_archive.empty() || !mc_state_path.empty())) {
    return td::Status::Error("--block-boc replay requires --mc-proof and predecessor-bound --account-proof inputs");
  }
  if (block_boc_mode && mc_proof_path.empty()) {
    return td::Status::Error("--block-boc replay requires --mc-proof");
  }
  if (!mc_proof_path.empty() && (!mc_archive.empty() || !mc_state_path.empty())) {
    return td::Status::Error("--mc-proof cannot be mixed with --mc-archive or --mc-state");
  }
  if (!library_body_paths.empty() && mc_proof_path.empty()) {
    return td::Status::Error("--library-bodies is only valid with --mc-proof");
  }
  std::unique_ptr<LoadedState> mc_state;
  if (!mc_proof_path.empty()) {
    TRY_RESULT(loaded, load_config_proof(mc_proof_path, target.mc_id));
    mc_state = std::make_unique<LoadedState>(std::move(loaded));
  } else {
    if (mc_archive.empty() || mc_state_path.empty()) {
      return td::Status::Error("replay requires --mc-proof or both --mc-archive and --mc-state");
    }
    TRY_RESULT(loaded, load_state_boc(mc_state_path, target.mc_id, "masterchain state"));
    TRY_STATUS(verify_masterchain_state(mc_archive, target.mc_id, loaded));
    mc_state = std::make_unique<LoadedState>(std::move(loaded));
  }

  if (!account_proof_specs.empty() && (!prev_state_path.empty() || !account_part_specs.empty())) {
    return td::Status::Error("--account-proof cannot be mixed with --prev-state or --account-part");
  }
  std::unique_ptr<LoadedState> prev_state;
  std::vector<LoadedAccountPart> account_parts;
  std::vector<LoadedAccountProof> account_proofs;
  if (!account_proof_specs.empty()) {
    std::set<StdSmcAddress> seen;
    account_proofs.reserve(account_proof_specs.size());
    for (const auto& spec : account_proof_specs) {
      TRY_RESULT(proof, load_account_proof(spec, target));
      if (!seen.insert(proof.address).second) {
        return td::Status::Error(PSLICE() << "duplicate account proof for " << proof.address.to_hex());
      }
      TRY_STATUS(verify_account_proof_base(archive, history, target, proof));
      account_proofs.push_back(std::move(proof));
    }
  } else {
    if (prev_state_path.empty()) {
      return td::Status::Error("replay requires --account-proof or --prev-state");
    }
    TRY_RESULT(loaded, load_state_boc_unchecked(prev_state_path, "account state"));
    TRY_STATUS(verify_account_state_base(archive, target, loaded));
    prev_state = std::make_unique<LoadedState>(std::move(loaded));
    account_parts.reserve(account_part_specs.size());
    for (const auto& spec : account_part_specs) {
      TRY_RESULT(part, load_account_part(spec, *prev_state));
      account_parts.push_back(std::move(part));
    }
  }
  std::unique_ptr<LoadedLibraryBodies> library_bodies;
  if (!library_body_paths.empty()) {
    TRY_RESULT(loaded, load_library_bodies(library_body_paths));
    library_bodies = std::make_unique<LoadedLibraryBodies>(std::move(loaded));
  }
  std::unique_ptr<LoadedState> block_update_witness_base_state;
  if (!block_update_witness_base_state_path.empty()) {
    TRY_RESULT(loaded,
               load_state_boc_unchecked(block_update_witness_base_state_path, "target-update witness base state"));
    block_update_witness_base_state = std::make_unique<LoadedState>(std::move(loaded));
  }
  Ref<vm::Cell> out_msg_queue_state_proof;
  if (!out_msg_queue_proof_path.empty()) {
    TRY_RESULT(proof, load_out_msg_queue_proof(out_msg_queue_proof_path, target.prev[0]));
    out_msg_queue_state_proof = std::move(proof);
  }
  if (use_block_update_witness) {
    TRY_RESULT(witness, build_target_update_predecessor_witness(archive, history, target, account_proofs,
                                                                block_update_witness_base_state.get(),
                                                                out_msg_queue_state_proof));
    predecessor_state_witness = std::move(witness);
  }
  ReplayAccountArtifacts serial_artifacts;
  TRY_RESULT(replay, replay_transactions(archive, history, target, predecessor_state_witness, prev_state.get(),
                                         *mc_state, account_parts, account_proofs, library_bodies.get(),
                                         profile_ed25519, true, ReplayExecutionMode::full, &serial_artifacts, nullptr,
                                         predecessor_state_witness_source));
  std::optional<ParallelAccountReplayProbe> parallel_probe;
  if (account_workers != 0) {
    if (prev_state != nullptr || !account_parts.empty() || account_proofs.empty()) {
      return td::Status::Error("--account-workers requires complete --account-proof replay mode");
    }
    TRY_RESULT(probe, run_parallel_account_replay_probe(archive, history, target, *mc_state, account_proofs,
                                                        library_bodies.get(), profile_ed25519, replay, account_workers,
                                                        account_samples));
    parallel_probe = std::move(probe);
  }
  std::optional<OfflineCollatorReplayProbe> offline_collator_probe;
  if (offline_collator_workers != 0) {
    if (prev_state != nullptr || !account_parts.empty() || account_proofs.empty()) {
      return td::Status::Error("--offline-collator-workers requires complete --account-proof replay mode");
    }
    TRY_RESULT(probe, run_offline_collator_replay(archive, history, target, predecessor_state_witness,
                                                  predecessor_state_witness_source, *mc_state, account_proofs,
                                                  library_bodies.get(), profile_ed25519, replay,
                                                  std::move(serial_artifacts), offline_collator_workers));
    offline_collator_probe = std::move(probe);
  }
  return replay_json(target, prev_state.get(), account_proofs, *mc_state, library_bodies.get(), replay, profile_ed25519,
                     parallel_probe ? &parallel_probe.value() : nullptr,
                     offline_collator_probe ? &offline_collator_probe.value() : nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(verbosity_ERROR);
  std::string archive;
  std::string block_boc;
  std::string derive_block_boc_id_path;
  std::string export_block_boc_path;
  std::string mc_archive;
  std::string block_id;
  std::string prev_state;
  std::string mc_state;
  std::string mc_proof;
  std::string collated_data;
  std::string block_update_witness_base_state;
  std::string out_msg_queue_proof;
  std::vector<std::string> history_block_bocs;
  std::vector<std::string> library_bodies;
  std::string inspect_state;
  std::vector<std::string> account_parts;
  std::vector<std::string> account_proofs;
  int split_depth = 4;
  bool inspect = false;
  bool list_blocks = false;
  bool profile_ed25519 = false;
  bool use_block_update_witness = false;
  std::size_t account_workers = 0;
  std::size_t account_samples = 1;
  bool account_samples_explicit = false;
  std::size_t offline_collator_workers = 0;

  td::OptionParser options;
  options.set_description(
      "Inspect or transaction-replay one basechain block from a closed TON archive or verified block BOC");
  options.add_option('a', "archive", "closed archive .pack file", [&](td::Slice value) { archive = value.str(); });
  options.add_option(0, "block-boc", "raw block BOC; requires a full --block-id and predecessor-bound proofs",
                     [&](td::Slice value) { block_boc = value.str(); });
  options.add_option(0, "derive-block-boc-id",
                     "derive a block id from one BOC for diagnostics; the result is not an external trust anchor",
                     [&](td::Slice value) { derive_block_boc_id_path = value.str(); });
  options.add_option(0, "history-block-boc",
                     "intermediate raw block BOC linking an older account proof to the predecessor; may be repeated",
                     [&](td::Slice value) { history_block_bocs.push_back(value.str()); });
  options.add_option(0, "export-block-boc", "write one verified archive block to a new owner-only file",
                     [&](td::Slice value) { export_block_boc_path = value.str(); });
  options.add_option(0, "mc-archive", "closed masterchain archive .pack file",
                     [&](td::Slice value) { mc_archive = value.str(); });
  options.add_option('b', "block-id", "short id for --archive; full id for --block-boc",
                     [&](td::Slice value) { block_id = value.str(); });
  options.add_option('p', "prev-state", "predecessor ShardStateUnsplit BOC",
                     [&](td::Slice value) { prev_state = value.str(); });
  options.add_option('m', "mc-state", "referenced masterchain ShardStateUnsplit BOC",
                     [&](td::Slice value) { mc_state = value.str(); });
  options.add_option(0, "mc-proof", "verified liteServer.configInfo bundle from saveconfigproof",
                     [&](td::Slice value) { mc_proof = value.str(); });
  options.add_option(0, "collated-data", "raw block-candidate collated-data BOC with predecessor-state witness",
                     [&](td::Slice value) { collated_data = value.str(); });
  options.add_option(0, "block-update-witness",
                     "offline only: use the finalized target MerkleUpdate old view as a hindsight predecessor witness",
                     [&]() { use_block_update_witness = true; });
  options.add_option(0, "block-update-witness-base-state",
                     "sparse ancestor state advanced through --history-block-boc files for the hindsight witness",
                     [&](td::Slice value) { block_update_witness_base_state = value.str(); });
  options.add_option(0, "out-msg-queue-proof",
                     "verified predecessor liteServer.blockOutMsgQueueSize response for the hindsight witness",
                     [&](td::Slice value) { out_msg_queue_proof = value.str(); });
  options.add_option(0, "library-bodies",
                     "content-hash-checked liteServer.libraryResult from savelibraries; may be repeated",
                     [&](td::Slice value) { library_bodies.push_back(value.str()); });
  options.add_option('s', "account-part", "split-state account part as HEX_PREFIX=PATH; may be repeated",
                     [&](td::Slice value) { account_parts.push_back(value.str()); });
  options.add_option(0, "account-proof", "verified account proof as ACCOUNT_HEX=PATH; may be repeated",
                     [&](td::Slice value) { account_proofs.push_back(value.str()); });
  options.add_checked_option(
      'd', "split-depth", "account prefix depth for --inspect (default: 4)", [&](td::Slice value) {
        TRY_RESULT_ASSIGN(split_depth, td::to_integer_safe<int>(value));
        if (split_depth <= 0 || split_depth > kMaxHexSplitDepth || split_depth % 4 != 0) {
          return td::Status::Error("split depth must be a positive multiple of 4 and at most 60");
        }
        return td::Status::OK();
      });
  options.add_option('i', "inspect", "print exact state ids required by the selected block", [&]() { inspect = true; });
  options.add_option(0, "list-blocks", "list verified block files and their masterchain references in --archive",
                     [&]() { list_blocks = true; });
  options.add_option(0, "inspect-state", "inspect a whole-state BOC or split-state Merkle header",
                     [&](td::Slice value) { inspect_state = value.str(); });
  options.add_option(0, "profile-ed25519", "time Ed25519 verification during offline transaction replay",
                     [&]() { profile_ed25519 = true; });
  options.add_checked_option(0, "account-workers", "run the isolated account-proof replay probe with 1..64 workers",
                             [&](td::Slice value) {
                               TRY_RESULT_ASSIGN(account_workers, td::to_integer_safe<std::size_t>(value));
                               if (account_workers == 0 || account_workers > 64) {
                                 return td::Status::Error("account worker count must be between 1 and 64");
                               }
                               return td::Status::OK();
                             });
  options.add_checked_option(0, "account-samples",
                             "odd paired serial/parallel sample count for --account-workers (1..31)",
                             [&](td::Slice value) {
                               account_samples_explicit = true;
                               TRY_RESULT_ASSIGN(account_samples, td::to_integer_safe<std::size_t>(value));
                               if (account_samples == 0 || account_samples > 31 || account_samples % 2 == 0) {
                                 return td::Status::Error("account sample count must be odd and between 1 and 31");
                               }
                               return td::Status::OK();
                             });
  options.add_checked_option(0, "offline-collator-workers",
                             "run opt-in immutable account execution plus one serial shadow commit with 1..64 workers",
                             [&](td::Slice value) {
                               TRY_RESULT_ASSIGN(offline_collator_workers, td::to_integer_safe<std::size_t>(value));
                               if (offline_collator_workers == 0 || offline_collator_workers > 64) {
                                 return td::Status::Error("offline collator worker count must be between 1 and 64");
                               }
                               return td::Status::OK();
                             });
  options.add_option('h', "help", "print help", [&]() {
    char buffer[16384];
    td::StringBuilder out(td::MutableSlice{buffer, sizeof(buffer)});
    out << options;
    std::cout << out.as_cslice().str();
    std::_Exit(0);
  });

  auto parse_status = options.run(argc, argv);
  if (parse_status.is_error()) {
    std::cerr << "Error: " << parse_status.move_as_error().to_string() << '\n';
    return 1;
  }
  const bool has_replay_inputs =
      !mc_archive.empty() || !prev_state.empty() || !mc_state.empty() || !mc_proof.empty() || !collated_data.empty() ||
      !library_bodies.empty() || !account_parts.empty() || !account_proofs.empty() || !history_block_bocs.empty() ||
      profile_ed25519 || use_block_update_witness || !block_update_witness_base_state.empty() ||
      !out_msg_queue_proof.empty() || account_workers != 0 || account_samples_explicit || offline_collator_workers != 0;
  if (account_samples_explicit && account_workers == 0) {
    std::cerr << "Error: --account-samples requires --account-workers\n";
    return 1;
  }
  if (use_block_update_witness && !collated_data.empty()) {
    std::cerr << "Error: --block-update-witness cannot be combined with --collated-data\n";
    return 1;
  }
  if (!block_update_witness_base_state.empty() && !use_block_update_witness) {
    std::cerr << "Error: --block-update-witness-base-state requires --block-update-witness\n";
    return 1;
  }
  if (!out_msg_queue_proof.empty() && !use_block_update_witness) {
    std::cerr << "Error: --out-msg-queue-proof requires --block-update-witness\n";
    return 1;
  }
  if (!derive_block_boc_id_path.empty()) {
    if (!archive.empty() || !block_boc.empty() || !export_block_boc_path.empty() || !block_id.empty() ||
        !inspect_state.empty() || list_blocks || inspect || split_depth != 4 || has_replay_inputs) {
      std::cerr << "Error: --derive-block-boc-id cannot be combined with other modes or options\n";
      return 1;
    }
  } else if (!inspect_state.empty()) {
    if (!archive.empty() || !block_boc.empty() || !export_block_boc_path.empty() || !block_id.empty() || list_blocks ||
        inspect || has_replay_inputs) {
      std::cerr << "Error: --inspect-state cannot be combined with block-source, replay, or block-inspection options\n";
      return 1;
    }
  } else if (list_blocks) {
    if (archive.empty()) {
      std::cerr << "Error: --list-blocks requires --archive\n";
      return 1;
    }
    if (!block_boc.empty() || !export_block_boc_path.empty() || !block_id.empty() || inspect || split_depth != 4 ||
        has_replay_inputs) {
      std::cerr << "Error: --list-blocks cannot be combined with block replay or inspection options\n";
      return 1;
    }
  } else if (!export_block_boc_path.empty()) {
    if (archive.empty() || block_id.empty()) {
      std::cerr << "Error: --export-block-boc requires --archive and a short --block-id\n";
      return 1;
    }
    if (!block_boc.empty() || inspect || split_depth != 4 || has_replay_inputs) {
      std::cerr << "Error: --export-block-boc cannot be combined with replay or inspection options\n";
      return 1;
    }
  } else {
    if (block_id.empty()) {
      std::cerr << "Error: --block-id is required\n";
      return 1;
    }
    if (archive.empty() == block_boc.empty()) {
      std::cerr << "Error: select exactly one block source with --archive or --block-boc\n";
      return 1;
    }
    if (!history_block_bocs.empty() && block_boc.empty()) {
      std::cerr << "Error: --history-block-boc is only valid with --block-boc\n";
      return 1;
    }
    if (inspect && has_replay_inputs) {
      std::cerr << "Error: --inspect cannot be combined with replay inputs\n";
      return 1;
    }
  }

  try {
    td::Result<std::string> result = td::Status::Error("uninitialized mode");
    if (!derive_block_boc_id_path.empty()) {
      result = derive_block_boc_id_json(derive_block_boc_id_path);
    } else if (list_blocks) {
      result = list_archive_blocks(archive);
    } else if (!export_block_boc_path.empty()) {
      auto requested_id = BlockId::from_str(block_id);
      if (requested_id.is_error()) {
        result = requested_id.move_as_error();
      } else {
        result = export_block_boc(archive, requested_id.move_as_ok(), export_block_boc_path);
      }
    } else if (!inspect_state.empty()) {
      auto state = load_state_boc_unchecked(inspect_state, "state");
      if (state.is_error()) {
        result = state.move_as_error();
      } else {
        result = inspect_state_json(state.move_as_ok(), split_depth);
      }
    } else {
      result = run(archive, block_boc, mc_archive, block_id, inspect, collated_data, use_block_update_witness,
                   block_update_witness_base_state, out_msg_queue_proof, prev_state, mc_state, mc_proof,
                   history_block_bocs, library_bodies, account_parts, account_proofs, split_depth, profile_ed25519,
                   account_workers, account_samples, offline_collator_workers);
    }
    if (result.is_error()) {
      std::cerr << "Error: " << result.move_as_error().to_string() << '\n';
      return 2;
    }
    std::cout << result.move_as_ok() << '\n';
    return 0;
  } catch (const vm::VmError& error) {
    std::cerr << "Error: VM error: " << error.get_msg() << '\n';
  } catch (const vm::VmVirtError& error) {
    std::cerr << "Error: VM virtualization error: " << error.get_msg() << '\n';
  } catch (const vm::CellBuilder::CellCreateError&) {
    std::cerr << "Error: cell creation failed\n";
  } catch (const vm::CellBuilder::CellWriteError&) {
    std::cerr << "Error: cell write failed\n";
  }
  return 2;
}
