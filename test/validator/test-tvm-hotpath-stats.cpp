// SPDX-License-Identifier: LGPL-2.0-or-later

#include "td/utils/tests.h"
#include "validator/interfaces/tvm-hotpath-stats.h"

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

  stats.record(code_hash, first_account, make_time(0.1, 0.08), 100, 100, 10);
  stats.record(code_hash, first_account, make_time(0.2, 0.16), 200, 200, 20);
  stats.record(code_hash, second_account, make_time(0.3, 0.24), 300, 1000, 30);

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
  stats.record(make_hash(1), account, make_time(0.1, 0.1), 1, 1, 1);
  stats.record(make_hash(2), account, make_time(0.2, 0.2), 1, 1, 1);
  stats.record(make_hash(3), account, make_time(0.5, 0.5), 1, 1, 1);

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
  first.record(make_hash(1), account, make_time(0.1, 0.08), 10, 10, 1);
  second.record(make_hash(1), account, make_time(0.2, 0.16), 20, 20, 2);
  second.record(make_hash(2), account, make_time(0.3, 0.24), 30, 30, 3);

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
  stats.record(make_hash(1), make_hash(2), make_time(0.1, 0.08), 10, 10, 1);

  ASSERT_EQ(stats.size(), 0u);
  ASSERT_EQ(stats.total_executions(), 1u);
  ASSERT_EQ(stats.total_time().real, 0.1);
}

TEST(TvmHotpathStats, BoundsAccountCardinalityAndSerializesGasDifference) {
  TvmHotpathStats stats;
  const auto code_hash = make_hash(1);
  for (td::uint32 i = 0; i < 20; ++i) {
    stats.record(code_hash, make_hash(100 + i), make_time(0.01, 0.008), 100, 50, 10);
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

}  // namespace
}  // namespace ton::validator::test
