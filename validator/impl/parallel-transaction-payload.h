// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "block/block.h"
#include "block/transaction.h"
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
  Hash256 total_fees_hash{};
  Hash256 effects_hash{};
  Hash256 proof_journal_hash{};
  std::uint64_t transaction_start_lt{0};
  std::uint64_t transaction_end_lt{0};
  std::uint64_t gas_used{0};
  std::uint8_t original_account_status{0};
  std::uint8_t end_account_status{0};
  block::CurrencyCollection total_fees;
  td::Ref<vm::Cell> transaction_root;
  td::Ref<vm::Cell> post_account_state;
  std::optional<Hash256> inbound_message_hash;
  td::Ref<vm::Cell> inbound_message;
  std::vector<OutboundMessageEffect> outbound_messages;
};

enum class PayloadError {
  none,
  missing_transaction,
  missing_post_account_state,
  mutable_payload_cell,
  malformed_transaction,
  malformed_state_update,
  malformed_total_fees,
  invalid_post_account_state,
  post_state_hash_mismatch,
  malformed_description,
  invalid_in_message,
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
  input_message_hash_mismatch,
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

// Basechain Transaction::update_limits() can be reproduced from validated
// canonical effects. Masterchain public-library accounting, account-dictionary
// proofs and queue/descriptor effects are separate coordinator operations.
enum class BasechainLimitError {
  none,
  size_mismatch,
  missing_transaction,
  missing_post_account_state,
};

struct BasechainLimitApplyResult {
  BasechainLimitError error{BasechainLimitError::none};
  std::size_t applied_transactions{0};

  explicit operator bool() const {
    return error == BasechainLimitError::none;
  }
};

struct BasechainLimitContext {
  bool account_is_first{false};
  bool charge_gas{true};
};

// Applies a validated prefix to a shadow copy and publishes it atomically.
// Context flags must be derived by the coordinator from the account chain and
// message phase, never accepted from a worker. The target is unchanged on
// validation failure.
BasechainLimitApplyResult apply_basechain_block_limits_atomic(block::BlockLimitStatus& target,
                                                              const std::vector<CanonicalTransactionEffects>& effects,
                                                              const std::vector<BasechainLimitContext>& contexts);

struct OutboundRegistrationContext {
  bool metadata_enabled{false};
  td::optional<block::MsgMetadata> metadata;
};

struct CanonicalOutboundRegistrationBatch {
  std::vector<block::NewOutMsg> messages;
  std::optional<ton::LogicalTime> min_message_lt;
  std::size_t extra_out_msgs_delta{0};
};

enum class OutboundRegistrationError {
  none,
  missing_transaction,
  missing_message,
  invalid_message,
  message_hash_mismatch,
  logical_time_mismatch,
};

struct OutboundRegistrationResult {
  OutboundRegistrationError error{OutboundRegistrationError::none};
  std::optional<std::size_t> message_index;
  std::optional<CanonicalOutboundRegistrationBatch> batch;

  explicit operator bool() const {
    return error == OutboundRegistrationError::none;
  }
};

// Reconstructs exactly the NewOutMsg objects created by
// Collator::register_new_msgs(). Metadata is coordinator-owned routing context
// and is never accepted from the worker payload.
OutboundRegistrationResult materialize_outbound_registrations(const CanonicalTransactionEffects& effects,
                                                              const OutboundRegistrationContext& context);

struct InboundDescriptorContext {
  td::Ref<vm::Cell> message_envelope;
  bool dequeued_from_current_shard{false};
};

struct CanonicalInboundDescriptorDelta {
  Hash256 message_hash{};
  td::Ref<vm::Cell> in_msg_descriptor;
  td::Ref<vm::Cell> out_msg_descriptor;
};

enum class InboundDescriptorError {
  none,
  missing_transaction,
  missing_inbound_message,
  missing_message_envelope,
  invalid_inbound_message,
  non_internal_inbound_message,
  malformed_message_envelope,
  envelope_message_mismatch,
  cannot_serialize_in_msg,
  cannot_serialize_out_msg,
};

struct InboundDescriptorResult {
  InboundDescriptorError error{InboundDescriptorError::none};
  std::optional<CanonicalInboundDescriptorDelta> delta;

  explicit operator bool() const {
    return error == InboundDescriptorError::none;
  }
};

// Materializes the exact msg_import_fin descriptor and, when the message is
// dequeued from this shard's own queue, its paired msg_export_deq_imm record.
// Queue deletion and dictionary insertion stay serialized coordinator work.
InboundDescriptorResult materialize_inbound_internal_descriptors(const CanonicalTransactionEffects& effects,
                                                                 const InboundDescriptorContext& context);

const char* to_string(PayloadError error);
const char* to_string(PrecommitError error);
const char* to_string(BasechainLimitError error);
const char* to_string(OutboundRegistrationError error);
const char* to_string(InboundDescriptorError error);

}  // namespace ton::validator::parallel_inbound
