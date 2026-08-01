// SPDX-License-Identifier: LGPL-2.0-or-later

#include "td/utils/tests.h"
#include "validator/interfaces/tvm-hotpath-stats.h"
#include "validator/interfaces/validator-manager.h"

namespace ton::validator::test {
namespace {

td::Bits256 make_hash(td::uint32 value) {
  td::Bits256 result = td::Bits256::zero();
  result.as_array()[28] = static_cast<unsigned char>(value >> 24);
  result.as_array()[29] = static_cast<unsigned char>(value >> 16);
  result.as_array()[30] = static_cast<unsigned char>(value >> 8);
  result.as_array()[31] = static_cast<unsigned char>(value);
  return result;
}

td::RealCpuTimer::Time make_time(double real, double cpu) {
  return {.real = real, .cpu = cpu};
}

TEST(TvmHotpathStats, AggregatesCodeHashAndAccountConcentration) {
  TvmHotpathStats stats;
  const auto code_hash = make_hash(1);
  const auto first_account = make_hash(100);
  const auto second_account = make_hash(200);

  stats.record(code_hash, basechainId, first_account, make_time(0.1, 0.08), 100, 100, 10);
  stats.record(code_hash, basechainId, first_account, make_time(0.2, 0.16), 200, 200, 20);
  stats.record(code_hash, basechainId, second_account, make_time(0.3, 0.24), 300, 1000, 30);

  ASSERT_EQ(stats.total_executions(), 3u);
  ASSERT_EQ(stats.size(), 1u);
  const auto entries = stats.entries_by_wall();
  ASSERT_EQ(entries[0].executions, 3u);
  ASSERT_EQ(entries[0].vm_gas_used, 600u);
  ASSERT_EQ(entries[0].billed_gas_used, 1300u);
  ASSERT_EQ(entries[0].vm_steps, 60u);
  ASSERT_TRUE(std::abs(entries[0].observed_time.real - 0.6) < 1e-12);
  ASSERT_TRUE(std::abs(entries[0].observed_time.cpu - 0.48) < 1e-12);
  const auto top1 = entries[0].top_account_share(1);
  ASSERT_TRUE(std::abs(top1.first - 2.0 / 3.0) < 1e-12);
  ASSERT_TRUE(std::abs(top1.second - 2.0 / 3.0) < 1e-12);
  const auto top10 = entries[0].top_account_share(10);
  ASSERT_EQ(top10.first, 1.0);
  ASSERT_EQ(top10.second, 1.0);
}

TEST(TvmHotpathStats, ReplacesSmallestEntryAndReportsError) {
  TvmHotpathStats stats(2);
  const auto account = make_hash(100);
  stats.record(make_hash(1), basechainId, account, make_time(0.1, 0.1), 1, 1, 1);
  stats.record(make_hash(2), basechainId, account, make_time(0.2, 0.2), 1, 1, 1);
  stats.record(make_hash(3), basechainId, account, make_time(0.5, 0.5), 1, 1, 1);

  ASSERT_EQ(stats.size(), 2u);
  ASSERT_EQ(stats.replacements(), 1u);
  const auto entries = stats.entries_by_wall();
  ASSERT_EQ(entries[0].code_hash, make_hash(3));
  ASSERT_TRUE(std::abs(entries[0].estimated_wall - 0.6) < 1e-12);
  ASSERT_TRUE(std::abs(entries[0].wall_error - 0.1) < 1e-12);
  ASSERT_EQ(entries[0].observed_time.real, 0.5);
}

TEST(TvmHotpathStats, MergePreservesTotalsAndCapacity) {
  TvmHotpathStats first(2);
  TvmHotpathStats second(2);
  const auto account = make_hash(100);
  first.record(make_hash(1), basechainId, account, make_time(0.1, 0.08), 10, 10, 1);
  second.record(make_hash(1), basechainId, account, make_time(0.2, 0.16), 20, 20, 2);
  second.record(make_hash(2), basechainId, account, make_time(0.3, 0.24), 30, 30, 3);

  first.merge(second);

  ASSERT_EQ(first.total_executions(), 3u);
  ASSERT_EQ(first.size(), 2u);
  ASSERT_TRUE(std::abs(first.total_time().real - 0.6) < 1e-12);
  const auto entries = first.entries_by_wall();
  ASSERT_EQ(entries[0].code_hash, make_hash(1));
  ASSERT_EQ(entries[0].executions, 2u);
  ASSERT_EQ(entries[0].vm_gas_used, 30u);
  ASSERT_EQ(entries[0].billed_gas_used, 30u);
}

TEST(TvmHotpathStats, ZeroCapacityKeepsOnlyBoundedTotals) {
  TvmHotpathStats stats(0);
  stats.record(make_hash(1), basechainId, make_hash(2), make_time(0.1, 0.08), 10, 10, 1);

  ASSERT_EQ(stats.size(), 0u);
  ASSERT_EQ(stats.total_executions(), 1u);
  ASSERT_EQ(stats.total_time().real, 0.1);
}

TEST(TvmHotpathStats, BoundsAccountCardinalityAndSerializesGasDifference) {
  TvmHotpathStats stats;
  const auto code_hash = make_hash(1);
  for (td::uint32 i = 0; i < 20; ++i) {
    stats.record(code_hash, basechainId, make_hash(100 + i), make_time(0.01, 0.008), 100, 50, 10);
  }

  const auto entries = stats.entries_by_wall();
  ASSERT_EQ(entries[0].accounts_size, TvmHotpathStats::max_accounts_per_code);
  const auto top1 = entries[0].top_account_share(1);
  ASSERT_TRUE(top1.first <= top1.second);
  const auto output = stats.to_str(false);
  ASSERT_TRUE(output.find("vm_gas=2000") != std::string::npos);
  ASSERT_TRUE(output.find("billed_gas=1000") != std::string::npos);
  ASSERT_TRUE(output.find("top10_share=[") != std::string::npos);
}

TEST(TvmHotpathStats, DefaultOnlineModeRetainsAtMostSixtyFourCodeHashes) {
  TvmHotpathStats stats;
  for (td::uint32 code = 0; code < 100; ++code) {
    stats.record(make_hash(code), basechainId, make_hash(1000 + code), make_time(0.001, 0.0008), 10, 10, 1);
  }

  ASSERT_TRUE(!stats.is_exact());
  ASSERT_EQ(stats.capacity(), TvmHotpathStats::default_capacity);
  ASSERT_EQ(stats.size(), TvmHotpathStats::default_capacity);
  ASSERT_EQ(stats.replacements(), 100u - TvmHotpathStats::default_capacity);
  ASSERT_TRUE(stats.to_json(false).find("\"exact_complete\":false") != std::string::npos);
  ASSERT_TRUE(stats.entries_by_wall(1)[0].accounts_by_execution(0).empty());
}

TEST(TvmHotpathStats, ExactModeRetainsEveryCodeHashAndAccount) {
  TvmHotpathStats stats;
  stats.enable_exact();
  for (td::uint32 code = 0; code < 100; ++code) {
    for (td::uint32 account = 0; account <= code; ++account) {
      stats.record(make_hash(code), basechainId, make_hash(1000 + account), make_time(0.001, 0.0008), 10, 10, 1);
    }
  }

  ASSERT_TRUE(stats.is_exact());
  ASSERT_EQ(stats.size(), 100u);
  ASSERT_EQ(stats.replacements(), 0u);
  const auto entries = stats.entries_by_wall(100);
  ASSERT_EQ(entries[0].code_hash, make_hash(99));
  ASSERT_TRUE(entries[0].exact_accounts);
  ASSERT_EQ(entries[0].estimated_distinct_accounts(), 100u);
  const auto top1 = entries[0].top_account_share(1);
  ASSERT_TRUE(std::abs(top1.first - 0.01) < 1e-12);
  ASSERT_EQ(top1.first, top1.second);
}

TEST(TvmHotpathStats, ExactModeMergesAndExportsPaginatedJson) {
  TvmHotpathStats first;
  TvmHotpathStats second;
  first.enable_exact();
  second.enable_exact();
  first.record(make_hash(1), basechainId, make_hash(101), make_time(0.1, 0.08), 10, 11, 1);
  second.record(make_hash(1), basechainId, make_hash(102), make_time(0.3, 0.24), 20, 22, 2);
  second.record(make_hash(2), basechainId, make_hash(103), make_time(0.2, 0.16), 30, 33, 3);

  first.merge(second);

  ASSERT_EQ(first.size(), 2u);
  const auto entries = first.entries_by_wall(2);
  ASSERT_EQ(entries[0].code_hash, make_hash(1));
  ASSERT_EQ(entries[0].estimated_distinct_accounts(), 2u);
  const auto json = first.to_json(false, 1, 1);
  ASSERT_TRUE(json.find("\"exact\":true") != std::string::npos);
  ASSERT_TRUE(json.find("\"total_entries\":2") != std::string::npos);
  ASSERT_TRUE(json.find("\"offset\":1") != std::string::npos);
  ASSERT_TRUE(json.find("\"returned\":1") != std::string::npos);
  ASSERT_TRUE(json.find(make_hash(2).to_hex()) != std::string::npos);
}

TEST(TvmHotpathStats, RecordsAccountWorkOnlyInExactReplayMode) {
  using Phase = TvmHotpathStats::AccountWorkPhase;
  TvmHotpathStats bounded;
  bounded.record_account_work(Phase::inbound_internal, basechainId, make_hash(100), make_time(1.0, 0.8));
  ASSERT_TRUE(bounded.account_work_entries().empty());
  ASSERT_TRUE(!bounded.account_work_complete());

  TvmHotpathStats first;
  TvmHotpathStats second;
  first.enable_exact();
  second.enable_exact();
  first.record_account_work(Phase::inbound_internal, basechainId, make_hash(100), make_time(0.3, 0.2));
  second.record_account_work(Phase::inbound_internal, basechainId, make_hash(100), make_time(0.2, 0.1));
  second.record_account_work(Phase::external, basechainId, make_hash(100), make_time(0.5, 0.4));
  second.record_account_work(Phase::new_or_deferred, basechainId, make_hash(200), make_time(0.4, 0.3));

  first.merge(second);
  first.scale(0.5);

  ASSERT_TRUE(first.account_work_complete());
  const auto work = first.account_work_entries();
  ASSERT_EQ(work.size(), 3u);
  ASSERT_EQ(work[0].phase, Phase::inbound_internal);
  ASSERT_EQ(work[0].executions, 1u);
  ASSERT_TRUE(std::abs(work[0].observed_time.real - 0.25) < 1e-12);
  ASSERT_TRUE(std::abs(work[0].observed_time.cpu - 0.15) < 1e-12);
  ASSERT_EQ(work[1].phase, Phase::external);
  ASSERT_EQ(work[1].executions, 1u);
  ASSERT_TRUE(std::abs(work[1].observed_time.real - 0.25) < 1e-12);
  ASSERT_TRUE(std::abs(work[1].observed_time.cpu - 0.2) < 1e-12);
  ASSERT_EQ(work[2].phase, Phase::new_or_deferred);
  ASSERT_EQ(work[2].account.address, make_hash(200));
  ASSERT_TRUE(std::abs(work[2].observed_time.real - 0.2) < 1e-12);
  ASSERT_TRUE(std::abs(work[2].observed_time.cpu - 0.15) < 1e-12);
}

TEST(TvmHotpathStats, TreatsSameAddressInDifferentWorkchainsAsDistinctAccounts) {
  TvmHotpathStats stats;
  stats.enable_exact();
  const auto code_hash = make_hash(1);
  const auto address = make_hash(2);
  stats.record(code_hash, basechainId, address, make_time(0.1, 0.08), 10, 10, 1);
  stats.record(code_hash, masterchainId, address, make_time(0.1, 0.08), 10, 10, 1);

  const auto entries = stats.entries_by_wall(1);
  ASSERT_EQ(entries[0].estimated_distinct_accounts(), 2u);
  const auto json = stats.to_json(false);
  ASSERT_TRUE(json.find("\"workchain\":0") != std::string::npos);
  ASSERT_TRUE(json.find("\"workchain\":-1") != std::string::npos);
}

TEST(TvmHotpathStats, CpuJsonUsesCpuOrdering) {
  TvmHotpathStats stats;
  stats.enable_exact();
  stats.record(make_hash(1), basechainId, make_hash(101), make_time(1.0, 0.1), 10, 10, 1);
  stats.record(make_hash(2), basechainId, make_hash(102), make_time(0.5, 0.4), 10, 10, 1);

  const auto wall_json = stats.to_json(false, 0, 1);
  const auto cpu_json = stats.to_json(true, 0, 1);
  ASSERT_TRUE(wall_json.find(make_hash(1).to_hex()) != std::string::npos);
  ASSERT_TRUE(wall_json.find(make_hash(2).to_hex()) == std::string::npos);
  ASSERT_TRUE(cpu_json.find(make_hash(2).to_hex()) != std::string::npos);
  ASSERT_TRUE(cpu_json.find(make_hash(1).to_hex()) == std::string::npos);
}

TEST(TvmHotpathStats, AggregatesAndExportsEd25519Timing) {
  TvmHotpathStats first;
  TvmHotpathStats second;
  first.enable_exact();
  second.enable_exact();
  const auto code_hash = make_hash(1);
  const auto account = make_hash(2);
  first.record(code_hash, basechainId, account, make_time(0.4, 0.3), 10, 10, 1, 1, make_time(0.1, 0.08));
  second.record(code_hash, basechainId, account, make_time(0.6, 0.5), 20, 20, 2, 2, make_time(0.2, 0.16));

  first.merge(second);
  first.scale(0.5);

  const auto entries = first.entries_by_wall(1);
  ASSERT_EQ(entries[0].ed25519_verifications, 2u);
  ASSERT_TRUE(std::abs(entries[0].ed25519_time.real - 0.15) < 1e-12);
  const auto json = first.to_json(false);
  ASSERT_TRUE(json.find("\"total_ed25519_verifications\":2") != std::string::npos);
  ASSERT_TRUE(json.find("\"ed25519_verifications\":2") != std::string::npos);
  ASSERT_TRUE(json.find("\"ed25519_seconds\":0.15") != std::string::npos);
  ASSERT_TRUE(json.find("\"ed25519_share_of_tvm\":0.3") != std::string::npos);
}

TEST(ValidationReplay, ParsesOnlyCompleteAndValidBlockIds) {
  auto valid = BlockId::from_str("(0,8000000000000000,123456)");
  ASSERT_TRUE(valid.is_ok());
  ASSERT_EQ(valid.ok(), BlockId(0, shardIdAll, 123456));

  ASSERT_TRUE(BlockId::from_str("(0,8000000000000000,123456").is_error());
  ASSERT_TRUE(BlockId::from_str("(0,8000000000000000,123456)junk").is_error());
  ASSERT_TRUE(BlockId::from_str("(0,0,123456)").is_error());
  ASSERT_TRUE(BlockId::from_str("(-1,4000000000000000,123456)").is_error());
}

TEST(ValidationReplay, AggregatesEveryPreviouslyOmittedWorkTimeField) {
  CollationStats::WorkTimeStats collate_total;
  CollationStats::WorkTimeStats collate_block;
  collate_block.preinit = make_time(2.0, 4.0);
  collate_block.trx_tvm_profile = make_time(2.0, 4.0);
  collate_block.enqueue_new_messages = make_time(2.0, 4.0);
  collate_block.combine_account_transactions = make_time(2.0, 4.0);
  collate_block.create_shard_state = make_time(2.0, 4.0);
  collate_total += collate_block;
  collate_total *= 0.5;
  ASSERT_EQ(collate_total.preinit.real, 1.0);
  ASSERT_EQ(collate_total.trx_tvm_profile.real, 1.0);
  ASSERT_EQ(collate_total.enqueue_new_messages.real, 1.0);
  ASSERT_EQ(collate_total.combine_account_transactions.real, 1.0);
  ASSERT_EQ(collate_total.create_shard_state.real, 1.0);

  ValidationStats::WorkTimeStats validate_total;
  ValidationStats::WorkTimeStats validate_block;
  validate_block.unpack_block_candidate = make_time(2.0, 4.0);
  validate_block.process_mc_state = make_time(2.0, 4.0);
  validate_block.trx_tvm_profile = make_time(2.0, 4.0);
  validate_block.unpack_block_data = make_time(2.0, 4.0);
  validate_total += validate_block;
  validate_total *= 0.5;
  ASSERT_EQ(validate_total.unpack_block_candidate.real, 1.0);
  ASSERT_EQ(validate_total.process_mc_state.real, 1.0);
  ASSERT_EQ(validate_total.trx_tvm_profile.real, 1.0);
  ASSERT_EQ(validate_total.unpack_block_data.real, 1.0);
}

}  // namespace
}  // namespace ton::validator::test
