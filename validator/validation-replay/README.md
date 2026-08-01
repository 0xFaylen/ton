# Validation replay

The large single-shard execution track is documented in
[`PARALLEL_SINGLE_SHARD_EXECUTOR.ru.md`](PARALLEL_SINGLE_SHARD_EXECUTOR.ru.md).
Its first artifact is a pure prefix-safe account-lane scheduler with regression
tests. It is not wired into the live collator yet. The bounded source review is
kept in [`PARALLEL_EXECUTION_RESEARCH.md`](PARALLEL_EXECUTION_RESEARCH.md). The
current worker/coordinator commitment contract and its remaining safety gates
are specified in [`PSAE_RECEIPT_ABI.ru.md`](PSAE_RECEIPT_ABI.ru.md).

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
tvm-replay-bundle --archive <closed-shard.pack> \
  --block-id '(0,8000000000000000,SEQNO)' --inspect --split-depth 4
```

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

The replay JSON also contains `account_lane_ceiling`. It groups measured
transaction and TVM wall time by account and reports greedy ideal makespans for
1/2/4/8/16 workers. This is an execution-only ceiling: it excludes worker
contention, serial commit, block limits, cell-proof/state merge, network, and
consensus. It must not be reported as a TPS prediction or a measured parallel
speedup.

The shard archive anchors the predecessor state through the intervening block
chain. The masterchain archive anchors the configuration state by requiring its
root hash to equal the producing block's Merkle update. Both archive block files
are checked against the root and file hashes encoded in their filenames.

When complete historical states are unavailable, the same transaction replay
can use lite-server proof bundles:

```text
lite-client -c 'saveconfigproof mc-config.tl <masterchain-block-id-ext>'
lite-client -c 'saveaccountproof account.tl 0:<account> <masterchain-block-id-ext>'
lite-client -c 'savelibraries libraries.tl <library-hash>...'

tvm-replay-bundle \
  --archive <closed-shard.pack> \
  --block-id '(0,8000000000000000,SEQNO)' \
  --mc-proof mc-config.tl \
  --account-proof <account>=account.tl \
  --library-bodies libraries.tl
```

`saveconfigproof` and `saveaccountproof` validate the returned Merkle proofs
before writing them. The replay tool validates them again and rejects stale
account proofs when an intervening block changed that account. Repeat
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
benchmark, or `ValidateQuery`. It does not rebuild the shard-state root, the
Merkle update, the block root, routing, limits, or consensus behavior. Its sole
purpose is a small transaction-equivalence and workload-attribution gate before
investing in an executor optimization.

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
