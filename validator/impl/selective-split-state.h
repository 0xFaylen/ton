// SPDX-License-Identifier: LGPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "td/utils/Status.h"
#include "ton/ton-types.h"
#include "vm/cells/Cell.h"

namespace ton::validator {

struct SelectiveSplitStatePartDescriptor {
  ShardId effective_shard{};
  RootHash wrapped_root_hash{};
};

// Replay-only helper for materializing selected account prefixes from a split
// persistent state. It never accepts a reconstructed state whose root differs
// from the externally supplied state root.
class SelectiveSplitStateAssembler {
 public:
  static td::Result<std::unique_ptr<SelectiveSplitStateAssembler>> create(
      ShardIdFull shard, RootHash expected_state_root, td::Ref<vm::Cell> wrapped_header, td::uint32 split_depth);

  const std::vector<SelectiveSplitStatePartDescriptor>& parts() const {
    return parts_;
  }

  td::Status materialize(ShardId effective_shard, td::Ref<vm::Cell> wrapped_part);
  td::Result<td::Ref<vm::Cell>> virtualized_state_root() const;

 private:
  SelectiveSplitStateAssembler(ShardIdFull shard, RootHash expected_state_root, td::uint32 split_depth);

  td::Status initialize(td::Ref<vm::Cell> wrapped_header);

  ShardIdFull shard_;
  RootHash expected_state_root_;
  td::uint32 split_depth_{};
  std::vector<SelectiveSplitStatePartDescriptor> parts_;
  td::Ref<vm::Cell> raw_state_root_;
};

}  // namespace ton::validator
