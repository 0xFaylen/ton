// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <vector>

#include "parallel-transaction-payload.h"

namespace ton::validator::parallel_inbound {

using OutboundQueueKey = td::BitArray<352>;
using NewMessageQueue =
    std::priority_queue<block::NewOutMsg, std::vector<block::NewOutMsg>, std::greater<block::NewOutMsg>>;

struct ShadowAccountState {
  Hash256 state_hash{};
  td::Ref<vm::Cell> state;
  Hash256 last_transaction_hash{};
  std::uint64_t last_transaction_end_lt{0};
};

// Copyable coordinator-owned state used to validate a complete ready prefix
// before publishing any of its mutations. It models exact cells and counters,
// but deliberately does not pretend that std::map is a ShardAccounts or
// descriptor augmented dictionary root.
struct ShadowCoordinatorState {
  block::BlockLimitStatus block_limits;
  std::map<Hash256, ShadowAccountState> accounts;
  std::map<Hash256, td::Ref<vm::Cell>> in_msg_descriptors;
  std::map<Hash256, td::Ref<vm::Cell>> out_msg_descriptors;
  std::set<OutboundQueueKey> outbound_queue_entries;
  NewMessageQueue new_messages;
  std::optional<ton::LogicalTime> min_new_message_lt;
  ProcessedUptoFrontier processed_upto;

  explicit ShadowCoordinatorState(const block::BlockLimits& limits, ton::LogicalTime lt = 0);
  ShadowCoordinatorState(const ShadowCoordinatorState& other);
  ShadowCoordinatorState& operator=(const ShadowCoordinatorState&) = delete;

  void publish_from(ShadowCoordinatorState&& other);
};

struct CoordinatorCommitContext {
  BasechainLimitContext limit;
  OutboundRegistrationContext outbound_registration;
  std::optional<InboundDescriptorContext> inbound_descriptor;
  std::optional<OutboundQueueKey> outbound_queue_deletion;
};

enum class CoordinatorCommitError {
  none,
  size_mismatch,
  non_canonical_input,
  input_not_after_frontier,
  missing_effects,
  coordinator_only_item,
  account_mismatch,
  input_message_hash_mismatch,
  transaction_cell_hash_mismatch,
  post_state_cell_hash_mismatch,
  missing_account_state,
  account_state_hash_mismatch,
  pre_state_mismatch,
  outbound_registration_error,
  inbound_descriptor_error,
  unexpected_queue_deletion,
  missing_queue_deletion,
  queue_key_message_mismatch,
  queue_entry_not_found,
  duplicate_in_descriptor,
  duplicate_out_descriptor,
  block_limit_error,
  extra_out_msgs_overflow,
};

struct CoordinatorCommitResult {
  CoordinatorCommitError error{CoordinatorCommitError::none};
  OutboundRegistrationError outbound_error{OutboundRegistrationError::none};
  InboundDescriptorError descriptor_error{InboundDescriptorError::none};
  BasechainLimitError limit_error{BasechainLimitError::none};
  PrefixDecision decision;
  std::optional<std::size_t> item_index;

  explicit operator bool() const {
    return error == CoordinatorCommitError::none;
  }
};

struct AccountDictionaryDelta {
  Hash256 account{};
  td::Ref<vm::Cell> post_account_state;
  Hash256 last_transaction_hash{};
  std::uint64_t last_transaction_lt{0};
  bool existed_before{false};
  bool exists_after{false};
};

struct QueueDictionaryDeletion {
  OutboundQueueKey key;
  td::Ref<vm::Cell> expected_value;
};

// Every root is a wrapped HashmapAugE cell, matching the corresponding TL-B
// owner and preserving the valid empty-dictionary representation.
struct AugmentedDictionarySeed {
  int global_version{0};
  td::Ref<vm::Cell> shard_accounts_root;
  td::Ref<vm::Cell> in_msg_descr_root;
  td::Ref<vm::Cell> out_msg_descr_root;
  td::Ref<vm::Cell> out_msg_queue_root;
};

struct AugmentedDictionaryRoots {
  td::Ref<vm::Cell> shard_accounts_root;
  td::Ref<vm::Cell> in_msg_descr_root;
  td::Ref<vm::Cell> out_msg_descr_root;
  td::Ref<vm::Cell> out_msg_queue_root;
};

enum class AugmentedDictionaryCommitError {
  none,
  missing_seed_root,
  invalid_seed_dictionary,
  missing_post_account_state,
  account_add_failed,
  account_replace_failed,
  account_delete_failed,
  in_descriptor_add_failed,
  out_descriptor_add_failed,
  missing_expected_queue_value,
  queue_entry_not_found,
  queue_value_mismatch,
  cannot_serialize_queue_value,
  vm_error,
};

struct AugmentedDictionaryCommitResult {
  AugmentedDictionaryCommitError error{AugmentedDictionaryCommitError::none};
  std::optional<std::size_t> item_index;
  std::optional<AugmentedDictionaryRoots> roots;

  explicit operator bool() const {
    return error == AugmentedDictionaryCommitError::none;
  }
};

using ShadowCanStart = std::function<bool(const block::BlockLimitStatus&, std::size_t)>;

// Applies only a ready continuous prefix to a private copy and publishes that
// copy once. Normal scheduling stops preserve a useful prefix. A malformed
// coordinator delta returns commit_failure and leaves target byte-for-byte
// unchanged at this abstraction level.
CoordinatorCommitResult apply_ready_coordinator_prefix_atomic(
    ShadowCoordinatorState& target, const std::vector<WorkItem>& items,
    const std::vector<CompletionStatus>& completions,
    const std::vector<std::optional<CanonicalTransactionEffects>>& effects,
    const std::vector<CoordinatorCommitContext>& contexts, const ShadowCanStart& can_start = {});

// Applies consensus dictionary deltas to private AugmentedDictionary copies.
// Input roots are immutable and output roots are returned only after every
// mutation succeeds, so callers cannot publish a partial dictionary set.
AugmentedDictionaryCommitResult apply_augmented_dictionary_deltas_atomic(
    const AugmentedDictionarySeed& seed, const std::vector<AccountDictionaryDelta>& account_deltas,
    const std::map<Hash256, td::Ref<vm::Cell>>& in_msg_descriptors,
    const std::map<Hash256, td::Ref<vm::Cell>>& out_msg_descriptors,
    const std::vector<QueueDictionaryDeletion>& queue_deletions);

const char* to_string(CoordinatorCommitError error);
const char* to_string(AugmentedDictionaryCommitError error);

}  // namespace ton::validator::parallel_inbound
