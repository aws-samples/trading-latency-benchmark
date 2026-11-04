#!/usr/bin/env python3
"""Generate a markdown benchmark report from orchestrator output.

Phase A is the multicast one-way latency (publisher user-space send time to the
subscriber NIC's hardware receive stamp). Phase B is the unicast sockperf RTT, shown next to the multicast result as a floor.
Parsing and validity rules live in results.py, shared with collect_results.py.

Usage:
    python3 -m orchestrator run --stack-name MyStack | python3 report/save_report.py
    python3 report/save_report.py --input raw.txt [--detail-dir DIR] [--notes "..."]

A run that fails a gate (loss, clock, rate, missing subscriber) is still written, with
the failures at the top, so a bad run is visible instead of silently absent. The exit
status is 3 for such a run.
"""
from __future__ import annotations

import argparse
import os
import re
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import results as tr  # noqa: E402

RUNS_DIR = Path(__file__).resolve().parents[1] / "runs"
EXIT_INVALID_RUN = 3


def parse_header(raw: str) -> dict:
    """Run configuration from the header block (see results.parse_header)."""
    cfg = tr.parse_header(raw)
    if "rate" in cfg:
        cfg["send_rate_pps"] = cfg["rate"]
    if "duration" in cfg:
        cfg["duration_secs"] = cfg["duration"]
    if "publisher" in cfg:
        cfg["publisher_id"] = cfg["publisher"]
    if "subscribers" in cfg:
        cfg["subscriber_ids"] = cfg["subscribers"]
        cfg["num_subscribers"] = len(cfg["subscribers"])
    if "tools" in cfg:
        cfg["tool"] = cfg["tools"]
    return cfg


def parse_phase_b(raw: str) -> list[dict]:
    """Phase B sockperf unicast RTT results per subscriber (via results.parse_sockperf)."""
    results = []
    pattern = re.compile(r"--- SUBSCRIBER (\S+) ---\n(.*?)--- END \1 ---", re.DOTALL)
    for m in pattern.finditer(raw):
        sub_id, block = m.group(1), m.group(2)
        entry: dict = {"subscriber_id": sub_id}
        p = tr.parse_sockperf(block)
        if "avg_rtt_us" not in p:
            entry["error"] = block.strip().split("\n")[0] if block.strip() else "no sockperf output"
        else:
            entry.update(p)
        results.append(entry)
    return results


def _f(x: Any, nd: int = 1) -> str:
    return "n/a" if x is None else f"{float(x):.{nd}f}"


def _phase_a_section(raw: str, subs: List[tr.Subscriber], findings: List[tr.Finding],
                     summary: Dict[str, Any]) -> List[str]:
    L: List[str] = []
    sender = summary.get("sender")
    totals = tr.loss_totals(subs, sender)
    cfg = summary["config"]

    L.append("## Phase A - Multicast one-way latency")
    L.append("")
    L.append("Latency = subscriber NIC hardware receive stamp minus publisher user-space send time "
             "(CLOCK_REALTIME). It includes the publisher's send path and the clock error between "
             "the two hosts, so it is an upper bound on the transit time through the Transit Gateway.")
    L.append("")
    fails = [f for f in findings if f.level == "FAIL"]
    L.append(f"**Run status: {'INVALID' if fails else 'VALID'}**")
    L.append("")
    for f in findings:
        L.append(f"- {f.level} `{f.code}`: {f.message}")
    if findings:
        L.append("")

    if sender:
        parts = [f"sent {sender.get('sent', 'n/a')}"]
        if sender.get("achieved_pct") is not None:
            parts.append(f"{float(sender['achieved_pct']):.1f}% of the requested rate")
        if sender.get("late_max_us") is not None:
            parts.append(f"worst send lateness {sender['late_max_us']} us")
        L.append("- Sender: " + ", ".join(parts))
    if sender and sender.get("sendto_median_us") is not None:
        L.append(f"- Sender send-path cost (time inside sendto after the stamp): median {sender['sendto_median_us']} us, "
                 f"p99 {sender['sendto_p99_us']} us, max {sender['sendto_max_us']} us")
    if cfg.get("native_build"):
        L.append(f"- Native build: {cfg['native_build']}")
    L.append(f"- Packet loss, all subscribers (a failed subscriber counts as total loss): "
             f"{totals['lost']}/{totals['expected']} ({totals['pct']:.4f}%)")
    if totals["unmeasured"]:
        L.append(f"- {totals['unmeasured']} failed subscriber(s) are not in that figure (sender count unknown)")
    L.append("")

    for group, members in sorted(tr.group_subscribers(subs).items()):
        agg = tr.aggregate_group(members)
        L.append(f"### Path: {group} ({agg['subscribers_ok']}/{agg['subscribers']} subscribers reporting)")
        L.append("")
        L.append("| Subscriber | AZ | Min µs | Median µs | Mean µs | P95 µs | P99 µs | Max µs | Lost | Dups | Rx-buf drops | Neg |")
        L.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for s in members:
            sid = f"`..{s.subscriber_id[-12:]}`"
            if not s.ok:
                L.append(f"| {sid} | {s.az or '?'} | **FAILED**: {s.error} |||||||||")
                continue
            r = s.result
            neg = r.get("negative_hw_samples", 1 if float(r.get("min_latency_us", 0)) < 0 else 0)
            L.append(f"| {sid} | {s.az or '?'} | {_f(r.get('min_latency_us'))} | {_f(r.get('median_latency_us'))} "
                     f"| {_f(r.get('mean_latency_us'))} | {_f(r.get('p95_latency_us'))} | {_f(r.get('p99_latency_us'))} "
                     f"| {_f(r.get('max_latency_us'), 0)} | {r.get('packet_loss_count', 0)} "
                     f"| {r.get('duplicate_count', 0)} | {r.get('socket_drops', 0)} | {neg} |")
        L.append("")
        if not agg["subscribers_ok"]:
            continue
        if agg.get("warmup_secs"):
            L.append(f"Latency statistics exclude the first {agg['warmup_secs']:g} s of each receiver "
                     f"({agg['warmup_skipped']} packets); loss counts include them.")
            L.append("")
        L.append(f"**Aggregate, {group}**")
        L.append("")
        L.append("| Metric | Value |")
        L.append("|---|---|")
        if agg.get("pooled"):
            L.append(f"| Median (pooled over {agg['pooled_samples']} samples) | {_f(agg['median_us'])} µs |")
            L.append(f"| P95 (pooled) | {_f(agg['p95_us'])} µs |")
            L.append(f"| P99 (pooled) | {_f(agg['p99_us'])} µs |")
            if "p999_us" in agg:
                L.append(f"| P99.9 (pooled) | {_f(agg['p999_us'])} µs |")
            else:
                L.append(f"| P99.9 | not reported ({agg.get('p999_omitted', 'too few samples')}) |")
        else:
            L.append("| Pooled percentiles | not available (a subscriber's histogram is missing; pass --detail-dir) |")
            L.append(f"| Worst-subscriber P99 | {_f(agg.get('worst_subscriber_p99_us'))} µs |")
        if agg.get("mean_us") is not None:
            L.append(f"| Mean (sample-weighted) | {_f(agg['mean_us'])} µs |")
        L.append(f"| Min / Max | {_f(agg.get('min_us'))} / {_f(agg.get('max_us'), 0)} µs |")
        L.append(f"| Lost / expected | {agg['lost']}/{agg['expected']} |")
        if agg.get("div_median_us") is not None:
            L.append(f"| Software minus hardware stamp, median / P99 | {_f(agg['div_median_us'], 2)} / {_f(agg['div_p99_us'], 2)} µs |")
        L.append("")

    clocks = tr.parse_clock(raw)
    if clocks:
        L.append("### Clock state (chrony System time, µs)")
        L.append("")
        L.append("| Instance | Before | After |")
        L.append("|---|---|---|")
        for iid, ph in sorted(clocks.items()):
            L.append(f"| `..{iid[-12:]}` | {ph.get('pre', {}).get('system_time_us', 'n/a')} "
                     f"| {ph.get('post', {}).get('system_time_us', 'n/a')} |")
        L.append("")
    L.extend(_context_section(raw))
    return L


def _context_section(raw: str) -> List[str]:
    ctx = tr.parse_context(raw)
    if not ctx:
        return []
    common, differing = tr.context_summary(ctx)
    L = ["### Run context", "",
         "State in effect during the measurement (identical on every instance unless listed below).", "",
         "| Setting | Value |", "|---|---|"]
    for k, v in common.items():
        L.append(f"| {k} | {v or 'n/a'} |")
    L.append("")
    if differing:
        L.append("Differs between instances:")
        L.append("")
        L.append("| Setting | Instance | Value |")
        L.append("|---|---|---|")
        for k, vals in differing.items():
            for iid, v in sorted(vals.items()):
                L.append(f"| {k} | `..{iid[-12:]}` | {v or 'n/a'} |")
        L.append("")
    return L


def _floor_section(phase_a: list, valid_b: list) -> list[str]:
    """Multicast median against the unicast floor, per subscriber that has both measurements."""
    rtt = {r["subscriber_id"]: r["p50_us"] for r in valid_b if "p50_us" in r}
    rows = []
    for sub in phase_a:
        med = (sub.result or {}).get("median_latency_us") if sub.ok else None
        if med is not None and sub.subscriber_id in rtt:
            rows.append((sub, float(med), rtt[sub.subscriber_id] / 2))
    if not rows:
        return []
    lines = ["## Multicast against the unicast floor", "",
             "| Subscriber | Path | Multicast median µs | Unicast RTT/2 µs | Difference µs |",
             "|---|---|---|---|---|"]
    for sub, med, floor in rows:
        lines.append(f"| `..{sub.subscriber_id[-12:]}` | {sub.az_relation or '?'} | {med:.1f} | {floor:.1f} "
                     f"| {med - floor:+.1f} |")
    diffs = [m - f for _, m, f in rows]
    lines += ["", f"Median difference across subscribers: {statistics.median(diffs):+.1f} µs.", "",
              "The unicast figure is half the median sockperf round trip between the same hosts, with no "
              "gateway in the path. It assumes a symmetric path and uses software timestamps, while the "
              "multicast median uses a hardware receive stamp and includes the publisher's send path. Read "
              "the difference as an upper bound on what the gateway path adds, not as a measurement of it. "
              "Each figure comes from one flow, and latency differs between flows (see the README, "
              "\"Latency depends on the flow\"). A negative difference points to clock error or path "
              "asymmetry.", ""]
    return lines


def generate_report(
    config: dict,
    phase_a: list,
    phase_b: list[dict],
    instance_type: str,
    placement: str,
    notes: str,
    timestamp: datetime,
    raw: str = "",
    findings: Optional[List[tr.Finding]] = None,
    summary: Optional[Dict[str, Any]] = None,
) -> str:
    """Generate the markdown report string. `phase_a` is a list of tr.Subscriber."""
    lines: list[str] = []
    lines.append("# TGW Multicast Benchmark Report")
    lines.append("")
    lines.append("## Configuration")
    lines.append("")
    lines.append("| Parameter | Value |")
    lines.append("|-----------|-------|")
    lines.append(f"| Date | {timestamp.strftime('%Y-%m-%d %H:%M:%S UTC')} |")
    lines.append(f"| Instance Type | {instance_type} |")
    lines.append(f"| Placement Strategy | {placement} |")
    lines.append(f"| Num Subscribers | {config.get('num_subscribers', 'N/A')} |")
    lines.append(f"| Send Rate | {config.get('send_rate_pps', 'N/A')} pps |")
    lines.append(f"| Duration | {config.get('duration_secs', 'N/A')}s |")
    if config.get("payload_bytes"):
        lines.append(f"| Payload | {config['payload_bytes']} bytes |")
    lines.append(f"| Multicast Group | {config.get('multicast_group', 'N/A')}:{config.get('port', 'N/A')} |")
    lines.append(f"| Tools | {config.get('tool', 'N/A')} |")
    lines.append(f"| Stack | {config.get('stack_name', 'N/A')} |")
    lines.append(f"| Run ID | {config.get('run_id', 'N/A')} |")
    lines.append(f"| Publisher | {config.get('publisher_id', 'N/A')} |")
    if notes:
        lines.append(f"| Notes | {notes} |")
    lines.append("")

    if phase_a:
        if findings is None or summary is None:
            findings, summary = tr.evaluate(raw)
        lines.extend(_phase_a_section(raw, phase_a, findings, summary))

    valid_b = [r for r in phase_b if "avg_rtt_us" in r]
    failed_b = [r for r in phase_b if "error" in r]

    if phase_b:
        lines.append("## Phase B - Unicast RTT (sockperf under-load)")
        lines.append("")
        if failed_b:
            lines.append(f"**{len(failed_b)} subscriber(s) failed** (sockperf not available or no response).")
            lines.append("")
        if valid_b:
            lines.append("| Subscriber | Avg RTT µs | P50 µs | P90 µs | P99 µs | Min µs | Max µs |")
            lines.append("|------------|-----------|--------|--------|--------|--------|--------|")
            for r in valid_b:
                sub = r["subscriber_id"][-12:]
                lines.append(
                    f"| `..{sub}` | {r.get('avg_rtt_us', 0):.1f} | {r.get('p50_us', 0):.1f} "
                    f"| {r.get('p90_us', 0):.1f} | {r.get('p99_us', 0):.1f} "
                    f"| {r.get('min_us', 0):.1f} | {r.get('max_us', 0):.1f} |"
                )
            lines.append("")
            rtts = [r["avg_rtt_us"] for r in valid_b]
            p50s = [r["p50_us"] for r in valid_b if "p50_us" in r]
            lines.append("**Aggregate (equal weight per subscriber)**")
            lines.append("")
            lines.append("| Metric | Value |")
            lines.append("|--------|-------|")
            lines.append(f"| Mean of per-subscriber average RTTs | {statistics.mean(rtts):.1f} µs |")
            if p50s:
                lines.append(f"| Median of per-subscriber P50 RTTs | {statistics.median(p50s):.1f} µs |")
                lines.append(f"| P50 RTT / 2 (assumes a symmetric path) | {statistics.median(p50s)/2:.1f} µs |")
            lines.append("")
            lines.extend(_floor_section(phase_a, valid_b))
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Save benchmark results as markdown report.")
    parser.add_argument("--input", "-i", type=str, default=None, help="Input file (default: stdin)")
    parser.add_argument("--detail-dir", type=str, default=None,
                        help="Directory of <instance>.detail.json files (histograms) for pooled percentiles")
    parser.add_argument("--instance-type", type=str, default=None, help="Override instance type")
    parser.add_argument("--placement", type=str, default="single-az-cpg", help="Placement strategy")
    parser.add_argument("--notes", type=str, default="", help="Additional notes for the report header")
    parser.add_argument("--output-dir", type=str, default=None, help="Output directory (default: runs/)")
    parser.add_argument("--filename", type=str, default=None, help="Output filename (auto-generated if omitted)")
    parser.add_argument("--max-loss-pct", type=float, default=tr.DEFAULT_MAX_LOSS_PCT)
    parser.add_argument("--max-clock-offset-us", type=float, default=tr.DEFAULT_MAX_CLOCK_OFFSET_US)
    args = parser.parse_args()

    raw = open(args.input).read() if args.input else sys.stdin.read()

    config = parse_header(raw)
    subs = tr.parse_subscribers(raw)
    if args.detail_dir:
        n = tr.load_details(subs, args.detail_dir)
        print(f"Loaded {n} detail file(s) from {args.detail_dir}")
    phase_b = parse_phase_b(raw)

    if not subs and not phase_b:
        print("ERROR: No Phase A or Phase B results found in input.", file=sys.stderr)
        sys.exit(1)

    findings, summary = tr.evaluate(raw, max_loss_pct=args.max_loss_pct,
                                    max_clock_offset_us=args.max_clock_offset_us)
    # evaluate() parsed the subscribers itself; use the detail-bearing copies for aggregation
    summary["groups"] = {g: tr.aggregate_group(v) for g, v in tr.group_subscribers(subs).items()}

    instance_type = args.instance_type or config.get("instance_type") or "unknown"
    timestamp = datetime.now(timezone.utc)

    report = generate_report(config=config, phase_a=subs, phase_b=phase_b,
                             instance_type=instance_type, placement=args.placement,
                             notes=args.notes, timestamp=timestamp, raw=raw,
                             findings=findings, summary=summary)

    out_dir = Path(args.output_dir) if args.output_dir else RUNS_DIR
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.filename:
        filename = args.filename
    else:
        n_subs = config.get("num_subscribers", "X")
        date_str = timestamp.strftime("%Y%m%dT%H%M%S")
        filename = f"N{n_subs}_{instance_type}_{date_str}.md"

    out_path = out_dir / filename
    out_path.write_text(report)
    print(f"Report saved: {out_path}")

    raw_path = out_path.with_suffix(".raw.txt")
    raw_path.write_text(raw)
    print(f"Raw output:  {raw_path}")

    if any(f.level == "FAIL" for f in findings):
        print(tr.verdict_text(findings), file=sys.stderr)
        sys.exit(EXIT_INVALID_RUN)


if __name__ == "__main__":
    main()
