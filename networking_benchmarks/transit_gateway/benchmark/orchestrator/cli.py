"""Command-line parsing and validation for the run and prepare commands.

Values reach remote shell commands, so every one is checked here, before any AWS call.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from typing import List, Optional

VALID_TOOLS = ("mcast", "sockperf")  # also the order tools run in

BENCH_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # .../benchmark
TGW_DIR = os.path.dirname(BENCH_DIR)                                     # .../transit_gateway

EPILOG = """\
Examples:
  python3 -m orchestrator run --stack-name MyBenchStack --tool mcast --save-report
  python3 -m orchestrator run --stack-name MyBenchStack --tool mcast,sockperf --client-port 46000
(run from the benchmark/ directory)

The multicast sender and receiver are built on the instances at launch; the run stops with the
build log if they could not.

The first run against a stack (and any run after an instance reboots or a helper file changes)
prepares the instances automatically; `python3 -m orchestrator prepare` does that step on its own.
"""


class UsageError(Exception):
    pass


def _int(name: str, value: str) -> int:
    if not re.fullmatch(r"[0-9]+", value) or int(value) <= 0:
        raise UsageError(f"{name} must be a positive integer, got '{value}'")
    return int(value)


def _num(name: str, value: str) -> str:
    if not re.fullmatch(r"[0-9]+(\.[0-9]+)?", value):
        raise UsageError(f"{name} must be a number, got '{value}'")
    return value


def _add_common(p: argparse.ArgumentParser, state_dir: bool = True) -> None:
    p.add_argument("--stack-name", required=True, help="CloudFormation stack name")
    p.add_argument("--transport", default="ssm", help="ssm (SSM Run Command) or ssh (dev only: EC2 Instance "
                   "Connect key push + one reused SSH connection per instance; needs public IPs and port 22)")
    p.add_argument("--ssh-key", default=None, help="private key for --transport ssh (PATH.pub is pushed per run); "
                   "default: a throwaway key generated for the run")
    p.add_argument("--ssh-user", default="ec2-user", help="login user for --transport ssh")
    if state_dir:
        p.add_argument("--state-dir", default=os.path.join(TGW_DIR, ".bench-state"),
                       help="where the prepared-stack manifest is kept")


def _check_common(a: argparse.Namespace) -> None:
    if not re.fullmatch(r"[A-Za-z0-9-]+", a.stack_name):
        raise UsageError("--stack-name has unexpected characters")
    if a.transport not in ("ssm", "ssh"):
        raise UsageError(f"--transport must be ssm or ssh, got '{a.transport}'")
    if not re.fullmatch(r"[a-z_][a-z0-9_-]*", a.ssh_user):
        raise UsageError("--ssh-user has unexpected characters")


def run_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="python3 -m orchestrator run", description="Orchestrate a TGW multicast benchmark.",
                                epilog=EPILOG, allow_abbrev=False, formatter_class=argparse.RawDescriptionHelpFormatter)
    _add_common(p)
    p.add_argument("--group", default="239.1.1.1", help="multicast group address")
    p.add_argument("--port", default="5001", help="UDP port")
    p.add_argument("--rate", default="1000", help="send rate (pps for mcast, messages per second for sockperf)")
    p.add_argument("--duration", default="60", help="benchmark duration in seconds")
    p.add_argument("--payload-bytes", default="12", help="UDP payload size of the multicast sender, 12-1472 bytes "
                   "(12 holds the sequence number and timestamp; larger sizes are zero padded)")
    p.add_argument("--tool", action="append", default=[], help="benchmark tool(s), repeatable or comma separated: "
                   "mcast (TGW multicast one-way latency) and sockperf (unicast RTT floor, no TGW, reported next to "
                   "the multicast result). Default: mcast")
    p.add_argument("--max-loss-pct", default="2", help="fail the run above this loss per subscriber")
    p.add_argument("--max-clock-offset-us", default="10", help="fail the run when chrony System time exceeds this")
    p.add_argument("--instance-type", default="", help="instance type for the report (default: auto-detect)")
    p.add_argument("--placement", default="single-az-cpg", help="placement strategy for the report")
    p.add_argument("--notes", default="", help="additional notes for the report")
    p.add_argument("--detail-dir", default="", help="keep the receivers' histogram files here")
    p.add_argument("--src-port", default="", help="UDP source port for the multicast sender: N, or LO-HI to spread packets round-robin over one "
                   "socket per port. Default: a new ephemeral port every run, which is a new flow to the gateway")
    p.add_argument("--client-port", default="", help="UDP source port for the sockperf client, so the unicast floor "
                   "is one pinned flow (default: sockperf chooses)")
    p.add_argument("--sender-cpu", default="", help="pin the multicast sender to this CPU (taskset)")
    p.add_argument("--runs-dir", default="", help="where --save-report writes (default: ../runs)")
    p.add_argument("--save-report", action="store_true", help="save a markdown report to runs/")
    p.add_argument("--force-prepare", action="store_true", help="prepare the instances even if the manifest is valid")
    return p


def parse_run_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    a = run_parser().parse_args(argv)
    try:
        _check_common(a)
        tools: List[str] = []
        for part in a.tool:
            for t in part.split(","):
                t = re.sub(r"\s", "", t)
                if not t:
                    continue
                if t not in VALID_TOOLS:
                    raise UsageError(f"unknown --tool '{t}'. Valid choices: {' '.join(VALID_TOOLS)}")
                tools.append(t)
        if not tools:
            tools = ["mcast"]
        a.run_tools = [t for t in VALID_TOOLS if t in tools]
        a.port = _int("--port", a.port)
        a.rate = _int("--rate", a.rate)
        a.duration = _int("--duration", a.duration)
        a.payload_bytes = _int("--payload-bytes", a.payload_bytes)
        if not 12 <= a.payload_bytes <= 1472:
            raise UsageError(f"--payload-bytes must be 12-1472, got '{a.payload_bytes}'")
        _num("--max-loss-pct", a.max_loss_pct)
        _num("--max-clock-offset-us", a.max_clock_offset_us)
        if not re.fullmatch(r"[0-9]{1,3}(\.[0-9]{1,3}){3}", a.group):
            raise UsageError(f"--group must be an IPv4 address, got '{a.group}'")
        if a.sender_cpu and not re.fullmatch(r"[0-9]+", a.sender_cpu):
            raise UsageError(f"--sender-cpu must be a CPU number, got '{a.sender_cpu}'")
        if a.client_port and (_int("--client-port", a.client_port) > 65535):
            raise UsageError(f"--client-port must be 1-65535, got '{a.client_port}'")
        if a.src_port:
            m = re.fullmatch(r"([0-9]+)(?:-([0-9]+))?", a.src_port)
            lo, hi = (int(m.group(1)), int(m.group(2) or m.group(1))) if m else (0, 0)
            if not m or not 1 <= lo <= hi <= 65535 or hi - lo >= 1024:
                raise UsageError(f"--src-port must be N or LO-HI within 1-65535 (at most 1024 ports), got '{a.src_port}'")
    except UsageError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        raise SystemExit(1)
    mcast = "mcast" in a.run_tools
    a.needs_native = mcast
    a.runs_dir = a.runs_dir or os.path.join(TGW_DIR, "runs")
    return a


def parse_prepare_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="python3 -m orchestrator prepare", description="Provision and verify the benchmark "
                                "instances once, ahead of the first run. Safe to repeat.", allow_abbrev=False)
    _add_common(p)
    a = p.parse_args(argv)
    try:
        _check_common(a)
    except UsageError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        raise SystemExit(1)
    return a


def parse_check_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="python3 -m orchestrator check", allow_abbrev=False,
                                description="Report PTP and clock state on every instance. Exits non-zero if any "
                                "instance is not synced to the PHC or its chrony System time is above the limit.")
    _add_common(p, state_dir=False)
    p.add_argument("--verbose", "-v", action="store_true", help="also print the full per-instance diagnostic")
    p.add_argument("--max-sys-us", default=os.environ.get("MAX_SYS_US", "10"),
                   help="fail above this chrony System time in microseconds [default: 10]")
    a = p.parse_args(argv)
    try:
        _check_common(a)
        _num("--max-sys-us", a.max_sys_us)
    except UsageError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        raise SystemExit(1)
    return a
