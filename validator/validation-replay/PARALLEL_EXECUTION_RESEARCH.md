# Parallel single-shard execution research log

Cutoff: 2026-08-02. This file records the bounded three-round source pass used
to choose the implementation direction. Primary sources are preferred; project
marketing numbers are not treated as TON performance forecasts.

## Research questions

1. Which modern execution mechanisms transfer to TON without changing TVM or
   block semantics?
2. Which current TON Core tracks overlap the proposal?
3. What exact TON invariant replaces optimistic conflict detection?

## Round 1 — execution architecture

Queries covered Remora, Pilotfish, RapidLane and Block-STM v2.

| Source | Finding used | Confidence |
|---|---|---|
| [Remora paper](https://arxiv.org/abs/2607.02817) and [repository](https://github.com/remora-sys/remora) | Coordinator/worker asymmetry, deterministic object versioning, stateless/stateful separation and locality-aware scheduling are useful patterns. Its ownership/read-write assumptions are not a drop-in TON model. | high |
| [Pilotfish paper](https://arxiv.org/abs/2401.16292) and [Sui technical overview](https://www.sui.io/blog/pilotfish-execution-scalability-blockchain) | Versioned queues, execution workers, recovery and trusted per-validator scale-out are relevant. Distributed cross-object fetching is unnecessary for a TON transaction that mutates one destination account. | high |
| [RapidLane paper](https://arxiv.org/abs/2405.06117) | Deferred conflict computation helps contended shared objects but changes the programming/execution model. It is not the compatible first stage. | high |
| [Block-STM paper](https://aptoslabs.com/pdf/2203.06871.pdf), [Aptos core](https://github.com/aptos-labs/aptos-core/tree/main/execution) and [Aptos 2025 stack note](https://aptosnetwork.com/currents/the-new-aptos-tech-stack-and-innovation-enabling-license) | Optimistic read/write validation targets unknown multi-object conflicts. Public Block-STM v2 performance claims are context, not evidence for TON. | medium-high |

Decision after round 1: use deterministic account ownership and prefix commit;
do not add rollback, speculative read sets or cross-object fetch.

## Round 2 — integration patterns and overlap

Queries covered Agave scheduling, TON dedicated collators, Plumtree and the
Remora codebase.

| Source | Finding used | Confidence |
|---|---|---|
| [TON PR #2485](https://github.com/ton-blockchain/ton/pull/2485) | The open PR adds collator registration/delegation and dedicated collator production. It is the parent deployment boundary, not an intra-block account executor. | high |
| [TON PR #2505](https://github.com/ton-blockchain/ton/pull/2505) | Merged into testnet on 2026-07-29; it changes Plumtree stability/stats, not TVM or transaction scheduling. | high |
| [Agave repository](https://github.com/anza-xyz/agave) | Account-lock scheduling is a useful comparison, but Solana transactions declare broader account sets. TON's local transaction owner is already the destination account. | medium |

Decision after round 2: the executor must fit inside #2485 rather than compete
with it, and must not claim any benefit from #2505 network work.

## Round 3 — current TON roadmap

Queries covered the current TON Core shardchain-performance statements, open
parallel work and the CellDB replacement direction.

| Source | Finding used | Confidence |
|---|---|---|
| [TON Core channel](https://t.me/s/toncore) | Public next stages are collator+validator activation and a new database replacing CellDB 2.0/RocksDB. This supports keeping DB/network work outside the executor and defining an explicit interface to it. | high |
| [TON v2026.06 release](https://github.com/ton-blockchain/ton/releases/tag/v2026.06) | Current release work is networking/Plumtree, sync, consensus hardening and Global Version 15, not public intra-block parallel collation. | high |
| [TON open PR list](https://github.com/ton-blockchain/ton/pulls) | Public parallel BOC deserialization (#2305) is a cold/read path and does not implement steady-state account execution. Absence from public PRs cannot prove absence of private work. | medium |

Decision after round 3: proceed in a private/user-owned fork with an overlap
gate on every upstream refresh. Do not infer internal TON Core plans beyond the
two directions they announced.

## Local source findings

These findings come from the checked-out TON source, not from external prose:

- `OutputQueueMerger::MsgKeyValue::operator<` orders messages by `lt`, then the
  last 256 key bits (message hash).
- `Collator::process_inbound_internal_messages` drains that order before
  `process_external_and_new_messages` processes newly generated messages.
- `Collator::update_processed_upto` stores the last committed `(lt, hash)`.
- `ValidateQuery::check_transactions` already has a parallel account path using
  one `CheckAccountTxs` actor per AccountBlock.
- `CellUsageTree`, `current_tx_storage_dict_`, block limits and proof stats are
  mutable shared state and cannot be used concurrently without isolation.
- `Transaction` itself holds a `const Account&` and mutates that account only in
  `commit()`, but TVM cell loads mutate the shared usage/proof accounting before
  commit through `CellUsageTree` callbacks. This is the first concrete worker
  isolation boundary.

This turns the unresolved partial-worker question into a precise rule: compute
out of order if useful, but commit only the continuous global input prefix.

## Local replay checkpoint — 2026-07-31

Ten Debug offline replays of basechain block `87341675` completed from copied
archive/config/account-proof/library artifacts without reading the live node.
Each run covered the full block and preserved exact historical transaction and
account-state hashes: 51 transactions across 29 accounts. The median greedy
account-work ceiling was 1.998x/3.253x for 2/4 workers. It stayed at 3.253x for
8/16 workers because the hottest account represented a median 30.75% of measured
transaction work.

This is evidence that the account decomposition is worth carrying into the
shadow-collator gate, not evidence of achieved parallel speedup. The estimator
excludes serial commit, CellUsageTree/proof and state merge, block limits,
worker contention, network, and consensus. Absolute Debug timings are not used.
The Release build was not used because its existing CMake environment currently
fails to find `absl/hash/hash.h`; that build problem is independent of replay
equivalence and must be fixed before performance publication.

## Executable account replay checkpoint — 2026-08-02

The sparse replay tool now executes the account decomposition instead of only
projecting it. A complete account-proof replay establishes the canonical work
map, one-worker baseline, and historical result. Deterministic greedy LPT assigns
whole account chains to bounded threads; each thread runs the normal emulator on
its proof subset. The batch fails unless the lane union exactly matches all
historical transaction hashes, resulting account-state hashes, account-chain
lengths, and canonical effect counters.

Ten independent `-O3` Release processes for each width on copied basechain block
`87341675` all passed for 51 transactions and 29 accounts. Median measured wall
speedup was 1.57x at two workers, 2.15x at four, and 2.45x at eight;
interquartile ranges were 1.49-1.95x, 1.99-2.41x, and 2.20-2.73x respectively.
The median isolated serial replay rate was about 3.1k raw tx/s; the four- and
eight-worker medians were 6.6k and 8.0k raw tx/s.
These timings include thread creation/join and intentionally duplicate config
extraction, the full account dictionary scan, and shadow effect checking per
lane.
The in-process serial sample precedes the parallel sample; the JSON records this
cache/order caveat and the reported medians come from repeated processes.

This result invalidates the stronger concern that real TON emulator work cannot
scale across independent account chains on this input. It does not establish a
single-shard TPS gain: augmented-root commit, collator scheduling, reusable
workers, CellUsageTree/proof merge, network, consensus, block bytes, and a
representative multi-block workload remain outside the measurement. The Release
target now builds after supplying the checked-in Abseil include path to the local
CMake cache. This fixes the measurement environment, not any TON source or
dependency.

## Single-shard capacity boundary — 2026-08-02

The replay now reports protocol limits, exact block-file density, phase wall
times, and deliberately incomplete capacity projections in one JSON object.
The copied masterchain config proof at seqno `82773023` establishes:

- `max_block_bytes = 2,097,152`;
- `max_collated_bytes = 10,485,760`;
- target block rate `400 ms` and minimum block interval `300 ms`.

Config 23 establishes the active `BlockLimitStatus` thresholds for the
basechain: bytes and collated-data underload/soft/hard are
262,144/1,048,576/2,097,152; gas is 2,000,000/10,000,000/20,000,000; and
logical-time delta is 1,000/5,000/10,000. The replay now copies these real
limits into both shadow limit applications instead of constructing default
`BlockLimits` objects.

The 10 MiB collated-data limit is a separate witness budget and cannot be added
to the 2 MiB block budget. It is an outer serialized-candidate limit; Config 23
still supplies tighter collator estimation thresholds. In particular, the
often cited 1 MiB is the Config 23 soft threshold, not the hard candidate cap.
The target block file is 113,736 bytes for 51 raw
transactions across 29 accounts. Its measured density is 2,230.12 bytes per raw
transaction and its file is 5.42% of the configured block-byte limit.

At unchanged density, the linear hard candidate-byte projection is:

`51 * 2,097,152 / 113,736 / 0.4 = 2,350.94 raw tx/s`.

If, and only if, one workload operation consumes three raw transactions at that
same density, the corresponding conditional projection is 783.65 operations/s.
This is not jetton workload classification and is not a measured operation rate.
Config 23 size limits operate on `BlockLimitStatus::estimate_block_size` rather
than serialized candidate bytes. The current replay does not reconstruct the
complete live collator usage tree, descriptors, or collated-data estimate, so it
does not convert the 1 MiB soft threshold into TPS.

The Release account replay is faster than that byte projection even before
parallelism: approximately 3.1k raw tx/s serial, 6.6k at four workers and 8.0k
at eight. On this sample, isolated TVM/account compute is therefore not the
first projected ceiling. The executor still creates useful headroom for denser
or more expensive workloads, but it cannot by itself turn that headroom into
single-shard TPS.

The actual sustainable mainnet value remains unknown and is encoded as `null`.
It is bounded by the minimum of block bytes, collator wall time, validator
`ValidateQuery` wall time, exact state-root commit, and candidate delivery. A
single low-fill block cannot establish any of those saturated limits. The
previously discussed 476 jTPS number is excluded because the refreshed public
benchmark refs did not provide a reproducible primary artifact for it.

The 2026-08-02 overlap refresh found no public intra-block executor in current
`origin/master` or `origin/testnet`. PR #2485 remains the dedicated-collator
deployment boundary, while the testnet changes after merged PR #2505 remain in
the networking/Plumtree path. This is only a public-code overlap result; it does
not establish the absence of private work.

The next shadow instrumentation is now implemented but not yet measured. Exact
offline collation records only successful ordinary-transaction creation wall by
destination account and phase. The ValidationReplayer reports separate
`inbound_internal`, `external`, `new_or_deferred`, `special`, and `all_ordinary`
ceilings as `serial residue + greedy account critical path` for 1/2/4/8/16
workers. The all-phase view merges the same account before lane planning.
Default online collection remains bounded and stores no per-account work map. A
full run requires a copied validator database with the historical queues/states;
the latency-sensitive live node is explicitly out of scope.

The scheduler now also has an executable immutable worker-receipt header and a
deterministic account-chain validator. It rejects non-canonical input order,
predecessor/sequence mismatches, account-local completion holes, and worker
receipts for coordinator-only items. This is not live parallel execution:
global deltas, worker runtime, descriptor/limit application, and serial state
commit remain explicit implementation gates.

The first payload primitive is now executable as well. An anchored
`CellUsageJournal` records worker-local reference paths and cell commitments.
The coordinator resolves every path from its own immutable root before replaying
it into the serial usage tree. Unit tests preserve the exact Merkle-proof hash,
show arrival-order-independent journal union, and reject wrong anchors, paths,
cells, and duplicates without partially mutating the coordinator tree. It is not
wired to collator execution yet, and separate account-storage journals remain
open.

The first immutable Transaction payload is now executable. It materializes the
canonical Transaction root, post-account cell, and an ordered journal set; it
never retains the mutable Transaction/Account objects. The coordinator reparses
TL-B, derives account/pre/post/transaction hashes, gas, LT interval, and ordered
out-messages, then recomputes effects and journal commitments. A pure batch
precommit gate returns the initial checkpoints on any payload or receipt-chain
failure. Unit tests tamper every derived header field. A copied full-block
mainnet replay validated all 51 payloads and 42 ordered out-messages while
preserving exact transaction/account-state hashes. Journals were empty, so this
is an ABI/equivalence gate rather than measured parallel execution.

The next coordinator-delta subset is executable for basechain transactions.
Canonical effects now commit total fees and account status transitions, and a
fail-closed batch helper applies the same transaction-level LT, gas, proof/cell,
transaction-count and first-account-count mutations as `Transaction::update_limits`
to a shadow `BlockLimitStatus` before publishing it. Gas charging is a
coordinator-supplied per-transaction phase flag; it is not worker-controlled.
Offline replay additionally
requires the sum of canonical transaction fees for each account to equal its
existing `AccountBlock` augmentation.

The canonical message subset is also executable without changing the live
collator. It reconstructs `NewOutMsg` registrations from ordered transaction
outputs while accepting metadata only from coordinator context. For inbound
internal work it materializes the exact `msg_import_fin` descriptor and the
optional paired `msg_export_deq_imm`. The copied full-block replay reconstructed
all 42 registrations and matched cell hashes for 10 inbound and 10 paired
outbound descriptors. A separate atomic prefix helper validates the entire
canonical queue slice before callbacks and publishes `ProcessedUpto` only to
the last successfully committed item; pending, failed, limited and
commit-failed items cannot expose a completed suffix.

The message, account and limit subsets now have a single atomic shadow publish.
It first applies a canonical ready prefix to a private coordinator state and
publishes account cells, descriptor cells, descriptor-derived queue deletions,
new-message registrations, limit counters and `ProcessedUpto` together. Any
malformed effect leaves the original shadow state unchanged. A copied
full-block replay committed all 10 `msg_import_fin` entries in canonical
`(lt, hash)` order, covering five first-account counters, 10 exact InMsg cells,
10 paired OutMsg/dequeue cells and 22 newly registered messages. All 51
historical transaction and account-state hashes remained exact. This verifies
the coordinator contract for that bounded input; it does not construct
augmented dictionary roots or measure parallel speedup.

An additional fail-closed commit gate now operates on real TON
`AugmentedDictionary` instances. It mutates private copies of `ShardAccounts`,
`InMsgDescr`, `OutMsgDescr`, and `OutMsgQueue`; queue additions, replacements,
and deletions share one atomic batch. Replacements and deletions verify the
exact predecessor value, and no roots are returned unless the entire batch
succeeds. The copied replay combined and advanced the 29 account proofs to the
target predecessor, required that proof root to match the target block Merkle
update's old `ShardAccounts` root, bound all 29 account values, then removed the
coordinator's derived descriptors from the target dictionaries and replayed the
inserts. The result reproduced the target `InMsgDescr` and `OutMsgDescr` roots
exactly.

This validates two of four dictionary roots on the available fixture. Directly
applying the block Merkle update to an `AugmentedDictionary` fails on a pruned
branch: the update proves the old and new state hashes but does not retain every
sibling augmentation needed to enumerate changed dictionaries. Core resolves
this in `Collator::prepare_proofs`, called by `create_collated_data` after the
new shard state and block Merkle update have been built. `ValidateQuery` reads
those Merkle proofs from `candidate.collated_data` before validating the state
transition.

The replay tool now accepts that artifact through `--collated-data`. It requires
one predecessor-state Merkle proof with an exact virtual-root hash match,
enumerates all `OutMsgQueue` additions/replacements/deletions, applies the full
account and queue delta set atomically, and exposes four validated roots only
after exact target-root comparison. The copied archive does not contain the
historical candidate collated data, so the current mainnet-derived result
correctly remains at two roots. A target state, block BOC, or fabricated queue
baseline would make the check circular and is rejected as a substitute.

`ValidationReplayer` can now export one newly replayed candidate's collated-data
BOC from a single offline collate run. The export uses create-new semantics and
is disabled for ranges and validate-only mode. This provides the positive
four-root fixture path without retaining candidate bytes in the bounded run
history or changing consensus behavior. It still requires a dedicated copied
validator database; the latency-sensitive production node remains out of scope.

The descriptor baseline is `target minus validated deltas`; it is an exact
delta round-trip through the real augmented dictionaries, not proof of the
historical predecessor-to-target transition. Replay output encodes this as
`augmented_dictionary_historical_transition_proven=false`.

Masterchain public-library deltas, collator usage-tree proof size,
storage-dictionary updates, live `ProcessedUpto` wiring, value flow and full
state merge remain outside this gate. The remaining two roots are implemented
but unproven on the copied mainnet fixture until its matching collated-data
witness is available. No TPS or parallel speedup follows from this correctness
result.

## Copied-corpus expansion gate - 2026-08-02

The sparse replay can now enumerate a closed shard archive without trusting a
separate index. It checked the file and root hashes of 99 archived basechain
blocks, covering seqnos `87341652..87341750`, and exposed each exact block id,
masterchain reference, timestamp, serialized size, distinct-account count and
raw transaction count. The slice contains 1,470 raw transactions in 99 blocks;
68 blocks are non-empty and the files total 3,752,862 bytes. Over the 41-second
timestamp span the observed finalized rate is 35.85 raw tx/s. This is demand,
not capacity.

An ordinary least-squares diagnostic over all 99 blocks gives
`serialized_bytes = 5,057 + 2,212.39 * raw_transactions` with `R^2 = 0.9707`.
Extrapolating that workload mix to the 2 MiB serialized-candidate limit at the
400 ms target rate gives 2,364 raw tx/s. Separate fits over the 68 non-empty
blocks and the 12 blocks with at least 40 transactions give 2,444 and 2,370 raw
tx/s respectively. Their convergence supports the earlier single-block 2,351
raw tx/s byte-envelope estimate, but does not turn it into sustainable mainnet
throughput: the largest observed block is only 255,924 bytes, 12.20% of the
limit, so every hard-limit result still extrapolates far beyond the sample.

The largest copied block, `87341719`, contains 111 raw transactions across 77
accounts, with at most eight transactions on one account. It is the first
priority for a new exact-predecessor proof set because its existing proof set
is state-stale while its block, configuration proof and library bundle are
already available locally.

Two additional proof sets appeared complete by address coverage: 29 accounts
for block `87341683` and 77 accounts for block `87341719`. Both were rejected
before timing. In the first set, a required account had changed in intermediate
block `87341680`; in the second, a required account had changed in intermediate
block `87341718`. The proof roots therefore described older shard states even
though their account-address sets matched the target blocks. No performance
number from either block is retained.

Inspection now prints the exact predecessor shard block as the recommended
account-proof reference, and the loader accepts proofs directly bound to that
block in addition to the existing masterchain-bound form. This removes the
intermediate-history ambiguity for newly collected linear-block fixtures. The
direct predecessor path is source-validated and compiled, but a positive copied
fixture has not yet been collected. The existing 51-transaction fixture still
passes full replay and the executable four-worker equivalence gate. A copied
validator database remains necessary for real Collator and ValidateQuery wall
measurements; archive packages and account proofs are not a substitute.

## Dead ends and cautions

- Search results did not expose a public TON trace JIT or public intra-block
  parallel collator as of the cutoff. This is an overlap check, not proof that
  no private implementation exists.
- Agave search results were too broad to support a detailed 2026 scheduler
  claim; only the stable account-lock comparison is retained.
- Reported throughput from Remora, Pilotfish, Aptos or TON test campaigns is not
  transferred to this design. Different workloads, block limits and end-to-end
  boundaries make such arithmetic invalid.

## Next invalidation checks

Refresh the following before integration work or any public performance claim:

1. `origin/master`, `origin/testnet`, #2485 and recent TON PRs;
2. dedicated-collator parameter and actor changes;
3. new database interface and CellUsageTree/Merkle-update changes;
4. full offline collation/validation wall-time decomposition;
5. exact serial/parallel roots on mainnet-derived copied state.
