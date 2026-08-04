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
intermediate-history ambiguity for newly collected linear-block fixtures. A
fresh positive fixture is recorded below. A copied validator database remains
necessary for real Collator and ValidateQuery wall measurements; archive
packages and account proofs are not a substitute.

## Verified raw-block input gate - 2026-08-02

The fixture path no longer requires a complete shard archive package for the
target block. `lite-client saveblock` requests one full `BlockIdExt`, rejects a
different returned id, verifies both the serialized-file and BOC-root hashes,
and writes a new owner-only file without overwriting. The replay tool repeats
both checks before using `--block-boc`.

Raw input remains fail closed for older account proofs. Optional repeated
`--history-block-boc` inputs derive their own full ids from the block header and
the two computed hashes. Replay then checks the complete predecessor linkage,
shard, sequence, and per-account non-modification path from each proof base to
the target predecessor. It neither trusts filenames nor silently advances a
proof through a missing block.

The copied block `87341675` exercised this path with a 113,736-byte target BOC,
a 1,124-byte intermediate block `87341674`, the existing configuration proof,
29 account proofs based at `87341673`, and the public-library body bundle. The
raw-BOC and archive modes both replayed 51 transactions across 29 accounts.
After excluding timing-derived ranks and rates, their block/config/proof ids,
historical transaction and account-state equivalence, effect counters, gas,
VM-step totals, and per-code-hash/account distributions were identical. Missing
history, a wrong target root hash, a short target id, duplicate block sources,
and output overwrite were rejected.

This gate makes small, immutable fixtures collectable from a lite server. It
does not measure live-node capacity, run Collator or ValidateQuery, retain
historical candidate collated data, or establish sustainable single-shard TPS.
The network path is now exercised under the bounded collection described below.

## Fresh exact-predecessor mainnet gate - 2026-08-02

A bounded read-only lite-server pass collected and verified 32 consecutive
basechain block BOCs at seqnos `88028060..88028091`. The files contain 696 raw
transactions in total, 20 blocks are non-empty, and their serialized size is
1,850,656 bytes. Collection used block/proof/library queries only: no validator
restart, configuration change, database scan, consensus participation, or
validation workload was performed. The node service remained active with zero
restarts, `/var` remained at 37%, block-sync queues remained empty, and the
observed I/O pressure stayed below the collection stop threshold.

Block `88028077` was selected before timing because it is the largest member of
the slice: 374,873 bytes, 138 transactions, 108 touched accounts and at most 11
transactions on one account. Every account proof is bound to exact predecessor
`88028076`; the config proof is bound to masterchain block `83522569`.

The first strict replay failed on transaction LT `94139016000005`. It was not a
Config 45/precompiled-gas issue: the failing code hash differed from the active
precompiled entry. The direct library scan had found 11 public-library refs, but
those bodies contained three additional library refs. Running with the partial
set changed VM exit code from the historical 0 to 9 and produced two messages
instead of four. The loader now rejects any non-transitively-closed combined
library body set and reports the missing hashes. Supplying the three verified
transitive bodies restored exact execution.

The complete Release replay then matched all 138 historical transaction hashes
and all 138 resulting account-state hashes across 108/108 accounts. It validated
134 TVM executions, 19,226 VM steps, 791,308 billed gas and 30 Ed25519 checks.
The isolated account probe measured 166.568 ms serial and 46.424 ms with four
workers, a 3.588x wall speedup, with the 108 accounts distributed 26/28/27/27.
A later independent process passed the same exact equivalence gate and measured
2.981x. The spread confirms that one-shot timings are not a stable performance
result. Both samples include thread creation plus deliberately duplicated
per-lane setup; they exclude live Collator integration, reusable actor workers,
augmented-root commit, ValidateQuery, network and consensus.

The target is only 17.88% of the 2 MiB serialized candidate cap. Its linear
byte-envelope projection is 1,930 raw tx/s at the 400 ms configured target rate;
this is lower than the older sparse-corpus fits and remains an extrapolation of
a mixed, unsaturated workload. `mainnet_sustainable_raw_tps` therefore remains
`null`. The exact-predecessor proof set binds the old ShardAccounts root and the
replay reconstructs the two descriptor roots. Matching historical collated data
is still required for the ShardAccounts and OutMsgQueue transition roots; the
result correctly reports two validated roots rather than four.

## Tick/tock isolation gate - 2026-08-02

Source inspection confirms that `ValidateQuery` rejects tick/tock outside the
masterchain. The collator creates masterchain ticks before ordinary processing
and tocks after it, and accounts for them without user gas in its block-limit
update. They are deterministic system work on the masterchain, not basechain
user transactions and not independent account lanes for the single-shard
executor.

Hot-path telemetry now labels every TVM execution as `ordinary`, `tick_tock`, or
`other` in both the total and per-code-hash counters. Historical BOC inspection
separately classifies ordinary, tick, tock, storage, split, and merge transaction
descriptions. The classifier fails closed on malformed or unknown descriptions,
and the sum must equal the raw transaction count. This prevents permanent
masterchain system work from selecting an ordinary-contract JIT target or from
being reported as basechain workload.

The fresh basechain fixture `88028077` contains 138 ordinary transactions, zero
tick/tock transactions, and 134 ordinary TVM executions. A separate bounded
read-only collection of 16 consecutive masterchain blocks
`83536320..83536335` contains exactly 48 transactions: every block has one
ordinary transaction, one tick, and one tock. The files total 162,945 bytes.
This measured window supports treating two tick/tock transactions per
masterchain block as a stable serial residue; it does not prove a longer-term
frequency distribution.

Collection used block lookups and downloads only. The node remained active with
zero restarts, `/var` at 37%, and no configuration or database mutation. The
locally configured external lite-server endpoint subsequently refused a new
connection while the node's internal service remained healthy; already copied
BOCs were therefore classified offline. A diagnostic self-derived block id is
explicitly unanchored, while normal inspection and replay still require the
independently obtained full id.

The executor design consequence is narrow: tick/tock remains in the serial
masterchain floor. It does not lower the measured basechain account-lane ceiling
and cannot be used to claim a higher or lower sustainable basechain TPS.

## Reusable worker-pool gate - 2026-08-02

The executable account replay no longer creates and joins operating-system
threads inside each measured batch. One fixed pool waits until all workers are
ready, then executes repeated serial and parallel batches through the same
fail-closed barrier. A task exception fails the batch after all submitted work
has completed and does not terminate or poison the pool. Unit tests verify
worker identity reuse, concurrent execution, invalid-batch rejection, contained
task failure, and successful reuse after failure.

The benchmark accepts an odd `--account-samples` count up to 31 and alternates
serial-first and parallel-first pairs. Raw samples remain in JSON; summary wall
times and lane times are medians. Pool startup is measured separately and is
not included in either serial or parallel batch wall.

Five Release pairs on basechain block `88028077` again matched all 138
historical transaction hashes and resulting account-state hashes across 108
accounts. The four lanes contained 26/27/27/28 accounts. Serial batch samples
were 180.954, 181.519, 219.047, 193.244, and 181.343 ms; parallel samples were
49.879, 56.554, 58.285, 62.223, and 56.021 ms. The paired speedups were 3.628x,
3.210x, 3.758x, 3.106x, and 3.237x, with a 3.237x median. Pool startup was
0.391 ms. Median isolated replay rates were 760 raw tx/s serial and 2,440 raw
tx/s parallel.

This removes thread creation from the measured executor path and bounds the
one-shot variance observed earlier. It still duplicates per-lane setup and
shadow checks and does not include augmented-root commit, Collator mutation,
ValidateQuery, block delivery, or consensus. `mainnet_sustainable_raw_tps`
therefore remains `null`. The next gate is a disabled offline Collator path that
feeds immutable ordinary account chains to this pool, retains tick/tock on the
coordinator, and requires serial candidate and ValidateQuery equivalence.

## Offline immutable-account commit gate - 2026-08-02

The replay utility now has an opt-in `--offline-collator-workers` path. It does
not wire workers into the live Collator. Whole-account chains run concurrently
against immutable predecessor proofs and return canonical transaction effects,
coordinator inputs, and account dictionary deltas. A serial coordinator sorts
by canonical transaction/message/account keys, rejects duplicates, requires
exact equality with the serial artifact set, and then applies the existing
block-limit, coordinator, and augmented-root gates once.

The failure boundary is deliberately coarse. A worker exception, missing or
duplicate artifact, limit failure, coordinator failure, or root mismatch
discards the entire offline batch before publication. Ordinary basechain
transactions are the only accepted worker input. A tick/tock, storage, split,
or merge description makes the current probe fail closed; separating those
descriptions into serial coordinator work remains an integration gate. The
64-worker CLI bound is only a local resource guard.

Subphase instrumentation invalidated those initial whole-path numbers as a TON
performance result. Three serial runs spent 2.890, 2.882, and 2.898 s inside
predecessor-proof assembly, out of 2.899, 2.890, and 2.907 s for the complete
augmented-root phase. The actual descriptor baseline took about 1.2 ms, account
binding about 5.3 ms, and the available root commit about 1-2 ms. The apparent
serial root bottleneck was almost entirely the replay utility joining 108
separate lite-server account proofs one at a time. Live Collator does not do
this operation.

The proof path now uses TON's `MerkleProof::combine_fast()`, which is already
used by lite-client. A dedicated checked-in test generates disjoint proofs,
compares the fast and slow virtual roots, reads every merged path, checks repeat
determinism, and rejects empty, null, and different-root inputs. Three exact
corpus replays reduced predecessor-proof assembly to 8.976, 8.637, and 8.710 ms.
Complete serial replay fell to 199.688, 178.460, and 181.844 ms while preserving
all transaction, account-state, artifact, and available root checks. The roughly
330x proof-merge reduction corrects the benchmark; it is not a mainnet
optimization.

Five subsequent four-worker runs produced serial full-replay samples of
196.491, 214.769, 203.214, 194.558, and 180.947 ms. Parallel-account plus
serial-commit samples were 89.051, 78.121, 83.061, 101.674, and 84.467 ms.
Every exact paired run improved, by 1.91x to 2.75x, with a 2.21x median. Median
serial and prepared walls were 196.491 and 84.467 ms; for 138 raw transactions
they correspond to diagnostic replay rates of 702 and 1,634 raw tx/s. The
serial reference still precedes the prepared path in one process, so these are
not substituted for an alternating live-Collator benchmark. The copied-mainnet
corpus and raw process outputs remain local and are not included in this
repository.

One additional worker sweep kept the corpus and exact gates fixed. Prepared
walls for 1, 2, 4, and 8 workers were 225.504, 120.658, 81.566, and 74.215 ms,
or 612, 1,144, 1,692, and 1,859 diagnostic raw tx/s. The 4-to-8-worker step
saved only 7.351 ms because the serial commit remained about 39 ms. Each point
is one serial-first sample, so the curve locates the current serial floor but
does not establish sustainable shard capacity. The raw sweep outputs are not
published in this repository.

The reusable pool, proof merger, canonical payload, coordinator, scheduler, and
atomic root helpers have checked-in unit tests. Since 2026-08-04 the complete
offline orchestration path also has a checked-in positive integration test: the
`tvm-replay-four-root-fixture` CTest replays mainnet block `88028077` from a
compact committed BOC/proof fixture and requires the full fail-closed four-root
gate in both the serial replay and the offline collator probe. The external
copied-mainnet corpus is still required for timing sweeps, but no longer for
regression coverage of the correctness path.

The corpus still lacks the target candidate's historical collated-data witness.
A later block-bound OutMsgQueueInfo proof made a stricter hindsight gate
possible: combine all predecessor account proofs, bind the exact queue proof,
use the finalized target MerkleUpdate only as the old/new state commitment, and
reject any descriptor, account, or queue mismatch. The first attempt exposed a
coverage bug: the transaction effect set contains 100 generated outbound
messages, while the target OutMsgDescr also contains 14
`msg_export_deq_imm` records. The corrected gate removes all generated records,
requires every remainder to unpack as `msg_export_deq_imm`, binds its envelope
and reimported InMsg to the canonical transaction, and derives the queue key.
Block `88028077` then bound 138/138 InMsg and 114/114 OutMsg descriptors and
validated all four augmented roots. Because the witness uses finalized target
data, `augmented_dictionary_historical_transition_proven` deliberately remains
false.

Seven alternating account-replay pairs gave median serial/parallel walls of
37.096/19.479, 35.285/12.656, and 37.613/8.903 ms for 2, 4, and 8 workers.
The corresponding single serial-first full offline paths, including the
four-root gate, measured 47.077/30.916, 44.013/26.788, and 45.258/24.648 ms,
or 1.523x, 1.643x, and 1.836x. Exact artifact and replay-result checks passed
at every point. This still does not mutate a live Collator, create the candidate
BOC, run ValidateQuery, or measure delivery. The direct next gate remains a
copied mainnet validator database with byte-identical candidates and normal
ValidateQuery; `mainnet_sustainable_raw_tps` remains null.

## Checked-in four-root regression fixture - 2026-08-04

The four-root replay gate no longer depends on an uncommitted local corpus for
its positive test. `test/validator/data/four-root-88028077/` now contains the
raw target-block BOC, all 108 predecessor-bound account proofs, the config
proof, the predecessor `OutMsgQueueInfo` proof, and the transitively closed
library bodies, 1.2 MiB in total. The `tvm-replay-four-root-fixture` CTest
rebuilds the exact witness-mode command and requires `full_block` scope,
138/138 InMsg and 114/114 OutMsg descriptor bindings, and
`augmented_dictionary_roots_validated=4` from both the serial gate and the
`--offline-collator-workers 2` probe; any narrowing of the replay scope fails
the test. The block-workload classifier moved from the CLI into
`validator/validation-replay/block-workload.{h,cpp}` so that
`test-block-workload-fixture` can pin transaction kinds against two hash-pinned
blocks: masterchain block `83536321` with exactly one ordinary, one tick, and
one tock transaction, and the basechain fixture block with 138 ordinary
transactions across 108 accounts. Malformed and null transactions fail closed.

The masterchain fixture intentionally covers classification only. No
masterchain account proofs are committed, a tick/tock block still fails closed
in the offline collator probe, and separating tick/tock into a serial
coordinator lane remains an open integration gate. Fixture wall times are not
measurements; the timing corpus stays external, and
`mainnet_sustainable_raw_tps` remains `null`.

## Saturated jetton corpus gate - 2026-08-04 - REJECTED

The replay-only parallel Collator path was run for the first time against a
saturated jetton workload, and the byte-equality gate correctly rejected every
run. This section records a real open defect, not a passed milestone.

Setup: the jetton TPS benchmark tooling now runs on Windows. `td::mkpath`
accepts `/`-joined paths on Windows (it previously split only on `\`, so
benchmark tools silently failed to create directories), `bench_jetton.py`
gained `--build-dir`, an optional `choom` wrapper, and a `--vrp-workers` gate
phase that replays the most transaction-heavy wc0 blocks through
`vrp run --mode both --parallel-account-workers N` on the same tontester node
that produced them. `bench-state-gen self-test` passes on Windows; a
100,000-wallet-pair state (800,056 cells, 113 MB celldb) boots the network and
serves spam.

Workload: 27,000 wallet-v5 externals at 600/s for 45 s; 23,814 were included
within the measurement window, 1,304 included raw tx/s and 434 jetton
transfers/s as a local Windows diagnostic only. The three gated blocks carried
674-684 raw transactions at 1.30-1.32 MB serialized, past the 1 MiB Config 23
soft byte threshold.

Result: all 18 gate runs (blocks 10/14/58, workers 2/4/8, serial-first and
parallel-first) failed with the same deterministic signature:
`id=false, block_bytes=false, state_update:false`, while `value_flow`,
`extra`, `in_msg_descr`, `out_msg_descr`, `account_blocks`,
`collated_hash`, and `collated_bytes` all matched byte-for-byte. The parallel
candidate was 17,501 and 13,238 bytes smaller on blocks 58 and 10 and 2,536
bytes larger on block 14. The committed transaction set and all descriptor
roots are identical; only the Merkle-update cell-usage footprint diverges, in
both directions, reproducibly per block.

Interpretation: the worker cell-usage journal rebase does not reproduce the
serial pass's prev-state usage set under deep-inbound-queue jetton cascades.
The earlier synthetic corpora (independent compute messages, empty-body
transfers) did not exercise whatever path leaks or adds these reads. Because
collated data matches while the state update differs, the divergence is
specific to the usage set consumed by `MerkleUpdate` generation rather than to
the collated-proof builder. The executable path remains rejected for live
collation, and this defect must be root-caused before any further executor
claim. `mainnet_sustainable_raw_tps` remains `null`.

Repro artifacts: the node database containing blocks 10/14/58 is preserved
locally (969 MB) together with `vrp-gate.log`, `results.json`, and
`blocks.csv`; none of it is committed. The state is regenerable from seed
`ab..ab` with `--v5-count 100000`, but block byte-identity across regenerated
networks is not expected.

## State-update divergence localization - 2026-08-04 - STILL OPEN

The mismatch path now diffs the two candidates' Merkle-update cell footprints.
It walks both `state_update` BOCs, records every materialized cell with its
ref-index breadcrumb from the update root (`0/...` is the pruned old state,
`1/...` the new state), and reports cells one pass materialized while the
other pruned them. Diverged cells under the accounts subtree are decoded to an
account address and probed against the block's `AccountBlocks`. A new
replay-only `vrp run --watch-account <hex>` option then logs every collator
access to that account in both passes, including the worker's complete
cell-usage journal.

Measurement across three saturated jetton blocks and 18 gate runs is stable
and symmetric: each block diverges on exactly **two** cells, never more.

1. `path=0/...` (old state): one cell per block that the serial pass
   materialized and the parallel pass pruned. It decodes to a
   `ShardAccounts` leaf for an account that **does** have a committed
   transaction in the same block, and its hash differs per block.
2. `path=1/...` (new state): the same single cell `BEB0683E...`
   (80 bits, 1 ref) in every block and every worker count, materialized by
   the parallel pass and pruned by the serial one.

These two are one phenomenon seen from both sides. `MerkleUpdate::generate_raw`
prunes a new-state cell only when it is still a `UsageCell` carried over from
the previous state, and marks that path so the old side expands it. The
parallel pass therefore holds a **plain** cell where the serial pass holds a
usage-tracked one, so the new side keeps the body and the old side loses the
marked path. The watch trace shows the shared cell reached at two distinct
paths inside one worker journal, so a deduplicated cell reachable from several
trie positions is the prime suspect.

One candidate fix was implemented and rejected by measurement: rebasing
`total_state` wrappers from the message and storage contexts in addition to
the account context. It changed nothing (18/18 still failed, identical
footprint counts), so it was reverted rather than left in a consensus-critical
path as an unverified change. The diagnostics were kept.

## Outbound-message wrapper rebase - 2026-08-04 - PARTIAL FIX

Adding the account owner to the footprint diff showed the shared new-state
cell `BEB0683E...` sitting inside an account that is *not* the one whose
old-state leaf diverged. A worker's outbound message payload retains cells
loaded from that worker's private old-state snapshot; the payload was
published to `register_new_msgs` without rebasing, so when a later in-block
transaction persisted such a cell into its own account state, the coordinator
lost the usage link the serial pass kept.

`commit_parallel_inbound_transaction` now rebases `trans->out_msgs` through
the same account/message/storage contexts as `total_state`, before the
messages are registered. On a fresh saturated corpus this closed the original
divergence: **12 of 18 gate runs now produce byte-identical block and collated
data and pass the ordinary `ValidateQuery`**, including all six runs on two
blocks of 672 transactions each (445 and 419 of them executed through the
replay-only parallel path, gas 4.38M and 4.31M, `internal_load` 0.954 and
0.906, zero discarded prepared results).

Two findings keep this from being a success:

1. **Measured slowdown.** Every passing run has a serial/parallel speedup
   below 1.0: 0.58-0.97, median about 0.79, with no trend favouring more
   workers (2, 4 and 8 workers are indistinguishable inside that band). On
   this workload the replay-only parallel path is *slower* than serial
   collation. Prepare/worker/commit accounting must be decomposed before any
   executor gain is claimed; the earlier synthetic 1.19-1.74x numbers do not
   transfer to a saturated jetton block.
2. **A second, different defect remains.** The 684-transaction block failed
   all six of its runs with an unrelated signature: `account_blocks`,
   `in_msg_descr`, `out_msg_descr` and `value_flow` all differ, and the
   parallel candidate is 21,104 bytes *larger*. The two passes committed
   different amounts of work rather than pruning the same work differently,
   and the surviving footprint divergences sit under `path=0/0/...`
   (`OutMsgQueueInfo`), not under the accounts subtree. The untested
   hypothesis is that block-limit estimation via
   `update_account_dict_estimation`/`add_proof` diverges once a block reaches
   the byte boundary, so the two passes stop at different queue positions.
   Nothing in this section may be read as an equivalence result for
   limit-bound blocks.

## Limit-estimate convergence - 2026-08-04 - 18/24, ONE RESIDUAL

The stop-position hypothesis above was confirmed and mostly closed in two
steps. First, `trans->update_limits` was reordered after the wrapper rebase:
`add_proof(new_total_state)` classifies a retained old-state subtree as a
proof boundary only when its wrapper belongs to the state usage tree, so
worker-tree wrappers were descended into and deduplicated instead of being
counted as per-account external references, and the parallel pass's
block-size estimate drifted low by 6-18 KB. New replay-only stop telemetry
(`REPLAY_INBOUND_STOP`, logged at every inbound-phase exit under `is_replay`)
then showed byte-equal size estimates at every phase stop. Second, a
deterministic boundary guard keeps the limit-adjacent region on the serial
path (256 KiB byte and collated margins, 2M gas, 2000 lt against the
`cl_normal` thresholds): the 64-entry lookahead materializes queue cells
through the state usage tree before the serial pass would load them, and a
phase that ended with touched-but-unprocessed entries left those cells in the
collated proof, observed as a +1.5 KB collated-only mismatch on an otherwise
byte-identical block.

After both changes, 18 of 24 gate runs pass: limit-bound blocks of 672-682
transactions produce byte-identical block and collated data and pass the
ordinary `ValidateQuery`, with up to 377 of the block's transactions executed
through the replay-only parallel path. The measured speedups remain mostly
below 1.0 (0.68-1.41 across passing runs), so no performance claim changes.

One residual defect remains possible. On one of four gated blocks in one
corpus the serial pass stopped at 458 transactions and the parallel pass at
457: the size estimates differed by only about 100 bytes in 1.05 MB, but that
flipped one transaction exactly at the byte threshold. The residual comes
from wrapper-topology differences between the passes: for a deduplicated cell
the serial pass can retain a usage wrapper recorded on another account's
path, while the rebased worker state carries its own path's wrapper, and the
`external_refs` term of `add_proof` counts boundary encounters rather than
unique cells. Closing it for good requires either wrapper-topology parity or
a wrapper-independent size estimate; the latter would slightly change live
block packing and needs an explicit decision before implementation.

An independent reviewer audited all parallel-path changes and confirmed the
live-collation isolation site by site (startup rejection, `is_replay`
provenance, options provenance, zero-default watch account); the verdict was
safe to push with no consensus-path findings. The one major replay-only
finding - the rebase traversal visited a shared subtree once per path, which
an adversarial DAG-ladder contract could blow up exponentially - is fixed
with per-cell memoization. After that fix a fresh saturated corpus passed
**24 of 24** gate runs (four blocks of 670-672 transactions, workers 2/4/8,
both pass orders, byte-identical block and collated data, `ValidateQuery`
green).

## Wrapper-topology residual closed - 2026-08-04

New replay-only telemetry (`REPLAY_LIMIT_DELTA`, enabled by the vrp gate for
both passes) logs the per-transaction `st_stat` delta around
`trans->update_limits`, making the two passes comparable point by point. On
two saturated blocks the sequences diverged at exactly **one transaction per
block**: same account, same serial call site, but `dext=2` in the serial pass
versus `dext=0` in the parallel pass, with the retained old subtree counted
as ~17 extra proof cells (~924 estimate bytes) - precisely the observed
drift. The account was one that had already received a parallel-committed
transaction: its in-memory `code`/`data` fields kept worker-tree wrappers
(the reviewer's section-C observation), so a later serial transaction on the
same account executed through them and its retained cells lost the state-tree
linkage that `add_proof` uses to find old-state boundaries.

`commit_parallel_inbound_transaction` now rebases `new_code`, `new_data`,
and `new_library` alongside `new_total_state` and the outbound messages.
With the fix, per-transaction delta parity is exact: **zero** differences
across 686- and 682-transaction blocks, and the gate passes. The estimate
drift is closed at its source rather than masked by margins; the boundary
guard remains as the defense against lookahead-tail proof leakage, which is
a separate mechanism.

## Where the parallel path loses time - 2026-08-04

With equivalence holding, the gate's own counters decompose the parallel
pass. On 670-680-transaction blocks the worker phase is negligible: prepare
7-13 ms and worker execution 7-24 ms for 277-388 parallel transactions, while
the serial commit of those same transactions costs 84-130 ms. The batch
portion is therefore already faster than serial execution of the same
messages, yet whole-block collation was slower, so the loss was outside the
measured prepare/worker/commit scopes.

It was the phase-boundary flush. Committed accounts stay in
`replay_parallel_account_continuations_` for the whole collation, and every
`flush_parallel_account_continuations()` call replayed every retained
journal again, resolving each recorded path from the root - quadratic in the
number of parallel accounts. Skipping the repeat outright is wrong: the gate
rejected 6 of 18 runs, proving journals still grow after commit when a later
serial transaction reads through a worker wrapper that the account fields no
longer expose but other retained state still does. Versioning the flush by
journal entry count instead - replay only when new entries appeared - is both
correct (18 of 18 pass) and effective: the median serial/parallel speedup
moved from 0.73 to 0.94 (0.69-1.38 range), with 2 workers at 0.97.

The path is still not faster than serial collation on this workload. The
remaining serial residue is the ordinary commit path itself, not worker
scheduling, so adding workers changes nothing (2, 4 and 8 workers stay inside
the same band). Any further gain has to come from the commit/serialization
side, which is exactly what the W6 serialize-tail design targets.

## Mainnet corpus sizing - 2026-08-04 - read-only survey

A read-only listing of the archive node (no copy, no restart, no database
scan) establishes what a mainnet-database gate would actually cost.

The archive holds 311 GB of persistent states across six epochs. The newest,
masterchain seqno `83668373` from 2026-08-03, is 60.6 GB: a 9.21 GB
masterchain state plus 16 basechain account parts of about 3.2 GB each, plus
two zero-length split markers. Archive block packages are separate and much
cheaper, about 3.1 GB per 100k-block slice directory.

Two findings change the plan:

1. **The basechain has split.** Epochs `81237737` through `83363951` archive
   a single shard `8000000000000000`. The newest epoch archives
   `4000000000000000` and `c000000000000000`, eight account parts each, and
   the newest package slice contains per-shard archives for all three shard
   identities. The single-shard premise that this research has used since the
   first measurement no longer holds on current mainnet, and any capacity
   statement must say which shard topology it describes.
2. **A full epoch does not fit locally.** The working disk has 67.5 GiB free
   against a 60.6 GB epoch, and importing that into a validator database
   needs comparable space again. Copying one whole epoch is therefore not
   viable without another disk. The selective path - masterchain state plus
   only the account parts covering the target blocks' touched prefixes - is
   the practical option, and `SelectiveSplitStateAssembler` already exists
   for the offline tool but is not wired into the database import path.

No data was copied and the node was not loaded; this section records sizes
and a decision, not an action.

## Replay-only Collator integration - implementation gate

The Collator now contains a default-off, replay-only path for inbound internal
messages with non-empty bodies to previously untouched, distinct destination
accounts. Each worker gets private account, message, configuration, and
cell-usage state. A fixed 64-message lookahead feeds a reusable worker pool, so
the number of prepared tasks is independent of the worker count. The
coordinator rebases worker `UsageCell` paths onto the original proof anchors,
replays the recorded account/message/storage load journals, and commits
successful transactions in canonical `(lt, message hash)` order. Existing
tick/tock execution, empty-body transfers, and accounts already touched by
tick/tock, dispatch, or an earlier transaction stay serial. If a block limit or
internal timeout is reached after only part of a batch can commit, the
continuous canonical prefix is retained and the prepared suffix is discarded;
the queue tail and `ProcessedUpto` do not cross the missing suffix. This closes
the partial-frontier and proof-accounting gaps without adding speculative
execution or parallel scheduling within one account.

The corresponding `vrp` option requires `--mode both`. It always produces a
serial reference and a parallel candidate from the same replay inputs, rejects
any byte difference in block or collated data, and runs the ordinary
`ValidateQuery` on the parallel result. A live Collator rejects the non-zero
worker option, so this code does not change consensus behavior, configuration,
fees, TVM semantics, or validator voting.

The local one-validator integration gate is now measured on bounded synthetic
blocks. A 268-transaction compute block reached 10,019,448 gas and executed all
268 destination accounts through the replay-only path. Nine paired samples
produced byte-identical candidates and passed normal `ValidateQuery`; median
Collator speedups were 1.365x, 1.193x, and 1.741x for 2, 4, and 8 workers. A
separate exact-candidate boundary gate committed 100 transactions at 10,000,000
gas and discarded 28 prepared suffix results without advancing the queue
frontier. Cheap empty-body transfers remain serial by design and provide no
parallel speedup claim.

These are synthetic replay diagnostics, not sustainable shard TPS. The harness
records `mainnet_sustainable_raw_tps=null`; it does not exercise production
candidate delivery, Plumtree, multi-validator deadlines, database growth, or a
representative jetton/DEX workload. Matched runs on copied mainnet validator
state remain required before the implementation can advance beyond
experimental status.

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
