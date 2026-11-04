#!/usr/bin/env python3
"""
collect_results.py - publish a TGW multicast benchmark run to S3 and CloudWatch.

Reads the orchestrator's raw output, applies the validity gates, pools the receivers' histograms
and uploads a JSON report to S3 and summary metrics to CloudWatch.

Usage:
    python3 report/collect_results.py --input run.raw.txt --detail-dir run.details --s3-bucket my-bucket

A run that fails a validity gate (missing or errored subscriber, loss, clock, sender rate) is
refused: nothing is uploaded or published and the exit status is 3. --allow-partial publishes it
anyway, marked invalid, with the failures listed in the report.
"""

import argparse
import json
import logging
import os
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import boto3
from botocore.exceptions import ClientError

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import results as tr  # noqa: E402

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Parsing helpers
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
# Multicast (pooled histograms, validity gates)
# ---------------------------------------------------------------------------

def build_mcast_report(benchmark_id: str, config: Dict[str, Any], raw_text: str,
                       subs: List[Any], findings: List[Any]) -> Dict[str, Any]:
    """Report for the multicast tool. Groups are reported separately, never merged."""
    summary_sender = tr.parse_sender(raw_text)
    groups = {g: tr.aggregate_group(v) for g, v in tr.group_subscribers(subs).items()}
    per_sub = []
    for s in subs:
        entry: Dict[str, Any] = {"subscriber_id": s.subscriber_id, "az": s.az,
                                 "az_relation": s.az_relation, "ok": s.ok}
        if s.ok:
            entry.update(s.result)
        else:
            entry["error"] = s.error
        per_sub.append(entry)
    return {
        "benchmark_id": benchmark_id,
        "config": config,
        "valid": not any(f.level == "FAIL" for f in findings),
        "findings": [{"level": f.level, "code": f.code, "message": f.message} for f in findings],
        "sender": summary_sender,
        "loss": tr.loss_totals(subs, summary_sender),
        "per_subscriber": per_sub,
        "groups": groups,
        "aggregate_method": "pooled histograms where every subscriber supplied one; "
                            "otherwise worst-subscriber p99 and exact weighted mean only",
        "clock": tr.parse_clock(raw_text),
    }


def publish_mcast_to_cloudwatch(report: Dict[str, Any], namespace: str,
                                region: Optional[str] = None) -> None:
    """Publish per-path metrics. Latency metrics are only published when pooled (exact)."""
    try:
        cw = boto3.client("cloudwatch", region_name=region) if region else boto3.client("cloudwatch")
        data: List[Dict[str, Any]] = []
        for group, agg in report["groups"].items():
            dims = [{"Name": "BenchmarkId", "Value": report["benchmark_id"]},
                    {"Name": "Tool", "Value": "mcast"},
                    {"Name": "Path", "Value": group}]

            def add(name: str, value: Any, unit: str) -> None:
                if value is not None:
                    data.append({"MetricName": name, "Value": float(value), "Unit": unit, "Dimensions": dims})

            add("MeanLatencyUs", agg.get("mean_us"), "Microseconds")
            if agg.get("pooled"):
                add("MedianLatencyUs", agg.get("median_us"), "Microseconds")
                add("P99LatencyUs", agg.get("p99_us"), "Microseconds")
                add("P999LatencyUs", agg.get("p999_us"), "Microseconds")
            add("WorstSubscriberP99Us", agg.get("worst_subscriber_p99_us"), "Microseconds")
            if agg.get("expected"):
                add("PacketLossPct", 100.0 * agg["lost"] / agg["expected"], "Percent")
        for i in range(0, len(data), 1000):
            cw.put_metric_data(Namespace=namespace, MetricData=data[i:i + 1000])
        logger.info("Published %d metrics to CloudWatch namespace %s", len(data), namespace)
    except ClientError as exc:
        logger.error("CloudWatch publish failed: %s", exc)


def run_mcast(args: argparse.Namespace, raw_text: str) -> int:
    cfg_hdr = tr.parse_header(raw_text)
    subs = tr.parse_subscribers(raw_text)
    if args.detail_dir:
        logger.info("Loaded %d detail file(s)", tr.load_details(subs, args.detail_dir))
    findings, _ = tr.evaluate(raw_text, max_loss_pct=args.max_loss_pct,
                              max_clock_offset_us=args.max_clock_offset_us)
    invalid = any(f.level == "FAIL" for f in findings)
    print(tr.verdict_text(findings), file=sys.stderr)
    if invalid and not args.allow_partial:
        logger.error("Run is invalid; refusing to publish. Use --allow-partial to publish it marked invalid.")
        return 3

    benchmark_id = cfg_hdr.get("run_id") or "run-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    config = {
        "tool": "mcast",
        "multicast_group": args.multicast_group or cfg_hdr.get("multicast_group"),
        "port": args.port or cfg_hdr.get("port"),
        "num_subscribers": args.num_subscribers or len(subs),
        "send_rate": args.send_rate or cfg_hdr.get("rate"),
        "duration_secs": args.duration or cfg_hdr.get("duration"),
        "instance_type": args.instance_type or cfg_hdr.get("instance_type") or "unknown",
        "placement_strategy": args.placement_strategy,
        "base_ami": args.base_ami,
        "native_build": cfg_hdr.get("native_build"),
    }
    report = build_mcast_report(benchmark_id, config, raw_text, subs, findings)
    print(json.dumps(report, indent=2))

    if args.s3_bucket:
        if not upload_to_s3(report, args.s3_bucket, f"{args.s3_prefix}{benchmark_id}.json", region=args.region):
            logger.error("S3 upload failed - writing local fallback.")
            write_local_fallback(report, benchmark_id)
    else:
        logger.warning("No --s3-bucket specified; skipping S3 upload.")
        write_local_fallback(report, benchmark_id)
    publish_mcast_to_cloudwatch(report, args.cw_namespace, region=args.region)
    logger.info("Done. Benchmark ID: %s", benchmark_id)
    return 0


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate TGW multicast benchmark results.",
    )
    parser.add_argument(
        "--input", "-i",
        type=str,
        default=None,
        help="Path to file with orchestrator output (default: read stdin)",
    )
    parser.add_argument("--detail-dir", type=str, default=None,
                        help="Directory of <instance>.detail.json files (mcast pooled percentiles)")
    parser.add_argument("--allow-partial", action="store_true",
                        help="Publish an invalid mcast run, marked invalid, instead of refusing it")
    parser.add_argument("--max-loss-pct", type=float, default=tr.DEFAULT_MAX_LOSS_PCT)
    parser.add_argument("--max-clock-offset-us", type=float, default=tr.DEFAULT_MAX_CLOCK_OFFSET_US)
    parser.add_argument("--s3-bucket", type=str, default=None, help="S3 bucket for report upload")
    parser.add_argument("--s3-prefix", type=str, default="reports/", help="S3 key prefix")
    parser.add_argument("--region", type=str, default=None, help="AWS region")
    parser.add_argument("--cw-namespace", type=str, default="TGWMulticastBenchmark")
    parser.add_argument("--multicast-group", type=str, default=None)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--num-subscribers", type=int, default=None)
    parser.add_argument("--send-rate", type=int, default=None)
    parser.add_argument("--duration", type=int, default=None)
    parser.add_argument("--instance-type", type=str, default=None, help="Default: Instance-Type from the raw header")
    parser.add_argument("--placement-strategy", type=str, default="single-az-cpg")
    parser.add_argument("--base-ami", type=str, default=None)
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> None:
    args = parse_args(argv)
    if args.input:
        with open(args.input) as f:
            raw_text = f.read()
    else:
        raw_text = sys.stdin.read()
    sys.exit(run_mcast(args, raw_text))


if __name__ == "__main__":
    main()
