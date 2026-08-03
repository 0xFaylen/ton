// SPDX-License-Identifier: LGPL-2.0-or-later

#include <array>
#include <iostream>
#include <map>
#include <memory>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "impl/selective-split-state.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellUsageTree.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/UsageCell.h"
#include "vm/dict.h"

namespace ton::validator::test {
namespace {

constexpr td::uint32 kSplitDepth = 4;

struct SyntheticSplitState {
  td::Ref<vm::Cell> state_root;
  td::Ref<vm::Cell> wrapped_header;
  std::map<ShardId, td::Ref<vm::Cell>> parts;
  std::array<td::Bits256, 16> accounts;
};

ShardId effective_shard(unsigned prefix) {
  return (static_cast<ShardId>(prefix) << (64 - kSplitDepth)) | (1ULL << (63 - kSplitDepth));
}

td::Ref<vm::Cell> account_none() {
  return vm::CellBuilder().store_zeroes(1).finalize_novm();
}

td::Ref<vm::Cell> simple_account(const td::Bits256& account_id, td::uint64 last_transaction_lt) {
  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_ones_bool(1));
  ASSERT_TRUE(builder.store_long_bool(2, 2));   // addr_std$10
  ASSERT_TRUE(builder.store_zeroes_bool(1));   // no anycast
  ASSERT_TRUE(builder.store_long_bool(0, 8));  // basechain
  ASSERT_TRUE(builder.store_bits_bool(account_id.cbits(), 256));
  ASSERT_TRUE(builder.store_zeroes_bool(6));   // empty StorageUsed
  ASSERT_TRUE(builder.store_zeroes_bool(3));   // no StorageExtraInfo
  ASSERT_TRUE(builder.store_zeroes_bool(32));  // last_paid
  ASSERT_TRUE(builder.store_zeroes_bool(1));   // no due_payment
  ASSERT_TRUE(builder.store_long_bool(last_transaction_lt, 64));
  ASSERT_TRUE(builder.store_zeroes_bool(5));  // empty CurrencyCollection
  ASSERT_TRUE(builder.store_zeroes_bool(2));  // account_uninit$00
  auto result = builder.finalize_novm();
  ASSERT_TRUE(block::gen::t_Account.validate_ref(result));
  return result;
}

td::Ref<vm::Cell> build_state(std::array<td::Bits256, 16>& account_ids) {
  vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
  for (unsigned prefix = 0; prefix < account_ids.size(); ++prefix) {
    auto& account_id = account_ids[prefix];
    account_id.as_slice()[0] = static_cast<char>(prefix << 4);
    account_id.as_slice()[31] = static_cast<char>(prefix + 1);

    auto add_account = [&](const td::Bits256& id, td::uint64 lt) {
      vm::CellBuilder leaf;
      ASSERT_TRUE(leaf.store_ref_bool(simple_account(id, lt)));
      ASSERT_TRUE(leaf.store_zeroes_bool(256 + 64));
      ASSERT_TRUE(accounts.set_builder(id.bits(), 256, leaf, vm::Dictionary::SetMode::Add));
    };
    add_account(account_id, prefix + 1);

    td::Bits256 sibling_id;
    sibling_id.as_slice()[0] = static_cast<char>((prefix << 4) | 0x08);
    sibling_id.as_slice()[31] = static_cast<char>(prefix + 33);
    add_account(sibling_id, prefix + 33);
  }

  vm::CellBuilder accounts_builder;
  ASSERT_TRUE(accounts.append_dict_to_bool(accounts_builder));
  auto accounts_root = accounts_builder.finalize();

  vm::CellBuilder out_msg_queue_builder;
  ASSERT_TRUE(out_msg_queue_builder.store_zeroes_bool(1 + 64 + 2));

  vm::CellBuilder aux_builder;
  ASSERT_TRUE(aux_builder.store_zeroes_bool(128));
  ASSERT_TRUE(block::tlb::t_CurrencyCollection.null_value(aux_builder));
  ASSERT_TRUE(block::tlb::t_CurrencyCollection.null_value(aux_builder));
  ASSERT_TRUE(aux_builder.store_zeroes_bool(1));  // empty libraries
  ASSERT_TRUE(aux_builder.store_zeroes_bool(1));  // no master reference

  vm::CellBuilder state_builder;
  ASSERT_TRUE(state_builder.store_long_bool(0x9023afe2, 32));
  ASSERT_TRUE(state_builder.store_long_bool(-239, 32));
  ASSERT_TRUE((block::ShardId{basechainId, shardIdAll}.serialize(state_builder)));
  ASSERT_TRUE(state_builder.store_zeroes_bool(32 + 32 + 32 + 64 + 32));
  ASSERT_TRUE(state_builder.store_ref_bool(out_msg_queue_builder.finalize()));
  ASSERT_TRUE(state_builder.store_zeroes_bool(1));
  ASSERT_TRUE(state_builder.store_ref_bool(std::move(accounts_root)));
  ASSERT_TRUE(state_builder.store_ref_bool(aux_builder.finalize()));
  ASSERT_TRUE(state_builder.store_zeroes_bool(1));  // no masterchain custom state
  auto result = state_builder.finalize();
  ASSERT_TRUE(block::gen::t_ShardState.validate_ref(result));
  ASSERT_TRUE(block::tlb::t_ShardState.validate_ref(result));
  return result;
}

SyntheticSplitState split_state() {
  SyntheticSplitState result;
  result.state_root = build_state(result.accounts);

  block::gen::ShardStateUnsplit::Record state;
  ASSERT_TRUE(block::gen::t_ShardStateUnsplit.cell_unpack(result.state_root, state));

  auto unwrapped_accounts_root = state.accounts;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto accounts_root = vm::UsageCell::create(unwrapped_accounts_root, usage_tree->root_ptr());
  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(accounts_root), 256, block::tlb::aug_ShardAccounts, false};

  for (unsigned prefix = 0; prefix < 16; ++prefix) {
    td::BitArray<64> key;
    key.store_ulong(effective_shard(prefix));
    auto part = accounts;
    ASSERT_TRUE(part.cut_prefix_subdict(key.bits(), kSplitDepth));
    ASSERT_TRUE(!part.is_empty());
    result.parts.emplace(effective_shard(prefix), part.get_wrapped_dict_root());
  }

  auto accounts_proof = vm::MerkleProof::generate_raw(unwrapped_accounts_root, usage_tree.get()).move_as_ok();
  state.accounts = std::move(accounts_proof);
  vm::CellBuilder header_builder;
  ASSERT_TRUE(block::gen::t_ShardStateUnsplit.pack(header_builder, state));
  auto raw_header = header_builder.finalize();
  ASSERT_EQ(raw_header->get_hash(0), result.state_root->get_hash());
  result.wrapped_header = vm::CellBuilder::create_merkle_proof(std::move(raw_header));
  return result;
}

bool account_is_readable(const td::Ref<vm::Cell>& state_root, const td::Bits256& account_id) {
  block::gen::ShardStateUnsplit::Record state;
  if (!block::gen::t_ShardStateUnsplit.cell_unpack(state_root, state)) {
    return false;
  }
  vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(state.accounts), 256, block::tlb::aug_ShardAccounts, false};
  return accounts.lookup(account_id.bits(), 256).not_null();
}

bool account_lookup_fails_closed(const td::Ref<vm::Cell>& state_root, const td::Bits256& account_id) {
  try {
    return !account_is_readable(state_root, account_id);
  } catch (const vm::VmError&) {
    return true;
  } catch (const vm::VmVirtError&) {
    return true;
  }
}

TEST(SelectiveSplitState, MaterializesOnlySelectedPrefixesAndPreservesRoot) {
  SyntheticSplitState split;
  try {
    split = split_state();
  } catch (const vm::VmError& e) {
    std::cerr << "synthetic split failed: " << e.as_status().to_string() << std::endl;
    ASSERT_TRUE(false);
    return;
  } catch (const vm::VmVirtError&) {
    std::cerr << "synthetic split failed with VmVirtError" << std::endl;
    ASSERT_TRUE(false);
    return;
  }
  auto assembler = SelectiveSplitStateAssembler::create(
                       ShardIdFull{basechainId, shardIdAll}, RootHash{split.state_root->get_hash().bits()},
                       split.wrapped_header, kSplitDepth)
                       .move_as_ok();
  ASSERT_EQ(assembler->parts().size(), 16u);

  auto initial = assembler->virtualized_state_root().move_as_ok();
  ASSERT_EQ(initial->get_hash(), split.state_root->get_hash());
  ASSERT_TRUE(account_lookup_fails_closed(initial, split.accounts[3]));

  ASSERT_TRUE(assembler->materialize(effective_shard(3), split.parts.at(effective_shard(3))).is_ok());
  auto one_prefix = assembler->virtualized_state_root().move_as_ok();
  ASSERT_EQ(one_prefix->get_hash(), split.state_root->get_hash());
  ASSERT_TRUE(account_is_readable(one_prefix, split.accounts[3]));
  ASSERT_TRUE(account_lookup_fails_closed(one_prefix, split.accounts[4]));

  ASSERT_TRUE(assembler->materialize(effective_shard(11), split.parts.at(effective_shard(11))).is_ok());
  ASSERT_TRUE(assembler->materialize(effective_shard(3), split.parts.at(effective_shard(3))).is_ok());
  auto two_prefixes = assembler->virtualized_state_root().move_as_ok();
  ASSERT_EQ(two_prefixes->get_hash(), split.state_root->get_hash());
  ASSERT_TRUE(account_is_readable(two_prefixes, split.accounts[3]));
  ASSERT_TRUE(account_is_readable(two_prefixes, split.accounts[11]));
  ASSERT_TRUE(account_lookup_fails_closed(two_prefixes, split.accounts[4]));
}

TEST(SelectiveSplitState, RejectsWrongCommitmentsAndParts) {
  auto split = split_state();
  auto wrong_root = RootHash{account_none()->get_hash().bits()};
  ASSERT_TRUE(SelectiveSplitStateAssembler::create(ShardIdFull{basechainId, shardIdAll}, wrong_root,
                                                   split.wrapped_header, kSplitDepth)
                  .is_error());
  ASSERT_TRUE(SelectiveSplitStateAssembler::create(ShardIdFull{masterchainId, shardIdAll},
                                                   RootHash{split.state_root->get_hash().bits()}, split.wrapped_header,
                                                   kSplitDepth)
                  .is_error());
  ASSERT_TRUE(SelectiveSplitStateAssembler::create(ShardIdFull{basechainId, shardIdAll},
                                                   RootHash{split.state_root->get_hash().bits()}, split.wrapped_header,
                                                   64)
                  .is_error());
  ASSERT_TRUE(SelectiveSplitStateAssembler::create(ShardIdFull{basechainId, shardIdAll},
                                                   RootHash{split.state_root->get_hash().bits()}, split.wrapped_header,
                                                   8)
                  .is_error());

  auto assembler = SelectiveSplitStateAssembler::create(
                       ShardIdFull{basechainId, shardIdAll}, RootHash{split.state_root->get_hash().bits()},
                       split.wrapped_header, kSplitDepth)
                       .move_as_ok();
  ASSERT_TRUE(assembler->materialize(effective_shard(3), split.parts.at(effective_shard(4))).is_error());
  ASSERT_TRUE(assembler->materialize(shardIdAll, split.parts.at(effective_shard(3))).is_error());
  ASSERT_TRUE(assembler->materialize(effective_shard(3), {}).is_error());
}

}  // namespace
}  // namespace ton::validator::test
