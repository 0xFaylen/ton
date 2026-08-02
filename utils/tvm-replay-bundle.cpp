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
#include <map>
#include <memory>
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
#include "ton/lite-tl.hpp"
#include "validator/db/fileref.hpp"
#include "validator/db/package.hpp"
#include "validator/impl/parallel-coordinator-shadow.h"
#include "validator/impl/parallel-inbound-scheduler.h"
#include "validator/impl/parallel-transaction-payload.h"
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
using ton::validator::parallel_inbound::CanonicalTransactionPayload;
using ton::validator::parallel_inbound::CanonicalTransactionEffects;
using ton::validator::parallel_inbound::CoordinatorCommitContext;
using ton::validator::parallel_inbound::Hash256;
using ton::validator::parallel_inbound::InboundDescriptorContext;
using ton::validator::parallel_inbound::OutboundQueueKey;
using ton::validator::parallel_inbound::WorkItem;
using ton::validator::parallel_inbound::inspect_transaction_payload;

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
  int global_id = 0;
  bool after_split = false;
  bool after_merge = false;
  bool before_split = false;
  Ref<vm::Cell> in_msg_descr;
  Ref<vm::Cell> out_msg_descr;
  Ref<vm::Cell> account_blocks;
  Ref<vm::Cell> state_update;
};

struct LoadedState {
  std::shared_ptr<vm::StaticBagOfCellsDb> boc;
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

struct ReplayResult {
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
  std::size_t canonical_payloads_validated = 0;
  std::size_t canonical_payload_out_messages = 0;
  std::size_t canonical_outbound_registrations = 0;
  std::size_t canonical_inbound_fin_descriptors = 0;
  std::size_t canonical_outbound_deq_imm_descriptors = 0;
  std::size_t canonical_fee_augmentations_validated = 0;
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
  bool shard_accounts_predecessor_root_bound = false;
  std::size_t augmented_dictionary_roots_validated = 0;
  TvmHotpathStats hotpaths;
  std::map<StdSmcAddress, AccountWork> account_work;

  ReplayResult() {
    hotpaths.enable_exact();
  }
};

td::Result<std::pair<BlockIdExt, Ref<vm::Cell>>> load_block_from_archive(const std::string& archive,
                                                                         const BlockId& requested_id) {
  TRY_RESULT(package, ton::Package::open(archive, true, false));
  BlockIdExt found_id;
  Ref<vm::Cell> found_root;
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
          if (td::sha256_bits256(data) != block_ref.block_id.file_hash) {
            scan_status = td::Status::Error("requested block file hash does not match its archive filename");
            return;
          }
          auto root = vm::std_boc_deserialize(data.as_slice());
          if (root.is_error()) {
            scan_status = root.move_as_error_prefix("cannot deserialize requested block: ");
            return;
          }
          found_root = root.move_as_ok();
          if (td::Bits256(found_root->get_hash().bits()) != block_ref.block_id.root_hash) {
            scan_status = td::Status::Error("requested block root hash does not match its archive filename");
            found_root.clear();
            return;
          }
          found_id = block_ref.block_id;
        },
        [&](const auto&) {}));
    return scan_status.is_ok();
  }));

  TRY_STATUS(std::move(scan_status));
  if (found_root.is_null()) {
    return td::Status::Error(PSTRING() << "block " << requested_id.to_str() << " was not found in archive");
  }
  return std::make_pair(found_id, found_root);
}

td::Result<BlockContext> unpack_block_context(std::pair<BlockIdExt, Ref<vm::Cell>> block_data) {
  BlockContext result;
  result.id = block_data.first;
  result.root = std::move(block_data.second);
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
  result.after_merge = info.after_merge;
  result.before_split = info.before_split;
  result.rand_seed = extra.rand_seed;
  result.in_msg_descr = std::move(extra.in_msg_descr);
  result.out_msg_descr = std::move(extra.out_msg_descr);
  result.account_blocks = std::move(extra.account_blocks);
  result.state_update = std::move(block_record.state_update);
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
  return LoadedState{std::move(boc), std::move(root), std::move(state), actual_id, split_header};
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
  return LoadedState{nullptr, std::move(root), std::move(state), state_id, false};
}

td::Result<LoadedAccountProof> load_account_proof(const std::string& spec, const BlockIdExt& mc_id) {
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
  TRY_RESULT(info, proof.validate(mc_id, block::StdAddress(ton::basechainId, address)));
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

td::Status verify_account_proof_base(const std::string& archive, const BlockContext& target,
                                     const LoadedAccountProof& proof) {
  if (proof.shard_block.shard_full() != target.id.shard_full()) {
    return td::Status::Error("account proof shard does not match target block shard");
  }
  if (proof.shard_block.seqno() > target.prev[0].seqno()) {
    return td::Status::Error("account proof is newer than target predecessor");
  }

  BlockIdExt current = proof.shard_block;
  for (ton::BlockSeqno seqno = proof.shard_block.seqno() + 1; seqno <= target.prev[0].seqno(); ++seqno) {
    BlockId intermediate_id{target.id.shard_full(), seqno};
    TRY_RESULT(intermediate_data, load_block_from_archive(archive, intermediate_id));
    TRY_RESULT(intermediate, unpack_block_context(std::move(intermediate_data)));
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
  if (state.fetch_ulong(32) != 0x9023afe2U || !state.advance(328) || !state.advance_refs(1) ||
      !state.advance(1)) {
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

td::Result<std::pair<Ref<vm::Cell>, Ref<vm::Cell>>> extract_state_update_raw_views(
    const BlockContext& block_context) {
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

td::Result<Ref<vm::Cell>> build_predecessor_accounts_proof(const std::string& archive,
                                                           const BlockContext& target,
                                                           const std::vector<LoadedAccountProof>& proofs) {
  if (proofs.empty()) {
    return td::Status::Error("cannot build ShardAccounts proof without account proofs");
  }
  const auto base_block = proofs.front().shard_block;
  Ref<vm::Cell> combined;
  for (const auto& proof : proofs) {
    if (proof.shard_block != base_block || proof.state_proof.is_null()) {
      return td::Status::Error("account proofs do not share one shard-state root");
    }
    if (combined.is_null()) {
      combined = proof.state_proof;
    } else {
      TRY_RESULT(next, vm::MerkleProof::combine(std::move(combined), proof.state_proof));
      combined = std::move(next);
    }
  }

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
    BlockId intermediate_id{target.id.shard_full(), seqno};
    TRY_RESULT(intermediate_data, load_block_from_archive(archive, intermediate_id));
    TRY_RESULT(intermediate, unpack_block_context(std::move(intermediate_data)));
    if (intermediate.prev.size() != 1 || intermediate.prev[0] != current) {
      return td::Status::Error(PSLICE() << "non-linear account-proof history at " << intermediate.id.to_str());
    }
    TRY_RESULT(raw_views, extract_state_update_raw_views(intermediate));
    TRY_RESULT(old_accounts, extract_partial_shard_accounts_root(raw_views.first));
    TRY_RESULT(new_accounts, extract_partial_shard_accounts_root(raw_views.second));
    if (accounts_root->get_hash(0) != old_accounts->get_hash(0)) {
      return td::Status::Error("combined account proof does not match an intermediate ShardAccounts root");
    }
    TRY_RESULT(next_accounts,
               vm::MerkleUpdate::apply_raw(accounts_root, old_accounts, new_accounts, 0, 0));
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

td::Status collect_library_refs(Ref<vm::Cell> cell, std::set<vm::Cell::Hash>& visited, std::set<td::Bits256>& libraries,
                                int depth = 1024) {
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

td::Result<ReplayResult> replay_transactions(const std::string& archive, const BlockContext& target,
                                             const LoadedState* prev_state,
                                             const LoadedState& mc_state,
                                             const std::vector<LoadedAccountPart>& account_parts,
                                             const std::vector<LoadedAccountProof>& account_proofs,
                                             const LoadedLibraryBodies* library_bodies, bool profile_ed25519) {
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

  ReplayResult result;
  std::vector<CanonicalTransactionEffects> canonical_limit_effects;
  std::vector<ton::validator::parallel_inbound::BasechainLimitContext> canonical_limit_contexts;
  std::vector<ShadowCoordinatorCandidate> shadow_coordinator_candidates;
  std::vector<ton::validator::parallel_inbound::AccountDictionaryDelta> account_dictionary_deltas;
  td::Status replay_status = td::Status::OK();
  std::set<td::Bits256> missing_libraries;
  bool accounts_ok = account_blocks.check_for_each_extra([&](Ref<vm::CellSlice> account_block_slice, Ref<vm::CellSlice>,
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
    bool transactions_ok = transactions.check_for_each_extra(
        [&](Ref<vm::CellSlice> transaction_slice, Ref<vm::CellSlice>, td::ConstBitPtr tx_key, int tx_key_len) {
          if (tx_key_len != 64) {
            replay_status = td::Status::Error("invalid transaction key length");
            return false;
          }
          auto transaction = transaction_slice->prefetch_ref();
          if (transaction.is_null()) {
            replay_status = td::Status::Error("transaction dictionary contains a null transaction");
            return false;
          }
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
            replay_status =
                td::Status::Error(PSTRING() << "canonical PSAE payload validation failed for transaction "
                                            << tx_key.get_uint(64) << " of " << address.to_hex() << ": "
                                            << ton::validator::parallel_inbound::to_string(payload_result.error));
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
              if (envelope_cell.is_null() || !block::tlb::unpack_cell(envelope_cell, envelope) ||
                  envelope.msg.is_null() || envelope.msg->get_hash() != payload_effects.inbound_message->get_hash()) {
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
                  replay_status = td::Status::Error(PSTRING() << "cannot derive outbound queue key for "
                                                              << address.to_hex() << " at " << tx_key.get_uint(64));
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
          result.canonical_outbound_registrations += registrations.batch->messages.size();
          ++result.canonical_payloads_validated;
          result.canonical_payload_out_messages += payload_effects.outbound_messages.size();
          derived_account_fees += payload_effects.total_fees;
          if (!derived_account_fees.is_valid()) {
            replay_status =
                td::Status::Error(PSTRING() << "canonical fee accumulation failed for " << address.to_hex());
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
                             .outbound_registration = {.metadata_enabled =
                                                           config->has_capability(ton::capMsgMetadata),
                                                       .metadata = outbound_metadata},
                             .inbound_descriptor = coordinator_inbound_descriptor,
                             .outbound_queue_deletion = coordinator_queue_deletion},
                 .pre_account_state = pre_account_state});
          }
          first_account_transaction = false;
          ++result.transactions;
          auto& account_work = result.account_work[address];
          ++account_work.transactions;
          account_work.transaction_seconds += emulated.elapsed_time;
          if (emulated.vm.executed) {
            ++result.tvm_transactions;
            account_work.tvm_seconds += emulated.vm.time.real;
            result.hotpaths.record(emulated.vm.code_hash, target.id.id.workchain, address, emulated.vm.time,
                                   emulated.vm.vm_gas_used, emulated.vm.billed_gas_used, emulated.vm.vm_steps,
                                   emulated.vm.ed25519_verifications, emulated.vm.ed25519_time);
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
      account_dictionary_deltas.push_back(
          {.account = as_hash256(address),
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
  block::BlockLimits shadow_limits;
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

  std::sort(shadow_coordinator_candidates.begin(), shadow_coordinator_candidates.end(),
            [](const auto& left, const auto& right) { return left.work.key < right.work.key; });
  block::BlockLimits coordinator_limits;
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
        candidate.effects.account,
        ton::validator::parallel_inbound::ShadowAccountState{
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
    return td::Status::Error(PSTRING()
                             << "canonical atomic shadow coordinator failed at item "
                             << (coordinator_result.item_index ? td::to_string(coordinator_result.item_index.value())
                                                               : std::string("none"))
                             << ": "
                             << ton::validator::parallel_inbound::to_string(coordinator_result.error)
                             << ", outbound="
                             << ton::validator::parallel_inbound::to_string(coordinator_result.outbound_error)
                             << ", descriptor="
                             << ton::validator::parallel_inbound::to_string(coordinator_result.descriptor_error)
                             << ", limits="
                             << ton::validator::parallel_inbound::to_string(coordinator_result.limit_error));
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

  if (result.skipped_accounts == 0 && !account_proofs.empty()) {
    std::string root_stage = "extract_update_views";
    try {
      TRY_RESULT(update_views, extract_state_update_views(target));
      root_stage = "extract_predecessor_accounts_root";
      TRY_RESULT(target_predecessor_accounts_root, extract_partial_shard_accounts_root(update_views.first));
      root_stage = "build_predecessor_accounts_proof";
      TRY_RESULT(predecessor_accounts_root, build_predecessor_accounts_proof(archive, target, account_proofs));
      if (predecessor_accounts_root->get_hash() != target_predecessor_accounts_root->get_hash()) {
        return td::Status::Error("combined account proof disagrees with target Merkle-update predecessor root");
      }
      result.shard_accounts_predecessor_root_bound = true;

      root_stage = "strip_target_descriptors";
      block::tlb::Aug_InMsgDescr in_augmentation{config->get_global_version()};
      block::tlb::Aug_OutMsgDescr out_augmentation{config->get_global_version()};
      vm::AugmentedDictionary in_baseline{vm::load_cell_slice_ref(target.in_msg_descr), 256, in_augmentation};
      vm::AugmentedDictionary out_baseline{vm::load_cell_slice_ref(target.out_msg_descr), 256, out_augmentation};
      for (const auto& [hash, descriptor] : coordinator_state.in_msg_descriptors) {
        auto removed = in_baseline.lookup_delete(as_dictionary_key(hash));
        if (removed.is_null()) {
          return td::Status::Error("canonical InMsg descriptor is absent from target root");
        }
        TRY_RESULT(removed_cell, serialize_slice(std::move(removed)));
        if (removed_cell->get_hash() != descriptor->get_hash()) {
          return td::Status::Error("canonical InMsg descriptor value disagrees with target root");
        }
      }
      for (const auto& [hash, descriptor] : coordinator_state.out_msg_descriptors) {
        auto removed = out_baseline.lookup_delete(as_dictionary_key(hash));
        if (removed.is_null()) {
          return td::Status::Error("canonical OutMsg descriptor is absent from target root");
        }
        TRY_RESULT(removed_cell, serialize_slice(std::move(removed)));
        if (removed_cell->get_hash() != descriptor->get_hash()) {
          return td::Status::Error("canonical OutMsg descriptor value disagrees with target root");
        }
      }

      std::vector<ton::validator::parallel_inbound::QueueDictionaryDeletion> queue_deletions;

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

      root_stage = "apply_augmented_deltas";
      vm::AugmentedDictionary queue_out_of_scope{352, block::tlb::aug_OutMsgQueue};
      ton::validator::parallel_inbound::AugmentedDictionarySeed seed{
          .global_version = config->get_global_version(),
          .shard_accounts_root = predecessor_accounts.get_wrapped_dict_root(),
          .in_msg_descr_root = in_baseline.get_wrapped_dict_root(),
          .out_msg_descr_root = out_baseline.get_wrapped_dict_root(),
          .out_msg_queue_root = queue_out_of_scope.get_wrapped_dict_root()};
      const std::vector<ton::validator::parallel_inbound::AccountDictionaryDelta> account_deltas_out_of_scope;
      auto root_result = ton::validator::parallel_inbound::apply_augmented_dictionary_deltas_atomic(
          seed, account_deltas_out_of_scope, coordinator_state.in_msg_descriptors,
          coordinator_state.out_msg_descriptors, queue_deletions);
      if (!root_result) {
        return td::Status::Error(
            PSTRING() << "canonical augmented dictionary commit failed at item "
                      << (root_result.item_index ? td::to_string(root_result.item_index.value()) : std::string("none"))
                      << ": " << ton::validator::parallel_inbound::to_string(root_result.error));
      }
      const auto& roots = root_result.roots.value();
      if (roots.in_msg_descr_root->get_hash() != target.in_msg_descr->get_hash() ||
          roots.out_msg_descr_root->get_hash() != target.out_msg_descr->get_hash()) {
        return td::Status::Error("canonical augmented dictionary roots disagree with copied block artifacts");
      }
      result.augmented_dictionary_roots_validated = 2;
    } catch (vm::VmVirtError& error) {
      return td::Status::Error(PSTRING() << "augmented root gate virtualization error at " << root_stage << ": "
                                         << error.get_msg());
    } catch (vm::VmError& error) {
      return td::Status::Error(PSTRING() << "augmented root gate VM error at " << root_stage << ": "
                                         << error.get_msg());
    }
  }
  return result;
}

td::Result<std::string> inspect_json(const BlockContext& target, int split_depth) {
  TRY_RESULT(prefixes, collect_account_prefixes(target, split_depth));
  TRY_RESULT(accounts, collect_accounts(target));
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"inspect\",\"block_id\":\"" << target.id.to_str()
      << "\",\"global_id\":" << target.global_id << ",\"gen_utime\":" << target.gen_utime
      << ",\"after_split\":" << target.after_split << ",\"after_merge\":" << target.after_merge
      << ",\"before_split\":" << target.before_split << ",\"predecessors\":[";
  for (std::size_t i = 0; i < target.prev.size(); ++i) {
    if (i != 0) {
      out << ",";
    }
    out << "\"" << target.prev[i].to_str() << "\"";
  }
  out << "],\"masterchain_ref\":\"" << target.mc_id.to_str() << "\",\"split_depth\":" << split_depth
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

std::string inspect_state_json(const LoadedState& state) {
  td::StringBuilder out;
  out << "{\"schema_version\":1,\"mode\":\"inspect_state\",\"state_id\":\"" << state.id.to_str()
      << "\",\"global_id\":" << state.record.global_id << ",\"gen_utime\":" << state.record.gen_utime
      << ",\"gen_lt\":" << state.record.gen_lt << ",\"root_hash\":\"" << state.root->get_hash().to_hex()
      << "\",\"split_header\":" << state.split_header << "}";
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

std::string replay_json(const BlockContext& target, const LoadedState* account_state,
                        const std::vector<LoadedAccountProof>& account_proofs, const LoadedState& mc_state,
                        const LoadedLibraryBodies* library_bodies, const ReplayResult& replay, bool profile_ed25519) {
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
      << ",\"tvm_transactions\":" << replay.tvm_transactions << ",\"ed25519_profiled\":" << profile_ed25519
      << ",\"equivalence\":\"transaction_hash_and_account_state_hash\""
      << ",\"psae_payload_validation\":{\"canonical_payloads\":" << replay.canonical_payloads_validated
      << ",\"ordered_out_messages\":" << replay.canonical_payload_out_messages
      << ",\"outbound_registrations\":" << replay.canonical_outbound_registrations
      << ",\"inbound_fin_descriptors\":" << replay.canonical_inbound_fin_descriptors
      << ",\"outbound_deq_imm_descriptors\":" << replay.canonical_outbound_deq_imm_descriptors
      << ",\"fee_augmentations\":" << replay.canonical_fee_augmentations_validated
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
      << ",\"shard_account_proof_values_bound\":" << replay.shard_account_proof_values_bound
      << ",\"shard_accounts_predecessor_root_bound\":"
      << (replay.shard_accounts_predecessor_root_bound ? "true" : "false")
      << ",\"augmented_dictionary_roots_validated\":" << replay.augmented_dictionary_roots_validated
      << ",\"augmented_dictionary_root_scope\":\"InMsgDescr_OutMsgDescr\""
      << ",\"augmented_dictionary_baseline_source\":\"target_minus_validated_deltas\""
      << ",\"augmented_dictionary_historical_transition_proven\":false"
      << ",\"shard_accounts_root_status\":\"requires_full_predecessor_or_proof_aware_augmentation_merge\""
      << ",\"out_msg_queue_root_status\":\"requires_predecessor_queue_value_proof\""
      << ",\"block_size_status\":\"not_claimed_without_collator_usage_tree\""
      << ",\"proof_journals\":\"empty_in_transaction_replay\""
      << ",\"processed_upto\":\"atomic_shadow_prefix_applied_for_inbound_fin_subset\""
      << ",\"global_effects\":\"atomic_shadow_applied_not_live_collator_dictionaries\"}"
      << ",\"hotpaths_wall\":" << replay.hotpaths.to_json(false, 0, replay.hotpaths.size());
#if TD_WINDOWS
  out << ",\"hotpaths_cpu\":null,\"cpu_metric_status\":\"unsupported_windows_timer_resolution\"";
#else
  out << ",\"hotpaths_cpu\":" << replay.hotpaths.to_json(true, 0, replay.hotpaths.size())
      << ",\"cpu_metric_status\":\"available\"";
#endif
  out << ",\"account_lane_ceiling\":" << account_lane_ceiling_json(replay) << "}";
  return out.as_cslice().str();
}

td::Result<std::string> run(const std::string& archive, const std::string& mc_archive, const std::string& block_id_text,
                            bool inspect, const std::string& prev_state_path, const std::string& mc_state_path,
                            const std::string& mc_proof_path, const std::vector<std::string>& library_body_paths,
                            const std::vector<std::string>& account_part_specs,
                            const std::vector<std::string>& account_proof_specs, int split_depth,
                            bool profile_ed25519) {
  TRY_RESULT(requested_id, BlockId::from_str(block_id_text));
  TRY_RESULT(block_data, load_block_from_archive(archive, requested_id));
  TRY_RESULT(target, unpack_block_context(std::move(block_data)));
  if (inspect) {
    return inspect_json(target, split_depth);
  }

  TRY_STATUS(verify_replay_scope(target));
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
      TRY_RESULT(proof, load_account_proof(spec, target.mc_id));
      if (!seen.insert(proof.address).second) {
        return td::Status::Error(PSLICE() << "duplicate account proof for " << proof.address.to_hex());
      }
      TRY_STATUS(verify_account_proof_base(archive, target, proof));
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
  TRY_RESULT(replay, replay_transactions(archive, target, prev_state.get(), *mc_state, account_parts, account_proofs,
                                         library_bodies.get(), profile_ed25519));
  return replay_json(target, prev_state.get(), account_proofs, *mc_state, library_bodies.get(), replay,
                     profile_ed25519);
}

}  // namespace

int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(verbosity_ERROR);
  std::string archive;
  std::string mc_archive;
  std::string block_id;
  std::string prev_state;
  std::string mc_state;
  std::string mc_proof;
  std::vector<std::string> library_bodies;
  std::string inspect_state;
  std::vector<std::string> account_parts;
  std::vector<std::string> account_proofs;
  int split_depth = 4;
  bool inspect = false;
  bool profile_ed25519 = false;

  td::OptionParser options;
  options.set_description(
      "Inspect or transaction-replay one basechain block from a closed TON archive using state BOCs or lite proofs");
  options.add_option('a', "archive", "closed archive .pack file", [&](td::Slice value) { archive = value.str(); });
  options.add_option(0, "mc-archive", "closed masterchain archive .pack file",
                     [&](td::Slice value) { mc_archive = value.str(); });
  options.add_option('b', "block-id", "short block id: (workchain,shard,seqno)",
                     [&](td::Slice value) { block_id = value.str(); });
  options.add_option('p', "prev-state", "predecessor ShardStateUnsplit BOC",
                     [&](td::Slice value) { prev_state = value.str(); });
  options.add_option('m', "mc-state", "referenced masterchain ShardStateUnsplit BOC",
                     [&](td::Slice value) { mc_state = value.str(); });
  options.add_option(0, "mc-proof", "verified liteServer.configInfo bundle from saveconfigproof",
                     [&](td::Slice value) { mc_proof = value.str(); });
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
  options.add_option(0, "inspect-state", "inspect a whole-state BOC or split-state Merkle header",
                     [&](td::Slice value) { inspect_state = value.str(); });
  options.add_option(0, "profile-ed25519", "time Ed25519 verification during offline transaction replay",
                     [&]() { profile_ed25519 = true; });
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
  if (inspect_state.empty() && (archive.empty() || block_id.empty())) {
    std::cerr << "Error: --archive and --block-id are required\n";
    return 1;
  }

  try {
    td::Result<std::string> result = td::Status::Error("uninitialized mode");
    if (!inspect_state.empty()) {
      auto state = load_state_boc_unchecked(inspect_state, "state");
      if (state.is_error()) {
        result = state.move_as_error();
      } else {
        result = inspect_state_json(state.move_as_ok());
      }
    } else {
      result = run(archive, mc_archive, block_id, inspect, prev_state, mc_state, mc_proof, library_bodies,
                   account_parts, account_proofs, split_depth, profile_ed25519);
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
