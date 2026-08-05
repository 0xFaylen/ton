"""Single-node jetton TPS benchmark orchestrator (see benchmark/DESIGN.md).

Boots a tontester network whose workchain-0 zerostate was generated externally
by bench-state-gen (the state cells are checkpointed straight into the node's
celldb before first start), then drives bench-spam against the node's
liteserver and collects results.

With --smoke the spam phase is skipped: the network is booted from the external
state, wc0 progress is verified, and a single account (--probe-addr) is queried
via tonlib as the first integration gate.
"""

import argparse
import asyncio
import csv
import datetime
import json
import logging
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from pytoniq_core import Address
from tontester.install import Install
from tontester.network import FullNode, Network, StartOptions
from tontester.zerostate import ExternalBasechainState, SimplexConsensusConfig

from tl import JSONSerializable

l = logging.getLogger("bench_jetton")

FULL_SHARD = -(2**63)
# wc0 reaching this seqno proves the externally generated state was loaded and
# the collator can build blocks on top of it.
MIN_WC0_SEQNO = 3
# The huge state may take a while to open on first start.
STARTUP_TIMEOUT = 600
TRACK_SAMPLE = 0.01

# Validator log lines we scrape for collation/validation timings. Verified against:
#   validator/impl/collator.cpp:6480       LOG(WARNING) << "collation took " << ... << " s";
#   validator/impl/validate-query.cpp:7626 LOG(WARNING) << "validation took " << ... << "s";
#   validator/impl/collator.cpp:367        LOG(INFO) << "collation failed in " << ... << " s ";
# NOTE: both are td::PerfWarningTimer lines emitted only when the phase exceeds
# the 0.1 s threshold; fast blocks leave no trace.
# TODO(bench): for complete per-block timings hook into ValidatorManager's
# add_perf_timer_stat ("collate" stat) or the session-logs instead of grepping.
TIMING_MARKERS = ("collation took", "validation took", "collation failed in")


@dataclass(frozen=True)
class BenchParams:
    manifest: Path
    net_dir: Path
    celldb_checkpoint_dir: Path | None
    build_dir: Path | None
    rate: float
    duration: int
    warmup: int
    wallet_offset: int
    block_limit_mul: int
    gas_limit_mul: int
    out_dir: Path
    keep_net: bool
    smoke: bool
    probe_addr: str | None
    node_verbosity: int
    engine_args: tuple[str, ...]
    spam_args: tuple[str, ...]
    vrp_workers: tuple[int, ...]
    vrp_blocks: int
    vrp_samples: int


def _parse_args(argv: list[str] | None = None) -> BenchParams:
    parser = argparse.ArgumentParser(description=__doc__)
    _ = parser.add_argument(
        "--manifest", required=True, type=Path, help="manifest.json written by bench-state-gen"
    )
    _ = parser.add_argument(
        "--net-dir",
        type=Path,
        default=Path("/mnt/bench/net"),
        help="network working directory (recreated unless --keep-net)",
    )
    _ = parser.add_argument(
        "--celldb-checkpoint-dir",
        type=Path,
        default=None,
        help=(
            "physical location for the disposable celldb checkpoint (recreated every run); "
            "must be on the same filesystem as the manifest's celldb for hardlinks to work. "
            "When set, <node dir>/celldb becomes a symlink to it, which lets --net-dir live "
            "on a different device than the state (fsync-heavy small writes — consensus DB, "
            "statedb, archive, logs — split away from the big read workload)"
        ),
    )
    _ = parser.add_argument("--rate", type=float, default=500.0, help="external msgs per second")
    _ = parser.add_argument("--duration", type=int, default=60, help="spam duration, seconds")
    _ = parser.add_argument("--warmup", type=int, default=10, help="spam warmup, seconds")
    _ = parser.add_argument(
        "--wallet-offset",
        type=int,
        default=0,
        help="first wallet index to use (advance between runs to keep seqnos valid)",
    )
    _ = parser.add_argument("--block-limit-mul", type=int, default=1)
    _ = parser.add_argument("--gas-limit-mul", type=int, default=1)
    _ = parser.add_argument(
        "--out-dir", type=Path, default=Path("bench-out"), help="report directory"
    )
    _ = parser.add_argument(
        "--keep-net", action="store_true", help="do not delete --net-dir before the run"
    )
    _ = parser.add_argument(
        "--smoke", action="store_true", help="boot-only smoke run, skip bench-spam"
    )
    _ = parser.add_argument(
        "--probe-addr",
        default=None,
        help="wc0 account address (64 hex chars) to probe in --smoke mode",
    )
    _ = parser.add_argument(
        "--node-verbosity",
        type=int,
        default=1,
        help="validator-engine log verbosity (keep low for high-rate runs)",
    )
    _ = parser.add_argument(
        "--engine-arg",
        action="append",
        default=[],
        dest="engine_args",
        help="extra validator-engine CLI arg (repeatable), e.g. --engine-arg=--celldb-cache-size=34359738368",
    )
    _ = parser.add_argument(
        "--spam-arg",
        action="append",
        default=[],
        dest="spam_args",
        help="extra bench-spam CLI arg (repeatable), e.g. --spam-arg=--connections=8",
    )
    _ = parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="CMake build directory with the TON binaries (default: <repo root>/build)",
    )
    _ = parser.add_argument(
        "--vrp-workers",
        default="",
        help=(
            "comma-separated parallel-account worker counts (e.g. 2,4,8); when non-empty, "
            "after the spam phase the most transaction-heavy wc0 blocks are replayed through "
            "the replay-only parallel Collator gate (vrp run --mode both) and every run must "
            "produce a byte-identical candidate and pass ValidateQuery"
        ),
    )
    _ = parser.add_argument(
        "--vrp-blocks",
        type=int,
        default=3,
        help="number of most transaction-heavy wc0 blocks to gate (default: 3)",
    )
    _ = parser.add_argument(
        "--vrp-samples",
        type=int,
        default=2,
        help=(
            "paired samples per (block, workers); sample order alternates serial-first and "
            "parallel-first so cache warming cannot masquerade as executor speedup (default: 2)"
        ),
    )
    args = parser.parse_args(argv)
    celldb_checkpoint_dir = cast(Path | None, args.celldb_checkpoint_dir)
    build_dir = cast(Path | None, args.build_dir)
    vrp_workers_raw = cast(str, args.vrp_workers).strip()
    vrp_workers = tuple(int(w) for w in vrp_workers_raw.split(",") if w) if vrp_workers_raw else ()
    for w in vrp_workers:
        if w < 2 or w > 64:
            parser.error("--vrp-workers entries must be between 2 and 64")
    return BenchParams(
        manifest=cast(Path, args.manifest).absolute(),
        net_dir=cast(Path, args.net_dir).absolute(),
        celldb_checkpoint_dir=(
            celldb_checkpoint_dir.absolute() if celldb_checkpoint_dir is not None else None
        ),
        build_dir=build_dir.absolute() if build_dir is not None else None,
        rate=cast(float, args.rate),
        duration=cast(int, args.duration),
        warmup=cast(int, args.warmup),
        wallet_offset=cast(int, args.wallet_offset),
        block_limit_mul=cast(int, args.block_limit_mul),
        gas_limit_mul=cast(int, args.gas_limit_mul),
        out_dir=cast(Path, args.out_dir).absolute(),
        keep_net=cast(bool, args.keep_net),
        smoke=cast(bool, args.smoke),
        probe_addr=cast(str | None, args.probe_addr),
        node_verbosity=cast(int, args.node_verbosity),
        engine_args=tuple(cast(list[str], args.engine_args)),
        spam_args=tuple(cast(list[str], args.spam_args)),
        vrp_workers=vrp_workers,
        vrp_blocks=cast(int, args.vrp_blocks),
        vrp_samples=cast(int, args.vrp_samples),
    )


def _manifest_celldb_path(manifest: Path) -> Path:
    raw = cast(JSONSerializable, json.loads(manifest.read_text()))
    assert isinstance(raw, Mapping), f"{manifest}: manifest must be a JSON object"
    celldb_path = raw["celldb_path"]
    assert isinstance(celldb_path, str), f"{manifest}: field 'celldb_path' must be a string"
    return Path(celldb_path)


def _git_rev(repo_root: Path) -> str:
    return subprocess.run(
        ("git", "-C", str(repo_root), "rev-parse", "HEAD"),
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def _bench_exe(build_dir: Path, name: str) -> str:
    suffix = ".exe" if sys.platform == "win32" else ""
    return str(build_dir / f"benchmark/{name}{suffix}")


async def _run_choomed(cmd: list[str]) -> None:
    """Run a heavy subprocess, inheriting stdout/stderr.

    On Linux the process runs under `choom -n 1000 --`; where choom does not
    exist (Windows, macOS) it runs directly. Inherited stderr means e.g.
    bench-spam progress lines stream to the console.
    """
    full_cmd = ["choom", "-n", "1000", "--", *cmd] if shutil.which("choom") else cmd
    l.info(f"running: {shlex.join(full_cmd)}")
    started = time.monotonic()
    process = await asyncio.create_subprocess_exec(*full_cmd)
    return_code = await process.wait()
    elapsed = time.monotonic() - started
    if return_code != 0:
        raise RuntimeError(f"{cmd[0]} failed with code {return_code} after {elapsed:.1f}s")
    l.info(f"{cmd[0]} finished in {elapsed:.1f}s")


def _write_run_config(path: Path, params: BenchParams, repo_root: Path) -> None:
    config: dict[str, JSONSerializable] = {
        "manifest": str(params.manifest),
        "net_dir": str(params.net_dir),
        "celldb_checkpoint_dir": (
            str(params.celldb_checkpoint_dir) if params.celldb_checkpoint_dir is not None else None
        ),
        "rate": params.rate,
        "duration": params.duration,
        "warmup": params.warmup,
        "wallet_offset": params.wallet_offset,
        "block_limit_mul": params.block_limit_mul,
        "gas_limit_mul": params.gas_limit_mul,
        "out_dir": str(params.out_dir),
        "keep_net": params.keep_net,
        "smoke": params.smoke,
        "probe_addr": params.probe_addr,
        "engine_args": list(params.engine_args),
        "spam_args": list(params.spam_args),
        "git_rev": _git_rev(repo_root),
        "finished_at": datetime.datetime.now(datetime.UTC).isoformat(),
    }
    _ = path.write_text(json.dumps(config, indent=2) + "\n")


def _extract_timings(log_path: Path, out_path: Path) -> int:
    """Copy collation/validation timing lines from the validator log into out_path."""
    count = 0
    with log_path.open("r", errors="replace") as src, out_path.open("w") as dst:
        for line in src:
            if any(marker in line for marker in TIMING_MARKERS):
                _ = dst.write(line)
                count += 1
    return count


async def _smoke_probe(node: FullNode, probe_addr: str | None) -> int:
    if probe_addr is None:
        l.warning("--smoke without --probe-addr: network booted, skipping account probe")
        return 0
    client = await node.tonlib_client()
    address = Address((0, bytes.fromhex(probe_addr)))
    state = await client.raw_get_account_state(address)
    print(f"probe account 0:{probe_addr}: balance={state.balance} nanotons")
    if state.balance <= 0:
        l.error("probe account has no balance — external state not visible?")
        return 1
    return 0


async def _run_spam(install: Install, repo_root: Path, node: FullNode, params: BenchParams) -> int:
    endpoint = node.liteserver_endpoint()
    spam_dir = params.net_dir / "spam"
    spam_dir.mkdir(exist_ok=True)
    results_json = spam_dir / "results.json"
    blocks_csv = spam_dir / "blocks.csv"

    await _run_choomed(
        [
            _bench_exe(install.build_dir, "bench-spam"),
            "--manifest",
            str(params.manifest),
            "--contracts-dir",
            str(repo_root / "benchmark/contracts"),
            "--liteserver",
            f"{endpoint.host}:{endpoint.port}",
            "--liteserver-pubkey-b64",
            endpoint.pubkey_b64,
            "--rate",
            str(params.rate),
            "--duration",
            str(params.duration),
            "--warmup",
            str(params.warmup),
            "--wallet-offset",
            str(params.wallet_offset),
            "--track-sample",
            str(TRACK_SAMPLE),
            "--out",
            str(results_json),
            "--blocks-csv",
            str(blocks_csv),
            *params.spam_args,
        ]
    )

    params.out_dir.mkdir(parents=True, exist_ok=True)
    for artifact in (results_json, blocks_csv):
        if artifact.exists():
            _ = shutil.copy2(artifact, params.out_dir / artifact.name)
        else:
            l.warning(f"bench-spam did not produce {artifact}")
    _write_run_config(params.out_dir / "run-config.json", params, repo_root)
    n_timings = _extract_timings(node.log_path, params.out_dir / "node-timings.log")
    l.info(f"extracted {n_timings} collation/validation timing lines from the node log")
    l.info(f"report written to {params.out_dir}")
    return 0


_VRP_POLL_INTERVAL = 0.5
_VRP_RUN_TIMEOUT = 300.0
# Key=value pairs in `vrp show` status text, e.g. `speedup=1.741` or
# `exact_candidate_match=true`.
_VRP_KV_RE = re.compile(r"(\w+)=([0-9.eE+-]+|true|false)")


def _top_wc0_blocks(blocks_csv: Path, count: int) -> list[tuple[int, int]]:
    """Return [(seqno, n_txs)] of the most transaction-heavy wc0 blocks."""
    rows: list[tuple[int, int]] = []
    with blocks_csv.open() as f:
        for row in csv.DictReader(f):
            rows.append((int(row["seqno"]), int(row["n_txs"])))
    rows.sort(key=lambda r: (-r[1], r[0]))
    return rows[:count]


async def _vrp_wait_idle(node: FullNode) -> str:
    """Poll `vrp show` until no run is active; return the final show output."""
    deadline = time.monotonic() + _VRP_RUN_TIMEOUT
    while True:
        show = await node.engine_console.validation_replayer_command("show")
        if "Current run" not in show and "Runs in queue" not in show:
            return show
        if time.monotonic() > deadline:
            raise RuntimeError(f"vrp run did not finish within {_VRP_RUN_TIMEOUT}s:\n{show}")
        await asyncio.sleep(_VRP_POLL_INTERVAL)


def _vrp_last_run_status(show: str) -> tuple[int, str]:
    """Extract (idx, status text) of the newest past run from `vrp show` output."""
    starts = [m for m in re.finditer(r"^  #(\d+): ", show, re.MULTILINE)]
    if not starts:
        raise RuntimeError(f"no past runs in vrp show output:\n{show}")
    last = starts[-1]
    return int(last.group(1)), show[last.start() :]


async def _run_vrp_gate(node: FullNode, params: BenchParams) -> int:
    """Replay the most transaction-heavy wc0 blocks through the replay-only
    parallel Collator gate. Every run must produce a byte-identical candidate
    and pass ValidateQuery; any ERROR status fails the benchmark run."""
    blocks_csv = params.net_dir / "spam" / "blocks.csv"
    if not blocks_csv.exists():
        l.error(f"vrp gate: {blocks_csv} does not exist")
        return 1
    targets = _top_wc0_blocks(blocks_csv, params.vrp_blocks)
    if not targets:
        l.error("vrp gate: no wc0 blocks in blocks.csv")
        return 1
    l.info(f"vrp gate: blocks {[(s, n) for s, n in targets]}, workers {list(params.vrp_workers)}")

    results: list[dict[str, JSONSerializable]] = []
    raw_log: list[str] = []
    failures = 0
    for seqno, n_txs in targets:
        for workers in params.vrp_workers:
            for sample in range(params.vrp_samples):
                parallel_first = sample % 2 == 1
                command = (
                    f"run --mode both --log-work-time --parallel-account-workers {workers}"
                    + (" --parallel-first" if parallel_first else "")
                    + f" (0,8000000000000000,{seqno})"
                )
                l.info(f"vrp gate: {command}")
                started = await node.engine_console.validation_replayer_command(command)
                if "Started" not in started:
                    raise RuntimeError(f"unexpected vrp run response: {started}")
                show = await _vrp_wait_idle(node)
                run_idx, status = _vrp_last_run_status(show)
                raw_log.append(f"=== seqno={seqno} workers={workers} sample={sample} ===\n{status}")
                ok = "ERROR" not in status and "exact_candidate_match=true" in status
                failures += not ok
                parsed: dict[str, JSONSerializable] = {}
                for line in status.splitlines():
                    stripped = line.strip()
                    for prefix, section in (
                        ("Collate:", "collate"),
                        ("Block workload:", "workload"),
                        ("Parallel account replay:", "replay"),
                        ("Parallel account actual:", "actual"),
                        ("Validate:", "validate"),
                    ):
                        if stripped.startswith(prefix):
                            parsed[section] = dict(_VRP_KV_RE.findall(stripped))
                results.append(
                    {
                        "seqno": seqno,
                        "n_txs": n_txs,
                        "workers": workers,
                        "sample": sample,
                        "pass_order": "parallel-first" if parallel_first else "serial-first",
                        "run_idx": run_idx,
                        "ok": ok,
                        "status_fields": parsed,
                    }
                )
                if not ok:
                    l.error(f"vrp gate FAILED for {seqno} workers={workers}:\n{status}")
                    watch = re.search(r"account=-?\d+:([0-9A-F]{64})", status)
                    if watch is not None:
                        # Rerun the same block with the collator watching the
                        # divergent account so the node log records every
                        # access to it in both passes.
                        watch_command = command.replace(
                            " --parallel-account-workers",
                            f" --watch-account {watch.group(1)} --parallel-account-workers",
                        )
                        l.info(f"vrp gate: {watch_command}")
                        _ = await node.engine_console.validation_replayer_command(watch_command)
                        watch_show = await _vrp_wait_idle(node)
                        watch_idx, watch_status = _vrp_last_run_status(watch_show)
                        raw_log.append(
                            f"=== watch seqno={seqno} workers={workers} "
                            f"account={watch.group(1)} ===\n{watch_status}"
                        )
                        _ = await node.engine_console.validation_replayer_command(f"forget {watch_idx}")
                # Free retained hotpath results so 16-run history pressure does
                # not evict runs we still want to inspect manually.
                _ = await node.engine_console.validation_replayer_command(f"forget {run_idx}")

    params.out_dir.mkdir(parents=True, exist_ok=True)
    _ = (params.out_dir / "vrp-gate.json").write_text(
        json.dumps({"failures": failures, "runs": results}, indent=2) + "\n"
    )
    _ = (params.out_dir / "vrp-gate.log").write_text("\n".join(raw_log))
    watch_lines = [
        line for line in node.log_path.read_text(errors="replace").splitlines() if "WATCH_ACCOUNT" in line
    ]
    if watch_lines:
        _ = (params.out_dir / "vrp-watch.log").write_text("\n".join(watch_lines) + "\n")
    if failures:
        l.error(f"vrp gate: {failures} failed runs (see vrp-gate.log)")
        return 1
    l.info(f"vrp gate: all {len(results)} runs byte-identical and validated")
    return 0


async def _amain(params: BenchParams) -> int:
    repo_root = Path(__file__).resolve().parents[1]
    install = Install(params.build_dir or repo_root / "build", repo_root)
    install.tonlibjson.client_set_verbosity_level(1)

    external = ExternalBasechainState.from_manifest(params.manifest)
    celldb_src = _manifest_celldb_path(params.manifest)

    if not params.keep_net and params.net_dir.exists():
        l.info(f"removing previous network dir {params.net_dir}")
        shutil.rmtree(params.net_dir)
    params.net_dir.mkdir(parents=True, exist_ok=True)

    async with Network(install, params.net_dir) as network:
        config = network.config
        config.split = 0
        config.monitor_min_split = 0
        config.shard_validators = 1
        config.block_limit_mul = params.block_limit_mul
        config.gas_limit_mul = params.gas_limit_mul
        config.mc_consensus = SimplexConsensusConfig(target_block_rate_ms=400)
        config.shard_consensus = SimplexConsensusConfig(target_block_rate_ms=400)
        network.external_basechain = external

        dht = network.create_dht_node()
        node = network.create_full_node()
        node.make_initial_validator()
        node.announce_to(dht)

        # Pre-place the generated state's celldb into the node dir BEFORE the
        # first start (RocksDB checkpoint = hardlink copy, so each run gets a
        # disposable celldb). The engine runs with `--db .` and cwd=<node dir>,
        # so on startup this is indistinguishable from a restart.
        celldb_dst = node.directory / "celldb"
        if params.celldb_checkpoint_dir is not None:
            # Device split: the checkpoint stays on the state's filesystem
            # (hardlinks require it) while the node dir — and with it every
            # other write target (consensus DB, statedb, archive, logs) — can
            # live on another device. The engine follows the symlink.
            if params.celldb_checkpoint_dir.exists():
                l.info(f"removing previous celldb checkpoint {params.celldb_checkpoint_dir}")
                shutil.rmtree(params.celldb_checkpoint_dir)
            celldb_dst.symlink_to(params.celldb_checkpoint_dir)
            celldb_dst = params.celldb_checkpoint_dir
        await _run_choomed(
            [
                _bench_exe(install.build_dir, "bench-state-gen"),
                "checkpoint",
                "--src",
                str(celldb_src),
                "--dst",
                str(celldb_dst),
            ]
        )

        await dht.run()
        await node.run(
            StartOptions(
                args=("--disable-state-serializer", *params.engine_args),
                verbosity=params.node_verbosity,
            )
        )

        l.info(f"waiting for wc0 seqno >= {MIN_WC0_SEQNO} (proves the external state loaded)")
        async with asyncio.timeout(STARTUP_TIMEOUT):
            _ = await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=MIN_WC0_SEQNO)
        l.info("wc0 is producing blocks on top of the external state")

        if params.smoke:
            return await _smoke_probe(node, params.probe_addr)
        spam_result = await _run_spam(install, repo_root, node, params)
        if spam_result != 0 or not params.vrp_workers:
            return spam_result
        return await _run_vrp_gate(node, params)


def main(argv: list[str] | None = None) -> int:
    params = _parse_args(argv)
    logging.basicConfig(
        level=logging.INFO,
        format="[%(levelname)s][%(asctime)s][%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H-%M-%S",
    )
    return asyncio.run(_amain(params))


if __name__ == "__main__":
    sys.exit(main())
