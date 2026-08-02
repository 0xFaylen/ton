// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <vector>

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"

namespace ton::validator::parallel_inbound {

// Merges wrapped Merkle proofs in the supplied canonical order. Every proof
// must expose the same level-zero root. The fast TON merge preserves proof
// semantics while avoiding a full global cell-index rebuild after each item.
td::Result<td::Ref<vm::Cell>> merge_merkle_proofs_fast(const std::vector<td::Ref<vm::Cell>>& proofs);

}  // namespace ton::validator::parallel_inbound
