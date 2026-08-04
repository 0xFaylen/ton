// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <cstddef>

#include "td/utils/Status.h"
#include "validator/interfaces/tvm-hotpath-stats.h"
#include "vm/cells.h"

namespace ton::validator::replay {

struct TransactionKindCounts {
  std::size_t ordinary{0};
  std::size_t tick{0};
  std::size_t tock{0};
  std::size_t storage{0};
  std::size_t split_prepare{0};
  std::size_t split_install{0};
  std::size_t merge_prepare{0};
  std::size_t merge_install{0};

  void add(const TransactionKindCounts& other) {
    ordinary += other.ordinary;
    tick += other.tick;
    tock += other.tock;
    storage += other.storage;
    split_prepare += other.split_prepare;
    split_install += other.split_install;
    merge_prepare += other.merge_prepare;
    merge_install += other.merge_install;
  }

  std::size_t total() const {
    return ordinary + tick + tock + storage + split_prepare + split_install + merge_prepare + merge_install;
  }
};

struct BlockWorkloadSummary {
  std::size_t distinct_accounts{0};
  std::size_t raw_transactions{0};
  std::size_t max_account_transactions{0};
  TransactionKindCounts transaction_kinds;
};

// Classifies one serialized Transaction cell and increments the matching
// counter. Fails closed: a malformed or unknown description is an error, never
// a silently skipped or miscounted transaction.
td::Result<TvmHotpathStats::ExecutionKind> classify_and_count_transaction(td::Ref<vm::Cell> transaction,
                                                                          TransactionKindCounts& counts);

// Walks a block's AccountBlocks dictionary and returns per-kind transaction
// counts. The sum of all kind counters must equal the raw transaction count.
td::Result<BlockWorkloadSummary> summarize_account_blocks(td::Ref<vm::Cell> account_blocks);

}  // namespace ton::validator::replay
