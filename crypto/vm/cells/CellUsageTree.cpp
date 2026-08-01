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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <algorithm>

#include "vm/cells/CellUsageTree.h"

#include "DataCell.h"

namespace vm {
//
// CellUsageTree::NodePtr
//
bool CellUsageTree::NodePtr::on_load(const Cell::LoadedCell& loaded_cell) const {
  auto tree = tree_weak_.lock();
  if (!tree) {
    return false;
  }
  tree->on_load(node_id_, loaded_cell, *this);
  return true;
}

CellUsageTree::NodePtr CellUsageTree::NodePtr::create_child(unsigned ref_id) const {
  auto tree = tree_weak_.lock();
  if (!tree) {
    return {};
  }
  return {tree_weak_, tree->create_child(node_id_, ref_id)};
}

bool CellUsageTree::NodePtr::is_from_tree(const CellUsageTree* master_tree) const {
  DCHECK(master_tree);
  auto tree = tree_weak_.lock();
  if (tree.get() != master_tree) {
    return false;
  }
  return true;
}

bool CellUsageTree::NodePtr::mark_path(CellUsageTree* master_tree) const {
  DCHECK(master_tree);
  auto tree = tree_weak_.lock();
  if (tree.get() != master_tree) {
    return false;
  }
  master_tree->mark_path(node_id_);
  return true;
}

std::optional<std::vector<td::uint8>> CellUsageTree::NodePtr::path() const {
  auto tree = tree_weak_.lock();
  if (!tree || node_id_ == 0) {
    return std::nullopt;
  }
  return tree->get_path(node_id_);
}

//
// CellUsageTree
//
CellUsageTree::NodePtr CellUsageTree::root_ptr() {
  return {shared_from_this(), 1};
}

CellUsageTree::NodeId CellUsageTree::root_id() const {
  return 1;
};

bool CellUsageTree::is_loaded(NodeId node_id) const {
  if (use_mark_) {
    return nodes_[node_id].has_mark;
  }
  return nodes_[node_id].is_loaded;
}

bool CellUsageTree::has_mark(NodeId node_id) const {
  return nodes_[node_id].has_mark;
}

void CellUsageTree::set_mark(NodeId node_id, bool mark) {
  if (node_id == 0) {
    return;
  }
  nodes_[node_id].has_mark = mark;
}

void CellUsageTree::mark_path(NodeId node_id) {
  auto cur_node_id = get_parent(node_id);
  while (cur_node_id != 0) {
    if (has_mark(cur_node_id)) {
      break;
    }
    set_mark(cur_node_id);
    cur_node_id = get_parent(cur_node_id);
  }
}

CellUsageTree::NodeId CellUsageTree::get_parent(NodeId node_id) const {
  return nodes_[node_id].parent;
}

CellUsageTree::NodeId CellUsageTree::get_child(NodeId node_id, unsigned ref_id) const {
  DCHECK(ref_id < CellTraits::max_refs);
  return nodes_[node_id].children[ref_id];
}

void CellUsageTree::set_use_mark_for_is_loaded(bool use_mark) {
  use_mark_ = use_mark;
}

void CellUsageTree::on_load(NodeId node_id, const Cell::LoadedCell& loaded_cell, const NodePtr& tree_node) {
  if (ignore_loads_ || nodes_[node_id].is_loaded) {
    return;
  }
  nodes_[node_id].is_loaded = true;
  if (cell_load_callback_) {
    cell_load_callback_(loaded_cell);
  }
  if (cell_load_path_callback_) {
    auto loaded_cell_with_path = loaded_cell;
    loaded_cell_with_path.tree_node = tree_node;
    cell_load_path_callback_(loaded_cell_with_path);
  }
}

CellUsageTree::NodeId CellUsageTree::create_child(NodeId node_id, unsigned ref_id) {
  DCHECK(ref_id < CellTraits::max_refs);
  NodeId res = nodes_[node_id].children[ref_id];
  if (res) {
    return res;
  }
  res = create_node(node_id, ref_id);
  nodes_[node_id].children[ref_id] = res;
  return res;
}

CellUsageTree::NodeId CellUsageTree::create_node(NodeId parent, unsigned parent_ref_id) {
  NodeId res = static_cast<NodeId>(nodes_.size());
  nodes_.emplace_back();
  nodes_.back().parent = parent;
  nodes_.back().parent_ref_id = static_cast<td::uint8>(parent_ref_id);
  return res;
}

std::vector<td::uint8> CellUsageTree::get_path(NodeId node_id) const {
  CHECK(node_id > 0 && node_id < nodes_.size());
  std::vector<td::uint8> path;
  while (node_id != root_id()) {
    const auto parent = get_parent(node_id);
    CHECK(parent > 0);
    const auto ref_id = nodes_[node_id].parent_ref_id;
    CHECK(ref_id < CellTraits::max_refs);
    CHECK(nodes_[parent].children[ref_id] == node_id);
    path.push_back(ref_id);
    node_id = parent;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

}  // namespace vm
