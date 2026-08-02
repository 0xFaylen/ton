// SPDX-License-Identifier: LGPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "td/utils/crypto.h"
#include "vm/dict.h"

#include "parallel-transaction-payload.h"

namespace ton::validator::parallel_inbound {
namespace {

Hash256 to_hash256(const td::Bits256& value) {
  Hash256 result{};
  std::memcpy(result.data(), value.as_slice().data(), result.size());
  return result;
}

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

void feed_hash(td::Sha256State& state, const Hash256& hash) {
  state.feed(td::Slice{hash.data(), hash.size()});
}

Hash256 extract_hash(td::Sha256State& state) {
  td::Bits256 result;
  state.extract(result.as_slice(), true);
  return to_hash256(result);
}

td::Result<Hash256> currency_collection_hash(const block::CurrencyCollection& value) {
  vm::CellBuilder builder;
  if (!value.store(builder)) {
    return td::Status::Error("cannot serialize CurrencyCollection");
  }
  auto cell = builder.finalize_novm();
  if (cell.is_null()) {
    return td::Status::Error("cannot materialize CurrencyCollection cell");
  }
  return to_hash256(cell->get_hash().as_bits256());
}

td::Result<std::uint64_t> extract_gas_used(const td::Ref<vm::Cell>& description) {
  td::Ref<vm::CellSlice> compute_phase;
  const auto tag = block::gen::t_TransactionDescr.get_tag(vm::load_cell_slice(description));
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord: {
      block::gen::TransactionDescr::Record_trans_ord record;
      if (!tlb::unpack_cell(description, record)) {
        return td::Status::Error("cannot unpack ordinary transaction description");
      }
      compute_phase = std::move(record.compute_ph);
      break;
    }
    case block::gen::TransactionDescr::trans_tick_tock: {
      block::gen::TransactionDescr::Record_trans_tick_tock record;
      if (!tlb::unpack_cell(description, record)) {
        return td::Status::Error("cannot unpack tick-tock transaction description");
      }
      compute_phase = std::move(record.compute_ph);
      break;
    }
    case block::gen::TransactionDescr::trans_split_prepare: {
      block::gen::TransactionDescr::Record_trans_split_prepare record;
      if (!tlb::unpack_cell(description, record)) {
        return td::Status::Error("cannot unpack split-prepare transaction description");
      }
      compute_phase = std::move(record.compute_ph);
      break;
    }
    case block::gen::TransactionDescr::trans_merge_install: {
      block::gen::TransactionDescr::Record_trans_merge_install record;
      if (!tlb::unpack_cell(description, record)) {
        return td::Status::Error("cannot unpack merge-install transaction description");
      }
      compute_phase = std::move(record.compute_ph);
      break;
    }
    case block::gen::TransactionDescr::trans_storage:
    case block::gen::TransactionDescr::trans_split_install:
    case block::gen::TransactionDescr::trans_merge_prepare:
      return std::uint64_t{0};
    default:
      return td::Status::Error("unknown transaction description tag");
  }

  if (compute_phase.is_null()) {
    return td::Status::Error("transaction description has no compute phase");
  }
  const auto compute_tag = block::gen::t_TrComputePhase.get_tag(*compute_phase);
  if (compute_tag == block::gen::TrComputePhase::tr_phase_compute_skipped) {
    block::gen::TrComputePhase::Record_tr_phase_compute_skipped skipped;
    if (!tlb::csr_unpack(std::move(compute_phase), skipped)) {
      return td::Status::Error("cannot unpack skipped compute phase");
    }
    return std::uint64_t{0};
  }
  if (compute_tag != block::gen::TrComputePhase::tr_phase_compute_vm) {
    return td::Status::Error("unknown compute phase tag");
  }
  block::gen::TrComputePhase::Record_tr_phase_compute_vm executed;
  if (!tlb::csr_unpack(std::move(compute_phase), executed) || executed.r1.gas_used.is_null()) {
    return td::Status::Error("cannot unpack VM compute phase");
  }
  const auto gas_used = block::tlb::t_VarUInteger_7.as_uint(*executed.r1.gas_used);
  if (gas_used == std::numeric_limits<td::uint64>::max()) {
    return td::Status::Error("VM compute phase gas does not fit uint64");
  }
  return gas_used;
}

td::Result<Hash256> proof_journal_set_commitment(const std::vector<CellUsageJournal>& journals,
                                                 PayloadError& journal_error) {
  static constexpr char domain[] = "TON-PSAE-PROOF-JOURNAL-SET-V1";
  td::Sha256State state;
  state.init();
  state.feed(td::Slice{domain, sizeof(domain) - 1});
  feed_u64(state, journals.size());

  std::optional<std::pair<Hash256, Hash256>> previous;
  for (const auto& journal : journals) {
    auto validation = journal.validate();
    if (validation.is_error()) {
      journal_error = PayloadError::invalid_proof_journal;
      return validation;
    }
    const auto anchor = to_hash256(journal.anchor_root_hash());
    const auto commitment = to_hash256(journal.commitment());
    const auto key = std::make_pair(anchor, commitment);
    if (previous && !(previous.value() < key)) {
      journal_error = PayloadError::non_canonical_proof_journal_order;
      return td::Status::Error("proof journals are not in strict canonical order");
    }
    previous = key;
    feed_hash(state, anchor);
    feed_hash(state, commitment);
  }
  return extract_hash(state);
}

Hash256 effects_commitment(const CanonicalTransactionEffects& effects) {
  static constexpr char domain[] = "TON-PSAE-CANONICAL-TRANSACTION-EFFECTS-V3";
  td::Sha256State state;
  state.init();
  state.feed(td::Slice{domain, sizeof(domain) - 1});
  feed_hash(state, effects.account);
  feed_hash(state, effects.pre_account_state_hash);
  feed_hash(state, effects.transaction_hash);
  feed_hash(state, effects.post_account_state_hash);
  feed_hash(state, effects.total_fees_hash);
  feed_u64(state, effects.transaction_start_lt);
  feed_u64(state, effects.transaction_end_lt);
  feed_u64(state, effects.gas_used);
  feed_u64(state, effects.original_account_status);
  feed_u64(state, effects.end_account_status);
  feed_u64(state, effects.inbound_message_hash.has_value());
  if (effects.inbound_message_hash) {
    feed_hash(state, effects.inbound_message_hash.value());
  }
  feed_u64(state, effects.outbound_messages.size());
  for (const auto& message : effects.outbound_messages) {
    feed_u64(state, message.logical_time);
    feed_hash(state, message.message_hash);
  }
  return extract_hash(state);
}

PayloadValidationResult error(PayloadError value) {
  return {.error = value, .effects = std::nullopt};
}

}  // namespace

PayloadValidationResult inspect_transaction_payload(const CanonicalTransactionPayload& payload) {
  if (payload.transaction_root.is_null()) {
    return error(PayloadError::missing_transaction);
  }
  if (payload.post_account_state.is_null()) {
    return error(PayloadError::missing_post_account_state);
  }
  if (!payload.transaction_root->get_tree_node().empty() || !payload.post_account_state->get_tree_node().empty()) {
    return error(PayloadError::mutable_payload_cell);
  }

  block::gen::Transaction::Record transaction;
  if (!tlb::unpack_cell(payload.transaction_root, transaction)) {
    return error(PayloadError::malformed_transaction);
  }
  block::gen::HASH_UPDATE::Record state_update;
  if (!tlb::type_unpack_cell(transaction.state_update, block::gen::t_HASH_UPDATE_Account, state_update)) {
    return error(PayloadError::malformed_state_update);
  }
  block::CurrencyCollection total_fees;
  if (!total_fees.validate_unpack(transaction.total_fees)) {
    return error(PayloadError::malformed_total_fees);
  }
  auto total_fees_hash = currency_collection_hash(total_fees);
  if (total_fees_hash.is_error()) {
    return error(PayloadError::malformed_total_fees);
  }
  if (!block::gen::t_Account.validate_ref(payload.post_account_state)) {
    return error(PayloadError::invalid_post_account_state);
  }
  if (state_update.new_hash != payload.post_account_state->get_hash().bits()) {
    return error(PayloadError::post_state_hash_mismatch);
  }

  auto gas_result = extract_gas_used(transaction.description);
  if (gas_result.is_error()) {
    return error(PayloadError::malformed_description);
  }

  CanonicalTransactionEffects effects;
  effects.account = to_hash256(transaction.account_addr);
  effects.pre_account_state_hash = to_hash256(state_update.old_hash);
  effects.transaction_hash = to_hash256(payload.transaction_root->get_hash().as_bits256());
  effects.post_account_state_hash = to_hash256(state_update.new_hash);
  effects.total_fees_hash = total_fees_hash.move_as_ok();
  effects.transaction_start_lt = transaction.lt;
  if (transaction.outmsg_cnt < 0 || transaction.lt > std::numeric_limits<std::uint64_t>::max() -
                                                         static_cast<std::uint64_t>(transaction.outmsg_cnt) - 1) {
    return error(PayloadError::logical_time_overflow);
  }
  effects.transaction_end_lt = transaction.lt + static_cast<std::uint64_t>(transaction.outmsg_cnt) + 1;
  effects.gas_used = gas_result.move_as_ok();
  effects.original_account_status = static_cast<std::uint8_t>(transaction.orig_status);
  effects.end_account_status = static_cast<std::uint8_t>(transaction.end_status);
  effects.total_fees = std::move(total_fees);
  effects.transaction_root = payload.transaction_root;
  effects.post_account_state = payload.post_account_state;

  if (transaction.r1.in_msg.is_null()) {
    return error(PayloadError::invalid_in_message);
  }
  auto inbound_message = transaction.r1.in_msg->prefetch_ref();
  if (inbound_message.not_null()) {
    if (!block::gen::t_Message_Any.validate_ref(inbound_message)) {
      return error(PayloadError::invalid_in_message);
    }
    effects.inbound_message_hash = to_hash256(inbound_message->get_hash().as_bits256());
    effects.inbound_message = std::move(inbound_message);
  }

  try {
    vm::Dictionary out_messages{transaction.r1.out_msgs, 15};
    unsigned expected_index = 0;
    bool messages_ok = out_messages.check_for_each([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      if (key_len != 15 || key.get_uint(15) != expected_index || value.is_null() || value->size_ext() != 0x10000) {
        return false;
      }
      auto message = value->prefetch_ref();
      if (message.is_null() || !block::gen::t_Message_Any.validate_ref(message)) {
        return false;
      }
      effects.outbound_messages.push_back({.logical_time = transaction.lt + expected_index + 1,
                                           .message_hash = to_hash256(message->get_hash().as_bits256()),
                                           .message = std::move(message)});
      ++expected_index;
      return true;
    });
    if (!messages_ok || expected_index != static_cast<unsigned>(transaction.outmsg_cnt)) {
      return error(PayloadError::invalid_out_message_dictionary);
    }
  } catch (const vm::VmError&) {
    return error(PayloadError::invalid_out_message_dictionary);
  }

  auto journal_error = PayloadError::invalid_proof_journal;
  auto proof_commitment = proof_journal_set_commitment(payload.proof_journals, journal_error);
  if (proof_commitment.is_error()) {
    return error(journal_error);
  }
  effects.proof_journal_hash = proof_commitment.move_as_ok();
  effects.effects_hash = effects_commitment(effects);
  return {.effects = std::move(effects)};
}

td::Result<WorkerReceipt> build_worker_receipt(const MessageKey& input, std::uint64_t account_sequence,
                                               const CanonicalTransactionPayload& payload) {
  auto inspected = inspect_transaction_payload(payload);
  if (!inspected) {
    const auto* message = to_string(inspected.error);
    return td::Status::Error(td::Slice{message, std::strlen(message)});
  }
  auto& effects = inspected.effects.value();
  return WorkerReceipt{.input = input,
                       .account = effects.account,
                       .account_sequence = account_sequence,
                       .pre_account_state_hash = effects.pre_account_state_hash,
                       .transaction_hash = effects.transaction_hash,
                       .post_account_state_hash = effects.post_account_state_hash,
                       .effects_hash = effects.effects_hash,
                       .proof_journal_hash = effects.proof_journal_hash,
                       .transaction_start_lt = effects.transaction_start_lt,
                       .transaction_end_lt = effects.transaction_end_lt,
                       .gas_used = effects.gas_used};
}

PayloadValidationResult validate_worker_payload(const CanonicalTransactionPayload& payload,
                                                const WorkerReceipt& receipt) {
  auto result = inspect_transaction_payload(payload);
  if (!result) {
    return result;
  }
  const auto& effects = result.effects.value();
  if (receipt.account != effects.account) {
    return error(PayloadError::account_mismatch);
  }
  if (receipt.pre_account_state_hash != effects.pre_account_state_hash) {
    return error(PayloadError::pre_state_mismatch);
  }
  if (receipt.transaction_hash != effects.transaction_hash) {
    return error(PayloadError::transaction_hash_mismatch);
  }
  if (receipt.post_account_state_hash != effects.post_account_state_hash) {
    return error(PayloadError::post_state_mismatch);
  }
  if (receipt.effects_hash != effects.effects_hash) {
    return error(PayloadError::effects_hash_mismatch);
  }
  if (receipt.proof_journal_hash != effects.proof_journal_hash) {
    return error(PayloadError::proof_journal_hash_mismatch);
  }
  if (effects.inbound_message_hash && receipt.input.hash != effects.inbound_message_hash.value()) {
    return error(PayloadError::input_message_hash_mismatch);
  }
  if (receipt.transaction_start_lt != effects.transaction_start_lt) {
    return error(PayloadError::transaction_start_lt_mismatch);
  }
  if (receipt.transaction_end_lt != effects.transaction_end_lt) {
    return error(PayloadError::transaction_end_lt_mismatch);
  }
  if (receipt.gas_used != effects.gas_used) {
    return error(PayloadError::gas_used_mismatch);
  }
  return result;
}

PrecommitValidationResult validate_precommit_set(
    const std::vector<WorkItem>& items, const std::vector<std::optional<WorkerReceipt>>& receipts,
    const std::vector<std::optional<CanonicalTransactionPayload>>& payloads,
    const std::map<Hash256, AccountCheckpoint>& initial_checkpoints) {
  PrecommitValidationResult result;
  result.checkpoints = initial_checkpoints;
  if (items.size() != receipts.size() || items.size() != payloads.size()) {
    result.error = PrecommitError::size_mismatch;
    return result;
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    if (receipts[i] && !payloads[i]) {
      result.error = PrecommitError::receipt_without_payload;
      result.item_index = i;
      return result;
    }
    if (!receipts[i] && payloads[i]) {
      result.error = PrecommitError::payload_without_receipt;
      result.item_index = i;
      return result;
    }
    if (!receipts[i]) {
      continue;
    }
    auto payload_result = validate_worker_payload(*payloads[i], *receipts[i]);
    if (!payload_result) {
      result.error = PrecommitError::payload_invalid;
      result.payload_error = payload_result.error;
      result.item_index = i;
      result.verified_payloads = 0;
      return result;
    }
    ++result.verified_payloads;
  }

  auto receipt_result = validate_receipt_set(items, receipts, initial_checkpoints);
  if (!receipt_result) {
    result.error = PrecommitError::receipt_invalid;
    result.receipt_error = receipt_result.error;
    result.item_index = receipt_result.item_index;
    result.verified_payloads = 0;
    return result;
  }
  result.checkpoints = std::move(receipt_result.checkpoints);
  return result;
}

BasechainLimitApplyResult apply_basechain_block_limits_atomic(block::BlockLimitStatus& target,
                                                              const std::vector<CanonicalTransactionEffects>& effects,
                                                              const std::vector<BasechainLimitContext>& contexts) {
  if (effects.size() != contexts.size()) {
    return {.error = BasechainLimitError::size_mismatch};
  }
  for (const auto& transaction : effects) {
    if (transaction.transaction_root.is_null()) {
      return {.error = BasechainLimitError::missing_transaction};
    }
    if (transaction.post_account_state.is_null()) {
      return {.error = BasechainLimitError::missing_post_account_state};
    }
  }

  auto shadow = target;
  for (std::size_t i = 0; i < effects.size(); ++i) {
    const auto& transaction = effects[i];
    shadow.update_lt(transaction.transaction_end_lt);
    shadow.update_gas(contexts[i].charge_gas ? transaction.gas_used : 0);
    shadow.add_proof(transaction.post_account_state);
    shadow.add_cell(transaction.transaction_root);
    shadow.add_transaction();
    shadow.add_account(contexts[i].account_is_first);
  }

  target.cur_lt = shadow.cur_lt;
  target.gas_used = shadow.gas_used;
  target.st_stat = std::move(shadow.st_stat);
  target.accounts = shadow.accounts;
  target.transactions = shadow.transactions;
  target.extra_out_msgs = shadow.extra_out_msgs;
  target.collated_data_size_estimate = shadow.collated_data_size_estimate;
  target.public_library_diff = shadow.public_library_diff;
  return {.applied_transactions = effects.size()};
}

OutboundRegistrationResult materialize_outbound_registrations(const CanonicalTransactionEffects& effects,
                                                              const OutboundRegistrationContext& context) {
  if (effects.transaction_root.is_null()) {
    return {
        .error = OutboundRegistrationError::missing_transaction, .message_index = std::nullopt, .batch = std::nullopt};
  }

  CanonicalOutboundRegistrationBatch batch;
  batch.messages.reserve(effects.outbound_messages.size());
  for (std::size_t i = 0; i < effects.outbound_messages.size(); ++i) {
    const auto& effect = effects.outbound_messages[i];
    if (effect.message.is_null()) {
      return {.error = OutboundRegistrationError::missing_message, .message_index = i, .batch = std::nullopt};
    }
    if (!block::gen::t_Message_Any.validate_ref(effect.message)) {
      return {.error = OutboundRegistrationError::invalid_message, .message_index = i, .batch = std::nullopt};
    }
    if (to_hash256(effect.message->get_hash().as_bits256()) != effect.message_hash) {
      return {.error = OutboundRegistrationError::message_hash_mismatch, .message_index = i, .batch = std::nullopt};
    }
    if (effects.transaction_start_lt > std::numeric_limits<std::uint64_t>::max() - i - 1 ||
        effect.logical_time != effects.transaction_start_lt + i + 1) {
      return {.error = OutboundRegistrationError::logical_time_mismatch, .message_index = i, .batch = std::nullopt};
    }

    block::NewOutMsg message{effect.logical_time, effect.message, effects.transaction_root, static_cast<unsigned>(i)};
    if (context.metadata_enabled) {
      message.metadata = context.metadata;
    }
    if (!batch.min_message_lt || effect.logical_time < batch.min_message_lt.value()) {
      batch.min_message_lt = effect.logical_time;
    }
    batch.messages.push_back(std::move(message));
  }
  batch.extra_out_msgs_delta = batch.messages.size();
  return {.error = OutboundRegistrationError::none, .message_index = std::nullopt, .batch = std::move(batch)};
}

InboundDescriptorResult materialize_inbound_internal_descriptors(const CanonicalTransactionEffects& effects,
                                                                 const InboundDescriptorContext& context) {
  if (effects.transaction_root.is_null()) {
    return {.error = InboundDescriptorError::missing_transaction, .delta = std::nullopt};
  }
  if (!effects.inbound_message_hash || effects.inbound_message.is_null()) {
    return {.error = InboundDescriptorError::missing_inbound_message, .delta = std::nullopt};
  }
  if (context.message_envelope.is_null()) {
    return {.error = InboundDescriptorError::missing_message_envelope, .delta = std::nullopt};
  }
  if (!block::gen::t_Message_Any.validate_ref(effects.inbound_message) ||
      to_hash256(effects.inbound_message->get_hash().as_bits256()) != effects.inbound_message_hash.value()) {
    return {.error = InboundDescriptorError::invalid_inbound_message, .delta = std::nullopt};
  }

  auto message_slice = vm::load_cell_slice(effects.inbound_message);
  if (!block::tlb::t_Message.extract_info(message_slice) ||
      block::tlb::t_CommonMsgInfo.get_tag(message_slice) != block::gen::CommonMsgInfo::int_msg_info) {
    return {.error = InboundDescriptorError::non_internal_inbound_message, .delta = std::nullopt};
  }

  block::tlb::MsgEnvelope::Record_std envelope;
  if (!block::tlb::unpack_cell(context.message_envelope, envelope) || envelope.msg.is_null() ||
      envelope.fwd_fee_remaining.is_null()) {
    return {.error = InboundDescriptorError::malformed_message_envelope, .delta = std::nullopt};
  }
  if (to_hash256(envelope.msg->get_hash().as_bits256()) != effects.inbound_message_hash.value()) {
    return {.error = InboundDescriptorError::envelope_message_mismatch, .delta = std::nullopt};
  }

  vm::CellBuilder builder;
  td::Ref<vm::Cell> in_msg;
  if (!(builder.store_long_bool(4, 3) && builder.store_ref_bool(context.message_envelope) &&
        builder.store_ref_bool(effects.transaction_root) &&
        block::tlb::t_Grams.store_integer_ref(builder, envelope.fwd_fee_remaining) &&
        std::move(builder).finalize_to(in_msg))) {
    return {.error = InboundDescriptorError::cannot_serialize_in_msg, .delta = std::nullopt};
  }

  CanonicalInboundDescriptorDelta delta;
  delta.message_hash = effects.inbound_message_hash.value();
  delta.in_msg_descriptor = std::move(in_msg);
  if (context.dequeued_from_current_shard) {
    vm::CellBuilder out_builder;
    if (!(out_builder.store_long_bool(4, 3) && out_builder.store_ref_bool(context.message_envelope) &&
          out_builder.store_ref_bool(delta.in_msg_descriptor) &&
          std::move(out_builder).finalize_to(delta.out_msg_descriptor))) {
      return {.error = InboundDescriptorError::cannot_serialize_out_msg, .delta = std::nullopt};
    }
  }
  return {.error = InboundDescriptorError::none, .delta = std::move(delta)};
}

const char* to_string(PayloadError error) {
  switch (error) {
    case PayloadError::none:
      return "none";
    case PayloadError::missing_transaction:
      return "missing_transaction";
    case PayloadError::missing_post_account_state:
      return "missing_post_account_state";
    case PayloadError::mutable_payload_cell:
      return "mutable_payload_cell";
    case PayloadError::malformed_transaction:
      return "malformed_transaction";
    case PayloadError::malformed_state_update:
      return "malformed_state_update";
    case PayloadError::malformed_total_fees:
      return "malformed_total_fees";
    case PayloadError::invalid_post_account_state:
      return "invalid_post_account_state";
    case PayloadError::post_state_hash_mismatch:
      return "post_state_hash_mismatch";
    case PayloadError::malformed_description:
      return "malformed_description";
    case PayloadError::invalid_in_message:
      return "invalid_in_message";
    case PayloadError::invalid_out_message_dictionary:
      return "invalid_out_message_dictionary";
    case PayloadError::logical_time_overflow:
      return "logical_time_overflow";
    case PayloadError::invalid_proof_journal:
      return "invalid_proof_journal";
    case PayloadError::non_canonical_proof_journal_order:
      return "non_canonical_proof_journal_order";
    case PayloadError::account_mismatch:
      return "account_mismatch";
    case PayloadError::pre_state_mismatch:
      return "pre_state_mismatch";
    case PayloadError::transaction_hash_mismatch:
      return "transaction_hash_mismatch";
    case PayloadError::post_state_mismatch:
      return "post_state_mismatch";
    case PayloadError::effects_hash_mismatch:
      return "effects_hash_mismatch";
    case PayloadError::proof_journal_hash_mismatch:
      return "proof_journal_hash_mismatch";
    case PayloadError::input_message_hash_mismatch:
      return "input_message_hash_mismatch";
    case PayloadError::transaction_start_lt_mismatch:
      return "transaction_start_lt_mismatch";
    case PayloadError::transaction_end_lt_mismatch:
      return "transaction_end_lt_mismatch";
    case PayloadError::gas_used_mismatch:
      return "gas_used_mismatch";
  }
  return "unknown";
}

const char* to_string(PrecommitError error) {
  switch (error) {
    case PrecommitError::none:
      return "none";
    case PrecommitError::size_mismatch:
      return "size_mismatch";
    case PrecommitError::receipt_without_payload:
      return "receipt_without_payload";
    case PrecommitError::payload_without_receipt:
      return "payload_without_receipt";
    case PrecommitError::payload_invalid:
      return "payload_invalid";
    case PrecommitError::receipt_invalid:
      return "receipt_invalid";
  }
  return "unknown";
}

const char* to_string(BasechainLimitError error) {
  switch (error) {
    case BasechainLimitError::none:
      return "none";
    case BasechainLimitError::size_mismatch:
      return "size_mismatch";
    case BasechainLimitError::missing_transaction:
      return "missing_transaction";
    case BasechainLimitError::missing_post_account_state:
      return "missing_post_account_state";
  }
  return "unknown";
}

const char* to_string(OutboundRegistrationError error) {
  switch (error) {
    case OutboundRegistrationError::none:
      return "none";
    case OutboundRegistrationError::missing_transaction:
      return "missing_transaction";
    case OutboundRegistrationError::missing_message:
      return "missing_message";
    case OutboundRegistrationError::invalid_message:
      return "invalid_message";
    case OutboundRegistrationError::message_hash_mismatch:
      return "message_hash_mismatch";
    case OutboundRegistrationError::logical_time_mismatch:
      return "logical_time_mismatch";
  }
  return "unknown";
}

const char* to_string(InboundDescriptorError error) {
  switch (error) {
    case InboundDescriptorError::none:
      return "none";
    case InboundDescriptorError::missing_transaction:
      return "missing_transaction";
    case InboundDescriptorError::missing_inbound_message:
      return "missing_inbound_message";
    case InboundDescriptorError::missing_message_envelope:
      return "missing_message_envelope";
    case InboundDescriptorError::invalid_inbound_message:
      return "invalid_inbound_message";
    case InboundDescriptorError::non_internal_inbound_message:
      return "non_internal_inbound_message";
    case InboundDescriptorError::malformed_message_envelope:
      return "malformed_message_envelope";
    case InboundDescriptorError::envelope_message_mismatch:
      return "envelope_message_mismatch";
    case InboundDescriptorError::cannot_serialize_in_msg:
      return "cannot_serialize_in_msg";
    case InboundDescriptorError::cannot_serialize_out_msg:
      return "cannot_serialize_out_msg";
  }
  return "unknown";
}

}  // namespace ton::validator::parallel_inbound
