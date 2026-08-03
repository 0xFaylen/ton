"""Measure replay-only parallel Collator scaling on bounded synthetic shard blocks."""

import asyncio
import json
import logging
import os
import re
import shutil
import statistics
import subprocess
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Awaitable, Callable, Iterator, TypeVar

from contract import WalletV1, WalletV1Blueprint, ton
from pytoniq_core import Address, Cell, StateInit, begin_cell
from tontester.install import Install
from tontester.network import Network, StartOptions

from tonlib import TonlibClient, TonlibError

FULL_SHARD = -(2**63)
T = TypeVar("T")
R = TypeVar("R")


def _env_int(name: str, default: int) -> int:
    value = int(os.environ.get(name, default))
    if value < 1:
        raise ValueError(f"{name} must be positive")
    return value


def _env_flag(name: str) -> bool:
    value = os.environ.get(name, "0")
    if value not in ("0", "1"):
        raise ValueError(f"{name} must be 0 or 1")
    return value == "1"


ROOT_WALLETS = _env_int("TON_BENCH_ROOT_WALLETS", 8)
CHILDREN_PER_ROOT = _env_int("TON_BENCH_CHILDREN_PER_ROOT", 8)
LEAVES_PER_CHILD = _env_int("TON_BENCH_LEAVES_PER_CHILD", 8)
SAMPLES = _env_int("TON_BENCH_SAMPLES", 3)
HOT_DIVISOR = _env_int("TON_BENCH_HOT_DIVISOR", 8)
SHARD_TARGET_MS = _env_int("TON_BENCH_SHARD_TARGET_MS", 12_000)
OVERALL_TIMEOUT = _env_int("TON_BENCH_OVERALL_TIMEOUT", 15 * 60)
COMPUTE_ROUNDS = _env_int("TON_BENCH_COMPUTE_ROUNDS", 256)
PRINT_JSON = _env_flag("TON_BENCH_PRINT_JSON")
REQUIRE_BOUNDARY_STOP = _env_flag("TON_BENCH_REQUIRE_BOUNDARY_STOP")
WORKERS = tuple(int(value) for value in os.environ.get("TON_BENCH_WORKERS", "2,4,8").split(","))
SEND_BATCH = 64
READ_BATCH = 16
DEPLOY_PARENT_BATCH = 8

if SAMPLES % 2 == 0:
    raise ValueError("TON_BENCH_SAMPLES must be odd so paired order has a median")
if not WORKERS or any(workers < 2 or workers > 64 for workers in WORKERS):
    raise ValueError("TON_BENCH_WORKERS must contain values from 2 through 64")
if COMPUTE_ROUNDS > 65535:
    raise ValueError("TON_BENCH_COMPUTE_ROUNDS must fit in 16 bits")


@dataclass
class BlockCorpus:
    label: str
    seqno: int
    raw_transactions: int
    distinct_accounts: int
    max_transactions_per_account: int
    round_transactions: int
    round_blocks: int


@dataclass
class ReplaySample:
    workers: int
    parallel_first: bool
    serial_time: float
    parallel_time: float
    speedup: float
    validate_time: float
    candidate_bytes: int
    collated_data_bytes: int
    transactions: int
    estimated_bytes: int
    gas: int
    lt_delta: int
    internal_load: float
    peak_limit_class: int
    attempts: int
    batches: int
    parallel_transactions: int
    serial_fallbacks: int
    empty_root_fallbacks: int
    boundary_stops: int
    discarded_prepared: int
    prepare_time: float
    worker_time: float
    commit_time: float


@dataclass
class ComputeContract:
    address: Address


@dataclass
class ComputeBlueprint:
    state_init: StateInit
    address: Address

    @classmethod
    def create(cls, code: Cell, seed: int) -> "ComputeBlueprint":
        data = begin_cell().store_uint(seed, 64).end_cell()
        state_init = StateInit(code=code, data=data)
        return cls(state_init=state_init, address=Address((0, state_init.serialize().hash)))

    def materialize(self, provider) -> ComputeContract:
        return ComputeContract(self.address)


def _chunks(items: list[T], size: int) -> Iterator[list[T]]:
    for offset in range(0, len(items), size):
        yield items[offset : offset + size]


async def _tonlib_retry(operation: Callable[[], Awaitable[R]]) -> R:
    for attempt in range(12):
        try:
            return await operation()
        except TonlibError:
            if attempt == 11:
                raise
            await asyncio.sleep(0.1 * (attempt + 1))
    raise AssertionError("unreachable")


async def _wait_wallet_seqno(wallet: WalletV1, expected: int, timeout: float = 30) -> None:
    async with asyncio.timeout(timeout):
        while (await _tonlib_retry(lambda: wallet.current)).seqno < expected:
            await asyncio.sleep(0.02)


async def _wait_funded(
    client: TonlibClient,
    blueprints: list[WalletV1Blueprint | ComputeBlueprint],
    timeout: float = 120,
) -> None:
    async with asyncio.timeout(timeout):
        while True:
            all_funded = True
            for chunk in _chunks(blueprints, READ_BATCH):
                states = await asyncio.gather(
                    *(
                        _tonlib_retry(
                            lambda address=blueprint.address: client.raw_get_account_state(address)
                        )
                        for blueprint in chunk
                    )
                )
                if any(state.balance <= 0 for state in states):
                    all_funded = False
                    break
            if all_funded:
                return
            await asyncio.sleep(0.05)


async def _deploy_many(parent: WalletV1, count: int, amount: str) -> list[WalletV1Blueprint]:
    seqno = (await _tonlib_retry(lambda: parent.current)).seqno
    blueprints: list[WalletV1Blueprint] = []
    for _ in range(count):
        blueprint = WalletV1Blueprint(workchain=-1)
        await parent.deploy(blueprint, ton(amount), seqno=seqno)
        seqno += 1
        await _wait_wallet_seqno(parent, seqno)
        blueprints.append(blueprint)
    return blueprints


async def _deploy_generation(
    parents: list[WalletV1], count: int, amount: str
) -> list[WalletV1Blueprint]:
    result: list[WalletV1Blueprint] = []
    for parent_chunk in _chunks(parents, DEPLOY_PARENT_BATCH):
        nested = await asyncio.gather(
            *(_deploy_many(parent, count, amount) for parent in parent_chunk)
        )
        result.extend(blueprint for group in nested for blueprint in group)
    return result


async def _wc0_tip(client: TonlibClient) -> int:
    mc_info = await _tonlib_retry(client.get_masterchain_info)
    assert mc_info.last is not None
    shards = await _tonlib_retry(lambda: client.get_shards(mc_info.last))
    return max(shard.seqno for shard in shards.shards if shard.workchain == 0)


async def _block_shape(client: TonlibClient, seqno: int) -> tuple[int, int, int]:
    block = await _tonlib_retry(
        lambda: client.lookup_block(workchain=0, shard=FULL_SHARD, seqno=seqno)
    )
    transactions = await _tonlib_retry(lambda: client.get_block_transactions(block))
    counts = Counter(transaction.account for transaction in transactions)
    return len(transactions), len(counts), max(counts.values(), default=0)


async def _collect_round(
    network: Network,
    client: TonlibClient,
    label: str,
    first_seqno: int,
    expected_transactions: int,
) -> BlockCorpus:
    next_seqno = first_seqno + 1
    total_transactions = 0
    shapes: list[tuple[int, int, int, int]] = []
    async with asyncio.timeout(120):
        while total_transactions < expected_transactions:
            tip = await _wc0_tip(client)
            while next_seqno <= tip:
                transactions, distinct, max_per_account = await _block_shape(client, next_seqno)
                shapes.append((transactions, distinct, max_per_account, next_seqno))
                total_transactions += transactions
                next_seqno += 1
            if total_transactions >= expected_transactions:
                break
            await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=tip + 1)
    if not shapes:
        raise RuntimeError(f"{label} produced no basechain blocks")
    transactions, distinct, max_per_account, seqno = max(shapes)
    return BlockCorpus(
        label=label,
        seqno=seqno,
        raw_transactions=transactions,
        distinct_accounts=distinct,
        max_transactions_per_account=max_per_account,
        round_transactions=total_transactions,
        round_blocks=len(shapes),
    )


async def _send_destination_deploys(
    sources: list[WalletV1],
    destinations: list[WalletV1Blueprint | ComputeBlueprint],
    source_seqno: int,
) -> None:
    pairs = list(zip(sources, destinations, strict=True))
    for chunk in _chunks(pairs, SEND_BATCH):
        await asyncio.gather(
            *(
                source.deploy(destination, ton("0.005"), seqno=source_seqno)
                for source, destination in chunk
            )
        )


async def _send_transfer_round(
    sources: list[WalletV1],
    destinations: list[WalletV1Blueprint | ComputeBlueprint],
    source_seqno: int,
    hot_count: int = 0,
    body: Cell | None = None,
) -> None:
    jobs: list[tuple[WalletV1, WalletV1Blueprint | ComputeBlueprint]] = []
    for index, source in enumerate(sources):
        destination = destinations[0] if index < hot_count else destinations[index]
        jobs.append((source, destination))
    for chunk in _chunks(jobs, SEND_BATCH):
        await asyncio.gather(
            *(
                source.transfer(
                    destination.address,
                    ton("0.001"),
                    body=body,
                    seqno=source_seqno,
                )
                for source, destination in chunk
            )
        )


def _compile_compute_contract(build_dir: Path, repo_root: Path, working_dir: Path) -> Cell:
    func_exe = build_dir / "crypto/func.exe"
    fift_exe = build_dir / "crypto/fift.exe"
    source = repo_root / "test/integration/contracts/compute-bound.fc"
    output_fif = working_dir / "compute-bound.fif"
    output_boc = working_dir / "compute-bound.boc"
    compile_result = subprocess.run(
        [
            func_exe,
            f"-W{output_boc.name}",
            f"-o{output_fif.name}",
            source,
        ],
        cwd=working_dir,
        capture_output=True,
        text=True,
        check=False,
    )
    if compile_result.returncode != 0:
        raise RuntimeError(f"FunC compilation failed:\n{compile_result.stderr}")
    fift_env = os.environ.copy()
    fift_env["FIFTPATH"] = str(repo_root / "crypto/fift/lib")
    assemble_result = subprocess.run(
        [fift_exe, output_fif.name],
        cwd=working_dir,
        env=fift_env,
        capture_output=True,
        text=True,
        check=False,
    )
    if assemble_result.returncode != 0:
        raise RuntimeError(f"Fift assembly failed:\n{assemble_result.stderr}")
    return Cell.one_from_boc(output_boc.read_bytes())


def _run_section(status: str, run_id: int) -> str:
    match = re.search(rf"(?ms)^  #{run_id}:.*?(?=^  #\d+:|\Z)", status)
    if not match:
        raise RuntimeError(f"run #{run_id} is missing from validation replay status:\n{status}")
    return match.group(0)


def _required(pattern: str, section: str, label: str) -> re.Match[str]:
    match = re.search(pattern, section)
    if not match:
        raise RuntimeError(f"{label} is missing from validation replay output:\n{section}")
    return match


def _parse_sample(section: str, workers: int, parallel_first: bool) -> ReplaySample:
    if "ERROR:" in section or "exact_candidate_match=true" not in section:
        raise RuntimeError(f"validation replay failed equivalence:\n{section}")
    collate = _required(
        r"Collate: size=(\d+)/(\d+), cdata_size=(\d+), time=([0-9.]+)",
        section,
        "collation summary",
    )
    workload = _required(
        r"Block workload: transactions=([0-9.]+), estimated_bytes=([0-9.]+), "
        r"gas=([0-9.]+), lt_delta=([0-9.]+), internal_load=([0-9.]+), "
        r"peak_limit_class=(\d+)",
        section,
        "block workload",
    )
    replay = _required(
        r"serial_time=([0-9.]+), parallel_time=([0-9.]+), speedup=([0-9.]+)",
        section,
        "paired replay timing",
    )
    actual = _required(
        r"Parallel account actual: attempts=([0-9.]+), batches=([0-9.]+), "
        r"transactions=([0-9.]+), serial_fallbacks=([0-9.]+), "
        r"empty_root_fallbacks=([0-9.]+), boundary_stops=([0-9.]+), "
        r"discarded_prepared=([0-9.]+), prepare_time=([0-9.]+), "
        r"worker_time=([0-9.]+), commit_time=([0-9.]+)",
        section,
        "actual parallel work",
    )
    validate = _required(r"Validate: time=([0-9.]+)", section, "ValidateQuery timing")

    return ReplaySample(
        workers=workers,
        parallel_first=parallel_first,
        serial_time=float(replay.group(1)),
        parallel_time=float(replay.group(2)),
        speedup=float(replay.group(3)),
        validate_time=float(validate.group(1)),
        candidate_bytes=int(collate.group(1)),
        collated_data_bytes=int(collate.group(3)),
        transactions=round(float(workload.group(1))),
        estimated_bytes=round(float(workload.group(2))),
        gas=round(float(workload.group(3))),
        lt_delta=round(float(workload.group(4))),
        internal_load=float(workload.group(5)),
        peak_limit_class=int(workload.group(6)),
        attempts=round(float(actual.group(1))),
        batches=round(float(actual.group(2))),
        parallel_transactions=round(float(actual.group(3))),
        serial_fallbacks=round(float(actual.group(4))),
        empty_root_fallbacks=round(float(actual.group(5))),
        boundary_stops=round(float(actual.group(6))),
        discarded_prepared=round(float(actual.group(7))),
        prepare_time=float(actual.group(8)),
        worker_time=float(actual.group(9)),
        commit_time=float(actual.group(10)),
    )


async def _run_replay(
    node,
    seqno: int,
    run_id: int,
    workers: int,
    parallel_first: bool,
) -> ReplaySample:
    order_flag = " --parallel-first" if parallel_first else ""
    command = (
        f"run --mode both --parallel-account-workers {workers}"
        f"{order_flag} (0,8000000000000000,{seqno})"
    )
    started = await node.engine_console.validation_replayer_command(command)
    if not started.startswith("Started"):
        raise RuntimeError(f"validation replay did not start: {started}")
    async with asyncio.timeout(120):
        while True:
            status = await node.engine_console.validation_replayer_command("show")
            if f"#{run_id}:" in status and "Current run" not in status:
                break
            await asyncio.sleep(0.1)
    return _parse_sample(_run_section(status, run_id), workers, parallel_first)


async def _benchmark_corpus(node, corpus: BlockCorpus, first_run_id: int):
    run_id = first_run_id
    by_workers: dict[str, object] = {}
    for workers in WORKERS:
        samples: list[ReplaySample] = []
        for sample_index in range(SAMPLES):
            parallel_first = sample_index % 2 == 1
            sample = await _run_replay(
                node,
                corpus.seqno,
                run_id,
                workers,
                parallel_first,
            )
            run_id += 1
            if sample.transactions != corpus.raw_transactions:
                raise RuntimeError(
                    f"Collator counted {sample.transactions} transactions, "
                    f"block contains {corpus.raw_transactions}"
                )
            if corpus.label == "compute_bound" and sample.parallel_transactions == 0:
                raise RuntimeError("compute benchmark block executed no transactions in parallel")
            samples.append(sample)
            print(
                f"{corpus.label}: workers={workers} "
                f"order={'parallel-first' if parallel_first else 'serial-first'} "
                f"serial={sample.serial_time:.6f}s parallel={sample.parallel_time:.6f}s "
                f"speedup={sample.speedup:.3f}x actual_parallel={sample.parallel_transactions}"
            )
        median_serial = statistics.median(sample.serial_time for sample in samples)
        median_parallel = statistics.median(sample.parallel_time for sample in samples)
        median_speedup = statistics.median(sample.speedup for sample in samples)
        by_workers[str(workers)] = {
            "median_serial_time": median_serial,
            "median_parallel_time": median_parallel,
            "median_paired_speedup": median_speedup,
            "diagnostic_serial_raw_tps": corpus.raw_transactions / median_serial,
            "diagnostic_parallel_raw_tps": corpus.raw_transactions / median_parallel,
            "samples": [asdict(sample) for sample in samples],
        }
    return by_workers, run_id


async def main() -> int:
    source_count = ROOT_WALLETS * CHILDREN_PER_ROOT * LEAVES_PER_CHILD
    if source_count < max(WORKERS):
        raise ValueError("source wallet count must be at least the largest worker count")
    repo_root = Path(__file__).resolve().parents[2]
    working_dir = repo_root / "test/integration/.network/single-shard-parallel-benchmark"
    shutil.rmtree(working_dir, ignore_errors=True)
    working_dir.mkdir(parents=True, exist_ok=True)
    build_dir = Path(os.environ.get("TON_BUILD_DIR", repo_root / "build"))
    install = Install(build_dir, repo_root)
    install.tonlibjson.client_set_verbosity_level(0)
    logging.basicConfig(level=logging.WARNING)

    print(
        f"source wallets: {ROOT_WALLETS} x {CHILDREN_PER_ROOT} x "
        f"{LEAVES_PER_CHILD} = {source_count}"
    )
    async with Network(install, working_dir) as network:
        assert network.config.mc_consensus is not None
        assert network.config.shard_consensus is not None
        network.config.mc_consensus.target_block_rate_ms = 40
        network.config.mc_consensus.first_block_timeout_ms = 80
        network.config.shard_consensus.target_block_rate_ms = SHARD_TARGET_MS
        network.config.shard_consensus.first_block_timeout_ms = SHARD_TARGET_MS + 500

        dht = network.create_dht_node()
        node = network.create_full_node()
        node.make_initial_validator()
        node.announce_to(dht)
        options = StartOptions(verbosity=2, console_verbosity=1)
        async with asyncio.TaskGroup() as start_group:
            _ = start_group.create_task(dht.run(options))
            _ = start_group.create_task(node.run(options))

        await network.wait_mc_block(seqno=3)
        await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=1)
        client = await node.tonlib_client()
        main_wallet = network.zerostate.main_wallet(client)

        print("deploying root source wallets")
        root_blueprints = await _deploy_many(main_wallet, ROOT_WALLETS, "5")
        await _wait_funded(client, root_blueprints)
        root_wallets = [blueprint.materialize(client) for blueprint in root_blueprints]

        print("deploying child source wallets")
        child_blueprints = await _deploy_generation(root_wallets, CHILDREN_PER_ROOT, "0.4")
        await _wait_funded(client, child_blueprints)
        child_wallets = [blueprint.materialize(client) for blueprint in child_blueprints]

        print("deploying leaf source wallets")
        leaf_blueprints = await _deploy_generation(child_wallets, LEAVES_PER_CHILD, "0.03")
        await _wait_funded(client, leaf_blueprints)
        sources = [blueprint.materialize(client) for blueprint in leaf_blueprints]
        destinations = [WalletV1Blueprint(workchain=0) for _ in sources]

        print("deploying basechain destinations")
        await _send_destination_deploys(sources, destinations, source_seqno=0)
        await _wait_funded(client, destinations)

        independent_first = await _wc0_tip(client)
        print("sending independent destination round")
        await _send_transfer_round(sources, destinations, source_seqno=1)
        independent = await _collect_round(
            network,
            client,
            "independent",
            independent_first,
            source_count,
        )
        print(f"independent corpus: {asdict(independent)}")

        hotspot_first = await _wc0_tip(client)
        hot_count = max(2, source_count // HOT_DIVISOR)
        print(f"sending hotspot round with {hot_count} messages to one account")
        await _send_transfer_round(
            sources,
            destinations,
            source_seqno=2,
            hot_count=hot_count,
        )
        hotspot = await _collect_round(
            network,
            client,
            "hotspot",
            hotspot_first,
            source_count,
        )
        print(f"hotspot corpus: {asdict(hotspot)}")

        compute_code = _compile_compute_contract(build_dir, repo_root, working_dir)
        compute_destinations = [
            ComputeBlueprint.create(compute_code, seed) for seed in range(source_count)
        ]
        print(f"deploying compute-bound destinations with {COMPUTE_ROUNDS} rounds")
        compute_deploy_first = await _wc0_tip(client)
        await _send_destination_deploys(sources, compute_destinations, source_seqno=3)
        compute_deploy = await _collect_round(
            network,
            client,
            "compute_deploy",
            compute_deploy_first,
            source_count,
        )
        print(
            f"compute deployment barrier: {compute_deploy.round_transactions} transactions "
            f"in {compute_deploy.round_blocks} blocks"
        )

        compute_first = await _wc0_tip(client)
        compute_body = begin_cell().store_uint(COMPUTE_ROUNDS, 16).end_cell()
        print("sending compute-bound destination round")
        await _send_transfer_round(
            sources,
            compute_destinations,
            source_seqno=4,
            body=compute_body,
        )
        compute_bound = await _collect_round(
            network,
            client,
            "compute_bound",
            compute_first,
            source_count,
        )
        print(f"compute-bound corpus: {asdict(compute_bound)}")

        print("running paired replay matrix")
        independent_results, next_run_id = await _benchmark_corpus(node, independent, 0)
        hotspot_results, next_run_id = await _benchmark_corpus(node, hotspot, next_run_id)
        compute_results, _ = await _benchmark_corpus(node, compute_bound, next_run_id)

        if REQUIRE_BOUNDARY_STOP:
            compute_samples = [
                sample
                for worker_result in compute_results.values()
                for sample in worker_result["samples"]
            ]
            if not compute_samples or any(
                sample["boundary_stops"] == 0 or sample["discarded_prepared"] == 0
                for sample in compute_samples
            ):
                raise RuntimeError("compute replay did not exercise prefix-safe boundary discard")

        report = {
            "classification": "synthetic_replay_diagnostic_not_sustainable_mainnet_tps",
            "source_wallets": source_count,
            "compute_rounds": COMPUTE_ROUNDS,
            "samples_per_worker": SAMPLES,
            "workers": [1, *WORKERS],
            "shard_target_ms_for_message_accumulation": SHARD_TARGET_MS,
            "mainnet_sustainable_raw_tps": None,
            "corpora": {
                "independent": {
                    "block": asdict(independent),
                    "workers": independent_results,
                },
                "hotspot": {
                    "block": asdict(hotspot),
                    "workers": hotspot_results,
                },
                "compute_bound": {
                    "block": asdict(compute_bound),
                    "workers": compute_results,
                },
            },
        }
        report_path = working_dir.parent / "single-shard-parallel-benchmark-report.json"
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print("=== single_shard_parallel_benchmark PASS ===")
        print(f"report: {report_path}")
        if PRINT_JSON:
            print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(asyncio.wait_for(main(), OVERALL_TIMEOUT)))
