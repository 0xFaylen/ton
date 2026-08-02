// SPDX-License-Identifier: LGPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "impl/parallel-coordinator-shadow.h"
#include "impl/parallel-transaction-payload.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellUsageTree.h"

namespace ton::validator::parallel_inbound::test {
namespace {

Hash256 hash(std::uint64_t value) {
  Hash256 result{};
  for (std::size_t i = 0; i < 8; ++i) {
    result[result.size() - 1 - i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
  return result;
}

td::Bits256 bits(std::uint64_t value) {
  td::Bits256 result;
  const auto source = hash(value);
  std::memcpy(result.as_slice().data(), source.data(), source.size());
  return result;
}

Hash256 cell_hash(const td::Ref<vm::Cell>& cell) {
  Hash256 result{};
  const auto source = cell->get_hash().as_slice();
  std::memcpy(result.data(), source.data(), result.size());
  return result;
}

bool checkpoints_equal(const std::map<Hash256, AccountCheckpoint>& left,
                       const std::map<Hash256, AccountCheckpoint>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (const auto& [account, checkpoint] : left) {
    const auto found = right.find(account);
    if (found == right.end() || checkpoint.state_hash != found->second.state_hash ||
        checkpoint.next_sequence != found->second.next_sequence ||
        checkpoint.last_transaction_end_lt != found->second.last_transaction_end_lt) {
      return false;
    }
  }
  return true;
}

td::Ref<vm::CellSlice> none() {
  return vm::CellBuilder().store_zeroes(1).as_cellslice_ref();
}

td::Ref<vm::Cell> account_none() {
  return vm::CellBuilder().store_zeroes(1).finalize_novm();
}

td::Ref<vm::CellSlice> std_address(std::uint64_t value) {
  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_long_bool(2, 2));  // addr_std$10
  ASSERT_TRUE(builder.store_zeroes_bool(1));   // anycast:(Maybe Anycast)
  ASSERT_TRUE(builder.store_long_bool(0, 8));  // workchain_id:int8
  ASSERT_TRUE(builder.store_bits_bool(bits(value).cbits(), 256));
  return builder.as_cellslice_ref();
}

td::Ref<vm::Cell> simple_account(std::uint64_t address, std::uint64_t last_transaction_lt) {
  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_ones_bool(1));                           // account$1
  ASSERT_TRUE(builder.append_cellslice_bool(std_address(address)));  // addr:MsgAddressInt
  ASSERT_TRUE(builder.store_zeroes_bool(6));                         // used:StorageUsed
  ASSERT_TRUE(builder.store_zeroes_bool(3));                         // storage_extra_none$000
  ASSERT_TRUE(builder.store_zeroes_bool(32));                        // last_paid:uint32
  ASSERT_TRUE(builder.store_zeroes_bool(1));                         // due_payment:(Maybe Grams)
  ASSERT_TRUE(builder.store_long_bool(last_transaction_lt, 64));
  ASSERT_TRUE(builder.store_zeroes_bool(5));  // balance:CurrencyCollection
  ASSERT_TRUE(builder.store_zeroes_bool(2));  // account_uninit$00
  auto result = builder.finalize_novm();
  ASSERT_TRUE(block::gen::t_Account.validate_ref(result));
  return result;
}

td::Ref<vm::Cell> internal_message(std::uint64_t source, std::uint64_t destination, std::uint64_t created_lt) {
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  info.ihr_disabled = true;
  info.bounce = true;
  info.bounced = false;
  info.src = std_address(source);
  info.dest = std_address(destination);
  info.value = block::CurrencyCollection{0}.pack();
  info.extra_flags = vm::CellBuilder().store_zeroes(4).as_cellslice_ref();
  info.fwd_fee = vm::CellBuilder().store_zeroes(4).as_cellslice_ref();
  info.created_lt = created_lt;
  info.created_at = 1'700'000'000;
  td::Ref<vm::Cell> info_cell;
  ASSERT_TRUE(block::gen::t_CommonMsgInfo.cell_pack(info_cell, info));

  block::gen::Message::Record message;
  message.info = vm::load_cell_slice_ref(info_cell);
  message.init = none();
  message.body = vm::CellBuilder().store_zeroes(1).as_cellslice_ref();
  td::Ref<vm::Cell> result;
  ASSERT_TRUE(block::gen::t_Message_Any.cell_pack(result, message));
  ASSERT_TRUE(block::gen::t_Message_Any.validate_ref(result));
  return result;
}

td::Ref<vm::Cell> message_envelope(const td::Ref<vm::Cell>& message, std::uint64_t forwarding_fee) {
  block::tlb::MsgEnvelope::Record_std envelope;
  envelope.cur_addr = 0;
  envelope.next_addr = 0;
  envelope.fwd_fee_remaining = td::make_refint(forwarding_fee);
  envelope.msg = message;
  td::Ref<vm::Cell> result;
  ASSERT_TRUE(block::tlb::t_MsgEnvelope.pack_cell(result, envelope));
  return result;
}

td::Ref<vm::Cell> skipped_ordinary_description() {
  block::gen::TrComputePhase::Record_tr_phase_compute_skipped skipped;
  skipped.reason = block::gen::ComputeSkipReason::cskip_no_state;
  td::Ref<vm::Cell> compute;
  ASSERT_TRUE(block::gen::t_TrComputePhase.cell_pack(compute, skipped));

  block::gen::TransactionDescr::Record_trans_ord description;
  description.credit_first = false;
  description.storage_ph = none();
  description.credit_ph = none();
  description.compute_ph = vm::load_cell_slice_ref(compute);
  description.action = none();
  description.aborted = true;
  description.bounce = none();
  description.destroyed = false;
  td::Ref<vm::Cell> result;
  ASSERT_TRUE(block::gen::t_TransactionDescr.cell_pack(result, description));
  return result;
}

td::Ref<vm::Cell> make_transaction_with_pre_hash(std::uint64_t account, const td::Bits256& declared_pre_state,
                                                 const td::Bits256& declared_post_state, std::uint64_t lt,
                                                 std::uint64_t total_fees = 0, td::Ref<vm::Cell> in_message = {},
                                                 std::vector<td::Ref<vm::Cell>> out_messages = {}) {
  block::gen::HASH_UPDATE::Record update_record;
  update_record.old_hash = declared_pre_state;
  update_record.new_hash = declared_post_state;
  td::Ref<vm::Cell> state_update;
  ASSERT_TRUE(block::gen::t_HASH_UPDATE_Account.cell_pack(state_update, update_record));

  block::gen::Transaction::Record transaction;
  transaction.account_addr = bits(account);
  transaction.lt = lt;
  transaction.prev_trans_hash = bits(900 + account);
  transaction.prev_trans_lt = lt - 1;
  transaction.now = 1'700'000'000;
  transaction.outmsg_cnt = static_cast<int>(out_messages.size());
  transaction.orig_status = block::gen::AccountStatus::acc_state_nonexist;
  transaction.end_status = block::gen::AccountStatus::acc_state_nonexist;
  vm::CellBuilder in_message_builder;
  ASSERT_TRUE(in_message_builder.store_maybe_ref(std::move(in_message)));
  transaction.r1.in_msg = in_message_builder.as_cellslice_ref();
  vm::Dictionary out_dictionary{15};
  for (unsigned i = 0; i < out_messages.size(); ++i) {
    ASSERT_TRUE(out_dictionary.set_ref(td::BitArray<15>{i}, std::move(out_messages[i]), vm::Dictionary::SetMode::Add));
  }
  vm::CellBuilder out_dictionary_builder;
  ASSERT_TRUE(std::move(out_dictionary).append_dict_to_bool(out_dictionary_builder));
  transaction.r1.out_msgs = out_dictionary_builder.as_cellslice_ref();
  block::CurrencyCollection{static_cast<long long>(total_fees)}.pack_to(transaction.total_fees);
  transaction.state_update = std::move(state_update);
  transaction.description = skipped_ordinary_description();

  td::Ref<vm::Cell> aux;
  ASSERT_TRUE(block::gen::t_Transaction_aux.cell_pack(aux, transaction.r1));
  vm::CellBuilder fees_builder;
  ASSERT_TRUE(block::gen::t_CurrencyCollection.store_from(fees_builder, transaction.total_fees));

  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_long_bool(7, 4));
  ASSERT_TRUE(builder.store_bits_bool(transaction.account_addr.cbits(), 256));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(transaction.lt, 64));
  ASSERT_TRUE(builder.store_bits_bool(transaction.prev_trans_hash.cbits(), 256));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(transaction.prev_trans_lt, 64));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(transaction.now, 32));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(transaction.outmsg_cnt, 15));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(static_cast<unsigned>(transaction.orig_status), 2));
  ASSERT_TRUE(builder.store_ulong_rchk_bool(static_cast<unsigned>(transaction.end_status), 2));
  ASSERT_TRUE(builder.store_ref_bool(std::move(aux)));
  ASSERT_TRUE(block::gen::t_CurrencyCollection.store_from(builder, transaction.total_fees));
  ASSERT_TRUE(builder.store_ref_bool(transaction.state_update));
  ASSERT_TRUE(builder.store_ref_bool(transaction.description));
  td::Ref<vm::Cell> root;
  ASSERT_TRUE(std::move(builder).finalize_to(root));
  ASSERT_TRUE(block::gen::t_Transaction.validate_ref(root));
  return root;
}

td::Ref<vm::Cell> make_transaction(std::uint64_t account, std::uint64_t pre_state,
                                   const td::Bits256& declared_post_state, std::uint64_t lt,
                                   std::uint64_t total_fees = 0, td::Ref<vm::Cell> in_message = {},
                                   std::vector<td::Ref<vm::Cell>> out_messages = {}) {
  return make_transaction_with_pre_hash(account, bits(pre_state), declared_post_state, lt, total_fees,
                                        std::move(in_message), std::move(out_messages));
}

CanonicalTransactionPayload payload(std::uint64_t account, std::uint64_t pre_state, std::uint64_t lt,
                                    std::uint64_t total_fees = 0, td::Ref<vm::Cell> in_message = {},
                                    std::vector<td::Ref<vm::Cell>> out_messages = {}) {
  auto post = account_none();
  auto transaction = make_transaction(account, pre_state, post->get_hash().as_bits256(), lt, total_fees,
                                      std::move(in_message), std::move(out_messages));
  return {.transaction_root = std::move(transaction), .post_account_state = std::move(post), .proof_journals = {}};
}

CanonicalTransactionPayload payload_from_state(std::uint64_t account, const td::Ref<vm::Cell>& pre_state,
                                               std::uint64_t lt, td::Ref<vm::Cell> in_message = {},
                                               std::vector<td::Ref<vm::Cell>> out_messages = {}) {
  auto post = account_none();
  auto transaction =
      make_transaction_with_pre_hash(account, pre_state->get_hash().as_bits256(), post->get_hash().as_bits256(), lt, 0,
                                     std::move(in_message), std::move(out_messages));
  return {.transaction_root = std::move(transaction), .post_account_state = std::move(post), .proof_journals = {}};
}

MessageKey input(std::uint64_t lt, std::uint64_t message_hash) {
  return {.lt = lt, .hash = hash(message_hash)};
}

td::Ref<vm::Cell> shard_account_value(const td::Ref<vm::Cell>& account, std::uint64_t transaction_hash,
                                      std::uint64_t transaction_lt) {
  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_ref_bool(account));
  ASSERT_TRUE(builder.store_bits_bool(bits(transaction_hash).cbits(), 256));
  ASSERT_TRUE(builder.store_long_bool(transaction_lt, 64));
  return builder.finalize_novm();
}

td::Ref<vm::Cell> enqueued_message_value(std::uint64_t enqueued_lt, const td::Ref<vm::Cell>& envelope) {
  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_long_bool(enqueued_lt, 64));
  ASSERT_TRUE(builder.store_ref_bool(envelope));
  return builder.finalize_novm();
}

TEST(ParallelTransactionPayload, DerivesCanonicalReceiptFieldsFromCells) {
  const auto canonical = payload(10, 100, 11);
  const auto inspected = inspect_transaction_payload(canonical);
  ASSERT_TRUE(inspected);
  ASSERT_EQ(inspected.effects->account, hash(10));
  ASSERT_EQ(inspected.effects->pre_account_state_hash, hash(100));
  ASSERT_EQ(inspected.effects->post_account_state_hash, cell_hash(canonical.post_account_state));
  ASSERT_EQ(inspected.effects->transaction_start_lt, 11u);
  ASSERT_EQ(inspected.effects->transaction_end_lt, 12u);
  ASSERT_EQ(inspected.effects->gas_used, 0u);
  ASSERT_EQ(inspected.effects->original_account_status, block::gen::AccountStatus::acc_state_nonexist);
  ASSERT_EQ(inspected.effects->end_account_status, block::gen::AccountStatus::acc_state_nonexist);
  ASSERT_TRUE(inspected.effects->total_fees == block::CurrencyCollection{0});
  ASSERT_EQ(inspected.effects->transaction_root->get_hash(), canonical.transaction_root->get_hash());
  ASSERT_EQ(inspected.effects->post_account_state->get_hash(), canonical.post_account_state->get_hash());
  ASSERT_TRUE(inspected.effects->outbound_messages.empty());

  auto receipt = build_worker_receipt(input(1, 1), 0, canonical).move_as_ok();
  ASSERT_TRUE(validate_worker_payload(canonical, receipt));
}

TEST(ParallelTransactionPayload, CommitsCanonicalTotalFees) {
  const auto zero_fees = inspect_transaction_payload(payload(10, 100, 11, 0));
  const auto nonzero_fees = inspect_transaction_payload(payload(10, 100, 11, 123456));
  ASSERT_TRUE(zero_fees);
  ASSERT_TRUE(nonzero_fees);
  ASSERT_TRUE(nonzero_fees.effects->total_fees == block::CurrencyCollection{123456});
  ASSERT_TRUE(zero_fees.effects->total_fees_hash != nonzero_fees.effects->total_fees_hash);
  ASSERT_TRUE(zero_fees.effects->effects_hash != nonzero_fees.effects->effects_hash);
}

TEST(ParallelTransactionPayload, ReconstructsCanonicalNewOutMsgRegistration) {
  auto first_message = internal_message(10, 20, 100);
  auto second_message = internal_message(10, 30, 101);
  auto inspected = inspect_transaction_payload(payload(10, 100, 11, 0, {}, {first_message, second_message}));
  ASSERT_TRUE(inspected);
  ASSERT_EQ(inspected.effects->outbound_messages.size(), 2u);

  block::MsgMetadata metadata{2, 0, bits(10), 9};
  auto registered =
      materialize_outbound_registrations(*inspected.effects, {.metadata_enabled = true, .metadata = metadata});
  ASSERT_TRUE(registered);
  ASSERT_TRUE(registered.batch);
  ASSERT_EQ(registered.batch->messages.size(), 2u);
  ASSERT_EQ(registered.batch->extra_out_msgs_delta, 2u);
  ASSERT_EQ(registered.batch->min_message_lt, std::optional<ton::LogicalTime>{12});
  ASSERT_EQ(registered.batch->messages[0].lt, 12u);
  ASSERT_EQ(registered.batch->messages[1].lt, 13u);
  ASSERT_EQ(registered.batch->messages[0].msg_idx, 0u);
  ASSERT_EQ(registered.batch->messages[1].msg_idx, 1u);
  ASSERT_EQ(registered.batch->messages[0].msg->get_hash(), first_message->get_hash());
  ASSERT_EQ(registered.batch->messages[1].msg->get_hash(), second_message->get_hash());
  ASSERT_EQ(registered.batch->messages[0].trans->get_hash(), inspected.effects->transaction_root->get_hash());
  ASSERT_TRUE(registered.batch->messages[0].metadata);
  ASSERT_TRUE(registered.batch->messages[0].metadata.value() == metadata);

  auto without_metadata =
      materialize_outbound_registrations(*inspected.effects, {.metadata_enabled = false, .metadata = metadata});
  ASSERT_TRUE(without_metadata);
  ASSERT_TRUE(!without_metadata.batch->messages[0].metadata);

  auto tampered = *inspected.effects;
  tampered.outbound_messages[0].message_hash = hash(999);
  ASSERT_EQ(materialize_outbound_registrations(tampered, {}).error, OutboundRegistrationError::message_hash_mismatch);
  tampered = *inspected.effects;
  ++tampered.outbound_messages[1].logical_time;
  ASSERT_EQ(materialize_outbound_registrations(tampered, {}).error, OutboundRegistrationError::logical_time_mismatch);
}

TEST(ParallelTransactionPayload, MaterializesExactInboundInternalDescriptorPair) {
  auto message = internal_message(20, 10, 100);
  auto inspected = inspect_transaction_payload(payload(10, 100, 11, 0, message));
  ASSERT_TRUE(inspected);
  ASSERT_EQ(inspected.effects->inbound_message_hash, std::optional<Hash256>{cell_hash(message)});
  ASSERT_EQ(inspected.effects->inbound_message->get_hash(), message->get_hash());
  auto bound_receipt =
      build_worker_receipt({.lt = 1, .hash = cell_hash(message)}, 0, payload(10, 100, 11, 0, message)).move_as_ok();
  ASSERT_TRUE(validate_worker_payload(payload(10, 100, 11, 0, message), bound_receipt));
  bound_receipt.input.hash = hash(999);
  ASSERT_EQ(validate_worker_payload(payload(10, 100, 11, 0, message), bound_receipt).error,
            PayloadError::input_message_hash_mismatch);
  auto envelope = message_envelope(message, 777);

  auto descriptors = materialize_inbound_internal_descriptors(
      *inspected.effects, {.message_envelope = envelope, .dequeued_from_current_shard = true});
  ASSERT_TRUE(descriptors);
  ASSERT_TRUE(descriptors.delta);
  ASSERT_EQ(descriptors.delta->message_hash, cell_hash(message));
  ASSERT_TRUE(descriptors.delta->in_msg_descriptor.not_null());
  ASSERT_TRUE(descriptors.delta->out_msg_descriptor.not_null());

  block::gen::InMsg::Record_msg_import_fin in_record;
  ASSERT_TRUE(block::gen::t_InMsg.cell_unpack(descriptors.delta->in_msg_descriptor, in_record));
  ASSERT_EQ(in_record.in_msg->get_hash(), envelope->get_hash());
  ASSERT_EQ(in_record.transaction->get_hash(), inspected.effects->transaction_root->get_hash());
  ASSERT_EQ(td::cmp(block::tlb::t_Grams.as_integer(in_record.fwd_fee), td::make_refint(777)), 0);

  block::gen::OutMsg::Record_msg_export_deq_imm out_record;
  ASSERT_TRUE(block::gen::t_OutMsg.cell_unpack(descriptors.delta->out_msg_descriptor, out_record));
  ASSERT_EQ(out_record.out_msg->get_hash(), envelope->get_hash());
  ASSERT_EQ(out_record.reimport->get_hash(), descriptors.delta->in_msg_descriptor->get_hash());

  auto imported_only = materialize_inbound_internal_descriptors(
      *inspected.effects, {.message_envelope = envelope, .dequeued_from_current_shard = false});
  ASSERT_TRUE(imported_only);
  ASSERT_TRUE(imported_only.delta->out_msg_descriptor.is_null());

  auto other_message = internal_message(30, 10, 101);
  ASSERT_EQ(materialize_inbound_internal_descriptors(*inspected.effects,
                                                     {.message_envelope = message_envelope(other_message, 777)})
                .error,
            InboundDescriptorError::envelope_message_mismatch);
  ASSERT_EQ(materialize_inbound_internal_descriptors(*inspect_transaction_payload(payload(10, 100, 11)).effects,
                                                     {.message_envelope = envelope})
                .error,
            InboundDescriptorError::missing_inbound_message);
}

TEST(ParallelCoordinatorShadow, AtomicallyPublishesAccountMessagesDescriptorsQueueAndFrontier) {
  auto pre_state = vm::CellBuilder().store_ones(1).finalize_novm();
  auto inbound = internal_message(20, 10, 100);
  auto outbound = internal_message(10, 30, 101);
  auto canonical = payload_from_state(10, pre_state, 11, inbound, {outbound});
  auto inspected = inspect_transaction_payload(canonical);
  ASSERT_TRUE(inspected);
  auto envelope = message_envelope(inbound, 777);

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  ShadowCoordinatorState state{limits};
  state.accounts.emplace(hash(10), ShadowAccountState{.state_hash = cell_hash(pre_state), .state = pre_state});
  OutboundQueueKey queue_key;
  ASSERT_TRUE(block::compute_out_msg_queue_key(envelope, queue_key));
  state.outbound_queue_entries.insert(queue_key);

  const WorkItem work{{100, cell_hash(inbound)}, hash(10)};
  CoordinatorCommitContext context{
      .limit = {.account_is_first = true, .charge_gas = true},
      .outbound_registration = {},
      .inbound_descriptor = InboundDescriptorContext{.message_envelope = envelope, .dequeued_from_current_shard = true},
      .outbound_queue_deletion = queue_key};
  auto result = apply_ready_coordinator_prefix_atomic(state, {work}, {CompletionStatus::succeeded},
                                                      {*inspected.effects}, {context});

  ASSERT_TRUE(result);
  ASSERT_EQ(result.decision.committed_count, 1u);
  ASSERT_EQ(result.decision.stop_reason, PrefixStopReason::end_of_input);
  ASSERT_EQ(state.accounts.at(hash(10)).state_hash, inspected.effects->post_account_state_hash);
  ASSERT_EQ(state.accounts.at(hash(10)).last_transaction_hash, inspected.effects->transaction_hash);
  ASSERT_EQ(state.accounts.at(hash(10)).last_transaction_end_lt, inspected.effects->transaction_end_lt);
  ASSERT_EQ(state.in_msg_descriptors.size(), 1u);
  ASSERT_EQ(state.out_msg_descriptors.size(), 1u);
  ASSERT_TRUE(state.outbound_queue_entries.empty());
  ASSERT_EQ(state.new_messages.size(), 1u);
  ASSERT_EQ(state.new_messages.top().msg->get_hash(), outbound->get_hash());
  ASSERT_EQ(state.min_new_message_lt, std::optional<ton::LogicalTime>{12});
  ASSERT_EQ(state.block_limits.transactions, 1u);
  ASSERT_EQ(state.block_limits.accounts, 1u);
  ASSERT_EQ(state.block_limits.extra_out_msgs, 1u);
  ASSERT_EQ(state.processed_upto.last_processed, std::optional<MessageKey>{work.key});
}

TEST(ParallelCoordinatorShadow, NormalStopPublishesOnlyTheReadyPrefixAndCanResume) {
  auto first_pre = vm::CellBuilder().store_long(1, 2).finalize_novm();
  auto second_pre = vm::CellBuilder().store_long(2, 2).finalize_novm();
  auto first_inbound = internal_message(20, 10, 100);
  auto second_inbound = internal_message(30, 11, 101);
  auto first_payload = payload_from_state(10, first_pre, 11, first_inbound);
  auto second_payload = payload_from_state(11, second_pre, 21, second_inbound);
  auto first = inspect_transaction_payload(first_payload);
  auto second = inspect_transaction_payload(second_payload);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  ShadowCoordinatorState state{limits};
  state.accounts.emplace(hash(10), ShadowAccountState{.state_hash = cell_hash(first_pre), .state = first_pre});
  state.accounts.emplace(hash(11), ShadowAccountState{.state_hash = cell_hash(second_pre), .state = second_pre});
  const std::vector<WorkItem> items{{{100, cell_hash(first_inbound)}, hash(10)},
                                    {{101, cell_hash(second_inbound)}, hash(11)}};
  const std::vector<CoordinatorCommitContext> contexts{
      {.limit = {.account_is_first = true},
       .outbound_registration = {},
       .inbound_descriptor = InboundDescriptorContext{.message_envelope = message_envelope(first_inbound, 10)},
       .outbound_queue_deletion = std::nullopt},
      {.limit = {.account_is_first = true},
       .outbound_registration = {},
       .inbound_descriptor = InboundDescriptorContext{.message_envelope = message_envelope(second_inbound, 20)},
       .outbound_queue_deletion = std::nullopt}};

  auto partial = apply_ready_coordinator_prefix_atomic(
      state, items, {CompletionStatus::succeeded, CompletionStatus::pending}, {*first.effects, std::nullopt}, contexts);
  ASSERT_TRUE(partial);
  ASSERT_EQ(partial.decision.committed_count, 1u);
  ASSERT_EQ(partial.decision.stop_reason, PrefixStopReason::pending);
  ASSERT_EQ(state.accounts.at(hash(10)).state_hash, first.effects->post_account_state_hash);
  ASSERT_EQ(state.accounts.at(hash(11)).state_hash, cell_hash(second_pre));
  ASSERT_EQ(state.in_msg_descriptors.size(), 1u);
  ASSERT_EQ(state.processed_upto.last_processed, std::optional<MessageKey>{items[0].key});

  auto resumed = apply_ready_coordinator_prefix_atomic(state, {items[1]}, {CompletionStatus::succeeded},
                                                       {*second.effects}, {contexts[1]});
  ASSERT_TRUE(resumed);
  ASSERT_EQ(state.accounts.at(hash(11)).state_hash, second.effects->post_account_state_hash);
  ASSERT_EQ(state.in_msg_descriptors.size(), 2u);
  ASSERT_EQ(state.processed_upto.last_processed, std::optional<MessageKey>{items[1].key});
}

TEST(ParallelCoordinatorShadow, CommitErrorDiscardsTheEntireCandidatePrefix) {
  auto first_pre = vm::CellBuilder().store_long(1, 2).finalize_novm();
  auto second_pre = vm::CellBuilder().store_long(2, 2).finalize_novm();
  auto first_inbound = internal_message(20, 10, 100);
  auto second_inbound = internal_message(30, 11, 101);
  auto first = inspect_transaction_payload(payload_from_state(10, first_pre, 11, first_inbound));
  auto second = inspect_transaction_payload(payload_from_state(11, second_pre, 21, second_inbound));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  ShadowCoordinatorState state{limits};
  state.accounts.emplace(hash(10), ShadowAccountState{.state_hash = cell_hash(first_pre), .state = first_pre});
  state.accounts.emplace(hash(11), ShadowAccountState{.state_hash = cell_hash(second_pre), .state = second_pre});
  state.in_msg_descriptors.emplace(cell_hash(second_inbound), account_none());
  const std::vector<WorkItem> items{{{100, cell_hash(first_inbound)}, hash(10)},
                                    {{101, cell_hash(second_inbound)}, hash(11)}};
  const std::vector<CoordinatorCommitContext> contexts{
      {.limit = {.account_is_first = true},
       .outbound_registration = {},
       .inbound_descriptor = InboundDescriptorContext{.message_envelope = message_envelope(first_inbound, 10)},
       .outbound_queue_deletion = std::nullopt},
      {.limit = {.account_is_first = true},
       .outbound_registration = {},
       .inbound_descriptor = InboundDescriptorContext{.message_envelope = message_envelope(second_inbound, 20)},
       .outbound_queue_deletion = std::nullopt}};

  auto rejected =
      apply_ready_coordinator_prefix_atomic(state, items, {CompletionStatus::succeeded, CompletionStatus::succeeded},
                                            {*first.effects, *second.effects}, contexts);
  ASSERT_EQ(rejected.error, CoordinatorCommitError::duplicate_in_descriptor);
  ASSERT_EQ(rejected.item_index, std::optional<std::size_t>{1});
  ASSERT_EQ(rejected.decision.stop_reason, PrefixStopReason::commit_failure);
  ASSERT_EQ(state.accounts.at(hash(10)).state_hash, cell_hash(first_pre));
  ASSERT_EQ(state.accounts.at(hash(11)).state_hash, cell_hash(second_pre));
  ASSERT_EQ(state.in_msg_descriptors.size(), 1u);
  ASSERT_EQ(state.block_limits.transactions, 0u);
  ASSERT_TRUE(state.new_messages.empty());
  ASSERT_TRUE(!state.processed_upto.last_processed);
}

TEST(ParallelCoordinatorShadow, RejectsMissingQueueEntryWithoutPublishing) {
  auto pre_state = vm::CellBuilder().store_ones(1).finalize_novm();
  auto inbound = internal_message(20, 10, 100);
  auto inspected = inspect_transaction_payload(payload_from_state(10, pre_state, 11, inbound));
  ASSERT_TRUE(inspected);
  auto envelope = message_envelope(inbound, 777);
  OutboundQueueKey queue_key;
  ASSERT_TRUE(block::compute_out_msg_queue_key(envelope, queue_key));

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  ShadowCoordinatorState state{limits};
  state.accounts.emplace(hash(10), ShadowAccountState{.state_hash = cell_hash(pre_state), .state = pre_state});
  const WorkItem work{{100, cell_hash(inbound)}, hash(10)};
  CoordinatorCommitContext context{
      .limit = {.account_is_first = true},
      .outbound_registration = {},
      .inbound_descriptor = InboundDescriptorContext{.message_envelope = envelope, .dequeued_from_current_shard = true},
      .outbound_queue_deletion = queue_key};

  auto rejected = apply_ready_coordinator_prefix_atomic(state, {work}, {CompletionStatus::succeeded},
                                                        {*inspected.effects}, {context});
  ASSERT_EQ(rejected.error, CoordinatorCommitError::queue_entry_not_found);
  ASSERT_EQ(state.accounts.at(hash(10)).state_hash, cell_hash(pre_state));
  ASSERT_TRUE(state.in_msg_descriptors.empty());
  ASSERT_TRUE(state.out_msg_descriptors.empty());
  ASSERT_EQ(state.block_limits.transactions, 0u);
  ASSERT_TRUE(!state.processed_upto.last_processed);
}

TEST(ParallelCoordinatorShadow, RejectsTamperedCanonicalCellWithoutPublishing) {
  auto pre_state = vm::CellBuilder().store_ones(1).finalize_novm();
  auto inbound = internal_message(20, 10, 100);
  auto inspected = inspect_transaction_payload(payload_from_state(10, pre_state, 11, inbound));
  ASSERT_TRUE(inspected);

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  ShadowCoordinatorState state{limits};
  state.accounts.emplace(hash(10), ShadowAccountState{.state_hash = cell_hash(pre_state), .state = pre_state});
  auto tampered = *inspected.effects;
  tampered.post_account_state = pre_state;
  const WorkItem work{{100, cell_hash(inbound)}, hash(10)};
  CoordinatorCommitContext context{
      .limit = {.account_is_first = true},
      .outbound_registration = {},
      .inbound_descriptor = InboundDescriptorContext{.message_envelope = message_envelope(inbound, 777)},
      .outbound_queue_deletion = std::nullopt};

  auto rejected =
      apply_ready_coordinator_prefix_atomic(state, {work}, {CompletionStatus::succeeded}, {tampered}, {context});
  ASSERT_EQ(rejected.error, CoordinatorCommitError::post_state_cell_hash_mismatch);
  ASSERT_EQ(state.accounts.at(hash(10)).state_hash, cell_hash(pre_state));
  ASSERT_EQ(state.block_limits.transactions, 0u);
  ASSERT_TRUE(state.in_msg_descriptors.empty());
  ASSERT_TRUE(!state.processed_upto.last_processed);
}

TEST(ParallelTransactionPayload, AppliesBasechainBlockLimitEffectsAtomically) {
  const auto first = inspect_transaction_payload(payload(10, 100, 11, 10));
  const auto second = inspect_transaction_payload(payload(10, 200, 21, 20));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  block::BlockLimitStatus actual{limits, 5};
  block::BlockLimitStatus expected{limits, 5};
  const std::vector<CanonicalTransactionEffects> effects{*first.effects, *second.effects};

  expected.update_lt(first.effects->transaction_end_lt);
  expected.update_gas(first.effects->gas_used);
  expected.add_proof(first.effects->post_account_state);
  expected.add_cell(first.effects->transaction_root);
  expected.add_transaction();
  expected.add_account(true);
  expected.update_lt(second.effects->transaction_end_lt);
  expected.update_gas(second.effects->gas_used);
  expected.add_proof(second.effects->post_account_state);
  expected.add_cell(second.effects->transaction_root);
  expected.add_transaction();
  expected.add_account(false);

  const auto applied =
      apply_basechain_block_limits_atomic(actual, effects, {{.account_is_first = true}, {.account_is_first = false}});
  ASSERT_TRUE(applied);
  ASSERT_EQ(applied.applied_transactions, 2u);
  ASSERT_EQ(actual.cur_lt, expected.cur_lt);
  ASSERT_EQ(actual.gas_used, expected.gas_used);
  ASSERT_EQ(actual.accounts, expected.accounts);
  ASSERT_EQ(actual.transactions, expected.transactions);
  ASSERT_EQ(actual.st_stat.get_total_stat(), expected.st_stat.get_total_stat());
  ASSERT_EQ(actual.estimate_block_size(), expected.estimate_block_size());

  const auto before_lt = actual.cur_lt;
  const auto before_gas = actual.gas_used;
  const auto before_accounts = actual.accounts;
  const auto before_transactions = actual.transactions;
  const auto before_stat = actual.st_stat.get_total_stat();
  const auto before_size = actual.estimate_block_size();
  auto invalid = effects;
  invalid[1].post_account_state.clear();
  const auto rejected =
      apply_basechain_block_limits_atomic(actual, invalid, {{.account_is_first = true}, {.account_is_first = false}});
  ASSERT_EQ(rejected.error, BasechainLimitError::missing_post_account_state);
  ASSERT_EQ(rejected.applied_transactions, 0u);
  ASSERT_EQ(actual.cur_lt, before_lt);
  ASSERT_EQ(actual.gas_used, before_gas);
  ASSERT_EQ(actual.accounts, before_accounts);
  ASSERT_EQ(actual.transactions, before_transactions);
  ASSERT_EQ(actual.st_stat.get_total_stat(), before_stat);
  ASSERT_EQ(actual.estimate_block_size(), before_size);

  ASSERT_EQ(apply_basechain_block_limits_atomic(actual, effects, {{.account_is_first = true}}).error,
            BasechainLimitError::size_mismatch);
}

TEST(ParallelTransactionPayload, AppliesPerTransactionGasPolicy) {
  auto charged = inspect_transaction_payload(payload(10, 100, 11));
  auto free = inspect_transaction_payload(payload(20, 200, 21));
  ASSERT_TRUE(charged);
  ASSERT_TRUE(free);
  charged.effects->gas_used = 70;
  free.effects->gas_used = 90;

  block::BlockLimits limits;
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  limits.usage_tree = usage_tree.get();
  block::BlockLimitStatus status{limits};
  const auto applied = apply_basechain_block_limits_atomic(
      status, {*charged.effects, *free.effects},
      {{.account_is_first = true, .charge_gas = true}, {.account_is_first = true, .charge_gas = false}});
  ASSERT_TRUE(applied);
  ASSERT_EQ(status.gas_used, 70u);
}

TEST(ParallelTransactionPayload, RejectsEveryTamperedDerivedHeaderField) {
  const auto canonical = payload(10, 100, 11);
  const auto valid = build_worker_receipt(input(1, 1), 0, canonical).move_as_ok();

  auto tampered = valid;
  tampered.account = hash(999);
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::account_mismatch);
  tampered = valid;
  tampered.pre_account_state_hash = hash(999);
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::pre_state_mismatch);
  tampered = valid;
  tampered.transaction_hash = hash(999);
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::transaction_hash_mismatch);
  tampered = valid;
  tampered.post_account_state_hash = hash(999);
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::post_state_mismatch);
  tampered = valid;
  tampered.effects_hash = hash(999);
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::effects_hash_mismatch);
  tampered = valid;
  tampered.proof_journal_hash = hash(999);
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::proof_journal_hash_mismatch);
  tampered = valid;
  ++tampered.transaction_start_lt;
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::transaction_start_lt_mismatch);
  tampered = valid;
  ++tampered.transaction_end_lt;
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::transaction_end_lt_mismatch);
  tampered = valid;
  ++tampered.gas_used;
  ASSERT_EQ(validate_worker_payload(canonical, tampered).error, PayloadError::gas_used_mismatch);
}

TEST(ParallelTransactionPayload, RejectsMissingMalformedAndMismatchedCells) {
  auto canonical = payload(10, 100, 11);
  auto malformed = canonical;
  malformed.transaction_root.clear();
  ASSERT_EQ(inspect_transaction_payload(malformed).error, PayloadError::missing_transaction);

  malformed = canonical;
  malformed.transaction_root = vm::CellBuilder().store_long(7, 3).finalize_novm();
  ASSERT_EQ(inspect_transaction_payload(malformed).error, PayloadError::malformed_transaction);

  malformed = canonical;
  malformed.post_account_state.clear();
  ASSERT_EQ(inspect_transaction_payload(malformed).error, PayloadError::missing_post_account_state);

  malformed = canonical;
  malformed.transaction_root = make_transaction(10, 100, td::Bits256::zero(), 11);
  ASSERT_EQ(inspect_transaction_payload(malformed).error, PayloadError::post_state_hash_mismatch);

  auto invalid_state = vm::CellBuilder().store_ones(8).finalize_novm();
  malformed = {.transaction_root = make_transaction(10, 100, invalid_state->get_hash().as_bits256(), 11),
               .post_account_state = std::move(invalid_state),
               .proof_journals = {}};
  ASSERT_EQ(inspect_transaction_payload(malformed).error, PayloadError::invalid_post_account_state);
}

TEST(ParallelTransactionPayload, RequiresCanonicalProofJournalOrder) {
  auto canonical = payload(10, 100, 11);
  canonical.proof_journals.emplace_back(bits(1));
  canonical.proof_journals.emplace_back(bits(2));
  auto ordered = canonical.proof_journals;
  std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
    return std::make_pair(left.anchor_root_hash(), left.commitment()) <
           std::make_pair(right.anchor_root_hash(), right.commitment());
  });
  canonical.proof_journals = std::move(ordered);
  ASSERT_TRUE(inspect_transaction_payload(canonical));

  std::reverse(canonical.proof_journals.begin(), canonical.proof_journals.end());
  ASSERT_EQ(inspect_transaction_payload(canonical).error, PayloadError::non_canonical_proof_journal_order);
}

TEST(ParallelTransactionPayload, BatchPrecommitIsAtomicOnPayloadOrChainFailure) {
  const std::vector<WorkItem> items{{input(1, 1), hash(10)}, {input(1, 2), hash(20)}};
  const std::vector<std::optional<CanonicalTransactionPayload>> payloads{payload(10, 100, 11), payload(20, 200, 21)};
  const std::vector<std::optional<WorkerReceipt>> receipts{
      build_worker_receipt(items[0].key, 0, *payloads[0]).move_as_ok(),
      build_worker_receipt(items[1].key, 0, *payloads[1]).move_as_ok()};
  const std::map<Hash256, AccountCheckpoint> initial{
      {hash(10), {.state_hash = hash(100), .next_sequence = 0, .last_transaction_end_lt = 10}},
      {hash(20), {.state_hash = hash(200), .next_sequence = 0, .last_transaction_end_lt = 20}}};

  const auto valid = validate_precommit_set(items, receipts, payloads, initial);
  ASSERT_TRUE(valid);
  ASSERT_EQ(valid.verified_payloads, 2u);
  ASSERT_EQ(valid.checkpoints.at(hash(10)).next_sequence, 1u);

  auto tampered_receipts = receipts;
  tampered_receipts[1]->effects_hash = hash(999);
  auto rejected = validate_precommit_set(items, tampered_receipts, payloads, initial);
  ASSERT_EQ(rejected.error, PrecommitError::payload_invalid);
  ASSERT_EQ(rejected.payload_error, PayloadError::effects_hash_mismatch);
  ASSERT_TRUE(checkpoints_equal(rejected.checkpoints, initial));

  tampered_receipts = receipts;
  tampered_receipts[1]->account_sequence = 1;
  rejected = validate_precommit_set(items, tampered_receipts, payloads, initial);
  ASSERT_EQ(rejected.error, PrecommitError::receipt_invalid);
  ASSERT_EQ(rejected.receipt_error, ReceiptError::account_sequence_mismatch);
  ASSERT_EQ(rejected.verified_payloads, 0u);
  ASSERT_TRUE(checkpoints_equal(rejected.checkpoints, initial));

  auto missing_payload = payloads;
  missing_payload[0].reset();
  rejected = validate_precommit_set(items, receipts, missing_payload, initial);
  ASSERT_EQ(rejected.error, PrecommitError::receipt_without_payload);
  ASSERT_TRUE(checkpoints_equal(rejected.checkpoints, initial));
}

TEST(ParallelCoordinatorShadow, AppliesAugmentedDictionaryDeltasAtomically) {
  constexpr int global_version = 15;
  auto first_account = simple_account(1, 10);
  auto second_account = simple_account(2, 20);
  auto post_account = simple_account(1, 11);
  auto first_value = shard_account_value(first_account, 100, 10);
  auto second_value = shard_account_value(second_account, 200, 20);
  vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
  ASSERT_TRUE(accounts.set(bits(1), vm::load_cell_slice(first_value), vm::Dictionary::SetMode::Add));
  ASSERT_TRUE(accounts.set(bits(2), vm::load_cell_slice(second_value), vm::Dictionary::SetMode::Add));

  block::tlb::Aug_InMsgDescr in_augmentation{global_version};
  block::tlb::Aug_OutMsgDescr out_augmentation{global_version};
  vm::AugmentedDictionary in_descriptors{256, in_augmentation};
  vm::AugmentedDictionary out_descriptors{256, out_augmentation};

  auto message = internal_message(1, 2, 30);
  auto envelope = message_envelope(message, 7);
  OutboundQueueKey queue_key;
  ASSERT_TRUE(block::compute_out_msg_queue_key(envelope, queue_key));
  auto queue_value = enqueued_message_value(30, envelope);
  vm::AugmentedDictionary queue{352, block::tlb::aug_OutMsgQueue};
  ASSERT_TRUE(queue.set(queue_key, vm::load_cell_slice(queue_value), vm::Dictionary::SetMode::Add));

  AugmentedDictionarySeed seed{.global_version = global_version,
                               .shard_accounts_root = accounts.get_wrapped_dict_root(),
                               .in_msg_descr_root = in_descriptors.get_wrapped_dict_root(),
                               .out_msg_descr_root = out_descriptors.get_wrapped_dict_root(),
                               .out_msg_queue_root = queue.get_wrapped_dict_root()};
  AccountDictionaryDelta account_delta{.account = hash(1),
                                       .post_account_state = post_account,
                                       .last_transaction_hash = hash(101),
                                       .last_transaction_lt = 11,
                                       .existed_before = true,
                                       .exists_after = true};
  QueueDictionaryDelta queue_deletion{
      .key = queue_key, .expected_value = queue_value, .post_value = {}, .existed_before = true, .exists_after = false};

  auto expected_accounts = accounts;
  auto expected_first_value = shard_account_value(post_account, 101, 11);
  ASSERT_TRUE(
      expected_accounts.set(bits(1), vm::load_cell_slice(expected_first_value), vm::Dictionary::SetMode::Replace));
  auto expected_queue = queue;
  ASSERT_TRUE(expected_queue.lookup_delete(queue_key).not_null());

  const auto applied = apply_augmented_dictionary_deltas_atomic(seed, {account_delta}, {}, {}, {queue_deletion});
  ASSERT_TRUE(applied);
  ASSERT_TRUE(applied.roots.has_value());
  ASSERT_EQ(applied.roots->shard_accounts_root->get_hash(), expected_accounts.get_wrapped_dict_root()->get_hash());
  ASSERT_EQ(applied.roots->in_msg_descr_root->get_hash(), seed.in_msg_descr_root->get_hash());
  ASSERT_EQ(applied.roots->out_msg_descr_root->get_hash(), seed.out_msg_descr_root->get_hash());
  ASSERT_EQ(applied.roots->out_msg_queue_root->get_hash(), expected_queue.get_wrapped_dict_root()->get_hash());
}

TEST(ParallelCoordinatorShadow, RejectsQueueValueMismatchWithoutPublishingDictionaryRoots) {
  constexpr int global_version = 15;
  auto original_account = simple_account(1, 10);
  auto post_account = simple_account(1, 11);
  vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
  auto account_value = shard_account_value(original_account, 100, 10);
  ASSERT_TRUE(accounts.set(bits(1), vm::load_cell_slice(account_value), vm::Dictionary::SetMode::Add));
  block::tlb::Aug_InMsgDescr in_augmentation{global_version};
  block::tlb::Aug_OutMsgDescr out_augmentation{global_version};
  vm::AugmentedDictionary in_descriptors{256, in_augmentation};
  vm::AugmentedDictionary out_descriptors{256, out_augmentation};

  auto message = internal_message(1, 2, 30);
  auto envelope = message_envelope(message, 7);
  OutboundQueueKey queue_key;
  ASSERT_TRUE(block::compute_out_msg_queue_key(envelope, queue_key));
  auto queue_value = enqueued_message_value(30, envelope);
  vm::AugmentedDictionary queue{352, block::tlb::aug_OutMsgQueue};
  ASSERT_TRUE(queue.set(queue_key, vm::load_cell_slice(queue_value), vm::Dictionary::SetMode::Add));

  AugmentedDictionarySeed seed{.global_version = global_version,
                               .shard_accounts_root = accounts.get_wrapped_dict_root(),
                               .in_msg_descr_root = in_descriptors.get_wrapped_dict_root(),
                               .out_msg_descr_root = out_descriptors.get_wrapped_dict_root(),
                               .out_msg_queue_root = queue.get_wrapped_dict_root()};
  const auto account_root_before = seed.shard_accounts_root->get_hash();
  const auto queue_root_before = seed.out_msg_queue_root->get_hash();
  AccountDictionaryDelta account_delta{.account = hash(1),
                                       .post_account_state = post_account,
                                       .last_transaction_hash = hash(101),
                                       .last_transaction_lt = 11,
                                       .existed_before = true,
                                       .exists_after = true};
  QueueDictionaryDelta bad_deletion{.key = queue_key,
                                    .expected_value = enqueued_message_value(31, envelope),
                                    .post_value = {},
                                    .existed_before = true,
                                    .exists_after = false};

  const auto rejected = apply_augmented_dictionary_deltas_atomic(seed, {account_delta}, {}, {}, {bad_deletion});
  ASSERT_EQ(rejected.error, AugmentedDictionaryCommitError::queue_value_mismatch);
  ASSERT_TRUE(!rejected.roots.has_value());
  ASSERT_EQ(seed.shard_accounts_root->get_hash(), account_root_before);
  ASSERT_EQ(seed.out_msg_queue_root->get_hash(), queue_root_before);
  vm::AugmentedDictionary unchanged_accounts{vm::load_cell_slice_ref(seed.shard_accounts_root), 256,
                                             block::tlb::aug_ShardAccounts};
  ASSERT_EQ(unchanged_accounts.lookup(bits(1))->prefetch_ref()->get_hash(), original_account->get_hash());
  vm::AugmentedDictionary unchanged_queue{vm::load_cell_slice_ref(seed.out_msg_queue_root), 352,
                                          block::tlb::aug_OutMsgQueue};
  ASSERT_TRUE(unchanged_queue.lookup(queue_key).not_null());
}

TEST(ParallelCoordinatorShadow, AppliesQueueAddAndReplaceInTheAtomicDictionaryCommit) {
  constexpr int global_version = 15;
  vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
  block::tlb::Aug_InMsgDescr in_augmentation{global_version};
  block::tlb::Aug_OutMsgDescr out_augmentation{global_version};
  vm::AugmentedDictionary in_descriptors{256, in_augmentation};
  vm::AugmentedDictionary out_descriptors{256, out_augmentation};

  auto first_envelope = message_envelope(internal_message(1, 2, 30), 7);
  auto second_envelope = message_envelope(internal_message(1, 3, 40), 8);
  OutboundQueueKey first_key;
  OutboundQueueKey second_key;
  ASSERT_TRUE(block::compute_out_msg_queue_key(first_envelope, first_key));
  ASSERT_TRUE(block::compute_out_msg_queue_key(second_envelope, second_key));
  auto first_value = enqueued_message_value(30, first_envelope);
  auto replaced_first_value = enqueued_message_value(31, first_envelope);
  auto second_value = enqueued_message_value(40, second_envelope);
  vm::AugmentedDictionary queue{352, block::tlb::aug_OutMsgQueue};
  ASSERT_TRUE(queue.set(first_key, vm::load_cell_slice(first_value), vm::Dictionary::SetMode::Add));

  AugmentedDictionarySeed seed{.global_version = global_version,
                               .shard_accounts_root = accounts.get_wrapped_dict_root(),
                               .in_msg_descr_root = in_descriptors.get_wrapped_dict_root(),
                               .out_msg_descr_root = out_descriptors.get_wrapped_dict_root(),
                               .out_msg_queue_root = queue.get_wrapped_dict_root()};
  std::vector<QueueDictionaryDelta> queue_deltas{{.key = first_key,
                                                  .expected_value = first_value,
                                                  .post_value = replaced_first_value,
                                                  .existed_before = true,
                                                  .exists_after = true},
                                                 {.key = second_key,
                                                  .expected_value = {},
                                                  .post_value = second_value,
                                                  .existed_before = false,
                                                  .exists_after = true}};

  auto expected_queue = queue;
  ASSERT_TRUE(
      expected_queue.set(first_key, vm::load_cell_slice(replaced_first_value), vm::Dictionary::SetMode::Replace));
  ASSERT_TRUE(expected_queue.set(second_key, vm::load_cell_slice(second_value), vm::Dictionary::SetMode::Add));

  const auto applied = apply_augmented_dictionary_deltas_atomic(seed, {}, {}, {}, queue_deltas);
  ASSERT_TRUE(applied);
  ASSERT_TRUE(applied.roots.has_value());
  ASSERT_EQ(applied.roots->out_msg_queue_root->get_hash(), expected_queue.get_wrapped_dict_root()->get_hash());
}

TEST(ParallelCoordinatorShadow, PreservesEmptyShardAccountsRepresentation) {
  constexpr int global_version = 15;
  auto original_account = simple_account(1, 10);
  vm::AugmentedDictionary accounts{256, block::tlb::aug_ShardAccounts};
  auto account_value = shard_account_value(original_account, 100, 10);
  ASSERT_TRUE(accounts.set(bits(1), vm::load_cell_slice(account_value), vm::Dictionary::SetMode::Add));
  block::tlb::Aug_InMsgDescr in_augmentation{global_version};
  block::tlb::Aug_OutMsgDescr out_augmentation{global_version};
  vm::AugmentedDictionary in_descriptors{256, in_augmentation};
  vm::AugmentedDictionary out_descriptors{256, out_augmentation};
  vm::AugmentedDictionary queue{352, block::tlb::aug_OutMsgQueue};
  AugmentedDictionarySeed seed{.global_version = global_version,
                               .shard_accounts_root = accounts.get_wrapped_dict_root(),
                               .in_msg_descr_root = in_descriptors.get_wrapped_dict_root(),
                               .out_msg_descr_root = out_descriptors.get_wrapped_dict_root(),
                               .out_msg_queue_root = queue.get_wrapped_dict_root()};
  AccountDictionaryDelta deletion{.account = hash(1),
                                  .post_account_state = {},
                                  .last_transaction_hash = {},
                                  .last_transaction_lt = 0,
                                  .existed_before = true,
                                  .exists_after = false};

  const auto applied = apply_augmented_dictionary_deltas_atomic(seed, {deletion}, {}, {}, {});
  ASSERT_TRUE(applied);
  ASSERT_TRUE(applied.roots.has_value());
  vm::AugmentedDictionary empty_accounts{vm::load_cell_slice_ref(applied.roots->shard_accounts_root), 256,
                                         block::tlb::aug_ShardAccounts};
  ASSERT_TRUE(empty_accounts.is_valid());
  ASSERT_TRUE(empty_accounts.get_root_cell().is_null());
}

}  // namespace
}  // namespace ton::validator::parallel_inbound::test
