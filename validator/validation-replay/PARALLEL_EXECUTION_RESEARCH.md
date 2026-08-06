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

1. **The archived epoch caught a load-driven split.** Epochs `81237737`
   through `83363951` archive a single shard `8000000000000000`; the newest
   epoch archives `4000000000000000` and `c000000000000000` with eight
   account parts each, and the newest package slice contains per-shard
   archives for all three shard identities. This is ordinary TON behaviour,
   not a change of premise: the basechain splits under load and merges back,
   so the steady state at average load remains one shard. It matters here in
   two ways. Operationally, a corpus fixture must record which topology its
   blocks came from, and a copy of a split epoch needs parts for the specific
   shard being replayed. Substantively, the split threshold is the research
   target itself: raising single-shard throughput is what moves the load
   level at which the basechain has to split, so the interesting quantity is
   the TPS at which the split triggers, measured on one shard.
2. **A full epoch does not fit locally.** The working disk has 67.5 GiB free
   against a 60.6 GB epoch, and importing that into a validator database
   needs comparable space again. Copying one whole epoch is therefore not
   viable without another disk.

### Prefix-selective import does not help a saturated block

The obvious saving - copy only the account parts covering a target block's
touched prefixes, using the existing `SelectiveSplitStateAssembler` - was
tested against the checked-in mainnet fixture before any code was written.
Block `88028077` touches 108 accounts, and their address prefixes cover
**all 16** nibbles at the archive's split depth (counts 3 to 11 per nibble).
A saturated block spreads across the whole address space by construction, so
prefix selection selects everything. The saving is zero exactly where the
gate needs it, and it would only help on small or synthetic blocks, which do
not need a mainnet corpus in the first place.

What remains genuinely selective is coarser: replay one shard, not the whole
epoch. For the newest archived epoch that is the 9.21 GB masterchain state
plus the eight account parts of one shard, about 34.9 GB, and the parts can
be imported one at a time and deleted as they are consumed so the peak
footprint stays near one part plus the growing database. Whether the
resulting database then fits in the remaining space is unmeasured, and a
second disk remains the clean answer.

No data was copied and the node was not loaded; this section records sizes,
a measurement that rejected an approach, and a decision, not an action.

## What actually limits a mainnet basechain block - 2026-08-05

A Linux build was not needed after all. The archive node runs a liteserver, so
the existing Windows `lite-client` and `tvm-replay-bundle` can characterize
real mainnet blocks directly with a handful of small read-only queries. Archive
pack sizes locate the busy periods for free: the largest single-shard pack in
the archive is `archive.83605800.0_8000000000000000.pack` at 24.4 MB.

The busiest basechain block sampled from that period, `88103235`, is
**394,829 bytes with 191 transactions across 133 accounts**, all ordinary, at
most 20 transactions on one account. That is **18.8% of the 2 MiB candidate
cap** and about 2,067 bytes per transaction.

Two consequences:

1. **Real mainnet blocks are nowhere near byte-saturated.** The earlier
   fixture block `88028077` sat at 17.88% of the cap and was treated as an
   unsaturated sample; the busiest block in the whole retained archive is at
   18.8%. Byte capacity is not what bounds mainnet basechain blocks today, so
   projections from the byte cap describe a ceiling the network does not
   currently approach.
2. **The synthetic jetton corpus is harder than mainnet.** Its blocks carry
   672 transactions in about 1.3 MB, roughly 3.5x the transaction count and
   3.3x the bytes of the busiest real block. The parallel-executor gate is
   therefore being verified against a workload denser than anything mainnet
   has produced in the retained window.

### The split is not triggered by instantaneous load

Sampling around the shard split that ended shard `8000000000000000`, the last
blocks before it were nearly empty: seqno `88615400` carried 11 transactions
in 32 KB and `88615500` carried 3 transactions in 8 KB. Load at the moment of
the split was negligible, so "the shard splits at N TPS" is not a statement
the data supports as written.

`Collator::check_block_overload` explains why. Each block shifts a 64-bit
`overload_history_`, sets the low bit when the block hit a soft block-limit
class, took too long to collate, or spent too long on the dispatch queue, and
also sets it when `out_msg_queue_size_ >= FORCE_SPLIT_QUEUE_SIZE` (4096)
regardless of that block's own load. `want_split` then follows
`history_weight`, which weights the last 16 blocks by 3, the previous 16 by 2
and the 16 before that by 1, against a fixed threshold. The decision is
therefore a hysteretic function of roughly the last 48 blocks plus the
outbound queue backlog, not of the current block. A split lands after the
burst that caused it has already passed, which is exactly what the samples
show.

The research question "at what TPS does the basechain split" therefore needs
restating before it can be measured: the quantity that drives splitting is
sustained overload weight - blocks repeatedly reaching a soft limit or
exceeding collation time - and the outbound queue backlog, accumulated over
tens of blocks. Raising single-shard throughput moves that threshold by making
fewer blocks reach a soft limit at the same offered load, and the honest
measurement is offered load versus overload-history weight, not a single TPS
number.

### Split cycling after the 2026-08-03 mandatory update

The 2026-08-03 11:00 UTC mandatory validator update was expected to end the
premature splits under non-peak load that followed the protocol-v2
activation. The archive's per-slice shard packs give a free split timeline:
a 100-block masterchain slice whose directory contains packs for shard
identities other than `8000000000000000` had a split basechain.

Fraction of slices with a split basechain, per 100k-mc-block directory:
before the update, `arch0830..0836` run 8%, 25%, 12%, 24%, 21%, 41%, 21%.
After it, `arch0837..0841` run **60%, 54%, 69%, 42%, 59%** (the last
directory partial, through midday 2026-08-05). On this node's data the
basechain spends a larger share of time split after the update than before
it. This documents the state as of the measurement date, not a claim that
the fix effort failed: the neighbor-reduction changes (#2512/#2513) were
still in testnet at the time of the mandatory update, and the pre-2026-07-22
baseline - before the protocol-v2 activation - is the normal state this
regression is being walked back to.

The split observed directly on 2026-08-05 at about 12:02 UTC ended shard
`8000000000000000` at seqno ~88615500 while the sampled blocks of the
preceding ~350-block window carried 0-18 transactions (8-47 KB). No byte- or
gas-load burst is visible anywhere near the history window. If the split was
technically justified, it was through the non-load overload bits - outbound
queue backlog, collation time, dispatch-queue time - which are exactly the
symptomatic triggers of the v2 regression. Cross-shard latency was not
measured here and may well have improved; but premature splitting under
near-zero load demonstrably continued two days after the update.

Correction to the update framing: the release that shipped is `2026.07`
(2026-08-03, node target `bb935a8`), and it **retains** the legacy broadcast
while improving the new one - not the legacy-broadcast shutdown a prior
status post had described. The masterchain block rate, measured here from
block header timestamps, confirms `2026.07` worked on the axis it targeted:
2.204 blk/s on 07-31 (the v2 regression, below the reported 2.30), rising
2.328 (08-02), 2.416 (08-04), 2.481 (08-05) back to the ~2.47 norm. So
cadence recovered, while the basechain still spent 60-69% of the post-update
window split. The two are consistent: the broadcast fix restored masterchain
cadence, but the split machinery's sensitivity to the timing/queue bits
persists, pending the neighbor-reduction changes (#2512/#2513) still in
testnet. This directly reinforces the executor's relevance: cadence is
restorable by fixing broadcast, but the timing component of split pressure is
attacked by making collation itself faster.

Config 8 global version on 2026-08-05 is **15** with capabilities 1006 -
this is the TON protocol v2 that the 2026-07-22 vote activated, and it is
live. Config 30 is a separate parameter: its `simplex_config_v2` carries
`protocol_version` 0 (masterchain) and 1 (shard), and the "set protocol to
stable version" vote that raises Config 30 is a distinct, later ballot still
in progress. The two must not be conflated - "the network is on v2" refers to
Config 8=15 (done); the pending vote is the Config 30 stable-protocol step.
The archive node used for these read-only measurements is a liteserver, not a
validator (no `validator` config block), so it neither votes nor collates -
it only serves the finalized data the validator set produced.

For this research the consequence is stable either way: the overload history
is fed by soft-limit hits, timing, and queue backlog, so an executor that
cuts collation wall time and drains queues faster attacks the split trigger
directly, and the honest bench measurement remains offered load versus
overload-history pressure with and without the parallel path.

## Offered load versus split pressure - 2026-08-05 - measured

`benchmark/overload_curve.py` runs the jetton bench at a ladder of external
rates, one fresh single-shard network per rate with mainnet-parity limits and
`max_split=0`, and scrapes every basechain `check_block_overload` decision
from the validator log. The parallel path is replay-only, so this is the
serial collator's baseline curve.

| ext/s | blocks | overloaded | byte-limit bits | slow-collation bits | want_split blocks | first want_split | median size est |
|------:|------:|----------:|---------------:|--------------------:|------------------:|-----------------:|----------------:|
|   100 |   143 |        5% |              0 |                   7 |                 0 |        never | 232 KB |
|   200 |    68 |       47% |              0 |                  32 |                 3 |           65 | 482 KB |
|   300 |    24 |       33% |              1 |                   7 |                 0 |        never | 710 KB |
|   450 |   132 |       73% |             59 |                  38 |                94 |           39 | 1,007 KB |
|   600 |   132 |       73% |             70 |                  27 |                92 |           41 | 1,057 KB |
|   900 |    68 |       71% |             46 |                   2 |                27 |           42 | 1,177 KB |

Findings, in order of importance:

1. **The byte soft limit is the split trigger under jetton load.** Byte-class
   overload bits go 0/0/1/59/70/46 as the median size estimate crosses the
   1 MiB Config 23 soft threshold between 300 and 450 ext/s. Gas never
   exceeded 4.5M against a 10M soft limit, and the outbound queue never
   approached the 4096 force-split threshold: on this workload the byte axis
   binds first, and by a wide margin.
2. **Sustained split demand begins between 300 and 450 ext/s offered**, about
   650-975 included raw tx/s at the measured 2.17 raw-transactions-per-
   external chain ratio. From 450 ext/s upward the shard requests a split
   within about 40 blocks of spam start and keeps requesting it.

   The byte boundary is coupled to collation speed, and the coupling matters
   more than the ceiling. Per-block bytes are offered load divided by block
   cadence: a collator that keeps the 400 ms target cadence hits the 1 MiB
   soft limit only at the true protocol ceiling (~2.5 MiB/s of payload,
   roughly 1,100+ raw tx/s at this workload's 2.2 KB/tx), while a collator
   that falls behind produces fewer, fatter blocks and reaches the same
   limit at a lower offered load. The 900 ext/s rung demonstrates this on
   the stand: cadence halved (68 blocks where 450-600 produced 132) and the
   median size estimate swelled to 1,177 KB. A faster executor therefore
   pushes the split threshold up toward the protocol ceiling - by holding
   cadence, not by shrinking content - and only past that ceiling is
   sharding the sole answer.
3. **Timing bits alone can fire splits, reproducing the July failure class in
   miniature.** At 200 ext/s - zero byte pressure, 482 KB median blocks -
   32 of 35 overload bits came from "collation takes too long" and briefly
   drove `want_split` with no load worth splitting for. On this laptop-class
   stand those bits fire earlier than they would on server hardware, which is
   a stand caveat and simultaneously the demonstration: whenever collation
   wall time degrades for any reason, the split machinery fires without real
   load. This is where a faster executor genuinely moves the curve - it
   removes the timing component of split pressure below the byte boundary.
4. **Real mainnet load sits below the pressure region.** The busiest archived
   mainnet block (394 KB, 191 tx) corresponds to the stand's 200-300 ext/s
   regime, where byte pressure is zero. Current mainnet splits therefore
   cannot be byte-driven; they are timing/queue-driven, consistent with the
   post-update split cycling recorded above.

Caveats: the 300 ext/s point captured only 24 blocks and the 200/900 points
68 each (bench-harness truncations on this stand); the slow-collation bit
threshold is hardware-relative; and included-TPS at each rung was not
separately recorded (the ext/s to raw-tx/s ratio is taken from the earlier
600 ext/s measurement). The shape of the curve is robust to all three.

## Where the collation wall actually goes - 2026-08-05 - Amdahl profile

The gate now passes `--log-work-time` to `vrp`, so the parallel pass reports
its 15-phase `CollationStats::WorkTimeStats` CPU breakdown. On a 678-executed
-transaction jetton block (382 ms CPU total):

| phase | ms | share | parallelizable |
|-------|---:|------:|:--:|
| `trx_tvm` (per-account execution) | 126 | 33% | yes |
| `create_collated_data` (proof build) | 37 | 10% | no (single candidate) |
| `trx_other` (per-tx non-TVM) | 32 | 8% | yes |
| `combine_account_transactions` | 30 | 8% | no |
| `create_shard_state` | 22 | 6% | no |
| `create_block_candidate` | 20 | 5% | no |
| `trx_storage_stat` | 8.5 | 2% | yes |
| `create_block` | 6 | 2% | no |
| remainder (preinit, queues, final stat) | ~1 | <1% | no |

Grouping by whether the work is per-account (parallelizable) or whole-block
finalization (inherently serial): the parallelizable fraction is
`(126+32+8.5)/(sum) = 166/281 ≈ 0.59`. Amdahl's law then bounds the executor
at 1.79x with 4 workers, 2.06x with 8, and 2.44x in the limit - *before* any
orchestration cost.

Two conclusions set the executor's real ceiling and the next target:

1. **The theoretical ceiling is modest, and orchestration erases it.** A 0.59
   parallelizable fraction can never exceed 2.44x, and the measured
   serial/parallel speedup is about 0.9-1.0. The gap between 1.79x (4-worker
   Amdahl) and the observed ~0.9 is the cost of the parallel path's own
   machinery: preparing per-account journals, rebasing worker cells onto the
   coordinator, and replaying journals at commit. Optimizing execution
   further is pointless while orchestration overhead exceeds the execution it
   saves; the next executor work is reducing that overhead, not adding
   workers. This also retires the W2-DESIGN expectation of "3-4x on the
   execution portion" for this workload: execution is only a third of the
   collation CPU, so even a free 4x on it yields at most ~1.3x overall.
2. **The largest serial finalization phase is proof construction.**
   `create_collated_data` at 10% is the biggest non-parallelizable cost and
   is exactly what the W6 serialize-tail overlap design targets: building the
   candidate's collated-data proof while later work proceeds. On this
   workload, overlapping proof construction is worth more than any additional
   execution parallelism. `combine_account_transactions` (building the
   `AccountBlocks` augmented dictionary) and the `create_shard_state` /
   `create_block` / `create_block_candidate` finalization chain are the next
   serial targets.

The measurement is CPU time, not wall; the point is the parallelizable-versus-
serial split, which wall timing only makes more adverse (execution overlaps
across workers while finalization does not).

## Orchestration overhead measured and trimmed - 2026-08-05

`commit_time` was a black box mixing coordinator-only work with ordinary
serial commit. Two subtimers now split it: `journal_replay_time` (replaying
worker cell-usage journals into the coordinator tree) and `rebase_time`
(rebasing worker wrappers onto coordinator anchors). On 283-290-transaction
parallel batches the split was commit 95-128 ms, of which journal replay
26-32 ms (25-27%), rebase 4-5 ms (4%), and 65-91 ms ordinary serial commit.

`CellUsageJournal::replay_into` re-resolved every entry's path from the root,
costing O(entries x depth) `load_cell` calls, although entries are recorded in
traversal order and share long prefixes. Both the cell walk and the
coordinator-node walk now cache the resolved chain and restart from the
longest common prefix. Journal replay dropped from a 26-32 ms range to a
17.9 ms median, roughly -40% on that phase, with the four-root unit tests and
an 18-of-18 gate run (three blocks, workers 2/4/8, both pass orders) all
green.

The honest outcome is that this barely moved the headline: the median
serial/parallel speedup went from 0.94 to 0.97 (range 0.67-1.48). Journal
replay was about 15% of commit and commit is itself only part of the pass, so
a 40% cut there is worth a few percent overall. What the measurement settles
is where the remaining time is *not*: after this change, coordinator-only
orchestration is roughly 22 ms (18 journal + 4 rebase) against about 108 ms of
commit, so orchestration is no longer the dominant overhead. The rest of
commit is ordinary serial work that a serial pass also pays.

Combined with the Amdahl profile, the conclusion for the executor is now
concrete: per-account execution is 59% of collation CPU and already runs
concurrently; orchestration is trimmed to a small residue; and the ceiling is
set by the serial finalization tail - proof construction, `AccountBlocks`
assembly, shard-state and candidate creation. Further executor speedup
requires overlapping or restructuring that tail (the W6 serialize-tail
direction), not more workers and not more orchestration tuning.

## W6 target located, and one W6 approach ruled out - 2026-08-05

`create_collated_data` is the largest serial finalization phase, so it was
instrumented before any restructuring. Its three independent
`MerkleProof::generate` walks now have their own timers. On 678-transaction
blocks, `create_collated_data` costs 57-63 ms, split as:

| subphase | ms | note |
|----------|---:|------|
| previous-state Merkle proof | 32-38 | one walk over the whole prev state |
| neighbor out-queue proofs | 0.02-0.04 | negligible |
| account storage-dict proofs | ~0.00 | none on this workload |
| remainder (`prepare_proofs` + continuation flush) | ~20-25 | dictionary diff scans |

**The obvious W6 approach is dead.** Parallelizing the independent proof
generations across roots - prev state, neighbor queues, storage dicts - buys
nothing, because on this workload everything except the previous-state proof
is free. Any real gain has to come from the single prev-state walk or from
`prepare_proofs`.

That reshapes the remaining options honestly:

1. **Incremental proof construction.** Inclusion in the proof is monotonic: a
   cell that gets loaded during execution is in the final proof, and unloaded
   branches are pruned. In principle the proof tree can be built as loads
   happen instead of by a full walk afterwards, which is what would truly
   overlap this cost with execution. This touches the cell-loading hot path
   of every collation, so it is a large, risky change that must not be
   attempted without the byte-identity gate proving equivalence on every
   step - which now exists.
2. **`prepare_proofs` diff scans.** `old_account_dict->scan_diff(*account_dict)`
   and the out-queue `scan_diff` walk both dictionary versions to touch the
   changed paths. At ~20-25 ms this is comparable to the proof walk itself and
   is plain serial work whose result the parallel path already partly knows:
   the coordinator committed each account and each queue mutation and could
   record the changed keys as it went, instead of rediscovering them by a full
   diff afterwards. This is a smaller, better-contained change than (1).

No implementation was attempted in this pass; the measurement exists to stop
the wrong one from being built.

### Option 2 measured and also rejected

Splitting the remainder further: `prepare_proofs` is 14-17 ms and the
continuation flush is **0.15 ms** - the journal-replay versioning from earlier
today reduced the phase-boundary flush to nothing, so the earlier "~20-25 ms
remainder" was mostly `prepare_proofs` alone.

`prepare_proofs` cannot be replaced by the coordinator's known changed keys as
proposed. Its `scan_diff` calls have no useful return value: the callbacks do
nothing but return true. Their entire purpose is the side effect of *touching
cells* so the collated proof covers the changed dictionary paths, and
`scan_diff` already skips identical subtrees by hash, so it is proportional to
the diff rather than to the dictionary. Replacing it with per-key lookups
would walk from the root once per key - likely slower - and, more seriously,
would touch a different cell set, changing the collated data byte-for-byte in
a consensus-critical structure. At 14-17 ms out of a ~500 ms collation, that
is roughly 3% for a real byte-identity risk. Not worth doing.

### The serial tail is diffuse - no single lever remains

The measurement that matters is the shape, not any single phase. Serial
finalization on a 678-transaction block is spread across comparable pieces:
`combine_account_transactions` 44-46 ms, `create_shard_state` 33-38 ms,
`create_block_candidate` 32 ms, `create_collated_data` 34-45 ms (of which the
prev-state proof is 20-28), `create_block` 12-15 ms. The largest single serial
phase is now `combine_account_transactions` - building the `AccountBlocks`
augmented dictionary - not proof construction.

So W6 as originally scoped ("overlap the serialize tail") would, even if
perfectly executed against `create_collated_data`, remove under 10% of
collation. The honest position at the end of this pass: per-account execution
is already parallel, orchestration overhead is trimmed to a small residue, and
what remains is five separate serial phases of 12-46 ms each, none dominant.
There is no remaining single change with a large payoff; further gains require
either restructuring several finalization phases at once or the incremental
proof construction of option 1, both of which are projects rather than
optimizations, and both of which must be run against the byte-identity gate at
every step.

## Mixed jetton+compute corpus - 2026-08-06 - decisive executor experiment

The benchmark stack now generates the realistic mixed workload the executor
question needed. `bench-state-gen --compute-count N` adds active compute-bound
contract accounts (`test/integration/contracts/compute-bound.fc` compiled into
`benchmark/contracts/compute-bound.code.boc`; distinct addresses via a 64-bit
instance data cell), and `bench-spam --compute-share p --compute-rounds-min/max`
turns a deterministic per-wallet fraction of externals into 0.6 TON calls with a
16-bit rounds body. Calibration against gated blocks gives roughly
`gas = 5k + 127 * rounds`, so rounds 800..7000 cover ~105k..900k gas per call -
inside the 1M per-transaction basechain limit, and heavy enough that a 15% share
makes blocks gas-bound (10M soft) before they are byte-bound.

### Seventh defect: lookahead collated-proof leak on gas-bound blocks

The first gas-bound gate run (all-compute externals at 4000 rounds) failed with
a new signature: block data byte-identical, collated data +756 bytes in the
parallel pass, all block components matching. Root cause: the 64-entry parallel
lookahead materialized queue entries through the state usage tree before
execution, and its safety relied on the boundary guard's 2M-gas margin -
sized for ~30k-gas jetton transfers. A ~560k-gas transaction batch blows
through that margin, the commit loop stops mid-batch at the 10M soft limit,
and the touched-but-uncommitted tail stays in the collated-data prev-state
proof. No margin can fix this class: a worst-case-safe margin would be
64 x 1M gas, larger than the whole block budget.

The fix removes the mechanism instead of tuning it: a replay-only shadow
`OutputQueueMerger` over the same queue roots serves lookahead/prepare under
`state_usage_tree_` ignore_loads, so preparation leaves no trace in the proof.
The real merger is touched only by the commit loop and the serial path, whose
lazy materialization (whole equal-lt groups, recording on) reproduces exactly
the serial pass's touch set. Anchors derived from shadow-traversed cells remain
real coordinator tree nodes because ignore_loads suppresses only load marks,
not node creation or tree-node propagation; shadow/real position sync is
fail-closed and every commit still cross-checks lt/source/hash against the real
merger. The previously failing signature now passes, including a mid-batch
boundary stop that discarded 24 prepared results, and the old jetton corpus
still passes 18/18 with an unchanged median speedup (0.99, range 0.95-1.42).

### Mixed-corpus gate results

Main runs: 100k wallet pairs + 1024 compute accounts, 450 ext/s for 45 s,
compute share 0.15, rounds 800..7000 (mean gas per heavy call ~500k). Gated
blocks carried 642-654 transactions at 12.4-12.8M gas (`internal_load`
1.00-1.02) and 1.81-1.96 MB estimated block size - saturated on the gas axis
and touching the byte hard region, denser than anything mainnet has produced.

Across two runs, 31 of 32 completed gate runs produced byte-identical block and
collated data and passed `ValidateQuery` (run 2 was cut short at 14/14 by a
stand-level validator crash; see below). 119-161 of ~650 transactions per block
executed through the parallel path (the rest are externals and same-account
repeats, which stay serial by design), every pass hit exactly one gas boundary
stop, and the inbound-phase stop telemetry was byte-identical across all runs
of a block (same txs, gas, size estimate, proof counters).

Median serial/parallel Collator speedups on the mix: 1.04 at 2 workers, 1.06
at 4, 1.05 at 8 (range 0.96-1.13 excluding one 1.82 cache-order outlier whose
paired sample read 1.00). The decisive answer, on the numbers:

1. **On a realistic mix the executor is barely net-positive (~+5%)** - far
   from the 1.19-1.74x of compute-only blocks and just above the 0.97-0.99 of
   jetton-only blocks. Interpolating the three corpora: the executor pays off
   in proportion to the fraction of block gas spent in parallel-eligible
   heavy TVM execution, and a realistic 15% DeFi-like share is not enough.
2. **Worker count is irrelevant (2 = 4 = 8)**, confirming again that the
   ceiling is the serial phase structure - externals processing, same-account
   chains, and the finalization tail - not execution bandwidth.

### Eighth defect - collated-data replay nondeterminism, NOT an executor defect

One saturated run of 32 failed with a +28-byte collated-only mismatch (block
data identical) on a block whose other five runs, including the opposite pass
order at the same worker count, passed. A later verification pass caught the
same class in a much starker form: on a 233-transaction block with an empty
inbound queue, 2 of 4 gate runs failed with collated-only deltas of -204 and
+493 bytes - **while `attempts=0`**: the parallel batch path never executed a
single statement (queue_eof), both passes ran byte-identical code, and the
serial pass's collated bytes were stable across runs while the
workers-flagged pass varied in both directions. Inbound stop telemetry was
byte-identical in every case.

The correct conclusion is that this class is not in the parallel executor at
all: it is a nondeterminism in the collated-data cell footprint between
repeated collations of the same block inside one process - some
cache-state-dependent or wall-clock-dependent touch in ordinary collation.
The gate compares two collations, so it flakes at whatever rate that
nondeterminism fires (~3% on saturated mixed blocks, ~50% on one observed
small pure-new-message block shape). The mismatch handler now appends a
collated-data footprint diff (multi-root walk with per-root breadcrumbs,
reusing the state-update divergence reporter), so the next occurrence names
the exact diverged cells; 15 subsequent gate runs did not reproduce it.
Until localized, isolated collated-only gate failures on otherwise passing
blocks must be read as this replay nondeterminism, not as executor
regressions - and equally, the class must be closed before any live-collation
claim, because live candidates would inherit the same instability.

### Stand reliability note

The Windows stand's validator-engine sporadically dies with access violations
(exit 0xC0000005) and occasional heap-corruption events, killing roughly one
bench run in three under compute load. Windows Event Log shows the identical
crash class on 2026-08-03 across three different binaries - before any of
this session's changes - and one captured backtrace points into the
FastSyncOverlay broadcast path during spam, far from the replay code. It is a
pre-existing stand flake: reruns succeed, and no crash has ever produced a
wrong gate verdict (fail-loud holds). A symbolized crash-dump session is
warranted if the rate worsens.

## Execution-architecture survey refresh - 2026-08-06

A bounded source pass over Solana/Agave, Aptos, Sui, and Monad (primary
sources: papers, code, engineering blogs; vendor TPS claims labeled as such,
never adopted) plus a same-day TON upstream check. Mechanics only; nothing
here is a performance forecast for TON.

### What the other stacks actually did

- **Solana/Agave** requires declared read/write account sets and schedules on
  account locks. Its production arc is instructive: the v1.18 "central
  scheduler" built a priority/dependency graph (prio-graph) over pending
  transactions; by v2.2-2.3 Anza removed the graph from the hot path because
  building it cost more than it saved, replacing it with a greedy
  lock-conflict check ([anza.xyz/blog/introducing-the-central-scheduler](https://www.anza.xyz/blog/introducing-the-central-scheduler-an-optional-feature-of-agave-v1-18),
  [helius.dev agave-v2.1/v2.3 notes](https://www.helius.dev/blog/agave-v21-update-all-you-need-to-know)).
  Firedancer isolates conflict-free microblock packing on a dedicated `pack`
  tile so execution tiles run lock-free
  ([fd_pack.c](https://github.com/firedancer-io/firedancer/blob/main/src/disco/pack/fd_pack.c)).
  Agave 3.x/4.0 latency work moved replay-side verification off the critical
  path (async dispatch, join-on-demand).
- **Aptos Block-STM** ([arXiv:2203.06871](https://arxiv.org/abs/2203.06871))
  is optimistic execution in preset block order over multi-version memory,
  with aborted incarnations' write sets serving as dependency estimates. The
  published 110-170k TPS figures are synthetic p2p blocks. "Block-STM v2" has
  no public paper as of today - only vendor claims (256-core scaling) and
  aptos-core PRs enabling it on internal test fleets. The adjacent
  RapidLane/deferred-objects work ([arXiv:2405.06117](https://arxiv.org/abs/2405.06117))
  attacks hot-account contention by deferring commutative sub-operations to a
  sequential post-phase - i.e. by redefining the conflict unit, not by
  smarter scheduling.
- **Sui**: Pilotfish ([paper](https://sonnino.com/papers/pilotfish.pdf))
  distributes one validator's execution across machines with per-object
  versioned queues delivering consensus-ordered operation streams; it scales
  linearly only when compute-bound and remains an unshipped prototype. What
  shipped in production is consensus-side: Mysticeti v2 + Transaction Driver
  (default since node v1.60, 2025-11) folds per-transaction certification
  into consensus blocks, removing serial certificate aggregation
  ([blog.sui.io/mysticeti-v2-sui-consensus](https://blog.sui.io/mysticeti-v2-sui-consensus/)).
- **Solana slot-time reduction** ([SIMD-0525](https://simd.mixy.one/simd/0525-reduce-slot-times/)):
  400ms -> 200ms in four feature-gated 50ms steps (350ms on testnet as of
  2026-08; first mainnet step targeted 2026-08-17 with Agave v4.2). The SIMD
  explicitly credits prior Turbine/Replay overhead reductions as the enabler:
  the slot was cut only after per-slot fixed costs shrank. By itself the
  change is roughly TPS-neutral (half the budget, twice the rate) - it is a
  latency upgrade. The TON-relevant reading: per-block fixed serial costs
  bound how short a block can be. TON's measured ~150ms finalization tail is
  37% of the 400ms target cadence and would be 75% of a hypothetical 200ms
  one - tail reduction is the precondition both for throughput at the current
  cadence and for any future cadence decrease, which is exactly the
  pipelined-finalization direction chosen below.
- **Monad** (mainnet 2025-11-24) executes all block transactions
  speculatively in parallel and merges read/write sets serially in block
  order, re-executing on conflict; the design bet is that re-execution is
  memory-hot and cheap. MonadDB backs this with a natively versioned Patricia
  trie and io_uring async state reads
  ([docs.monad.xyz parallel-execution](https://docs.monad.xyz/monad-arch/execution/parallel-execution),
  [monaddb](https://docs.monad.xyz/monad-arch/execution/monaddb)). The 10k
  TPS figure is claimed capacity, not observed sustained load.

### Transferable to TON

1. **Greedy-over-graph, validated in production.** Solana's reversal from
   prio-graph to greedy is independent evidence for what this fork measured:
   when the conflict test is trivial (TON: destination account, known for
   free), any scheduling machinery beyond the cheapest conflict check is pure
   overhead. Our 0.97x jetton result is the same economics Anza hit.
2. **Tail overlap, not more workers.** Every stack, once execution
   parallelized, moved to its serial residue: Solana to scheduling/replay
   I/O, Sui to certificate aggregation, Monad to state materialization,
   Aptos to the commit path. This matches the measured TON profile exactly
   (59% execution already parallel; a diffuse ~150ms finalization tail).
   The transferable direction is overlapping/pipelining the tail with
   execution - Firedancer's pack/execute separation and Agave 4.0's
   off-critical-path verification are working precedents.
3. **Worker self-scheduling from a shared index** (Block-STM's collaborative
   scheduler) rather than a central dispatch hop - relevant to the ~35ms
   orchestration cost that erased the jetton win.
4. **Async, versioned state I/O** (MonadDB): prefetching accounts referenced
   by the inbound queue before execution and starting prev-state proof work
   at collation begin both apply without touching block format - the proof
   is over the previous state, fully known at collation start.
5. **Per-object versioned queues** (Pilotfish) are structurally the same as
   whole-account chains delivered in (lt, hash) order - external validation
   of this fork's account-lane architecture, including its finding that
   scaling is linear only when compute-bound (our 1.19-1.74x vs 0.97x).

### Not transferable, and why

1. **Optimistic execution + re-execution** (Block-STM, Monad): solves
   conflict-set discovery for shared-state VMs. TON's conflict set is exact
   by construction; speculation adds re-execution cost and threatens the
   byte-identical candidate gate for nothing.
2. **Multi-version memory with abort/ESTIMATE machinery**: justified only
   when conflicts are unknown. They are known here.
3. **Priority-fee scheduling freedom**: TON's canonical (lt, hash) order
   leaves the collator no ordering freedom to optimize; only the
   conflict-handling half of those schedulers is meaningful for TON.
4. **Declared access lists as an API**: unnecessary (destination account is
   implicit) and impossible to refine (sub-account state is not declarable
   in the message format without a protocol change).
5. **Consensus-path restructuring** (Mysticeti v2, MonadBFT): out of scope
   by definition; TON's candidate/validate flow stays.
6. **Sui's owned-object no-consensus lane**: presumes client-ordered
   per-object causality; TON's lt is chain-assigned and everything already
   flows through collation.
7. **Distributed cross-machine execution** (Pilotfish): targets aggregate-CPU
   scarcity, which is not the measured bottleneck (a single-machine serial
   tail is); network hops would land on exactly the orchestration path that
   already erased the jetton win. TON's protocol answer to that regime is
   sharding.

### TON upstream status (checked 2026-08-06)

- **PR #2485 (dedicated collators) was closed unmerged today and superseded
  by [PR #2523 "Collators"](https://github.com/ton-blockchain/ton/pull/2523)**
  (SpyCheese, `ton-blockchain:collators` -> `testnet`, 38 commits, +2815/-717):
  an on-chain validator-registry contract in Tolk, new ConfigParam 46, and a
  simplification from shard-scoped to node-level collators with delegation.
  Review is just starting. The overlap conclusion is unchanged - it is the
  deployment boundary this executor would sit inside, not an intra-block
  transaction executor - but every future overlap refresh must now track
  #2523, not #2485.
- PR #2513 (lower ihave fanout) merged 2026-08-02; #2512 (same against
  `testnet-stripped`) still open. Master since 2026-08-01 carries only the
  v2026.07 merge (QUIC/broadcast performance) and the fanout change - nothing
  touching collator.cpp, block limits, or Config 23/29.
- No public code yet for the announced CellDB 2.0/RocksDB replacement; the
  latest public CellDB work remains the v2 line merged in 2025. Absence of
  public code is not absence of private work.

## Next engineering project - decision 2026-08-06

Candidates were incremental prev-state proof construction, serial-tail
restructuring, and the PHASE2 admission/scaffolding track. The numbers now
line up behind one direction:

- The executor question is answered: ~+5% on a realistic mix, invariant in
  worker count. Additional execution parallelism has no remaining payoff on
  any measured corpus short of compute-only blocks.
- The serial finalization tail is ~150ms of a ~400-500ms saturated collation
  (combine_account_transactions 44-46ms, create_shard_state 33-38ms,
  create_block_candidate 32ms, create_collated_data 34-45ms with 20-28ms of
  prev-state proof, create_block 12-15ms), and the survey shows every peer
  stack converged on exactly this target once execution parallelized.
- The split-pressure measurements make collation wall time the lever that
  moves the shard-split threshold: holding 0.4s cadence pushes the byte
  boundary toward the ~2.5 MiB/s protocol ceiling.

The overlap budget is now measured, not assumed. Replay-only telemetry
(`REPLAY_COMBINE_PHASES`, the collation phase of each account's last
committed transaction, logged at `combine_account_transactions`) on three
saturated mixed-corpus blocks (3/3 byte-identical, serial and parallel
histograms equal): 141/186/222 accounts closed their chains in the inbound
phase versus 504/461/418 in the externals phase, and **zero** accounts had
their last transaction in the new/deferred phase - at saturation freshly
generated internals are deferred through the queue, so no account reopens
after the externals phase. Consequences: (a) 22-35% of `AccountBlocks` can
be combined while the externals phase is still executing; (b) every
external-phase account received exactly one transaction on this workload and
was never touched again, so a speculative streaming combine - build each
account's block when its transaction commits, invalidate and recombine on a
later touch - would cover nearly 100% of the 44-46ms combine cost, with the
adversarial worst case bounded by recombining only re-touched accounts.

The chosen project is **pipelined finalization**: overlap the tail phases
with the execution window instead of running them as a monolithic post-phase.
Concretely, in order: (1) incremental `AccountBlocks` assembly - combine each
account's transaction dictionary as its chain closes during collation rather
than in one 44-46ms pass at the end; (2) prev-state proof construction
started at collation begin and advanced as loads occur (the W6 option 1 that
was deferred until a byte-identity gate existed - it now exists and has
caught seven defects); (3) shard-state/candidate serialization overlap where
dependency order allows. Every step lands under the vrp byte-identity gate
plus ValidateQuery, with serial-first/parallel-first pairing, on both the
jetton and mixed corpora. PHASE2's admission/scaffolding (W3 external
re-admission) stays queued behind this: it improves goodput under
over-saturation but does not move collation wall time, which is what both
the split threshold and the executor ceiling are bound by.

## W7 roadmap: the streaming collator - 2026-08-06

The synthesis of every measurement and the survey. Three TON-unique
properties no peer stack has: the conflict unit is free and exact
(destination account - no declared access lists, no speculation); a large
share of block N+1's workload (inbound internals) is deterministically known
in canonical (lt, hash) order the moment block N's execution ends, because
the queue is persistent state; and full collated data makes the candidate
self-sufficient, so candidate production is the only latency-critical
artifact. Today the collator discards property two entirely: every block
pays `execution (~330ms) + serial tail (~150ms)` in sequence.

Target: cadence bound by max(execution, tail) instead of their sum, then
tail approaching zero.

- **Phase A - streaming tail inside the block** (started; overlap budget
  measured above): speculative incremental AccountBlocks, incremental
  prev-state proof, per-lane state serialization. Expectation: tail 150ms ->
  ~50ms. Solana's SIMD-0525 sequence (shrink per-slot fixed costs first, cut
  the slot after) is the working precedent.
- **Phase B - cross-block pipeline**: execute block N+1's inbound phase
  while block N's tail serializes in the background. N's post-state is fixed
  at execution end, not serialization end. This is a scope-limited
  reactivation of optimistic collation (the Accelerator-class path removed
  in the Simplex transition), restricted to the execution phase on the
  collator's own candidate - a natural fit for PR #2523's dedicated
  collators producing consecutive blocks. Expectation: the serial tail
  leaves the critical path; the "collation takes too long" split-trigger
  bits - the dominant premature-split cause today - stop firing.
- **Phase C - block-transcending account lanes**: lanes do not stop at the
  block boundary; the boundary is a seal point (multi-frontier commit:
  queue/account/gas/byte/lt frontiers checked at ordered commit) while
  execution streams on. An account untouched in block N executes its N+1
  queue message with zero speculation - its state is already final. Monad
  needs speculation for this; Solana has no queue-as-state at all.

Honest ceiling math: the single-shard protocol ceiling is the byte envelope,
~2.5 MiB/s payload = ~2,300 raw tx/s = ~1,000+ jetton operations/s at a held
400ms cadence. The current split threshold sits at 650-975 raw tx/s and is
timing/soft-byte-driven. Hard-cap blocks need ~5,750 tx/s of execution
bandwidth; serial replay already measures 3.1k and account-parallel workers
cover the rest. A+B+C therefore target the protocol ceiling - roughly 3.5x
the current split threshold and 7x observed jetton TPS - with no protocol,
block-format, or consensus change. Sustaining that is also the engineering
prerequisite for a Config 23 soft-limit vote (1 MiB -> 2 MiB), which would
double the ceiling again toward the 1,500-2,500 jetton-ops/s target band.
Every step lands under the existing byte-identity + ValidateQuery gate.

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
