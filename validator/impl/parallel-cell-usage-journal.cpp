// SPDX-License-Identifier: LGPL-2.0-or-later

#include <array>
#include <set>

#include "td/utils/crypto.h"
#include "vm/cells/DataCell.h"

#include "parallel-cell-usage-journal.h"

namespace ton::validator::parallel_inbound {
namespace {

std::array<td::uint8, 8> encode_u64(td::uint64 value) {
  std::array<td::uint8, 8> encoded{};
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    encoded[encoded.size() - i - 1] = static_cast<td::uint8>(value >> (i * 8));
  }
  return encoded;
}

void feed_u64(td::Sha256State& state, td::uint64 value) {
  const auto encoded = encode_u64(value);
  state.feed(td::Slice{encoded.data(), encoded.size()});
}

}  // namespace

td::Status CellUsageJournal::record(const vm::LoadedCell& loaded_cell) {
  if (loaded_cell.data_cell.is_null()) {
    return td::Status::Error("cell-usage journal received an empty data cell");
  }
  auto path = loaded_cell.tree_node.path();
  if (!path) {
    return td::Status::Error("cell-usage journal received a load outside its usage tree");
  }
  entries_.push_back({std::move(*path), loaded_cell.data_cell->get_hash().as_bits256(), loaded_cell.effective_level});
  return td::Status::OK();
}

td::Status CellUsageJournal::validate() const {
  std::set<std::vector<td::uint8>> paths;
  for (const auto& entry : entries_) {
    if (entry.ref_path.size() > vm::CellTraits::max_depth) {
      return td::Status::Error("cell-usage journal path exceeds the maximum cell depth");
    }
    for (auto ref_id : entry.ref_path) {
      if (ref_id >= vm::CellTraits::max_refs) {
        return td::Status::Error("cell-usage journal contains an invalid reference index");
      }
    }
    if (entry.effective_level > vm::CellTraits::max_level) {
      return td::Status::Error("cell-usage journal contains an invalid effective level");
    }
    if (!paths.insert(entry.ref_path).second) {
      return td::Status::Error("cell-usage journal contains a duplicate path");
    }
  }
  return td::Status::OK();
}

td::Status CellUsageJournal::replay_into(const td::Ref<vm::Cell>& pure_root,
                                         const std::shared_ptr<vm::CellUsageTree>& coordinator_tree) const {
  if (!coordinator_tree) {
    return td::Status::Error("cell-usage journal replay requires a root and a coordinator tree");
  }
  return replay_into(pure_root, coordinator_tree->root_ptr());
}

td::Status CellUsageJournal::replay_into(const td::Ref<vm::Cell>& pure_root,
                                         const vm::CellUsageTree::NodePtr& coordinator_anchor) const {
  TRY_STATUS(validate());
  if (pure_root.is_null() || coordinator_anchor.empty()) {
    return td::Status::Error("cell-usage journal replay requires a root and a coordinator anchor");
  }
  if (!pure_root->get_tree_node().empty()) {
    return td::Status::Error("cell-usage journal replay requires an immutable pure root");
  }
  if (pure_root->get_hash().as_bits256() != anchor_root_hash_) {
    return td::Status::Error("cell-usage journal anchor does not match the coordinator root");
  }

  std::vector<vm::LoadedCell> resolved_cells;
  resolved_cells.reserve(entries_.size());
  // Entries are recorded in traversal order, so consecutive paths share long
  // prefixes. Keep the resolved chain and restart from the longest common
  // prefix: walking from the root for every entry costs O(entries * depth)
  // cell loads and dominates the coordinator's commit phase. Cells on a
  // cached prefix were already validated when they were first resolved.
  std::vector<td::uint8> cached_path;
  std::vector<td::Ref<vm::Cell>> chain;
  chain.push_back(pure_root);
  for (const auto& entry : entries_) {
    std::size_t common = 0;
    while (common < cached_path.size() && common < entry.ref_path.size() &&
           cached_path[common] == entry.ref_path[common]) {
      ++common;
    }
    cached_path.resize(common);
    chain.resize(common + 1);
    td::Ref<vm::Cell> current = chain.back();
    for (std::size_t i = common; i < entry.ref_path.size(); ++i) {
      if (!current->get_tree_node().empty()) {
        return td::Status::Error("cell-usage journal path entered a mutable usage tree");
      }
      TRY_RESULT(loaded, current->load_cell());
      current = loaded.data_cell->get_ref(entry.ref_path[i]);
      if (current.is_null()) {
        return td::Status::Error("cell-usage journal path is absent from the coordinator root");
      }
      cached_path.push_back(entry.ref_path[i]);
      chain.push_back(current);
    }

    if (!current->get_tree_node().empty()) {
      return td::Status::Error("cell-usage journal target entered a mutable usage tree");
    }
    TRY_RESULT(loaded, current->load_cell());
    if (loaded.data_cell->get_hash().as_bits256() != entry.cell_hash ||
        loaded.effective_level != entry.effective_level) {
      return td::Status::Error("cell-usage journal entry does not match the coordinator cell");
    }
    resolved_cells.push_back(std::move(loaded));
  }

  // The coordinator-node walk has the same prefix structure; cache it too.
  std::vector<td::uint8> cached_node_path;
  std::vector<vm::CellUsageTree::NodePtr> node_chain;
  node_chain.push_back(coordinator_anchor);
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const auto& ref_path = entries_[i].ref_path;
    std::size_t common = 0;
    while (common < cached_node_path.size() && common < ref_path.size() &&
           cached_node_path[common] == ref_path[common]) {
      ++common;
    }
    cached_node_path.resize(common);
    node_chain.resize(common + 1);
    auto coordinator_node = node_chain.back();
    for (std::size_t j = common; j < ref_path.size(); ++j) {
      coordinator_node = coordinator_node.create_child(ref_path[j]);
      cached_node_path.push_back(ref_path[j]);
      node_chain.push_back(coordinator_node);
    }
    resolved_cells[i].tree_node = coordinator_node;
    if (!coordinator_node.on_load(resolved_cells[i])) {
      return td::Status::Error("cell-usage journal could not update the coordinator usage tree");
    }
  }
  return td::Status::OK();
}

td::Bits256 CellUsageJournal::commitment() const {
  static constexpr char domain[] = "TON-PSAE-CELL-USAGE-JOURNAL-V1";
  td::Sha256State state;
  state.init();
  state.feed(td::Slice{domain, sizeof(domain) - 1});
  state.feed(anchor_root_hash_.as_slice());
  feed_u64(state, entries_.size());
  for (const auto& entry : entries_) {
    feed_u64(state, entry.ref_path.size());
    if (!entry.ref_path.empty()) {
      state.feed(td::Slice{entry.ref_path.data(), entry.ref_path.size()});
    }
    state.feed(entry.cell_hash.as_slice());
    feed_u64(state, entry.effective_level);
  }
  td::Bits256 result;
  state.extract(result.as_slice(), true);
  return result;
}

}  // namespace ton::validator::parallel_inbound
