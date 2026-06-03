#!/usr/bin/env python3
"""
collect_results.py — Aggregate TGW multicast benchmark results.

Reads per-subscriber output (from run_benchmark.sh stdout), parses
sockperf/iperf3 metrics, computes aggregate statistics, uploads a JSON
report to S3, and publishes summary metrics to CloudWatch.

Usage:
    # Pipe from run_benchmark.sh
    scripts/run_benchmark.sh --stack-name MyStack | python3 scripts/collect_results.py --tool sockperf --s3-bucket my-bucket

    # Or read from a saved file
    python3 scripts/collect_results.py --input results.txt --tool sockperf --s3-bucket my-bucket
"""

import argparse
import json
import logging
import math
import os
import re
import statistics
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import boto3
from botocore.exceptions import ClientError

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

def parse_subscriber_blocks(raw_text: str) -> Dict[str, str]:
    """Extract per-subscriber output blocks from run_benchmark.sh output.

    Looks for ``--- SUBSCRIBER <id> ---`` / ``--- END <id> ---`` delimiters.
    Returns a dict mapping subscriber_id -> raw output text.
    """
    blocks: Dict[str, str] = {}
    pattern = re.compile(
        r"--- SUBSCRIBER (\S+) ---\n(.*?)--- END \1 ---",
        re.DOTALL,
    )
    for match in pattern.finditer(raw_text):
        subscriber_id = match.group(1)
        block_text = match.group(2).strip()
        blocks[subscriber_id] = block_text
    return blocks


def parse_sockperf_output(raw_output: str) -> Dict[str, Any]:
    """Parse sockperf under-load / ping-pong output into metrics dict.

    Extracts latency summary, percentiles, and packet counts from the
    receiver-side output captured by run_benchmark.sh.
    """
    metrics: Dict[str, Any] = {
        "min_latency_us": 0.0,
        "max_latency_us": 0.0,
        "mean_latency_us": 0.0,
        "median_latency_us": 0.0,
        "p95_latency_us": 0.0,
        "p99_latency_us": 0.0,
        "total_received": 0,
        "total_expected": 0,
        "packet_loss_count": 0,
    }

    # Summary latency line:
    #   sockperf: Summary: Latency is 89.123 usec
    m = re.search(r"Summary:\s*Latency\s+is\s+([\d.]+)\s*usec", raw_output)
    if m:
        metrics["mean_latency_us"] = float(m.group(1))

    # Total run line:
    #   sockperf: [Total Run] ... SentMessages=10000; ReceivedMessages=9998
    m = re.search(
        r"SentMessages\s*=\s*(\d+)\s*;\s*ReceivedMessages\s*=\s*(\d+)",
        raw_output,
    )
    if m:
        metrics["total_expected"] = int(m.group(1))
        metrics["total_received"] = int(m.group(2))
        metrics["packet_loss_count"] = metrics["total_expected"] - metrics["total_received"]

    # Percentile lines (sockperf under-load --full-rtt output):
    #   sockperf: % percentile  99.000 =   450.123 usec
    #   sockperf: % percentile  99.900 =   800.456 usec
    percentiles: Dict[float, float] = {}
    for pm in re.finditer(
        r"percentile\s+([\d.]+)\s*=\s*([\d.]+)\s*usec", raw_output
    ):
        pct = float(pm.group(1))
        val = float(pm.group(2))
        percentiles[pct] = val

    if percentiles:
        if 50.0 in percentiles:
            metrics["median_latency_us"] = percentiles[50.0]
        if 95.0 in percentiles:
            metrics["p95_latency_us"] = percentiles[95.0]
        if 99.0 in percentiles:
            metrics["p99_latency_us"] = percentiles[99.0]
        if 99.9 in percentiles:
            # Use 99.9 as a better upper bound if available
            pass

    # Min / Max latency lines:
    #   sockperf: Summary: ... min=45.000 usec ...
    m_min = re.search(r"\bmin\s*=\s*([\d.]+)", raw_output)
    if m_min:
        metrics["min_latency_us"] = float(m_min.group(1))

    m_max = re.search(r"\bmax\s*=\s*([\d.]+)", raw_output)
    if m_max:
        metrics["max_latency_us"] = float(m_max.group(1))

    return metrics


def parse_iperf3_output(raw_output: str) -> Dict[str, Any]:
    """Parse iperf3 output (JSON or text) into metrics dict.

    iperf3 doesn't natively report per-packet latency, so latency fields
    are set to 0.0.  We extract throughput and packet loss from the summary.
    """
    metrics: Dict[str, Any] = {
        "min_latency_us": 0.0,
        "max_latency_us": 0.0,
        "mean_latency_us": 0.0,
        "median_latency_us": 0.0,
        "p95_latency_us": 0.0,
        "p99_latency_us": 0.0,
        "total_received": 0,
        "total_expected": 0,
        "packet_loss_count": 0,
    }

    # Try JSON output first (iperf3 -J)
    try:
        data = json.loads(raw_output)
        if "end" in data and "sum" in data["end"]:
            summary = data["end"]["sum"]
            metrics["total_expected"] = summary.get("packets", 0)
            lost = summary.get("lost_packets", 0)
            metrics["packet_loss_count"] = lost
            metrics["total_received"] = metrics["total_expected"] - lost
        return metrics
    except (json.JSONDecodeError, KeyError):
        pass

    # Fallback: parse text summary
    # Example: [SUM]   0.00-60.00  sec  ... 10000/10000 (0%) ...
    m = re.search(r"(\d+)/(\d+)\s*\(([\d.]+)%\)", raw_output)
    if m:
        lost = int(m.group(1))
        total = int(m.group(2))
        metrics["total_expected"] = total
        metrics["packet_loss_count"] = lost
        metrics["total_received"] = total - lost

    return metrics


# ---------------------------------------------------------------------------
# Aggregate statistics (importable for property tests)
# ---------------------------------------------------------------------------

def compute_aggregate_statistics(
    per_subscriber_metrics: List[Dict[str, Any]],
) -> Dict[str, Any]:
    """Compute aggregate statistics from a list of per-subscriber parsed_metrics dicts.

    Each element in *per_subscriber_metrics* must contain at minimum:
        - min_latency_us, max_latency_us, mean_latency_us, median_latency_us
        - p95_latency_us, p99_latency_us
        - total_received, total_expected, packet_loss_count

    Returns a dict matching the aggregate schema from the design document.
    """
    if not per_subscriber_metrics:
        raise ValueError("per_subscriber_metrics must be non-empty")

    total_received = sum(m["total_received"] for m in per_subscriber_metrics)
    total_expected = sum(m["total_expected"] for m in per_subscriber_metrics)

    packet_loss_pct = (
        ((total_expected - total_received) / total_expected * 100)
        if total_expected > 0
        else 0.0
    )

    # Collect all per-subscriber latency values for aggregate computation
    all_min = [m["min_latency_us"] for m in per_subscriber_metrics]
    all_max = [m["max_latency_us"] for m in per_subscriber_metrics]
    all_mean = [m["mean_latency_us"] for m in per_subscriber_metrics]
    all_median = [m["median_latency_us"] for m in per_subscriber_metrics]
    all_p95 = [m["p95_latency_us"] for m in per_subscriber_metrics]
    all_p99 = [m["p99_latency_us"] for m in per_subscriber_metrics]

    return {
        "total_received": total_received,
        "total_expected": total_expected,
        "packet_loss_pct": round(packet_loss_pct, 6),
        "min_latency_us": math.floor(min(all_min) * 1000) / 1000,
        "max_latency_us": math.ceil(max(all_max) * 1000) / 1000,
        "mean_latency_us": round(statistics.mean(all_mean), 3),
        "median_latency_us": round(statistics.median(all_median), 3),
        "p95_latency_us": round(statistics.mean(all_p95), 3),
        "p99_latency_us": round(statistics.mean(all_p99), 3),
    }


# ---------------------------------------------------------------------------
# S3 upload + CloudWatch publish
# ---------------------------------------------------------------------------

def upload_to_s3(
    report: Dict[str, Any],
    bucket: str,
    key: str,
    region: Optional[str] = None,
) -> bool:
    """Upload *report* as JSON to S3.  Returns True on success."""
    try:
        s3 = boto3.client("s3", region_name=region) if region else boto3.client("s3")
        s3.put_object(
            Bucket=bucket,
            Key=key,
            Body=json.dumps(report, indent=2),
            ContentType="application/json",
        )
        logger.info("Uploaded report to s3://%s/%s", bucket, key)
        return True
    except ClientError as exc:
        logger.error("S3 upload failed: %s", exc)
        return False


def publish_to_cloudwatch(
    aggregate: Dict[str, Any],
    benchmark_id: str,
    namespace: str = "TGWMulticastBenchmark",
    region: Optional[str] = None,
) -> None:
    """Publish summary metrics to CloudWatch."""
    try:
        cw = (
            boto3.client("cloudwatch", region_name=region)
            if region
            else boto3.client("cloudwatch")
        )
        dimensions = [{"Name": "BenchmarkId", "Value": benchmark_id}]
        metric_data = [
            {
                "MetricName": "MeanLatencyUs",
                "Value": aggregate["mean_latency_us"],
                "Unit": "Microseconds",
                "Dimensions": dimensions,
            },
            {
                "MetricName": "P99LatencyUs",
                "Value": aggregate["p99_latency_us"],
                "Unit": "Microseconds",
                "Dimensions": dimensions,
            },
            {
                "MetricName": "PacketLossPct",
                "Value": aggregate["packet_loss_pct"],
                "Unit": "Percent",
                "Dimensions": dimensions,
            },
        ]
        cw.put_metric_data(Namespace=namespace, MetricData=metric_data)
        logger.info("Published %d metrics to CloudWatch namespace %s", len(metric_data), namespace)
    except ClientError as exc:
        logger.error("CloudWatch publish failed: %s", exc)


def write_local_fallback(report: Dict[str, Any], benchmark_id: str) -> str:
    """Write report JSON to local filesystem as fallback. Returns the file path."""
    fallback_dir = os.path.join(os.getcwd(), "benchmark-results")
    os.makedirs(fallback_dir, exist_ok=True)
    path = os.path.join(fallback_dir, f"{benchmark_id}.json")
    with open(path, "w") as f:
        json.dump(report, f, indent=2)
    logger.info("Wrote fallback report to %s", path)
    return path


# ---------------------------------------------------------------------------
# Report builder
# ---------------------------------------------------------------------------

def build_report(
    benchmark_id: str,
    config: Dict[str, Any],
    subscriber_results: List[Dict[str, Any]],
    aggregate: Dict[str, Any],
) -> Dict[str, Any]:
    """Build the full aggregate report JSON matching the design schema."""
    per_subscriber = []
    for sr in subscriber_results:
        entry = {
            "subscriber_id": sr["subscriber_id"],
            "total_received": sr["parsed_metrics"]["total_received"],
            "packet_loss_count": sr["parsed_metrics"]["packet_loss_count"],
            "min_latency_us": sr["parsed_metrics"]["min_latency_us"],
            "max_latency_us": sr["parsed_metrics"]["max_latency_us"],
            "mean_latency_us": sr["parsed_metrics"]["mean_latency_us"],
            "median_latency_us": sr["parsed_metrics"]["median_latency_us"],
            "p95_latency_us": sr["parsed_metrics"]["p95_latency_us"],
            "p99_latency_us": sr["parsed_metrics"]["p99_latency_us"],
        }
        per_subscriber.append(entry)

    return {
        "benchmark_id": benchmark_id,
        "config": config,
        "per_subscriber": per_subscriber,
        "aggregate": aggregate,
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate TGW multicast benchmark results.",
    )
    parser.add_argument(
        "--input", "-i",
        type=str,
        default=None,
        help="Path to file with run_benchmark.sh output (default: read stdin)",
    )
    parser.add_argument("--tool", type=str, default="sockperf", choices=["sockperf", "iperf3"])
    parser.add_argument("--s3-bucket", type=str, default=None, help="S3 bucket for report upload")
    parser.add_argument("--s3-prefix", type=str, default="reports/", help="S3 key prefix")
    parser.add_argument("--region", type=str, default=None, help="AWS region")
    parser.add_argument("--cw-namespace", type=str, default="TGWMulticastBenchmark")
    parser.add_argument("--multicast-group", type=str, default="239.1.1.1")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--num-subscribers", type=int, default=None)
    parser.add_argument("--send-rate", type=int, default=1000)
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--instance-type", type=str, default="c6in.large")
    parser.add_argument("--placement-strategy", type=str, default="single-az-cpg")
    parser.add_argument("--base-ami", type=str, default=None)
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> None:
    args = parse_args(argv)

    # Read input
    if args.input:
        with open(args.input) as f:
            raw_text = f.read()
    else:
        raw_text = sys.stdin.read()

    # Parse subscriber blocks
    blocks = parse_subscriber_blocks(raw_text)
    if not blocks:
        logger.error("No subscriber result blocks found in input.")
        sys.exit(1)

    logger.info("Found %d subscriber result block(s).", len(blocks))

    # Choose parser
    parser_fn = parse_sockperf_output if args.tool == "sockperf" else parse_iperf3_output

    # Parse each subscriber
    subscriber_results: List[Dict[str, Any]] = []
    for sub_id, raw_output in blocks.items():
        parsed = parser_fn(raw_output)
        subscriber_results.append({
            "subscriber_id": sub_id,
            "tool": args.tool,
            "multicast_group": args.multicast_group,
            "port": args.port,
            "raw_output": raw_output,
            "parsed_metrics": parsed,
        })

    # Compute aggregate
    all_metrics = [sr["parsed_metrics"] for sr in subscriber_results]
    aggregate = compute_aggregate_statistics(all_metrics)

    # Build benchmark ID
    benchmark_id = "run-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")

    # Build config
    config = {
        "tool": args.tool,
        "multicast_group": args.multicast_group,
        "port": args.port,
        "num_subscribers": args.num_subscribers or len(blocks),
        "send_rate": args.send_rate,
        "duration_secs": args.duration,
        "instance_type": args.instance_type,
        "placement_strategy": args.placement_strategy,
        "base_ami": args.base_ami,
    }

    # Build report
    report = build_report(benchmark_id, config, subscriber_results, aggregate)

    # Print report to stdout
    print(json.dumps(report, indent=2))

    # Upload to S3
    if args.s3_bucket:
        s3_key = f"{args.s3_prefix}{benchmark_id}.json"
        success = upload_to_s3(report, args.s3_bucket, s3_key, region=args.region)
        if not success:
            logger.error("S3 upload failed — writing local fallback.")
            write_local_fallback(report, benchmark_id)
    else:
        logger.warning("No --s3-bucket specified; skipping S3 upload.")
        write_local_fallback(report, benchmark_id)

    # Publish to CloudWatch
    publish_to_cloudwatch(
        aggregate,
        benchmark_id,
        namespace=args.cw_namespace,
        region=args.region,
    )

    logger.info("Done. Benchmark ID: %s", benchmark_id)


if __name__ == "__main__":
    main()
