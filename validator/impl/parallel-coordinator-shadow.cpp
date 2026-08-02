// SPDX-License-Identifier: LGPL-2.0-or-later

#include <cstring>
#include <limits>
#include <utility>

#include "block/block-parse.h"
#include "parallel-coordinator-shadow.h"

namespace ton::validator::parallel_inbound {
namespace {

void copy_block_limit_status(block::BlockLimitStatus& target, const block::BlockLimitStatus& source) {
  target.cur_lt = source.cur_lt;
  target.gas_used = source.gas_used;
  target.st_stat = source.st_stat;
  target.accounts = source.accounts;
  target.transactions = source.transactions;
  target.extra_out_msgs = source.extra_out_msgs;
  target.collated_data_size_estimate = source.collated_data_size_estimate;
  target.public_library_diff = source.public_library_diff;
}

bool queue_key_matches_message(const OutboundQueueKey& key, const Hash256& message_hash) {
  return std::memcmp(key.data() + 12, message_hash.data(), message_hash.size()) == 0;
}

Hash256 cell_hash(const td::Ref<vm::Cell>& cell) {
  Hash256 result{};
  std::memcpy(result.data(), cell->get_hash().as_slice().data(), result.size());
  return result;
}

td::BitArray<256> dictionary_key(const Hash256& hash) {
  td::BitArray<256> result;
  std::memcpy(result.data(), hash.data(), hash.size());
  return result;
}

td::Ref<vm::Cell> serialize_value(const td::Ref<vm::CellSlice>& value) {
  if (value.is_null()) {
    return {};
  }
  vm::CellBuilder builder;
  if (!builder.append_cellslice_bool(value->clone())) {
    return {};
  }
  return builder.finalize_novm();
}

CoordinatorCommitResult commit_error(CoordinatorCommitError error, std::size_t item_index) {
  CoordinatorCommitResult result;
  result.error = error;
  result.decision.stop_reason = PrefixStopReason::commit_failure;
  result.item_index = item_index;
  return result;
}

}  // namespace

ShadowCoordinatorState::ShadowCoordinatorState(const block::BlockLimits& limits, ton::LogicalTime lt)
    : block_limits(limits, lt) {
}

ShadowCoordinatorState::ShadowCoordinatorState(const ShadowCoordinatorState& other)
    : block_limits(other.block_limits.limits),
      accounts(other.accounts),
      in_msg_descriptors(other.in_msg_descriptors),
      out_msg_descriptors(other.out_msg_descriptors),
      outbound_queue_entries(other.outbound_queue_entries),
      new_messages(other.new_messages),
      min_new_message_lt(other.min_new_message_lt),
      processed_upto(other.processed_upto) {
  copy_block_limit_status(block_limits, other.block_limits);
}

void ShadowCoordinatorState::publish_from(ShadowCoordinatorState&& other) {
  CHECK(&block_limits.limits == &other.block_limits.limits);
  copy_block_limit_status(block_limits, other.block_limits);
  accounts = std::move(other.accounts);
  in_msg_descriptors = std::move(other.in_msg_descriptors);
  out_msg_descriptors = std::move(other.out_msg_descriptors);
  outbound_queue_entries = std::move(other.outbound_queue_entries);
  new_messages = std::move(other.new_messages);
  min_new_message_lt = other.min_new_message_lt;
  processed_upto = other.processed_upto;
}

CoordinatorCommitResult apply_ready_coordinator_prefix_atomic(
    ShadowCoordinatorState& target, const std::vector<WorkItem>& items,
    const std::vector<CompletionStatus>& completions,
    const std::vector<std::optional<CanonicalTransactionEffects>>& effects,
    const std::vector<CoordinatorCommitContext>& contexts, const ShadowCanStart& can_start) {
  CoordinatorCommitResult result;
  if (items.size() != completions.size() || items.size() != effects.size() || items.size() != contexts.size()) {
    result.error = CoordinatorCommitError::size_mismatch;
    return result;
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0 && !(items[i - 1].key < items[i].key)) {
      result.error = CoordinatorCommitError::non_canonical_input;
      result.item_index = i;
      return result;
    }
    if (target.processed_upto.last_processed &&
        !(target.processed_upto.last_processed.value() < items[i].key)) {
      result.error = CoordinatorCommitError::input_not_after_frontier;
      result.item_index = i;
      return result;
    }
  }

  ShadowCoordinatorState shadow{target};
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (can_start && !can_start(shadow.block_limits, i)) {
      result.decision.stop_reason = PrefixStopReason::block_limit;
      break;
    }
    if (completions[i] == CompletionStatus::pending) {
      result.decision.stop_reason = PrefixStopReason::pending;
      break;
    }
    if (completions[i] == CompletionStatus::failed) {
      result.decision.stop_reason = PrefixStopReason::worker_failure;
      break;
    }
    if (!effects[i]) {
      return commit_error(CoordinatorCommitError::missing_effects, i);
    }
    if (!items[i].account) {
      return commit_error(CoordinatorCommitError::coordinator_only_item, i);
    }

    const auto& effect = effects[i].value();
    const auto& context = contexts[i];
    if (effect.account != items[i].account.value()) {
      return commit_error(CoordinatorCommitError::account_mismatch, i);
    }
    if (!effect.inbound_message_hash || effect.inbound_message_hash.value() != items[i].key.hash) {
      return commit_error(CoordinatorCommitError::input_message_hash_mismatch, i);
    }
    if (effect.transaction_root.is_null() || cell_hash(effect.transaction_root) != effect.transaction_hash) {
      return commit_error(CoordinatorCommitError::transaction_cell_hash_mismatch, i);
    }
    if (effect.post_account_state.is_null() || cell_hash(effect.post_account_state) != effect.post_account_state_hash) {
      return commit_error(CoordinatorCommitError::post_state_cell_hash_mismatch, i);
    }
    auto account = shadow.accounts.find(effect.account);
    if (account == shadow.accounts.end()) {
      return commit_error(CoordinatorCommitError::missing_account_state, i);
    }
    if (account->second.state.is_null() || cell_hash(account->second.state) != account->second.state_hash) {
      return commit_error(CoordinatorCommitError::account_state_hash_mismatch, i);
    }
    if (account->second.state_hash != effect.pre_account_state_hash) {
      return commit_error(CoordinatorCommitError::pre_state_mismatch, i);
    }

    auto registrations = materialize_outbound_registrations(effect, context.outbound_registration);
    if (!registrations) {
      auto error = commit_error(CoordinatorCommitError::outbound_registration_error, i);
      error.outbound_error = registrations.error;
      return error;
    }

    std::optional<CanonicalInboundDescriptorDelta> descriptor_delta;
    if (context.inbound_descriptor) {
      auto descriptors = materialize_inbound_internal_descriptors(effect, context.inbound_descriptor.value());
      if (!descriptors) {
        auto error = commit_error(CoordinatorCommitError::inbound_descriptor_error, i);
        error.descriptor_error = descriptors.error;
        return error;
      }
      descriptor_delta = std::move(descriptors.delta.value());
    }

    if (!descriptor_delta && context.outbound_queue_deletion) {
      return commit_error(CoordinatorCommitError::unexpected_queue_deletion, i);
    }
    const bool needs_queue_deletion = descriptor_delta && descriptor_delta->out_msg_descriptor.not_null();
    if (needs_queue_deletion != context.outbound_queue_deletion.has_value()) {
      return commit_error(CoordinatorCommitError::missing_queue_deletion, i);
    }
    if (context.outbound_queue_deletion) {
      if (!effect.inbound_message_hash ||
          !queue_key_matches_message(context.outbound_queue_deletion.value(), effect.inbound_message_hash.value())) {
        return commit_error(CoordinatorCommitError::queue_key_message_mismatch, i);
      }
      if (!shadow.outbound_queue_entries.count(context.outbound_queue_deletion.value())) {
        return commit_error(CoordinatorCommitError::queue_entry_not_found, i);
      }
    }
    if (descriptor_delta && shadow.in_msg_descriptors.count(descriptor_delta->message_hash)) {
      return commit_error(CoordinatorCommitError::duplicate_in_descriptor, i);
    }
    if (descriptor_delta && descriptor_delta->out_msg_descriptor.not_null() &&
        shadow.out_msg_descriptors.count(descriptor_delta->message_hash)) {
      return commit_error(CoordinatorCommitError::duplicate_out_descriptor, i);
    }
    if (registrations.batch->extra_out_msgs_delta >
        std::numeric_limits<unsigned>::max() - shadow.block_limits.extra_out_msgs) {
      return commit_error(CoordinatorCommitError::extra_out_msgs_overflow, i);
    }

    auto limit_result = apply_basechain_block_limits_atomic(shadow.block_limits, {effect}, {context.limit});
    if (!limit_result) {
      auto error = commit_error(CoordinatorCommitError::block_limit_error, i);
      error.limit_error = limit_result.error;
      return error;
    }

    account->second.state_hash = effect.post_account_state_hash;
    account->second.state = effect.post_account_state;
    account->second.last_transaction_hash = effect.transaction_hash;
    account->second.last_transaction_end_lt = effect.transaction_end_lt;
    if (descriptor_delta) {
      shadow.in_msg_descriptors.emplace(descriptor_delta->message_hash, descriptor_delta->in_msg_descriptor);
      if (descriptor_delta->out_msg_descriptor.not_null()) {
        shadow.out_msg_descriptors.emplace(descriptor_delta->message_hash, descriptor_delta->out_msg_descriptor);
      }
    }
    if (context.outbound_queue_deletion) {
      shadow.outbound_queue_entries.erase(context.outbound_queue_deletion.value());
    }
    for (auto& message : registrations.batch->messages) {
      shadow.new_messages.push(std::move(message));
    }
    shadow.block_limits.extra_out_msgs += static_cast<unsigned>(registrations.batch->extra_out_msgs_delta);
    if (registrations.batch->min_message_lt &&
        (!shadow.min_new_message_lt ||
         registrations.batch->min_message_lt.value() < shadow.min_new_message_lt.value())) {
      shadow.min_new_message_lt = registrations.batch->min_message_lt;
    }
    ++result.decision.committed_count;
  }

  if (result.decision.committed_count == items.size()) {
    result.decision.stop_reason = PrefixStopReason::end_of_input;
  }
  if (result.decision.committed_count != 0) {
    shadow.processed_upto.last_processed = items[result.decision.committed_count - 1].key;
  }
  target.publish_from(std::move(shadow));
  return result;
}

AugmentedDictionaryCommitResult apply_augmented_dictionary_deltas_atomic(
    const AugmentedDictionarySeed& seed, const std::vector<AccountDictionaryDelta>& account_deltas,
    const std::map<Hash256, td::Ref<vm::Cell>>& in_msg_descriptors,
    const std::map<Hash256, td::Ref<vm::Cell>>& out_msg_descriptors,
    const std::vector<QueueDictionaryDeletion>& queue_deletions) {
  AugmentedDictionaryCommitResult result;
  if (seed.shard_accounts_root.is_null() || seed.in_msg_descr_root.is_null() || seed.out_msg_descr_root.is_null() ||
      seed.out_msg_queue_root.is_null()) {
    result.error = AugmentedDictionaryCommitError::missing_seed_root;
    return result;
  }

  try {
    block::tlb::Aug_InMsgDescr in_augmentation{seed.global_version};
    block::tlb::Aug_OutMsgDescr out_augmentation{seed.global_version};
    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(seed.shard_accounts_root), 256,
                                     block::tlb::aug_ShardAccounts};
    vm::AugmentedDictionary in_descriptors{vm::load_cell_slice_ref(seed.in_msg_descr_root), 256, in_augmentation};
    vm::AugmentedDictionary out_descriptors{vm::load_cell_slice_ref(seed.out_msg_descr_root), 256, out_augmentation};
    vm::AugmentedDictionary out_queue{vm::load_cell_slice_ref(seed.out_msg_queue_root), 352,
                                      block::tlb::aug_OutMsgQueue};
    if (!accounts.is_valid() || !in_descriptors.is_valid() || !out_descriptors.is_valid() || !out_queue.is_valid()) {
      result.error = AugmentedDictionaryCommitError::invalid_seed_dictionary;
      return result;
    }

    for (std::size_t i = 0; i < account_deltas.size(); ++i) {
      const auto& delta = account_deltas[i];
      const auto key = dictionary_key(delta.account);
      if (!delta.exists_after) {
        if (!delta.existed_before || accounts.lookup_delete(key).is_null()) {
          result.error = AugmentedDictionaryCommitError::account_delete_failed;
          result.item_index = i;
          return result;
        }
        continue;
      }
      if (delta.post_account_state.is_null()) {
        result.error = AugmentedDictionaryCommitError::missing_post_account_state;
        result.item_index = i;
        return result;
      }
      vm::CellBuilder builder;
      if (!(builder.store_ref_bool(delta.post_account_state) &&
            builder.store_bytes_bool(delta.last_transaction_hash.data(), delta.last_transaction_hash.size()) &&
            builder.store_long_bool(delta.last_transaction_lt, 64))) {
        result.error = delta.existed_before ? AugmentedDictionaryCommitError::account_replace_failed
                                            : AugmentedDictionaryCommitError::account_add_failed;
        result.item_index = i;
        return result;
      }
      const auto mode = delta.existed_before ? vm::Dictionary::SetMode::Replace : vm::Dictionary::SetMode::Add;
      if (!accounts.set_builder(key, builder, mode)) {
        result.error = delta.existed_before ? AugmentedDictionaryCommitError::account_replace_failed
                                            : AugmentedDictionaryCommitError::account_add_failed;
        result.item_index = i;
        return result;
      }
    }

    std::size_t index = 0;
    for (const auto& [hash, descriptor] : in_msg_descriptors) {
      const auto key = dictionary_key(hash);
      if (descriptor.is_null() ||
          !in_descriptors.set(key, vm::load_cell_slice(descriptor), vm::Dictionary::SetMode::Add)) {
        result.error = AugmentedDictionaryCommitError::in_descriptor_add_failed;
        result.item_index = index;
        return result;
      }
      ++index;
    }
    index = 0;
    for (const auto& [hash, descriptor] : out_msg_descriptors) {
      const auto key = dictionary_key(hash);
      if (descriptor.is_null() ||
          !out_descriptors.set(key, vm::load_cell_slice(descriptor), vm::Dictionary::SetMode::Add)) {
        result.error = AugmentedDictionaryCommitError::out_descriptor_add_failed;
        result.item_index = index;
        return result;
      }
      ++index;
    }
    for (std::size_t i = 0; i < queue_deletions.size(); ++i) {
      const auto& deletion = queue_deletions[i];
      if (deletion.expected_value.is_null()) {
        result.error = AugmentedDictionaryCommitError::missing_expected_queue_value;
        result.item_index = i;
        return result;
      }
      auto removed = out_queue.lookup_delete(deletion.key);
      if (removed.is_null()) {
        result.error = AugmentedDictionaryCommitError::queue_entry_not_found;
        result.item_index = i;
        return result;
      }
      auto removed_cell = serialize_value(removed);
      if (removed_cell.is_null()) {
        result.error = AugmentedDictionaryCommitError::cannot_serialize_queue_value;
        result.item_index = i;
        return result;
      }
      if (removed_cell->get_hash() != deletion.expected_value->get_hash()) {
        result.error = AugmentedDictionaryCommitError::queue_value_mismatch;
        result.item_index = i;
        return result;
      }
    }

    result.roots = AugmentedDictionaryRoots{.shard_accounts_root = accounts.get_wrapped_dict_root(),
                                            .in_msg_descr_root = in_descriptors.get_wrapped_dict_root(),
                                            .out_msg_descr_root = out_descriptors.get_wrapped_dict_root(),
                                            .out_msg_queue_root = out_queue.get_wrapped_dict_root()};
    return result;
  } catch (vm::VmVirtError&) {
    result.error = AugmentedDictionaryCommitError::vm_error;
    result.roots.reset();
    return result;
  } catch (vm::VmError&) {
    result.error = AugmentedDictionaryCommitError::vm_error;
    result.roots.reset();
    return result;
  }
}

const char* to_string(CoordinatorCommitError error) {
  switch (error) {
    case CoordinatorCommitError::none:
      return "none";
    case CoordinatorCommitError::size_mismatch:
      return "size_mismatch";
    case CoordinatorCommitError::non_canonical_input:
      return "non_canonical_input";
    case CoordinatorCommitError::input_not_after_frontier:
      return "input_not_after_frontier";
    case CoordinatorCommitError::missing_effects:
      return "missing_effects";
    case CoordinatorCommitError::coordinator_only_item:
      return "coordinator_only_item";
    case CoordinatorCommitError::account_mismatch:
      return "account_mismatch";
    case CoordinatorCommitError::input_message_hash_mismatch:
      return "input_message_hash_mismatch";
    case CoordinatorCommitError::transaction_cell_hash_mismatch:
      return "transaction_cell_hash_mismatch";
    case CoordinatorCommitError::post_state_cell_hash_mismatch:
      return "post_state_cell_hash_mismatch";
    case CoordinatorCommitError::missing_account_state:
      return "missing_account_state";
    case CoordinatorCommitError::account_state_hash_mismatch:
      return "account_state_hash_mismatch";
    case CoordinatorCommitError::pre_state_mismatch:
      return "pre_state_mismatch";
    case CoordinatorCommitError::outbound_registration_error:
      return "outbound_registration_error";
    case CoordinatorCommitError::inbound_descriptor_error:
      return "inbound_descriptor_error";
    case CoordinatorCommitError::unexpected_queue_deletion:
      return "unexpected_queue_deletion";
    case CoordinatorCommitError::missing_queue_deletion:
      return "missing_queue_deletion";
    case CoordinatorCommitError::queue_key_message_mismatch:
      return "queue_key_message_mismatch";
    case CoordinatorCommitError::queue_entry_not_found:
      return "queue_entry_not_found";
    case CoordinatorCommitError::duplicate_in_descriptor:
      return "duplicate_in_descriptor";
    case CoordinatorCommitError::duplicate_out_descriptor:
      return "duplicate_out_descriptor";
    case CoordinatorCommitError::block_limit_error:
      return "block_limit_error";
    case CoordinatorCommitError::extra_out_msgs_overflow:
      return "extra_out_msgs_overflow";
  }
  return "unknown";
}

const char* to_string(AugmentedDictionaryCommitError error) {
  switch (error) {
    case AugmentedDictionaryCommitError::none:
      return "none";
    case AugmentedDictionaryCommitError::missing_seed_root:
      return "missing_seed_root";
    case AugmentedDictionaryCommitError::invalid_seed_dictionary:
      return "invalid_seed_dictionary";
    case AugmentedDictionaryCommitError::missing_post_account_state:
      return "missing_post_account_state";
    case AugmentedDictionaryCommitError::account_add_failed:
      return "account_add_failed";
    case AugmentedDictionaryCommitError::account_replace_failed:
      return "account_replace_failed";
    case AugmentedDictionaryCommitError::account_delete_failed:
      return "account_delete_failed";
    case AugmentedDictionaryCommitError::in_descriptor_add_failed:
      return "in_descriptor_add_failed";
    case AugmentedDictionaryCommitError::out_descriptor_add_failed:
      return "out_descriptor_add_failed";
    case AugmentedDictionaryCommitError::missing_expected_queue_value:
      return "missing_expected_queue_value";
    case AugmentedDictionaryCommitError::queue_entry_not_found:
      return "queue_entry_not_found";
    case AugmentedDictionaryCommitError::queue_value_mismatch:
      return "queue_value_mismatch";
    case AugmentedDictionaryCommitError::cannot_serialize_queue_value:
      return "cannot_serialize_queue_value";
    case AugmentedDictionaryCommitError::vm_error:
      return "vm_error";
  }
  return "unknown";
}

}  // namespace ton::validator::parallel_inbound
