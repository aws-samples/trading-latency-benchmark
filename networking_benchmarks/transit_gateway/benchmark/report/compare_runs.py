#!/usr/bin/env python3
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
"""Summarise repeated runs and compare labelled groups of them.

One run's percentiles describe that run. Whether two configurations differ is a question
about run-to-run spread, so this tool works on whole runs: each raw file contributes one
value per statistic, and the interval is a bootstrap over runs.

Usage:
    compare_runs.py --run native:runs/a.raw.txt --run native:runs/b.raw.txt \\
                    --run python:runs/c.raw.txt --run python:runs/d.raw.txt

Histogram files are looked up in `<raw dir>/<Run-ID>.details/` (what --save-report writes);
without them a run's median is the mean of its subscribers' medians. Runs that fail a
validity gate are listed and left out. A bootstrap interval needs 5+ runs per group; with fewer, the
per-run values and their min-max spread are the result.
"""
from __future__ import annotations

import argparse
import os
import random
import statistics
import sys
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import results as tr  # noqa: E402

# A bootstrap over fewer runs just reproduces the observed min-max; the spread is shown regardless.
MIN_RUNS_FOR_INTERVAL = 5
BOOTSTRAP_ITERATIONS = 10000
STATS = (("median_us", "median"), ("p95_us", "p95"), ("p99_us", "p99"))


def load_run(path: str) -> Tuple[Dict[str, Optional[float]], List[tr.Finding], Dict[str, object]]:
    """(statistics of one run, its validity findings, its header)."""
    raw = open(path).read()
    cfg = tr.parse_header(raw)
    subs = tr.parse_subscribers(raw)
    run_id = cfg.get("run_id")
    det = os.path.join(os.path.dirname(os.path.abspath(path)), f"{run_id}.details") if run_id else ""
    if det and os.path.isdir(det):
        tr.load_details(subs, det)
    findings, _ = tr.evaluate(raw)
    ok = [s for s in subs if s.ok]
    agg = tr.aggregate_group(ok) if ok else {}
    out: Dict[str, Optional[float]] = {}
    if agg.get("pooled"):
        for key, _ in STATS:
            out[key] = agg.get(key)
    elif ok:
        # no histograms: the mean of the subscribers' own values, which is not a pooled percentile
        for key, field in (("median_us", "median_latency_us"), ("p95_us", "p95_latency_us"), ("p99_us", "p99_latency_us")):
            vals = [float(s.result[field]) for s in ok if field in s.result]
            out[key] = statistics.mean(vals) if vals else None
    out["pooled"] = 1.0 if agg.get("pooled") else 0.0
    return out, findings, cfg


def bootstrap_mean_ci(values: List[float], rng: random.Random, level: float = 0.95) -> Tuple[float, float]:
    means = []
    n = len(values)
    for _ in range(BOOTSTRAP_ITERATIONS):
        means.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    means.sort()
    lo = means[int((1 - level) / 2 * BOOTSTRAP_ITERATIONS)]
    hi = means[int((1 + level) / 2 * BOOTSTRAP_ITERATIONS) - 1]
    return lo, hi


def bootstrap_diff_ci(a: List[float], b: List[float], rng: random.Random, level: float = 0.95) -> Tuple[float, float]:
    diffs = []
    for _ in range(BOOTSTRAP_ITERATIONS):
        ma = sum(a[rng.randrange(len(a))] for _ in range(len(a))) / len(a)
        mb = sum(b[rng.randrange(len(b))] for _ in range(len(b))) / len(b)
        diffs.append(ma - mb)
    diffs.sort()
    return diffs[int((1 - level) / 2 * BOOTSTRAP_ITERATIONS)], diffs[int((1 + level) / 2 * BOOTSTRAP_ITERATIONS) - 1]


def summarise(groups: Dict[str, List[Dict[str, Optional[float]]]], skipped: Dict[str, List[str]], seed: int = 1) -> str:
    rng = random.Random(seed)
    L: List[str] = ["# Run comparison", ""]
    for label, runs in groups.items():
        L.append(f"## {label} ({len(runs)} valid run(s)" + (f", {len(skipped.get(label, []))} excluded" if skipped.get(label) else "") + ")")
        L.append("")
        for msg in skipped.get(label, []):
            L.append(f"- excluded: {msg}")
        if skipped.get(label):
            L.append("")
        if any(not r.get("pooled") for r in runs):
            L.append("Some runs have no histogram file: their values are means of per-subscriber percentiles.")
            L.append("")
        L.append("| Statistic | Per-run values (µs) | Mean | Min-Max | Std dev | 95% interval of the mean |")
        L.append("|---|---|---|---|---|---|")
        for key, name in STATS:
            vals = [r[key] for r in runs if r.get(key) is not None]
            if not vals:
                continue
            sd = f"{statistics.stdev(vals):.1f}" if len(vals) > 1 else "n/a"
            if len(vals) >= MIN_RUNS_FOR_INTERVAL:
                lo, hi = bootstrap_mean_ci(vals, rng)
                ci = f"{lo:.1f} to {hi:.1f}"
            else:
                ci = f"n/a (need {MIN_RUNS_FOR_INTERVAL}+ runs)"
            L.append(f"| {name} | {', '.join(f'{v:.1f}' for v in vals)} | {statistics.mean(vals):.1f} "
                     f"| {min(vals):.1f}-{max(vals):.1f} | {sd} | {ci} |")
        L.append("")
    labels = list(groups)
    if len(labels) == 2:
        a, b = labels
        va = [r["median_us"] for r in groups[a] if r.get("median_us") is not None]
        vb = [r["median_us"] for r in groups[b] if r.get("median_us") is not None]
        L.append(f"## {a} minus {b}, median latency")
        L.append("")
        if len(va) >= MIN_RUNS_FOR_INTERVAL and len(vb) >= MIN_RUNS_FOR_INTERVAL:
            lo, hi = bootstrap_diff_ci(va, vb, rng)
            d = statistics.mean(va) - statistics.mean(vb)
            verdict = "the interval excludes 0" if (lo > 0 or hi < 0) else "the interval includes 0: no difference is shown"
            L.append(f"Difference of the mean run medians: {d:+.1f} µs, 95% interval {lo:+.1f} to {hi:+.1f} µs; {verdict}.")
        else:
            L.append(f"Not enough runs for an interval (need {MIN_RUNS_FOR_INTERVAL}+ valid runs in each group).")
        L.append("")
    return "\n".join(L)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", action="append", required=True, metavar="LABEL:RAW_FILE",
                    help="a raw run file and the group it belongs to (repeatable)")
    ap.add_argument("--seed", type=int, default=1, help="bootstrap seed (default 1; the result is reproducible)")
    args = ap.parse_args(argv)

    groups: Dict[str, List[Dict[str, Optional[float]]]] = {}
    skipped: Dict[str, List[str]] = {}
    for spec in args.run:
        label, _, path = spec.partition(":")
        if not path:
            ap.error(f"--run expects LABEL:RAW_FILE, got '{spec}'")
        stats, findings, _cfg = load_run(path)
        fails = [f for f in findings if f.level == "FAIL"]
        groups.setdefault(label, [])
        if fails or not any(k in stats for k, _ in STATS):
            skipped.setdefault(label, []).append(f"{os.path.basename(path)}: " + ("; ".join(f.code for f in fails) or "no results"))
            continue
        groups[label].append(stats)
    print(summarise(groups, skipped, args.seed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
