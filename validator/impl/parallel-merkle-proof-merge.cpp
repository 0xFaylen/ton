// SPDX-License-Identifier: LGPL-2.0-or-later

#include "td/utils/StringBuilder.h"
#include "vm/cells/MerkleProof.h"

#include "parallel-merkle-proof-merge.h"

namespace ton::validator::parallel_inbound {

td::Result<td::Ref<vm::Cell>> merge_merkle_proofs_fast(const std::vector<td::Ref<vm::Cell>>& proofs) {
  if (proofs.empty()) {
    return td::Status::Error("cannot merge an empty Merkle-proof set");
  }
  if (proofs.size() == 1) {
    if (proofs.front().is_null()) {
      return td::Status::Error("Merkle proof 0 is null");
    }
    TRY_RESULT(ignored_virtual_root, vm::MerkleProof::virtualize(proofs.front()));
    static_cast<void>(ignored_virtual_root);
    return proofs.front();
  }

  td::Ref<vm::Cell> combined;
  for (std::size_t i = 0; i < proofs.size(); ++i) {
    if (proofs[i].is_null()) {
      return td::Status::Error(PSTRING() << "Merkle proof " << i << " is null");
    }
    if (combined.is_null()) {
      combined = proofs[i];
      continue;
    }
    auto next = vm::MerkleProof::combine_fast(std::move(combined), proofs[i]);
    if (next.is_error()) {
      return next.move_as_error_prefix(PSTRING() << "cannot merge Merkle proof " << i << ": ");
    }
    combined = next.move_as_ok();
  }
  return combined;
}

}  // namespace ton::validator::parallel_inbound
