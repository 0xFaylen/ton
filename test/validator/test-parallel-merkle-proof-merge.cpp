// SPDX-License-Identifier: LGPL-2.0-or-later

#include <cstdint>
#include <memory>
#include <vector>

#include "impl/parallel-merkle-proof-merge.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/CellUsageTree.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/UsageCell.h"

namespace ton::validator::parallel_inbound::test {
namespace {

td::Ref<vm::Cell> build_tree(unsigned depth, std::uint32_t prefix = 0) {
  if (depth == 0) {
    return vm::CellBuilder().store_long(prefix, 32).finalize();
  }
  return vm::CellBuilder()
      .store_ref(build_tree(depth - 1, prefix << 1))
      .store_ref(build_tree(depth - 1, (prefix << 1) | 1))
      .finalize();
}

td::Ref<vm::Cell> proof_for_path(const td::Ref<vm::Cell>& root, unsigned depth, std::uint32_t path) {
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_root = vm::UsageCell::create(root, usage_tree->root_ptr());
  auto current = usage_root;
  for (unsigned level = 0; level < depth; ++level) {
    auto slice = vm::load_cell_slice(current);
    current = slice.prefetch_ref((path >> (depth - level - 1)) & 1);
  }
  vm::load_cell_slice(current).prefetch_long(32);
  return vm::MerkleProof::generate(usage_root, usage_tree.get()).move_as_ok();
}

std::uint32_t read_path(const td::Ref<vm::Cell>& root, unsigned depth, std::uint32_t path) {
  auto current = root;
  for (unsigned level = 0; level < depth; ++level) {
    auto slice = vm::load_cell_slice(current);
    current = slice.prefetch_ref((path >> (depth - level - 1)) & 1);
  }
  return static_cast<std::uint32_t>(vm::load_cell_slice(current).fetch_ulong(32));
}

TEST(ParallelMerkleProofMerge, MatchesSlowMergeAndExposesEveryPath) {
  constexpr unsigned depth = 8;
  auto root = build_tree(depth);
  std::vector<td::Ref<vm::Cell>> proofs;
  std::vector<std::uint32_t> paths;
  for (std::uint32_t path = 3; path < (1U << depth); path += 17) {
    paths.push_back(path);
    proofs.push_back(proof_for_path(root, depth, path));
  }

  td::Ref<vm::Cell> slow;
  for (const auto& proof : proofs) {
    slow = slow.is_null() ? proof : vm::MerkleProof::combine(std::move(slow), proof).move_as_ok();
  }
  auto fast = merge_merkle_proofs_fast(proofs).move_as_ok();
  auto repeated = merge_merkle_proofs_fast(proofs).move_as_ok();
  ASSERT_EQ(fast->get_hash(), repeated->get_hash());

  auto slow_root = vm::MerkleProof::virtualize(slow).move_as_ok();
  auto fast_root = vm::MerkleProof::virtualize(fast).move_as_ok();
  ASSERT_EQ(root->get_hash(), slow_root->get_hash());
  ASSERT_EQ(slow_root->get_hash(), fast_root->get_hash());
  for (auto path : paths) {
    ASSERT_EQ(path, read_path(fast_root, depth, path));
  }
}

TEST(ParallelMerkleProofMerge, RejectsInvalidSets) {
  ASSERT_TRUE(merge_merkle_proofs_fast({}).is_error());
  ASSERT_TRUE(merge_merkle_proofs_fast({td::Ref<vm::Cell>{}}).is_error());
  ASSERT_TRUE(merge_merkle_proofs_fast({vm::CellBuilder().store_long(1, 1).finalize()}).is_error());

  constexpr unsigned depth = 3;
  auto first = proof_for_path(build_tree(depth, 0), depth, 1);
  auto second = proof_for_path(build_tree(depth, 1), depth, 2);
  ASSERT_TRUE(merge_merkle_proofs_fast({first, second}).is_error());
}

}  // namespace
}  // namespace ton::validator::parallel_inbound::test
