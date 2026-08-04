// SPDX-License-Identifier: LGPL-2.0-or-later

#include <string>

#include "block/block-auto.h"
#include "common/checksum.h"
#include "ton/ton-types.h"
#include "td/utils/filesystem.h"
#include "td/utils/tests.h"
#include "validator/validation-replay/block-workload.h"
#include "vm/boc.h"
#include "vm/cells/CellBuilder.h"

namespace ton::validator::replay::test {
namespace {

struct FixtureBlock {
  td::Ref<vm::Cell> root;
  ton::BlockSeqno seqno = 0;
  td::Ref<vm::Cell> account_blocks;
};

FixtureBlock load_fixture_block(const std::string& file_name, const std::string& expected_file_hash,
                                const std::string& expected_root_hash) {
  const std::string path = std::string(TON_REPLAY_FIXTURE_DIR) + "/" + file_name;
  auto data = td::read_file(path);
  LOG_CHECK(data.is_ok()) << "cannot read fixture " << path << ": " << data.error();
  auto bytes = data.move_as_ok();
  ASSERT_EQ(expected_file_hash, td::sha256_bits256(bytes.as_slice()).to_hex());

  auto root = vm::std_boc_deserialize(bytes.as_slice());
  LOG_CHECK(root.is_ok()) << "cannot deserialize fixture " << path << ": " << root.error();
  FixtureBlock result;
  result.root = root.move_as_ok();
  ASSERT_EQ(expected_root_hash, td::Bits256{result.root->get_hash().bits()}.to_hex());

  block::gen::Block::Record block_record;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  ASSERT_TRUE(tlb::unpack_cell(result.root, block_record));
  ASSERT_TRUE(tlb::unpack_cell(block_record.info, info));
  ASSERT_TRUE(tlb::unpack_cell(block_record.extra, extra));
  result.seqno = info.seq_no;
  result.account_blocks = extra.account_blocks;
  return result;
}

TEST(BlockWorkloadFixture, MasterchainBlockContainsExactlyOneTickAndOneTock) {
  const auto block = load_fixture_block("mc-83536321.boc",
                                        "2D0E7BA7F7370FC0C1719C8C68FE5516A709E1C71666DAB132CC0502E1F246F3",
                                        "E36C7EF7EE62B90BC78DA16FB5E98F57E6588137BF80853917E8F6544CF15B91");
  ASSERT_EQ(83536321u, block.seqno);

  auto summary = summarize_account_blocks(block.account_blocks);
  LOG_CHECK(summary.is_ok()) << summary.error();
  const auto workload = summary.move_as_ok();
  ASSERT_EQ(static_cast<std::size_t>(3), workload.raw_transactions);
  ASSERT_EQ(static_cast<std::size_t>(3), workload.transaction_kinds.total());
  ASSERT_EQ(static_cast<std::size_t>(1), workload.transaction_kinds.ordinary);
  ASSERT_EQ(static_cast<std::size_t>(1), workload.transaction_kinds.tick);
  ASSERT_EQ(static_cast<std::size_t>(1), workload.transaction_kinds.tock);
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.storage);
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.split_prepare);
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.split_install);
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.merge_prepare);
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.merge_install);
}

TEST(BlockWorkloadFixture, BasechainFourRootBlockIsAllOrdinary) {
  const auto block = load_fixture_block("four-root-88028077/block-88028077.boc",
                                        "EDC7BAA4189751C07CBF423D3EC1DCB6502C192C52428D2E20F6F3D3837D2C6F",
                                        "3D60B72C796B49E117A9E2FD3AA451E1D4D51D719E5713F8975D7593C7E557A4");
  ASSERT_EQ(88028077u, block.seqno);

  auto summary = summarize_account_blocks(block.account_blocks);
  LOG_CHECK(summary.is_ok()) << summary.error();
  const auto workload = summary.move_as_ok();
  ASSERT_EQ(static_cast<std::size_t>(138), workload.raw_transactions);
  ASSERT_EQ(static_cast<std::size_t>(138), workload.transaction_kinds.ordinary);
  ASSERT_EQ(static_cast<std::size_t>(108), workload.distinct_accounts);
  ASSERT_EQ(static_cast<std::size_t>(138), workload.transaction_kinds.total());
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.tick);
  ASSERT_EQ(static_cast<std::size_t>(0), workload.transaction_kinds.tock);
}

TEST(BlockWorkloadFixture, ClassifierFailsClosedOnMalformedTransaction) {
  TransactionKindCounts counts;
  auto malformed = classify_and_count_transaction(vm::CellBuilder().finalize(), counts);
  ASSERT_TRUE(malformed.is_error());
  ASSERT_EQ(static_cast<std::size_t>(0), counts.total());

  auto null_cell = classify_and_count_transaction({}, counts);
  ASSERT_TRUE(null_cell.is_error());
  ASSERT_EQ(static_cast<std::size_t>(0), counts.total());
}

}  // namespace
}  // namespace ton::validator::replay::test
