#!/usr/bin/env python3
"""Generate a markdown benchmark report from run_benchmark.sh output.

Parses Phase A (multicast one-way latency with HW timestamps) and Phase B
(unicast sockperf RTT) results, then writes a structured markdown report to
the runs/ directory.

Usage:
    # Pipe directly from run_benchmark.sh
    scripts/run_benchmark.sh --stack-name MyStack | python3 scripts/save_report.py

    # Or from a saved raw output file
    python3 scripts/save_report.py --input /tmp/benchmark_output.txt

    # Override auto-detected config
    python3 scripts/save_report.py --input raw.txt --instance-type m7i.4xlarge --notes "Scaling test"
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path


RUNS_DIR = Path(__file__).resolve().parents[1] / "runs"


def parse_header(raw: str) -> dict:
    """Extract configuration from the benchmark header block."""
    config = {}
    m = re.search(r"Stack:\s+(\S+)", raw)
    if m:
        config["stack_name"] = m.group(1)
    m = re.search(r"Tool:\s+(\S+)", raw)
    if m:
        config["tool"] = m.group(1)
    m = re.search(r"Group:\s+([\d.]+)", raw)
    if m:
        config["multicast_group"] = m.group(1)
    m = re.search(r"Port:\s+(\d+)", raw)
    if m:
        config["port"] = int(m.group(1))
    m = re.search(r"Rate:\s+(\d+)", raw)
    if m:
        config["send_rate_pps"] = int(m.group(1))
    m = re.search(r"Duration:\s+(\d+)s", raw)
    if m:
        config["duration_secs"] = int(m.group(1))
    m = re.search(r"Publisher:\s+(\S+)", raw)
    if m:
        config["publisher_id"] = m.group(1)
    m = re.search(r"Subscribers:\s+(.+)", raw)
    if m:
        config["subscriber_ids"] = m.group(1).strip().split()
        config["num_subscribers"] = len(config["subscriber_ids"])
    return config


def parse_phase_a(raw: str) -> list[dict]:
    """Parse Phase A multicast JSON results per subscriber."""
    results = []
    pattern = re.compile(r"(i-\S+) multicast loss: (\{.*?\})")
    for m in pattern.finditer(raw):
        sub_id = m.group(1)
        try:
            data = json.loads(m.group(2))
            data["subscriber_id"] = sub_id
            results.append(data)
        except json.JSONDecodeError:
            pass
    return results


def parse_phase_b(raw: str) -> list[dict]:
    """Parse Phase B sockperf unicast RTT results per subscriber."""
    results = []
    pattern = re.compile(
        r"--- SUBSCRIBER (\S+) ---\n(.*?)--- END \1 ---", re.DOTALL
    )
    for m in pattern.finditer(raw):
        sub_id = m.group(1)
        block = m.group(2)
        entry: dict = {"subscriber_id": sub_id}

        rtt_m = re.search(r"avg-rtt=([\d.]+)", block)
        if rtt_m:
            entry["avg_rtt_us"] = float(rtt_m.group(1))
        else:
            if "command not found" in block or "No messages" in block:
                entry["error"] = block.strip().split("\n")[0]
            results.append(entry)
            continue

        for pct_m in re.finditer(r"percentile\s+([\d.]+)\s*=\s*([\d.]+)", block):
            pct = float(pct_m.group(1))
            val = float(pct_m.group(2))
            if pct == 50.0:
                entry["p50_us"] = val
            elif pct == 90.0:
                entry["p90_us"] = val
            elif pct == 99.0:
                entry["p99_us"] = val

        min_m = re.search(r"<MIN> observation\s*=\s*([\d.]+)", block)
        if min_m:
            entry["min_us"] = float(min_m.group(1))
        max_m = re.search(r"<MAX> observation\s*=\s*([\d.]+)", block)
        if max_m:
            entry["max_us"] = float(max_m.group(1))

        results.append(entry)
    return results


def generate_report(
    config: dict,
    phase_a: list[dict],
    phase_b: list[dict],
    instance_type: str,
    placement: str,
    notes: str,
    timestamp: datetime,
) -> str:
    """Generate the markdown report string."""
    lines: list[str] = []

    lines.append(f"# TGW Multicast Benchmark Report")
    lines.append("")
    lines.append("## Configuration")
    lines.append("")
    lines.append(f"| Parameter | Value |")
    lines.append(f"|-----------|-------|")
    lines.append(f"| Date | {timestamp.strftime('%Y-%m-%d %H:%M:%S UTC')} |")
    lines.append(f"| Instance Type | {instance_type} |")
    lines.append(f"| Placement Strategy | {placement} |")
    lines.append(f"| Num Subscribers | {config.get('num_subscribers', 'N/A')} |")
    lines.append(f"| Send Rate | {config.get('send_rate_pps', 'N/A')} pps |")
    lines.append(f"| Duration | {config.get('duration_secs', 'N/A')}s |")
    lines.append(f"| Multicast Group | {config.get('multicast_group', 'N/A')}:{config.get('port', 'N/A')} |")
    lines.append(f"| Tool | {config.get('tool', 'N/A')} |")
    lines.append(f"| Stack | {config.get('stack_name', 'N/A')} |")
    lines.append(f"| Publisher | {config.get('publisher_id', 'N/A')} |")
    if notes:
        lines.append(f"| Notes | {notes} |")
    lines.append("")

    # Phase A
    if phase_a:
        lines.append("## Phase A — Multicast One-Way Latency (NIC HW RX Timestamps)")
        lines.append("")

        all_hw = all(r.get("hw_timestamp_count", 0) > 0 for r in phase_a)
        total_loss = sum(r.get("packet_loss_count", 0) for r in phase_a)
        total_sent = sum(r.get("total_expected", 0) for r in phase_a)
        ts_variant = phase_a[0].get("so_timestamping_variant", "unknown")

        lines.append(f"- Timestamp source: `{ts_variant}` — {'all HW' if all_hw else 'MIXED (check below)'}")
        lines.append(f"- Total packet loss: {total_loss}/{total_sent} ({total_loss/max(total_sent,1)*100:.4f}%)")
        lines.append("")

        lines.append("### Per-Subscriber Results")
        lines.append("")
        lines.append("| Subscriber | Min µs | Median µs | Mean µs | P95 µs | P99 µs | Max µs | Loss | HW Count |")
        lines.append("|------------|--------|-----------|---------|--------|--------|--------|------|----------|")
        for r in phase_a:
            sub = r.get("subscriber_id", "?")[-12:]
            lines.append(
                f"| `..{sub}` "
                f"| {r.get('min_latency_us', 0):.1f} "
                f"| {r.get('median_latency_us', 0):.1f} "
                f"| {r.get('mean_latency_us', 0):.1f} "
                f"| {r.get('p95_latency_us', 0):.1f} "
                f"| {r.get('p99_latency_us', 0):.1f} "
                f"| {r.get('max_latency_us', 0):.0f} "
                f"| {r.get('packet_loss_count', 0)} "
                f"| {r.get('hw_timestamp_count', 0)} |"
            )
        lines.append("")

        # Aggregate
        means = [r["mean_latency_us"] for r in phase_a if "mean_latency_us" in r]
        medians = [r["median_latency_us"] for r in phase_a if "median_latency_us" in r]
        p99s = [r["p99_latency_us"] for r in phase_a if "p99_latency_us" in r]
        mins = [r["min_latency_us"] for r in phase_a if "min_latency_us" in r]
        maxs = [r["max_latency_us"] for r in phase_a if "max_latency_us" in r]

        lines.append("### Aggregate")
        lines.append("")
        lines.append("| Metric | Value |")
        lines.append("|--------|-------|")
        if medians:
            lines.append(f"| Median (of medians) | {statistics.median(medians):.1f} µs |")
        if means:
            lines.append(f"| Mean (of means) | {statistics.mean(means):.1f} µs |")
        if p99s:
            lines.append(f"| P99 (worst) | {max(p99s):.1f} µs |")
        if mins:
            lines.append(f"| Min (best) | {min(mins):.1f} µs |")
        if maxs:
            lines.append(f"| Max (worst tail) | {max(maxs):.0f} µs |")
        lines.append(f"| Packet Loss | {total_loss}/{total_sent} ({total_loss/max(total_sent,1)*100:.4f}%) |")
        lines.append("")

    # Phase B
    valid_b = [r for r in phase_b if "avg_rtt_us" in r]
    failed_b = [r for r in phase_b if "error" in r]

    if phase_b:
        lines.append("## Phase B — Unicast RTT (sockperf under-load)")
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
                    f"| `..{sub}` "
                    f"| {r.get('avg_rtt_us', 0):.1f} "
                    f"| {r.get('p50_us', 0):.1f} "
                    f"| {r.get('p90_us', 0):.1f} "
                    f"| {r.get('p99_us', 0):.1f} "
                    f"| {r.get('min_us', 0):.1f} "
                    f"| {r.get('max_us', 0):.1f} |"
                )
            lines.append("")

            rtts = [r["avg_rtt_us"] for r in valid_b]
            p50s = [r["p50_us"] for r in valid_b if "p50_us" in r]
            lines.append("### Aggregate")
            lines.append("")
            lines.append("| Metric | Value |")
            lines.append("|--------|-------|")
            lines.append(f"| Avg RTT (mean of avgs) | {statistics.mean(rtts):.1f} µs |")
            if p50s:
                lines.append(f"| P50 RTT (median of p50s) | {statistics.median(p50s):.1f} µs |")
                lines.append(f"| Implied one-way (p50/2) | {statistics.median(p50s)/2:.1f} µs |")
            lines.append("")

    # PTP pre-flight
    if "PTP pre-flight" in "".join(lines) or re.search(r"PHC verified", raw_text_global[0] if raw_text_global else ""):
        pass  # Already implied by HW timestamp counts

    return "\n".join(lines)


raw_text_global: list[str] = []


def main() -> None:
    parser = argparse.ArgumentParser(description="Save benchmark results as markdown report.")
    parser.add_argument("--input", "-i", type=str, default=None, help="Input file (default: stdin)")
    parser.add_argument("--instance-type", type=str, default=None, help="Override instance type")
    parser.add_argument("--placement", type=str, default="single-az-cpg", help="Placement strategy")
    parser.add_argument("--notes", type=str, default="", help="Additional notes for the report header")
    parser.add_argument("--output-dir", type=str, default=None, help="Output directory (default: runs/)")
    parser.add_argument("--filename", type=str, default=None, help="Output filename (auto-generated if omitted)")
    args = parser.parse_args()

    if args.input:
        with open(args.input) as f:
            raw = f.read()
    else:
        raw = sys.stdin.read()

    raw_text_global.append(raw)

    config = parse_header(raw)
    phase_a = parse_phase_a(raw)
    phase_b = parse_phase_b(raw)

    if not phase_a and not phase_b:
        print("ERROR: No Phase A or Phase B results found in input.", file=sys.stderr)
        sys.exit(1)

    instance_type = args.instance_type or "unknown"
    if not args.instance_type:
        m = re.search(r"instance.type[=: ]+(\S+)", raw, re.IGNORECASE)
        if m:
            instance_type = m.group(1)

    timestamp = datetime.now(timezone.utc)

    report = generate_report(
        config=config,
        phase_a=phase_a,
        phase_b=phase_b,
        instance_type=instance_type,
        placement=args.placement,
        notes=args.notes,
        timestamp=timestamp,
    )

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

    # Also save raw output alongside for reproducibility
    raw_path = out_path.with_suffix(".raw.txt")
    raw_path.write_text(raw)
    print(f"Raw output:  {raw_path}")


if __name__ == "__main__":
    main()
