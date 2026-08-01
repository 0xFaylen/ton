// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "td/utils/Status.h"
#include "vm/cells/Cell.h"

#include "parallel-cell-usage-journal.h"
#include "parallel-inbound-scheduler.h"

namespace ton::validator::parallel_inbound {

// A worker must materialize cells before returning. In particular, this
// payload deliberately cannot retain block::transaction::Transaction or its
// Account reference: both are mutable/account-lane-local objects.
struct CanonicalTransactionPayload {
  td::Ref<vm::Cell> transaction_root;
  td::Ref<vm::Cell> post_account_state;
  std::vector<CellUsageJournal> proof_journals;
};

struct OutboundMessageEffect {
  std::uint64_t logical_time{0};
  Hash256 message_hash{};
  td::Ref<vm::Cell> message;
};

// Consensus-visible, account-local effects derived from the canonical TL-B
// cells. Global block mutations (limits, descriptors, queues and ProcessedUpto)
// are intentionally not part of this object and remain coordinator work.
struct CanonicalTransactionEffects {
  Hash256 account{};
  Hash256 pre_account_state_hash{};
  Hash256 transaction_hash{};
  Hash256 post_account_state_hash{};
  Hash256 effects_hash{};
  Hash256 proof_journal_hash{};
  std::uint64_t transaction_start_lt{0};
  std::uint64_t transaction_end_lt{0};
  std::uint64_t gas_used{0};
  std::vector<OutboundMessageEffect> outbound_messages;
};

enum class PayloadError {
  none,
  missing_transaction,
  missing_post_account_state,
  mutable_payload_cell,
  malformed_transaction,
  malformed_state_update,
  invalid_post_account_state,
  post_state_hash_mismatch,
  malformed_description,
  invalid_out_message_dictionary,
  logical_time_overflow,
  invalid_proof_journal,
  non_canonical_proof_journal_order,
  account_mismatch,
  pre_state_mismatch,
  transaction_hash_mismatch,
  post_state_mismatch,
  effects_hash_mismatch,
  proof_journal_hash_mismatch,
  transaction_start_lt_mismatch,
  transaction_end_lt_mismatch,
  gas_used_mismatch,
};

struct PayloadValidationResult {
  PayloadError error{PayloadError::none};
  std::optional<CanonicalTransactionEffects> effects;

  explicit operator bool() const {
    return error == PayloadError::none;
  }
};

// Parses and validates the canonical cells and derives all receipt
// commitments. No coordinator state is mutated.
PayloadValidationResult inspect_transaction_payload(const CanonicalTransactionPayload& payload);

// Worker-side convenience builder. The coordinator must still call
// validate_worker_payload() and must not trust the returned header.
td::Result<WorkerReceipt> build_worker_receipt(const MessageKey& input, std::uint64_t account_sequence,
                                               const CanonicalTransactionPayload& payload);

// Coordinator-side fail-closed validation. Every value derivable from the
// canonical cells is recomputed rather than trusted from the worker header.
PayloadValidationResult validate_worker_payload(const CanonicalTransactionPayload& payload,
                                                const WorkerReceipt& receipt);

enum class PrecommitError {
  none,
  size_mismatch,
  receipt_without_payload,
  payload_without_receipt,
  payload_invalid,
  receipt_invalid,
};

struct PrecommitValidationResult {
  PrecommitError error{PrecommitError::none};
  PayloadError payload_error{PayloadError::none};
  ReceiptError receipt_error{ReceiptError::none};
  std::optional<std::size_t> item_index;
  std::size_t verified_payloads{0};
  std::map<Hash256, AccountCheckpoint> checkpoints;

  explicit operator bool() const {
    return error == PrecommitError::none;
  }
};

// Pure precommit gate for a whole canonical input vector. On any error the
// returned checkpoints equal initial_checkpoints, so a caller cannot
// accidentally publish a partially validated account frontier.
PrecommitValidationResult validate_precommit_set(
    const std::vector<WorkItem>& items, const std::vector<std::optional<WorkerReceipt>>& receipts,
    const std::vector<std::optional<CanonicalTransactionPayload>>& payloads,
    const std::map<Hash256, AccountCheckpoint>& initial_checkpoints);

const char* to_string(PayloadError error);
const char* to_string(PrecommitError error);

}  // namespace ton::validator::parallel_inbound
