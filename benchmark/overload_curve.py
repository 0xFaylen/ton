"""Offered load versus overload-history pressure (see PARALLEL_EXECUTION_RESEARCH.md).

Runs bench_jetton at a ladder of external-message rates, one fresh network per
rate, with node verbosity 3, then scrapes the validator log for the
Collator::check_block_overload decisions of every basechain collation:
block-limit class, overload/underload reason, out_msg_queue size, force-split
bit, and want_split/want_merge outcomes. The tontester config uses
max_split=0, so want_split never changes topology here: the measurement is
split *pressure*, not splitting.

The parallel executor path is replay-only and cannot run in live collation,
so this curve characterizes the serial collator. It is the baseline that any
executor claim must move.
"""

import argparse
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

# Actor tag of a basechain collation, e.g. "[!collate(0,8000000000000000):98]".
_TAG = re.compile(r"\[!collate\(0,8000000000000000\):(\d+)\]\s+(.*)$")

_PATTERNS = {
    "queue": re.compile(r"final out_msg_queue size is (\d+)"),
    "load": re.compile(
        r"block load statistics: gas=(\d+) lt_delta=(\d+) size_estimate=(\d+) collated_size_estimate=(\d+)"
    ),
    "overloaded": re.compile(r"block is overloaded \(([^)]+)\)"),
    "underloaded": re.compile(r"block is underloaded"),
    "force_split": re.compile(r"out_msg_queue reached force split limit \((\d+) >="),
    "want_split": re.compile(r"want_split set because of overload history ([0-9a-f]{16})"),
    "want_merge": re.compile(r"want_merge set because of underload history"),
    "timers": re.compile(
        r"Check block overload timers: wait_externals=([0-9.e+-]+) do_collate=([0-9.e+-]+) total=([0-9.e+-]+)"
    ),
}


def parse_node_log(log_path: Path) -> list[dict[str, object]]:
    blocks: dict[int, dict[str, object]] = {}
    with log_path.open(errors="replace") as f:
        for line in f:
            tag = _TAG.search(line)
            if tag is None:
                continue
            seqno = int(tag.group(1))
            message = tag.group(2)
            record = blocks.setdefault(seqno, {"seqno": seqno})
            if m := _PATTERNS["queue"].search(message):
                record["queue"] = int(m.group(1))
            elif m := _PATTERNS["load"].search(message):
                record["gas"] = int(m.group(1))
                record["lt_delta"] = int(m.group(2))
                record["size_est"] = int(m.group(3))
                record["collated_est"] = int(m.group(4))
            elif m := _PATTERNS["overloaded"].search(message):
                record["overloaded"] = m.group(1)
            elif _PATTERNS["underloaded"].search(message):
                record["underloaded"] = True
            elif m := _PATTERNS["force_split"].search(message):
                record["force_split_queue"] = int(m.group(1))
            elif m := _PATTERNS["want_split"].search(message):
                record["want_split_history"] = m.group(1)
            elif _PATTERNS["want_merge"].search(message):
                record["want_merge"] = True
            elif m := _PATTERNS["timers"].search(message):
                record["wait_externals_s"] = float(m.group(1))
                record["do_collate_s"] = float(m.group(2))
                record["total_s"] = float(m.group(3))
    # A seqno can be collated more than once (restarts); the log merge keeps
    # the union, which is acceptable for pressure statistics.
    return [blocks[s] for s in sorted(blocks)]


def summarize(rate: float, records: list[dict[str, object]]) -> dict[str, object]:
    with_load = [r for r in records if "gas" in r]
    overloaded = [r for r in records if "overloaded" in r]
    reasons: dict[str, int] = {}
    for r in overloaded:
        reasons[str(r["overloaded"])] = reasons.get(str(r["overloaded"]), 0) + 1
    queues = [int(r["queue"]) for r in records if "queue" in r]
    gases = [int(r["gas"]) for r in with_load]
    sizes = [int(r["size_est"]) for r in with_load]
    want_split = [r for r in records if "want_split_history" in r]
    first_split = min((int(r["seqno"]) for r in want_split), default=None)
    return {
        "rate": rate,
        "blocks": len(records),
        "blocks_with_load_stats": len(with_load),
        "overloaded_blocks": len(overloaded),
        "overload_share": round(len(overloaded) / len(records), 4) if records else None,
        "overload_reasons": reasons,
        "force_split_bits": sum(1 for r in records if "force_split_queue" in r),
        "want_split_blocks": len(want_split),
        "first_want_split_seqno": first_split,
        "want_merge_blocks": sum(1 for r in records if "want_merge" in r),
        "queue_max": max(queues, default=None),
        "queue_median": statistics.median(queues) if queues else None,
        "gas_median": statistics.median(gases) if gases else None,
        "gas_max": max(gases, default=None),
        "size_est_median": statistics.median(sizes) if sizes else None,
        "size_est_max": max(sizes, default=None),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--rates", default="300,600,900,1200,1500")
    parser.add_argument("--duration", type=int, default=45)
    parser.add_argument("--warmup", type=int, default=10)
    args = parser.parse_args(argv)

    rates = [float(r) for r in args.rates.split(",") if r]
    args.work_dir.mkdir(parents=True, exist_ok=True)
    curve: list[dict[str, object]] = []
    for rate in rates:
        tag = f"r{int(rate)}"
        net_dir = args.work_dir / f"net-{tag}"
        out_dir = args.work_dir / f"out-{tag}"
        print(f"=== rate {rate} ===", flush=True)
        run = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("bench_jetton.py")),
                "--manifest", str(args.manifest),
                "--build-dir", str(args.build_dir),
                "--net-dir", str(net_dir),
                "--out-dir", str(out_dir),
                "--rate", str(rate),
                "--duration", str(args.duration),
                "--warmup", str(args.warmup),
                "--node-verbosity", "3",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        node_log = net_dir / "node1" / "log"
        if not node_log.exists():
            print(f"rate {rate}: bench failed (exit {run.returncode}, no node log)", flush=True)
            continue
        records = parse_node_log(node_log)
        summary = summarize(rate, records)
        summary["bench_exit"] = run.returncode
        curve.append(summary)
        (args.work_dir / f"blocks-{tag}.json").write_text(json.dumps(records, indent=1) + "\n")
        print(json.dumps(summary), flush=True)

    (args.work_dir / "overload-curve.json").write_text(json.dumps(curve, indent=2) + "\n")
    print(f"curve written to {args.work_dir / 'overload-curve.json'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
