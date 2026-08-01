// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <memory>
#include <vector>

#include "td/utils/Status.h"
#include "td/utils/bits.h"
#include "vm/cells/Cell.h"
#include "vm/cells/CellUsageTree.h"

namespace ton::validator::parallel_inbound {

// A local-worker proof journal anchored to one immutable cell root. Entries do
// not carry trusted cells: the coordinator resolves every path from its own
// root, verifies the expected hash/level, and only then replays the load into
// its serial CellUsageTree.
class CellUsageJournal {
 public:
  struct Entry {
    std::vector<td::uint8> ref_path;
    td::Bits256 cell_hash = td::Bits256::zero();
    td::uint32 effective_level{0};
  };

  explicit CellUsageJournal(td::Bits256 anchor_root_hash) : anchor_root_hash_(anchor_root_hash) {
  }
  CellUsageJournal(td::Bits256 anchor_root_hash, std::vector<Entry> entries)
      : anchor_root_hash_(anchor_root_hash), entries_(std::move(entries)) {
  }

  td::Status record(const vm::LoadedCell& loaded_cell);
  td::Status validate() const;
  td::Status replay_into(const td::Ref<vm::Cell>& pure_root,
                         const std::shared_ptr<vm::CellUsageTree>& coordinator_tree) const;
  td::Bits256 commitment() const;

  const td::Bits256& anchor_root_hash() const {
    return anchor_root_hash_;
  }
  const std::vector<Entry>& entries() const {
    return entries_;
  }

 private:
  td::Bits256 anchor_root_hash_;
  std::vector<Entry> entries_;
};

}  // namespace ton::validator::parallel_inbound
