# Validation replay

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
VM steps, exact distinct-account count, top-account concentration, and the ten
most active workchain-and-address pairs for each returned code hash.

Collection work is timed separately as `trx_tvm_profile`; it is not included in
the per-code `time_tvm` values. Exact maps can make the replay itself slower,
but do not inflate the measured TVM execution time.

Completed results remain in memory until they are evicted by the 16-run history
limit, the process exits, or they are explicitly released:

```text
vrp forget <run_id>
```

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
