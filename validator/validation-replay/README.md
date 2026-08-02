# Validation replay

The bounded source review for the single-shard execution track is kept in
[`PARALLEL_EXECUTION_RESEARCH.md`](PARALLEL_EXECUTION_RESEARCH.md). Executable
worker/coordinator contracts live in `validator/impl/parallel-*` and their
regression tests live in `test/validator/test-parallel-*`. The implementation is
not wired into the live collator yet.

`ValidationReplayer` reruns collation, validation, or both against blocks and
states already present in a C++ validator database. It is intended for
measurement on a dedicated replay node, not for use in the consensus path of a
live validator.

## TVM hot-path modes

Normal collation and validation use a bounded Space-Saving sketch. It retains at
most 64 code hashes and at most 10 account candidates per code hash. This keeps
memory and metric cardinality bounded even when a block executes many unrelated
contracts.

An offline replay can opt into exact collection:

```text
vrp run-range --mode validate --exact-tvm-hotpaths --wc 0 --max-jobs 1 <first_mc_seqno> <last_mc_seqno>
```

In exact mode every code hash executed in the selected range and every account
execution count for that code hash are retained. The input range therefore
bounds memory use. Exact mode requires one job so that concurrent replays do not
distort timing. Start with a short range.

After the run completes, fetch paginated JSON:

```text
vrp hotpaths <run_id> --source validate --metric wall --offset 0 --limit 100
```

The response includes execution count, wall or CPU time, VM and billed gas,
VM steps, Ed25519 verification count and time, exact distinct-account count,
top-account concentration, and the ten most active workchain-and-address pairs
for each returned code hash. Ed25519 timing is enabled only by exact offline
replay; normal collation and validation do not start the inner crypto timer.

For a full-wall PSAE shadow projection, replay collation on a dedicated copied
validator database:

```text
vrp run --mode collate --exact-tvm-hotpaths '(0,8000000000000000,SEQNO)'
vrp hotpaths <run_id> --source collate --metric wall --offset 0 --limit 100
```

The response then includes phase-separated `account_lane_ceiling` scopes:
`inbound_internal`, `external`, `new_or_deferred`, `special`, and
`all_ordinary`. Each scope measures successful ordinary-transaction creation by
destination account and models the full outer collation wall as unchanged
serial residue plus a greedy 1/2/4/8/16-worker account critical path. The
`inbound_internal` scope is the honest P2 ceiling; `all_ordinary` describes the
later P4 end state and merges work for the same account across phases. The
projection excludes worker contention and receipt/state/proof merge overhead;
it is not a measured parallel speedup. Bounded online mode retains no
per-account timing map. Do not run this profiling mode on a validator
participating in consensus or on a latency-sensitive production node.

A single offline collation can export the exact candidate collated-data BOC:

```text
vrp run --mode collate --export-collated-data <new-path> '(0,8000000000000000,SEQNO)'
```

Export is unavailable for ranges and validate-only runs. The destination is
created with owner-only permissions and must not already exist. This only
persists the newly replayed candidate artifact; it does not change the database,
consensus state, or network configuration. Use it on a dedicated copied
validator database, then pass the file to `tvm-replay-bundle --collated-data`.

Collection work is timed separately as `trx_tvm_profile`; it is not included in
the per-code `time_tvm` values. Exact maps can make the replay itself slower,
but do not inflate the measured TVM execution time.

Completed results remain in memory until they are evicted by the 16-run history
limit, the process exits, or they are explicitly released:

```text
vrp forget <run_id>
```

## Sparse archive replay

`tvm-replay-bundle` is a narrower offline path for cases where a validator
database is not available. It reads a closed shard archive package, an explicit
masterchain state, and either a complete predecessor state or selected account
parts from a split persistent state:

```text
tvm-replay-bundle \
  --archive <closed-shard.pack> \
  --mc-archive <closed-masterchain.pack> \
  --block-id '(0,8000000000000000,SEQNO)' \
  --prev-state <shard-state-or-split-header.boc> \
  --mc-state <referenced-masterchain-state.boc> \
  --account-part E=<stateaccount-part.boc> \
  --profile-ed25519
```

Before copying state, inspect the block to obtain its exact predecessor,
masterchain reference, touched accounts, and required split-state prefixes:

```text
tvm-replay-bundle --archive <closed-shard.pack> --list-blocks

tvm-replay-bundle --archive <closed-shard.pack> \
  --block-id '(0,8000000000000000,SEQNO)' --inspect --split-depth 4
```

`--list-blocks` verifies every archived block file against the file and root
hashes in its filename before reporting the full block id, referenced
masterchain block, timestamp, and serialized size. The inspection output also
reports `recommended_account_proof_reference`. For a linear block this is the
exact predecessor shard block, which avoids advancing older account proofs
through intermediate blocks.

For every account covered by the supplied state, the tool re-executes the
historical transaction chain and requires both the serialized transaction hash
and the resulting account-state hash to match. It reports exact, unbounded
`code_hash` hot paths for that explicit input. If only some account parts are
provided, output is labelled `account_prefix_subset` and includes the number of
skipped accounts; it must not be presented as a full-block result.

`--profile-ed25519` adds the count and time spent inside real Ed25519 signature
verification to every code-hash entry. It does not bypass signature checking or
change gas accounting. The replay still requires exact transaction and
resulting account-state hashes.

Each replayed transaction is also passed through the in-process PSAE canonical
payload validator. It reparses the Transaction and post-account cells, derives
state/transaction hashes, gas, LT, account statuses and total fees, validates
the contiguous out-message dictionary, and reports counts under
`psae_payload_validation`. Per-account canonical fees must equal the existing
`AccountBlock` fee augmentation. The replay also applies the basechain subset
of `Transaction::update_limits` to a shadow status: max LT, gas, transaction and
first-account counters, plus the canonical Transaction/post-account cells.
This field is an ABI/equivalence check, not parallel execution. Transaction
replay supplies no proof journals or collator `CellUsageTree`, so it makes no
block-size claim. It also does not reconstruct the special mint/recover routing
context, so reported limit gas is the billed-gas sum rather than a claim about
the historical block-limit counter. Descriptor dictionaries, queues,
`ProcessedUpto`, account dictionary proofs, storage-dictionary updates, and
state merge are not applied to live collator dictionaries. `NewOutMsg`
registrations are materialized with coordinator-derived metadata, and every
ordinary `msg_import_fin` in the replay scope is rebuilt byte-for-byte together
with its optional `msg_export_deq_imm` pair. An atomic coordinator shadow now
applies the validated continuous prefix to exact account cells, descriptor-cell
maps, descriptor-derived 352-bit queue keys, the new-message heap, block-limit
counters, and a `ProcessedUpto` frontier. A normal pending/failed/limited stop
publishes only the ready prefix; any malformed delta discards the entire
candidate publish. These maps do not reproduce augmented dictionary roots, so
the result remains an offline consistency gate and is not wired into live
collation.

The same offline path now has a fail-closed augmented-dictionary commit gate.
It applies account, descriptor, and queue deltas to private `ShardAccounts`,
`InMsgDescr`, `OutMsgDescr`, and `OutMsgQueue` copies and returns roots only if
every mutation succeeds. Queue additions, replacements, and deletions are
handled by the same atomic batch. Every replacement or deletion must match the
exact predecessor value cell; a mismatch returns no roots. On the copied
full-block fixture, all
29 account values were bound to a combined predecessor proof and the 10
`msg_import_fin` plus 10 `msg_export_deq_imm` insertions reproduced the target
`InMsgDescr` and `OutMsgDescr` roots exactly. The combined account-proof root is
also required to equal the target block Merkle update's predecessor
`ShardAccounts` root.

This is a two-root gate unless candidate collated data is supplied. The block
Merkle update is hash-sufficient for applying the state transition, but it
prunes sibling augmentation values needed to enumerate and recompute changed
`ShardAccounts` and `OutMsgQueue` paths. Core creates the complete predecessor
proof later, while building `candidate.collated_data`. The available copied
fixture contains the block and account proofs, but not that candidate artifact.
The JSON therefore reports
`augmented_dictionary_roots_validated=2`, names the exact scope, and keeps
explicit status fields for the two unresolved roots. It also reports
`augmented_dictionary_baseline_source=target_minus_validated_deltas` and
`augmented_dictionary_historical_transition_proven=false`: the descriptor
check is an exact delta round-trip, not a complete historical state transition.
The result remains offline-only and is not wired into live collation.

`--collated-data <path>` accepts the raw candidate collated-data BOC. The tool
selects exactly one Merkle proof whose virtual root equals the target block's
predecessor state hash and rejects missing, duplicate, or unrelated witnesses.
With that witness it enumerates the complete queue diff, applies all account
and queue mutations through the atomic commit, and requires all four dictionary
roots to match. Supplying the flag makes this fail-closed: the replay cannot
succeed with fewer than four validated roots. A normal block BOC, archive
package, config proof, or target state is not an acceptable substitute.

The replay JSON also contains `account_lane_ceiling`. It groups measured
transaction and TVM wall time by account and reports greedy ideal makespans for
1/2/4/8/16 workers. This is an execution-only ceiling: it excludes worker
contention, serial commit, block limits, cell-proof/state merge, network, and
consensus. It must not be reported as a TPS prediction or a measured parallel
speedup.

`--account-workers N` adds an executable offline probe for complete
account-proof replays. It first repeats the full account set with one worker,
then partitions whole account chains with deterministic greedy LPT and replays
the lanes concurrently. Every lane uses the normal TVM emulator and historical
proof cells. The result is accepted only when the union preserves all historical
transaction hashes, resulting account-state hashes, canonical payload/effect
counters, and account-chain lengths. The requested worker count is capped at 64
as a local process-safety guard and is reduced to the number of touched accounts;
neither value is a TON protocol limit.

The probe includes thread creation/join and deliberately duplicates config
extraction, the full AccountBlocks scan, and shadow effect checks in every lane.
The reported sample always runs the serial baseline before the parallel sample,
so cache/order bias remains explicit in `known_biases` and repeated external
runs are required.
It excludes the augmented-root commit, a reusable actor/worker pool, live
collator integration, network, and consensus. Its `wall_speedup` is therefore a
measured isolated-account replay result, not shard TPS. On the copied 51-tx,
29-account block, ten independent `-O3` Release processes at each width passed
the equivalence gate. Median speedups were 1.57x at two workers, 2.15x at four,
and 2.45x at eight; interquartile ranges were 1.49-1.95x, 1.99-2.41x, and
2.20-2.73x respectively. Median isolated replay rates were about 3.1k raw tx/s
for the serial baseline, 6.6k at four workers, and 8.0k at eight workers. The
Release target builds after supplying the repository's vendored Abseil include
path to the local CMake cache; no dependency source was changed.

The output also contains `single_shard_capacity`. It reads Config 23/29/30 from
the state-bound masterchain proof and reports the exact archive block-file size.
For the copied basechain block `87341675`, Config 29 at masterchain seqno
`82773023` sets `max_block_bytes=2097152` and
`max_collated_bytes=10485760`. These are separate limits: the latter bounds
collated witness data and is not additional block payload. The 113,736-byte
sample contains 51 raw transactions, or 2,230 bytes/tx. A linear projection of
that exact density into 2 MiB at the Config 30 target rate of 400 ms is 2,351
raw tx/s. Dividing it by three gives a conditional 784 operations/s only for a
workload that actually consumes three raw transactions per operation at the
same byte density.

Config 23 supplies a second, different envelope used by `BlockLimitStatus`.
For this proof its byte and collated-data thresholds are 256 KiB underload,
1 MiB soft, and 2 MiB hard; gas thresholds are 2M/10M/20M and logical-time
deltas are 1,000/5,000/10,000. This explains the apparent 1 MiB versus 2 MiB
disagreement: 1 MiB is the normal soft threshold of the collator's estimated
block-size domain, while 2 MiB is both its hard threshold and the serialized
candidate cap. The 10 MiB Config 29 collated-data value is only the outer
serialized candidate envelope; it does not replace the tighter Config 23
collator thresholds.

Those two values are hard candidate-byte projections, not sustainable mainnet
throughput. Config 23 uses `BlockLimitStatus::estimate_block_size`, not the
serialized file size, so the tool deliberately leaves
`active_config23_limit_projection_raw_tps=null`. The sample block is only 5.42%
of `max_block_bytes`; it is neither saturated nor a
classified jetton or DEX workload. The JSON therefore keeps
`mainnet_sustainable_raw_tps=null` until live collator integration,
`ValidateQuery` wall time, exact four-root commit time, candidate delivery, and
a representative saturated multi-block corpus have all passed.

The shard archive anchors the predecessor state through the intervening block
chain. The masterchain archive anchors the configuration state by requiring its
root hash to equal the producing block's Merkle update. Both archive block files
are checked against the root and file hashes encoded in their filenames.

When complete historical states are unavailable, the same transaction replay
can use lite-server proof bundles:

```text
lite-client -c 'saveconfigproof mc-config.tl <masterchain-block-id-ext>'
lite-client -c 'saveaccountproof account.tl 0:<account> <predecessor-shard-block-id-ext>'
lite-client -c 'savelibraries libraries.tl <library-hash>...'

tvm-replay-bundle \
  --archive <closed-shard.pack> \
  --block-id '(0,8000000000000000,SEQNO)' \
  --mc-proof mc-config.tl \
  --collated-data candidate-collated-data.boc \
  --account-proof <account>=account.tl \
  --library-bodies libraries.tl \
  --account-workers 4
```

`--collated-data` is optional for transaction replay and the two descriptor
roots, but required for the four-root state-transition gate.

`saveconfigproof` and `saveaccountproof` validate the returned Merkle proofs
before writing them. For linear replay, bind every account proof to the exact
predecessor shard block printed by `--inspect`. The replay tool also accepts a
proof bound to the target block's masterchain reference, validates it again,
and rejects it if an intervening shard block changed that account. Repeat
`--account-proof` for all touched accounts to obtain `scope=full_block`.

Public-library cells are content addressed. `savelibraries` verifies each body
against its requested hash, and the replay tool repeats that check. A body
bundle does not prove that the library was registered in the historical
masterchain state, so output explicitly sets
`library_membership_at_target_proven=false`. A complete historical masterchain
state is the stronger input and sets it to true. `savelibrariesproof` can save a
state-bound `getLibrariesWithProof` response when the server still retains the
requested state, but `tvm-replay-bundle` does not currently consume that format.

The 64-code-hash limit applies only to always-on validator instrumentation. An
untrusted block can introduce arbitrarily many distinct code hashes, so live
memory and metric cardinality must remain bounded. The explicit offline replay
has a finite input and retains every observed hash; it does not impose the
64-entry limit.

This path is not a replacement for `ValidationReplayer`, `tontester`, the TPS
benchmark, or `ValidateQuery`. It does not rebuild the complete shard-state
root, the Merkle update, the block root, routing, all limits, or consensus
behavior. Its sole purpose is a transaction-equivalence, bounded coordinator,
and partial dictionary-root gate before investing in executor integration.

## Operational boundary

Exact mode does not change TVM semantics, gas accounting, block contents,
validation rules, configuration, or consensus. It does add instrumentation and
unbounded-by-cardinality result storage for the explicitly selected replay
range. Do not enable it on a validator participating in consensus.

Archive package files alone are not a replay database. The replayer also needs
matching shard and masterchain states, proofs, and block handles imported into a
C++ validator database. Copy only closed archive slices: a live head package may
still be appended while it is being copied. Verify source size and a digest
before and after transfer, then import and replay from a separate database.

The sparse archive tool likewise runs only against local copies. Never point it
at a live validator database, and never run experimental validator binaries on a
node participating in consensus.
