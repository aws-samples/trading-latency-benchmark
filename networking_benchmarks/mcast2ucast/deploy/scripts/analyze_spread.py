#!/usr/bin/env python3
"""
analyze_spread.py — Cross-receiver fan-out spread analysis.

Reads H*D CSV files (each emitted by latency_receiver --csv-out) of the form
'seq,rx_ns', joins on seq, computes per-packet spread = max(rx) - min(rx)
across all receivers, then emits JSON stats to stdout.

A packet is included in the spread distribution ONLY if every receiver got
it (full-fanout). Partial packets are tallied separately under per-receiver
loss counts.

Usage:
    analyze_spread.py <topology.json> <run_dir>

<topology.json> tells us the expected (host_index, port) tuples and how
many packets each receiver should have seen.
<run_dir> is the per-run results dir containing recv-h<i>-p<port>.csv
files scp'd by cmd_collect.

Output (to stdout, single JSON object):
{
  "num_subscriber_hosts": H,
  "num_subscriber_daemons_per_host": D,
  "total_subscribers": H*D,
  "fanout_spread_us": {
    "samples": <int>,    # count of seq with full fanout
    "mean": <float>,
    "std_dev": <float>,
    "p50": <float>,
    "p90": <float>,
    "p99": <float>,
    "max": <float>
  },
  "per_subscriber_loss": [
    {"host_index": 0, "port": 5001, "received": <int>, "lost": <int>},
    ...
  ]
}
"""

import csv
import json
import math
import sys
from pathlib import Path


def percentile(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = int((len(sorted_vals) - 1) * p / 100.0)
    return sorted_vals[idx]


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: analyze_spread.py <topology.json> <run_dir>\n")
        return 2

    topology_path = Path(sys.argv[1])
    run_dir = Path(sys.argv[2])

    with open(topology_path) as f:
        topo = json.load(f)
    H = topo["num_subscriber_hosts"]
    D = topo["num_subscriber_daemons_per_host"]

    # Map (host_index, port) -> {seq: rx_ns}
    per_recv: dict[tuple[int, int], dict[int, int]] = {}
    per_recv_received: dict[tuple[int, int], int] = {}

    for host_idx, host in enumerate(topo["subscriber_hosts"]):
        for port in host["daemon_ports"]:
            csv_path = run_dir / f"recv-h{host_idx}-p{port}.csv"
            d: dict[int, int] = {}
            if csv_path.exists():
                with open(csv_path) as f:
                    reader = csv.reader(f)
                    for row in reader:
                        if len(row) != 2:
                            continue
                        try:
                            seq = int(row[0]); rx_ns = int(row[1])
                        except ValueError:
                            continue
                        d[seq] = rx_ns
            per_recv[(host_idx, port)] = d
            per_recv_received[(host_idx, port)] = len(d)

    expected_count = max(per_recv_received.values()) if per_recv_received else 0
    # Find the union of seqs received by ANY receiver, then keep only those
    # received by ALL receivers.
    all_seqs: set[int] = set()
    for d in per_recv.values():
        all_seqs.update(d.keys())

    full_fanout_seqs = []
    for seq in all_seqs:
        if all(seq in d for d in per_recv.values()):
            full_fanout_seqs.append(seq)
    full_fanout_seqs.sort()

    spreads_ns: list[int] = []
    for seq in full_fanout_seqs:
        rxs = [d[seq] for d in per_recv.values()]
        spreads_ns.append(max(rxs) - min(rxs))

    # Convert to microseconds
    spreads_us = [s / 1000.0 for s in spreads_ns]
    spreads_us.sort()

    if spreads_us:
        mean = sum(spreads_us) / len(spreads_us)
        # std dev
        var = sum((s - mean) ** 2 for s in spreads_us) / len(spreads_us)
        std_dev = math.sqrt(var)
        p50 = percentile(spreads_us, 50)
        p90 = percentile(spreads_us, 90)
        p99 = percentile(spreads_us, 99)
        max_val = spreads_us[-1]
    else:
        mean = p50 = p90 = p99 = max_val = std_dev = 0.0

    per_sub_loss = []
    for host_idx, host in enumerate(topo["subscriber_hosts"]):
        for port in host["daemon_ports"]:
            received = per_recv_received[(host_idx, port)]
            lost = max(0, expected_count - received)
            per_sub_loss.append({
                "host_index": host_idx,
                "port": port,
                "received": received,
                "lost": lost,
            })

    out = {
        "num_subscriber_hosts": H,
        "num_subscriber_daemons_per_host": D,
        "total_subscribers": H * D,
        "fanout_spread_us": {
            "samples": len(spreads_us),
            "mean": round(mean, 2),
            "std_dev": round(std_dev, 2),
            "p50": round(p50, 2),
            "p90": round(p90, 2),
            "p99": round(p99, 2),
            "max": round(max_val, 2),
        },
        "per_subscriber_loss": per_sub_loss,
    }
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
