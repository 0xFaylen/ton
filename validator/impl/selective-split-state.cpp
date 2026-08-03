// SPDX-License-Identifier: LGPL-2.0-or-later

#include "selective-split-state.h"

#include <algorithm>
#include <limits>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/MerkleProof.h"
#include "vm/dict.h"

namespace ton::validator {
namespace {

constexpr td::uint32 kMaxEnumeratedPrefixBits = 20;

RootHash root_hash(const td::Ref<vm::Cell>& cell, int level = vm::Cell::max_level) {
  return RootHash{cell->get_hash(level).bits()};
}

td::Result<td::Ref<vm::Cell>> unpack_merkle_proof(td::Ref<vm::Cell> wrapped) {
  if (wrapped.is_null()) {
    return td::Status::Error("Split state header is null");
  }
  if (wrapped->get_level() != 0) {
    return td::Status::Error("Split state header MerkleProof must have level zero");
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(wrapped)};
  if (cs.special_type() != vm::Cell::SpecialType::MerkleProof || cs.size_refs() != 1) {
    return td::Status::Error("Split state header is not a MerkleProof");
  }
  return cs.prefetch_ref();
}

td::Result<block::gen::ShardStateUnsplit::Record> unpack_state(const td::Ref<vm::Cell>& root) {
  block::gen::ShardStateUnsplit::Record state;
  auto cs = vm::load_cell_slice(root);
  if (!block::gen::t_ShardStateUnsplit.unpack(cs, state) || !cs.empty_ext()) {
    return td::Status::Error("Cannot deserialize ShardStateUnsplit");
  }
  return state;
}

td::Result<td::Ref<vm::Cell>> pack_state(const block::gen::ShardStateUnsplit::Record& state) {
  vm::CellBuilder cb;
  if (!block::gen::t_ShardStateUnsplit.pack(cb, state)) {
    return td::Status::Error("Cannot serialize ShardStateUnsplit");
  }
  return cb.finalize();
}

struct ReplaceResult {
  td::Ref<vm::Cell> root;
  td::uint32 replacements{};
};

td::Result<ReplaceResult> replace_subtree(const td::Ref<vm::Cell>& current,
                                          const td::Ref<vm::Cell>& replacement) {
  if (current.is_null() || replacement.is_null()) {
    return td::Status::Error("Cannot replace a null split-state subtree");
  }
  if (current->get_hash(0) == replacement->get_hash(0) && current->get_depth(0) == replacement->get_depth(0)) {
    return ReplaceResult{replacement, 1};
  }
  // A level-zero subtree is complete: it cannot contain any pruned proof
  // boundary. This also prevents later materializations from rescanning
  // account parts that were already inserted and may be gigabytes large.
  if (current->get_level() == 0) {
    return ReplaceResult{current, 0};
  }

  vm::CellSlice cs{vm::NoVm{}, current};
  if (cs.is_special() && cs.special_type() == vm::Cell::SpecialType::PrunnedBranch) {
    return ReplaceResult{current, 0};
  }

  std::vector<td::Ref<vm::Cell>> refs;
  refs.reserve(cs.size_refs());
  td::uint32 replacements = 0;
  for (unsigned i = 0; i < cs.size_refs(); ++i) {
    TRY_RESULT(child, replace_subtree(cs.prefetch_ref(i), replacement));
    if (child.replacements > std::numeric_limits<td::uint32>::max() - replacements) {
      return td::Status::Error("Split-state subtree replacement count overflow");
    }
    replacements += child.replacements;
    refs.push_back(std::move(child.root));
  }

  if (replacements == 0) {
    return ReplaceResult{current, 0};
  }

  vm::CellBuilder cb;
  if (!cb.store_bits_bool(cs.fetch_bits(cs.size()))) {
    return td::Status::Error("Cannot copy split-state proof cell bits");
  }
  for (auto& ref : refs) {
    if (!cb.store_ref_bool(std::move(ref))) {
      return td::Status::Error("Cannot copy split-state proof cell reference");
    }
  }
  return ReplaceResult{cb.finalize(cs.is_special()), replacements};
}

td::BitArray<64> shard_prefix(ShardId shard) {
  td::BitArray<64> result;
  result.store_ulong(shard);
  return result;
}

}  // namespace

SelectiveSplitStateAssembler::SelectiveSplitStateAssembler(ShardIdFull shard, RootHash expected_state_root,
                                                           td::uint32 split_depth)
    : shard_(shard), expected_state_root_(expected_state_root), split_depth_(split_depth) {
}

td::Result<std::unique_ptr<SelectiveSplitStateAssembler>> SelectiveSplitStateAssembler::create(
    ShardIdFull shard, RootHash expected_state_root, td::Ref<vm::Cell> wrapped_header, td::uint32 split_depth) {
  auto result = std::unique_ptr<SelectiveSplitStateAssembler>(
      new SelectiveSplitStateAssembler(shard, expected_state_root, split_depth));
  TRY_STATUS(result->initialize(std::move(wrapped_header)));
  return std::move(result);
}

td::Status SelectiveSplitStateAssembler::initialize(td::Ref<vm::Cell> wrapped_header) {
  if (!shard_.is_valid_ext()) {
    return td::Status::Error("Invalid split-state shard");
  }
  const int shard_prefix_length = shard_pfx_len(shard_.shard);
  if (split_depth_ > 63 || shard_prefix_length >= static_cast<int>(split_depth_)) {
    return td::Status::Error("Invalid split-state depth for shard");
  }
  const td::uint32 enumerated_prefix_bits = split_depth_ - shard_prefix_length;
  if (enumerated_prefix_bits > kMaxEnumeratedPrefixBits) {
    return td::Status::Error("Split-state header has too many effective account prefixes for replay");
  }

  try {
    TRY_RESULT(raw_header, unpack_merkle_proof(wrapped_header));
    TRY_RESULT(virtual_header, vm::MerkleProof::virtualize(std::move(wrapped_header)));
    if (root_hash(virtual_header) != expected_state_root_) {
      return td::Status::Error("Hash mismatch in split-state header");
    }

    TRY_RESULT(virtual_state, unpack_state(virtual_header));
    block::ShardId embedded_shard;
    auto embedded_shard_cs = virtual_state.shard_id.write();
    if (!embedded_shard.deserialize(embedded_shard_cs) || !embedded_shard_cs.empty_ext() ||
        embedded_shard.workchain_id != shard_.workchain || embedded_shard.shard_pfx != shard_.shard) {
      return td::Status::Error("Shard mismatch in split-state header");
    }

    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(virtual_state.accounts), 256,
                                     block::tlb::aug_ShardAccounts, false};

    const td::uint64 part_count = td::uint64{1} << enumerated_prefix_bits;
    ShardId effective_shard =
        shard_.shard ^ (1ULL << (63 - shard_prefix_length)) ^ (1ULL << (63 - split_depth_));
    const ShardId increment = 1ULL << (64 - split_depth_);
    parts_.reserve(static_cast<std::size_t>(part_count));
    for (td::uint64 i = 0; i < part_count; ++i, effective_shard += increment) {
      auto prefix = shard_prefix(effective_shard);
      auto part = accounts;
      if (!part.cut_prefix_subdict(prefix.bits(), split_depth_)) {
        return td::Status::Error("Cannot inspect account prefix in split-state header");
      }
      if (!part.is_empty()) {
        auto wrapped_root = part.get_wrapped_dict_root();
        parts_.push_back({effective_shard, root_hash(wrapped_root)});
      }
    }

    auto header_without_accounts = virtual_state;
    header_without_accounts.accounts = vm::DataCell::create("", 0, {}, false).move_as_ok();
    TRY_RESULT(repacked_without_accounts, pack_state(header_without_accounts));
    if (repacked_without_accounts->is_virtualized()) {
      return td::Status::Error("Split-state header is pruned outside of accounts dictionary");
    }

    TRY_RESULT(raw_state, unpack_state(raw_header));
    static_cast<void>(raw_state);
    raw_state_root_ = std::move(raw_header);
    if (root_hash(raw_state_root_, 0) != expected_state_root_) {
      return td::Status::Error("Raw split-state header does not commit to expected state root");
    }
    return td::Status::OK();
  } catch (const vm::VmError& e) {
    return e.as_status().clone();
  } catch (const vm::VmVirtError&) {
    return td::Status::Error("Insufficient cells in split-state header");
  }
}

td::Status SelectiveSplitStateAssembler::materialize(ShardId effective_shard, td::Ref<vm::Cell> wrapped_part) {
  const auto descriptor = std::find_if(parts_.begin(), parts_.end(), [&](const auto& part) {
    return part.effective_shard == effective_shard;
  });
  if (descriptor == parts_.end()) {
    return td::Status::Error("Unexpected effective shard for split-state account part");
  }
  if (wrapped_part.is_null() || root_hash(wrapped_part) != descriptor->wrapped_root_hash) {
    return td::Status::Error("Hash mismatch in split-state account part");
  }

  try {
    TRY_RESULT(current_state, unpack_state(raw_state_root_));
    TRY_RESULT(virtual_current_root,
               vm::MerkleProof::virtualize(vm::CellBuilder::create_merkle_proof(raw_state_root_)));
    TRY_RESULT(virtual_current_state, unpack_state(virtual_current_root));

    auto prefix = shard_prefix(effective_shard);
    vm::AugmentedDictionary header_accounts{vm::load_cell_slice_ref(virtual_current_state.accounts), 256,
                                            block::tlb::aug_ShardAccounts, false};
    if (!header_accounts.cut_prefix_subdict(prefix.bits(), split_depth_, true) || header_accounts.is_empty()) {
      return td::Status::Error("Account prefix is absent from split-state header");
    }

    vm::AugmentedDictionary part_accounts{vm::load_cell_slice_ref(wrapped_part), 256,
                                          block::tlb::aug_ShardAccounts, false};
    if (!part_accounts.cut_prefix_subdict(prefix.bits(), split_depth_, true) || part_accounts.is_empty()) {
      return td::Status::Error("Account prefix is absent from split-state account part");
    }

    auto expected_subtree = header_accounts.get_root_cell();
    auto replacement_subtree = part_accounts.get_root_cell();
    if (expected_subtree.is_null() || replacement_subtree.is_null() ||
        expected_subtree->get_hash(0) != replacement_subtree->get_hash(0) ||
        expected_subtree->get_depth(0) != replacement_subtree->get_depth(0)) {
      return td::Status::Error("Split-state account part does not match header prefix commitment");
    }

    TRY_RESULT(replaced, replace_subtree(current_state.accounts, replacement_subtree));
    if (replaced.replacements != 1) {
      return td::Status::Error("Split-state account prefix did not match exactly one proof subtree");
    }
    current_state.accounts = std::move(replaced.root);
    TRY_RESULT(candidate_state_root, pack_state(current_state));
    if (root_hash(candidate_state_root, 0) != expected_state_root_) {
      return td::Status::Error("Materialized split state changed the expected state root");
    }
    raw_state_root_ = std::move(candidate_state_root);
    return td::Status::OK();
  } catch (const vm::VmError& e) {
    return e.as_status().clone();
  } catch (const vm::VmVirtError&) {
    return td::Status::Error("Insufficient cells while materializing split-state account prefix");
  }
}

td::Result<td::Ref<vm::Cell>> SelectiveSplitStateAssembler::virtualized_state_root() const {
  if (raw_state_root_.is_null() || root_hash(raw_state_root_, 0) != expected_state_root_) {
    return td::Status::Error("Selective split state has no valid root");
  }
  auto wrapped = vm::CellBuilder::create_merkle_proof(raw_state_root_);
  TRY_RESULT(root, vm::MerkleProof::virtualize(std::move(wrapped)));
  if (root_hash(root) != expected_state_root_) {
    return td::Status::Error("Virtualized selective split state changed the expected root");
  }
  return root;
}

}  // namespace ton::validator
