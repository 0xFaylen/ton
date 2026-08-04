// SPDX-License-Identifier: LGPL-2.0-or-later
#include "validator/validation-replay/block-workload.h"

#include <algorithm>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "ton/ton-types.h"
#include "vm/dict.h"

namespace ton::validator::replay {

td::Result<TvmHotpathStats::ExecutionKind> classify_and_count_transaction(td::Ref<vm::Cell> transaction,
                                                                          TransactionKindCounts& counts) {
  if (transaction.is_null()) {
    return td::Status::Error("cannot classify a null transaction");
  }
  block::gen::Transaction::Record record;
  if (!tlb::unpack_cell(transaction, record)) {
    return td::Status::Error("cannot unpack transaction while classifying its kind");
  }
  const auto tag = block::gen::t_TransactionDescr.get_tag(vm::load_cell_slice(record.description));
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord:
      ++counts.ordinary;
      return TvmHotpathStats::ExecutionKind::ordinary;
    case block::gen::TransactionDescr::trans_tick_tock: {
      block::gen::TransactionDescr::Record_trans_tick_tock tick_tock;
      if (!tlb::unpack_cell(record.description, tick_tock)) {
        return td::Status::Error("cannot unpack tick-tock transaction description while classifying its kind");
      }
      if (tick_tock.is_tock) {
        ++counts.tock;
      } else {
        ++counts.tick;
      }
      return TvmHotpathStats::ExecutionKind::tick_tock;
    }
    case block::gen::TransactionDescr::trans_storage:
      ++counts.storage;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_split_prepare:
      ++counts.split_prepare;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_split_install:
      ++counts.split_install;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_merge_prepare:
      ++counts.merge_prepare;
      return TvmHotpathStats::ExecutionKind::other;
    case block::gen::TransactionDescr::trans_merge_install:
      ++counts.merge_install;
      return TvmHotpathStats::ExecutionKind::other;
    default:
      return td::Status::Error("unknown transaction description tag while classifying its kind");
  }
}

td::Result<BlockWorkloadSummary> summarize_account_blocks(td::Ref<vm::Cell> account_blocks) {
  if (account_blocks.is_null()) {
    return td::Status::Error("cannot summarize a null AccountBlocks root");
  }
  vm::AugmentedDictionary dictionary{vm::load_cell_slice_ref(account_blocks), 256, block::tlb::aug_ShardAccountBlocks};
  BlockWorkloadSummary result;
  td::Status scan_status = td::Status::OK();
  const bool accounts_ok = dictionary.check_for_each_extra(
      [&](td::Ref<vm::CellSlice> account_block_slice, td::Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
        if (key_len != 256) {
          scan_status = td::Status::Error("invalid account block key length");
          return false;
        }
        const StdSmcAddress address = key;
        block::gen::AccountBlock::Record account_block;
        if (!tlb::csr_unpack(std::move(account_block_slice), account_block) || account_block.account_addr != address) {
          scan_status = td::Status::Error("cannot unpack AccountBlock");
          return false;
        }

        std::size_t account_transactions = 0;
        vm::AugmentedDictionary transactions{vm::DictNonEmpty(), std::move(account_block.transactions), 64,
                                             block::tlb::aug_AccountTransactions};
        const bool transactions_ok = transactions.check_for_each_extra(
            [&](td::Ref<vm::CellSlice> transaction_slice, td::Ref<vm::CellSlice>, td::ConstBitPtr, int tx_key_len) {
              auto transaction = transaction_slice->prefetch_ref();
              if (tx_key_len != 64 || transaction.is_null()) {
                scan_status = td::Status::Error("invalid transaction entry in AccountBlock");
                return false;
              }
              auto kind = classify_and_count_transaction(std::move(transaction), result.transaction_kinds);
              if (kind.is_error()) {
                scan_status = kind.move_as_error();
                return false;
              }
              ++account_transactions;
              return true;
            });
        if (!transactions_ok) {
          if (scan_status.is_ok()) {
            scan_status = td::Status::Error("cannot scan AccountBlock transactions");
          }
          return false;
        }
        ++result.distinct_accounts;
        result.raw_transactions += account_transactions;
        result.max_account_transactions = std::max(result.max_account_transactions, account_transactions);
        return true;
      });
  if (!accounts_ok) {
    if (scan_status.is_ok()) {
      scan_status = td::Status::Error("cannot scan AccountBlocks dictionary");
    }
    return scan_status;
  }
  if (result.transaction_kinds.total() != result.raw_transactions) {
    return td::Status::Error("transaction kind counts do not cover the complete block workload");
  }
  return result;
}

}  // namespace ton::validator::replay
