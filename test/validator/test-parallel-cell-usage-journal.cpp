// SPDX-License-Identifier: LGPL-2.0-or-later

#include "impl/parallel-cell-usage-journal.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/DataCell.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/UsageCell.h"

namespace ton::validator::parallel_inbound::test {
namespace {

td::Ref<vm::Cell> make_cell(td::uint64 value, std::vector<td::Ref<vm::Cell>> refs = {}) {
  vm::CellBuilder builder;
  builder.store_long(value, 64);
  for (auto& ref : refs) {
    builder.store_ref(std::move(ref));
  }
  return builder.finalize_novm();
}

td::Ref<vm::Cell> make_tree() {
  auto left_leaf = make_cell(11);
  auto right_leaf = make_cell(22);
  auto left = make_cell(1, {left_leaf});
  auto right = make_cell(2, {right_leaf});
  return make_cell(0, {left, right});
}

bool load_path(td::Ref<vm::Cell> current, const std::vector<unsigned>& path) {
  for (auto ref_id : path) {
    auto loaded = current->load_cell();
    if (loaded.is_error() || ref_id >= loaded.ok().data_cell->get_refs_cnt()) {
      return false;
    }
    current = vm::UsageCell::create(loaded.ok().data_cell->get_ref(ref_id),
                                    loaded.ok().tree_node.create_child(ref_id));
  }
  return current->load_cell().is_ok();
}

td::Ref<vm::Cell> proof_for(const td::Ref<vm::Cell>& root, const std::shared_ptr<vm::CellUsageTree>& tree) {
  return vm::MerkleProof::generate(root, tree.get()).move_as_ok();
}

CellUsageJournal record_journal(const td::Ref<vm::Cell>& root, const std::vector<std::vector<unsigned>>& paths) {
  CellUsageJournal journal{root->get_hash().as_bits256()};
  auto tree = std::make_shared<vm::CellUsageTree>();
  bool callback_ok = true;
  tree->set_cell_load_path_callback([&](const vm::LoadedCell& loaded) {
    if (journal.record(loaded).is_error()) {
      callback_ok = false;
    }
  });
  auto usage_root = vm::UsageCell::create(root, tree->root_ptr());
  for (const auto& path : paths) {
    ASSERT_TRUE(load_path(usage_root, path));
  }
  ASSERT_TRUE(callback_ok);
  ASSERT_TRUE(journal.validate().is_ok());
  return journal;
}

TEST(ParallelCellUsageJournal, ReplaysTheSameMerkleProofAsSerialLoads) {
  const auto root = make_tree();
  auto serial_tree = std::make_shared<vm::CellUsageTree>();
  auto serial_root = vm::UsageCell::create(root, serial_tree->root_ptr());
  ASSERT_TRUE(load_path(serial_root, {0, 0}));
  ASSERT_TRUE(load_path(serial_root, {1}));

  const auto journal = record_journal(root, {{0, 0}, {1}});
  auto coordinator_tree = std::make_shared<vm::CellUsageTree>();
  ASSERT_TRUE(journal.replay_into(root, coordinator_tree).is_ok());

  ASSERT_EQ(proof_for(root, serial_tree)->get_hash(), proof_for(root, coordinator_tree)->get_hash());
  ASSERT_EQ(journal.commitment(), record_journal(root, {{0, 0}, {1}}).commitment());
}

TEST(ParallelCellUsageJournal, KeepsTheLegacyLoadCallbackContractUnchanged) {
  const auto root = make_tree();
  auto tree = std::make_shared<vm::CellUsageTree>();
  bool legacy_called = false;
  bool path_called = false;
  tree->set_cell_load_callback([&](const vm::LoadedCell& loaded) {
    legacy_called = true;
    ASSERT_TRUE(loaded.tree_node.empty());
  });
  tree->set_cell_load_path_callback([&](const vm::LoadedCell& loaded) {
    path_called = true;
    ASSERT_TRUE(!loaded.tree_node.empty());
    ASSERT_TRUE(loaded.tree_node.path().has_value());
  });

  ASSERT_TRUE(load_path(vm::UsageCell::create(root, tree->root_ptr()), {}));
  ASSERT_TRUE(legacy_called);
  ASSERT_TRUE(path_called);
}

TEST(ParallelCellUsageJournal, UnionIsIndependentOfWorkerArrivalOrder) {
  const auto root = make_tree();
  const auto left = record_journal(root, {{0, 0}});
  const auto right = record_journal(root, {{1, 0}});

  auto left_then_right = std::make_shared<vm::CellUsageTree>();
  ASSERT_TRUE(left.replay_into(root, left_then_right).is_ok());
  ASSERT_TRUE(right.replay_into(root, left_then_right).is_ok());

  auto right_then_left = std::make_shared<vm::CellUsageTree>();
  ASSERT_TRUE(right.replay_into(root, right_then_left).is_ok());
  ASSERT_TRUE(left.replay_into(root, right_then_left).is_ok());

  ASSERT_EQ(proof_for(root, left_then_right)->get_hash(), proof_for(root, right_then_left)->get_hash());
}

TEST(ParallelCellUsageJournal, RepeatedFlushIsIdempotentAndLaterLoadsExtendTheProof) {
  const auto root = make_tree();
  CellUsageJournal journal{root->get_hash().as_bits256()};
  auto worker_tree = std::make_shared<vm::CellUsageTree>();
  bool recording_ok = true;
  worker_tree->set_cell_load_path_callback([&](const vm::LoadedCell& loaded) {
    if (journal.record(loaded).is_error()) {
      recording_ok = false;
    }
  });
  auto worker_root = vm::UsageCell::create(root, worker_tree->root_ptr());
  ASSERT_TRUE(load_path(worker_root, {0, 0}));
  ASSERT_TRUE(recording_ok);
  ASSERT_TRUE(journal.validate().is_ok());

  auto coordinator_tree = std::make_shared<vm::CellUsageTree>();
  std::size_t coordinator_loads = 0;
  coordinator_tree->set_cell_load_callback([&](const vm::LoadedCell&) { ++coordinator_loads; });
  ASSERT_TRUE(journal.replay_into(root, coordinator_tree).is_ok());
  const auto first_flush_loads = coordinator_loads;
  ASSERT_TRUE(first_flush_loads > 0u);

  ASSERT_TRUE(journal.replay_into(root, coordinator_tree).is_ok());
  ASSERT_EQ(coordinator_loads, first_flush_loads);

  ASSERT_TRUE(load_path(worker_root, {1, 0}));
  ASSERT_TRUE(recording_ok);
  ASSERT_TRUE(journal.validate().is_ok());
  ASSERT_TRUE(journal.replay_into(root, coordinator_tree).is_ok());
  ASSERT_TRUE(coordinator_loads > first_flush_loads);

  auto serial_tree = std::make_shared<vm::CellUsageTree>();
  auto serial_root = vm::UsageCell::create(root, serial_tree->root_ptr());
  ASSERT_TRUE(load_path(serial_root, {0, 0}));
  ASSERT_TRUE(load_path(serial_root, {1, 0}));
  ASSERT_EQ(proof_for(root, coordinator_tree)->get_hash(), proof_for(root, serial_tree)->get_hash());
}

TEST(ParallelCellUsageJournal, ReplaysBelowAnExistingCoordinatorAnchor) {
  const auto root = make_tree();
  const auto root_data = root->load_cell().move_as_ok().data_cell;
  const td::Ref<vm::Cell> subtree = root_data->get_ref(1);

  auto serial_tree = std::make_shared<vm::CellUsageTree>();
  auto serial_root = vm::UsageCell::create(root, serial_tree->root_ptr());
  ASSERT_TRUE(load_path(serial_root, {1, 0}));

  const auto journal = record_journal(subtree, {{0}});
  auto coordinator_tree = std::make_shared<vm::CellUsageTree>();
  ASSERT_TRUE(load_path(vm::UsageCell::create(root, coordinator_tree->root_ptr()), {}));
  ASSERT_TRUE(journal.replay_into(subtree, coordinator_tree->root_ptr().create_child(1)).is_ok());

  ASSERT_EQ(proof_for(root, serial_tree)->get_hash(), proof_for(root, coordinator_tree)->get_hash());
}

TEST(ParallelCellUsageJournal, RejectsWrongAnchorPathCellAndDuplicate) {
  const auto root = make_tree();
  const auto valid = record_journal(root, {{0, 0}});
  auto coordinator_tree = std::make_shared<vm::CellUsageTree>();

  CellUsageJournal wrong_anchor{td::Bits256::zero(), valid.entries()};
  ASSERT_TRUE(wrong_anchor.replay_into(root, coordinator_tree).is_error());

  auto mutable_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_root = vm::UsageCell::create(root, mutable_tree->root_ptr());
  ASSERT_TRUE(valid.replay_into(usage_root, coordinator_tree).is_error());

  auto malformed_entries = valid.entries();
  malformed_entries.front().ref_path.push_back(vm::CellTraits::max_refs);
  CellUsageJournal wrong_path{root->get_hash().as_bits256(), malformed_entries};
  ASSERT_TRUE(wrong_path.validate().is_error());

  malformed_entries = valid.entries();
  malformed_entries.front().cell_hash = td::Bits256::zero();
  CellUsageJournal wrong_cell{root->get_hash().as_bits256(), malformed_entries};
  ASSERT_TRUE(wrong_cell.replay_into(root, coordinator_tree).is_error());

  malformed_entries = valid.entries();
  malformed_entries.push_back(malformed_entries.front());
  CellUsageJournal duplicate{root->get_hash().as_bits256(), malformed_entries};
  ASSERT_TRUE(duplicate.validate().is_error());
}

TEST(ParallelCellUsageJournal, RejectionDoesNotPartiallyMutateCoordinatorTree) {
  const auto root = make_tree();
  const auto valid = record_journal(root, {{0}, {1}});
  auto malformed_entries = valid.entries();
  malformed_entries.back().cell_hash = td::Bits256::zero();
  CellUsageJournal malformed{root->get_hash().as_bits256(), malformed_entries};

  auto coordinator_tree = std::make_shared<vm::CellUsageTree>();
  ASSERT_TRUE(malformed.replay_into(root, coordinator_tree).is_error());
  auto empty_tree = std::make_shared<vm::CellUsageTree>();
  ASSERT_EQ(proof_for(root, coordinator_tree)->get_hash(), proof_for(root, empty_tree)->get_hash());
}

}  // namespace
}  // namespace ton::validator::parallel_inbound::test
