// SPDX-License-Identifier: LGPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "block/block-auto.h"
#include "block/block.h"
#include "impl/parallel-transaction-payload.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"

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

td::Ref<vm::Cell> make_transaction(std::uint64_t account, std::uint64_t pre_state,
                                   const td::Bits256& declared_post_state, std::uint64_t lt) {
  block::gen::HASH_UPDATE::Record update_record;
  update_record.old_hash = bits(pre_state);
  update_record.new_hash = declared_post_state;
  td::Ref<vm::Cell> state_update;
  ASSERT_TRUE(block::gen::t_HASH_UPDATE_Account.cell_pack(state_update, update_record));

  block::gen::Transaction::Record transaction;
  transaction.account_addr = bits(account);
  transaction.lt = lt;
  transaction.prev_trans_hash = bits(900 + account);
  transaction.prev_trans_lt = lt - 1;
  transaction.now = 1'700'000'000;
  transaction.outmsg_cnt = 0;
  transaction.orig_status = block::gen::AccountStatus::acc_state_nonexist;
  transaction.end_status = block::gen::AccountStatus::acc_state_nonexist;
  transaction.r1.in_msg = none();
  transaction.r1.out_msgs = none();
  block::CurrencyCollection{0}.pack_to(transaction.total_fees);
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

CanonicalTransactionPayload payload(std::uint64_t account, std::uint64_t pre_state, std::uint64_t lt) {
  auto post = account_none();
  auto transaction = make_transaction(account, pre_state, post->get_hash().as_bits256(), lt);
  return {.transaction_root = std::move(transaction), .post_account_state = std::move(post), .proof_journals = {}};
}

MessageKey input(std::uint64_t lt, std::uint64_t message_hash) {
  return {.lt = lt, .hash = hash(message_hash)};
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
  ASSERT_TRUE(inspected.effects->outbound_messages.empty());

  auto receipt = build_worker_receipt(input(1, 1), 0, canonical).move_as_ok();
  ASSERT_TRUE(validate_worker_payload(canonical, receipt));
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

}  // namespace
}  // namespace ton::validator::parallel_inbound::test
