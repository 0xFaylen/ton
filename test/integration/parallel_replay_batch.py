"""Gate exact selective Collator replay on cheap multi-account messages."""

import asyncio
import logging
import os
import re
import shutil
import sys
from collections import Counter
from pathlib import Path

from contract import WalletV1, WalletV1Blueprint, ton
from tontester.install import Install
from tontester.network import Network, StartOptions

from tonlib import TonlibClient

FULL_SHARD = -(2**63)
DESTINATIONS = 16
REPEATED_TRANSFERS = 4
OVERALL_TIMEOUT = 5 * 60


async def _wait_wallet_seqno(wallet: WalletV1, expected: int) -> None:
    async with asyncio.timeout(10):
        while (await wallet.current).seqno < expected:
            await asyncio.sleep(0.02)


async def _wait_deployed(client: TonlibClient, blueprints: list[WalletV1Blueprint]) -> None:
    async with asyncio.timeout(20):
        while True:
            states = await asyncio.gather(
                *(client.raw_get_account_state(blueprint.address) for blueprint in blueprints)
            )
            if all(state.balance > 0 for state in states):
                return
            await asyncio.sleep(0.05)


async def _wc0_tip(client: TonlibClient) -> int:
    mc_info = await client.get_masterchain_info()
    assert mc_info.last is not None
    shards = await client.get_shards(mc_info.last)
    return max(shard.seqno for shard in shards.shards if shard.workchain == 0)


async def _find_best_block(
    client: TonlibClient, first_seqno: int, last_seqno: int
) -> tuple[int, int, int, int]:
    best = (0, 0, 0, 0)
    for seqno in range(first_seqno, last_seqno + 1):
        block = await client.lookup_block(workchain=0, shard=FULL_SHARD, seqno=seqno)
        transactions = await client.get_block_transactions(block)
        counts = Counter(transaction.account for transaction in transactions)
        candidate = (
            len(transactions),
            len(counts),
            max(counts.values(), default=0),
            seqno,
        )
        if candidate[:3] > best[:3]:
            best = candidate
    transactions, distinct_accounts, max_per_account, seqno = best
    return seqno, transactions, distinct_accounts, max_per_account


async def _run_replay(node, seqno: int, run_id: int, parallel_first: bool) -> str:
    order_flag = " --parallel-first" if parallel_first else ""
    command = (
        f"run --mode both --parallel-account-workers 4{order_flag} (0,8000000000000000,{seqno})"
    )
    started = await node.engine_console.validation_replayer_command(command)
    if not started.startswith("Started"):
        raise RuntimeError(f"validation replay did not start: {started}")

    async with asyncio.timeout(30):
        while True:
            status = await node.engine_console.validation_replayer_command("show")
            if f"#{run_id}:" in status and "Current run" not in status:
                break
            await asyncio.sleep(0.1)
    if "ERROR:" in status:
        raise RuntimeError(f"validation replay failed:\n{status}")
    if status.count("exact_candidate_match=true") < run_id + 1:
        raise RuntimeError(f"candidate equality evidence is missing:\n{status}")
    if status.count("Validate: time=") < run_id + 1:
        raise RuntimeError(f"ValidateQuery evidence is missing:\n{status}")
    section = re.search(rf"(?ms)^  #{run_id}:.*?(?=^  #\d+:|\Z)", status)
    if not section or not re.search(
        r"Parallel account actual:.*transactions=0\.00", section.group(0)
    ):
        raise RuntimeError(
            f"empty-body messages unexpectedly entered parallel execution:\n{status}"
        )
    return status


async def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    working_dir = repo_root / "test/integration/.network/parallel-replay-batch"
    shutil.rmtree(working_dir, ignore_errors=True)
    working_dir.mkdir(parents=True, exist_ok=True)

    build_dir = Path(os.environ.get("TON_BUILD_DIR", repo_root / "build"))
    install = Install(build_dir, repo_root)
    install.tonlibjson.client_set_verbosity_level(0)
    logging.basicConfig(level=logging.WARNING)

    async with Network(install, working_dir) as network:
        assert network.config.mc_consensus is not None
        assert network.config.shard_consensus is not None
        network.config.mc_consensus.target_block_rate_ms = 80
        network.config.mc_consensus.first_block_timeout_ms = 160
        network.config.shard_consensus.target_block_rate_ms = 2500
        network.config.shard_consensus.first_block_timeout_ms = 2800

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
        seqno = (await main_wallet.current).seqno
        deployment_scan_first = await _wc0_tip(client)

        source_blueprints = [WalletV1Blueprint(workchain=-1) for _ in range(DESTINATIONS)]
        for blueprint in source_blueprints:
            await main_wallet.deploy(blueprint, ton("0.1"), seqno=seqno)
            seqno += 1
            await _wait_wallet_seqno(main_wallet, seqno)

        await _wait_deployed(client, source_blueprints)
        source_wallets = [blueprint.materialize(client) for blueprint in source_blueprints]
        destinations = [WalletV1Blueprint(workchain=0) for _ in range(DESTINATIONS)]
        for source, destination in zip(source_wallets, destinations, strict=True):
            await source.transfer(destination.address, ton("0.01"), seqno=0)

        await _wait_deployed(client, destinations)
        deployment_scan_last = await _wc0_tip(client)
        deployment_target, deployment_transactions, _, _ = await _find_best_block(
            client, deployment_scan_first + 1, deployment_scan_last
        )
        if deployment_transactions < 8:
            raise RuntimeError(
                f"empty-account-root corpus is insufficient: tx={deployment_transactions}"
            )

        first_wc0_seqno = deployment_scan_last
        for index, source in enumerate(source_wallets):
            destination = destinations[0] if index < REPEATED_TRANSFERS else destinations[index]
            await source.transfer(destination.address, ton("0.01"), seqno=1)
        await asyncio.gather(*(_wait_wallet_seqno(source, 2) for source in source_wallets))

        target = transactions = distinct_accounts = max_per_account = 0
        async with asyncio.timeout(30):
            while transactions < 8 or distinct_accounts < 6 or max_per_account < 2:
                last_wc0_seqno = await _wc0_tip(client)
                if last_wc0_seqno > first_wc0_seqno:
                    result = await _find_best_block(client, first_wc0_seqno + 1, last_wc0_seqno)
                    target, transactions, distinct_accounts, max_per_account = result
                if transactions >= 8 and distinct_accounts >= 6 and max_per_account >= 2:
                    break
                await network.wait_block(workchain=0, shard=FULL_SHARD, seqno=last_wc0_seqno + 1)

        initial_state_serial_gate = await _run_replay(
            node, deployment_target, 0, parallel_first=False
        )
        serial_first = await _run_replay(node, target, 1, parallel_first=False)
        parallel_first = await _run_replay(node, target, 2, parallel_first=True)

        print("=== parallel_replay_batch PASS ===")
        print(f"initial-state target:   {deployment_target}")
        print(f"initial-state tx:       {deployment_transactions}")
        print(f"target wc0 seqno:       {target}")
        print(f"raw transactions:      {transactions}")
        print(f"distinct accounts:     {distinct_accounts}")
        print(f"max tx per account:    {max_per_account}")
        print("initial-state serial-gate evidence:")
        print(initial_state_serial_gate.rstrip())
        print("serial-first evidence:")
        print(serial_first.rstrip())
        print("parallel-first evidence:")
        print(parallel_first.rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(asyncio.wait_for(main(), OVERALL_TIMEOUT)))
