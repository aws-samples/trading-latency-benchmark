"""Stage 3: one benchmark run against a prepared stack, then the verdict and the optional report.

Output is the raw stream that results.py parses. A run that fails a validity gate exits 3.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import List, Optional

from . import cli, emit
from . import manifest as mf
from .context import Ctx
from .model import Stack, stack_from_manifest
from .phases.clock_gate import run_gate
from .phases.mcast import run_mcast
from .phases.unicast import run_sockperf
from .prepare import bind_transport, explain_preflight_failure, native_problems, prepare
from .transport import make_transport
from .util import Abort, Console

EXIT_INVALID_RUN = 3


def _check_native(ctx: Ctx, stack: Stack, m: mf.Manifest) -> str:
    """Refuse to continue unless every instance has the same native build. Returns its BUILD_INFO line."""
    bad = native_problems(m, stack.all_ids)
    if bad:
        iid, state = next(iter(bad.items()))
        c = ctx.console
        c.err(f"ERROR: {iid} has no native build (state: {state or 'unknown'}).")
        c.err("       Refusing to continue: the multicast sender and receiver are not available there.")
        r = ctx.transport.run(iid, 30, "tail -n 20 /var/log/benchmark-native-build.log 2>/dev/null; true")
        if r.ok:
            for line in r.out.splitlines():
                c.err(f"       | {line}")
        c.err("       Fix the build: native_repo / native_ref must be reachable and contain the sources.")
        raise Abort(1)
    first_commit, first_build = "", ""
    for iid in stack.all_ids:
        build = m["instances"][iid]["native"]["build"]
        mt = re.match(r"commit=([0-9a-f]*)", build)
        commit = mt.group(1) if mt else ""
        if not first_commit:
            first_commit, first_build = commit, build
        elif commit != first_commit:
            ctx.console.err(f"ERROR: instances built different native commits ({first_commit} vs {commit}).")
            raise Abort(1)
    return first_build


def _settle(ctx: Ctx):
    """Make sure the manifest is true, then gate the clock.

    Returns (stack, manifest, gate outcomes, native build line).
    """
    cfg, c = ctx.cfg, ctx.console
    manifest = mf.load(ctx.state_dir, cfg.stack_name)
    problems = mf.manifest_problems(manifest, mf.local_hashes(ctx.bench_dir))
    if cfg.force_prepare:
        problems.insert(0, "--force-prepare")
    prepared = bool(problems)
    if problems:
        c.out(f">>> Preparing the stack first: {'; '.join(problems)}")
        manifest = prepare(ctx)
    else:
        stack = stack_from_manifest(cfg.stack_name, manifest)
        bind_transport(ctx, stack.all_ids, {i.id: i.public_ip for i in stack.all})
    while True:
        stack = stack_from_manifest(cfg.stack_name, manifest)
        build = _check_native(ctx, stack, manifest) if cfg.needs_native else ""
        outcomes, stale = run_gate(ctx, stack, manifest)
        if not stale:
            return stack, manifest, outcomes, build
        why = "; ".join(f"{i}: {', '.join(r)}" for i, r in stale.items())
        if prepared:
            c.err(f"ERROR: instances do not match what was just prepared ({why}).")
            raise Abort(1)
        c.out(f">>> Prepared state is stale ({why}); preparing again")
        manifest, prepared = prepare(ctx), True


def run_all(ctx: Ctx, run_id: str, detail_dir: str) -> None:
    cfg, c = ctx.cfg, ctx.console
    c.out("=== TGW Multicast Benchmark ===")
    for label, value in (("Stack:", cfg.stack_name), ("Run-ID:", run_id), ("Tools:", " ".join(cfg.run_tools)),
                         ("Group:", cfg.group), ("Port:", cfg.port), ("Rate:", cfg.rate),
                         ("Duration:", f"{cfg.duration}s"), ("Payload:", f"{cfg.payload_bytes} bytes")):
        c.out(f"{label:<10}{value}")
    c.out("")

    stack, manifest, gate, native_build = _settle(ctx)
    c.out(f"  Stack multicast group: {stack.group}")
    c.out(f"  Stack multicast port:  {stack.port}")
    c.out(f"Instance-Type: {cfg.instance_type or stack.publisher.itype}")
    c.out(f"Transport: {ctx.transport.name}")
    c.out(f"  Publisher:   {stack.publisher.id}")
    c.out(f"  Subscribers: {' '.join(s.id for s in stack.subscribers)}")
    c.out(f"  S3 Bucket:   {stack.bucket}")
    c.out("")
    if native_build:
        c.out(f"Native-Build: {native_build}")
    c.out("")
    ctx.mark("prepared-state check and clock gate")

    c.out(">>> Preflight: PHC, hardware timestamping and clock offset on all instances...")
    failed = False
    for o in gate:
        if o.ok:
            line = next((x for x in o.data["gate"].splitlines() if x.startswith("system_time_us=")), "")
            c.out(f"  {o.id} - clock and NIC ready ({line})")
        else:
            c.err(f"  FAIL {o.id} - preflight failed:")
            for x in o.data["gate"].splitlines():
                c.err(f"    {x}")
            failed = True
    if failed:
        explain_preflight_failure(ctx, stack.name)
        raise Abort(1)
    c.out("  All instances passed preflight.")
    c.out("")
    for o in gate:
        emit.clock_block(c, o.id, "pre", o.data["gate"])
    c.out("")
    ctx.mark("preflight")
    for inst in stack.all:
        emit.context_block(c, inst.id, inst.ami, manifest["instances"][inst.id]["context"])
    c.out("")

    if "mcast" in cfg.run_tools:
        run_mcast(ctx, stack, run_id, detail_dir)
    if "sockperf" in cfg.run_tools:
        run_sockperf(ctx, stack)
    c.out("=== Benchmark complete ===")
    c.out(f"S3 bucket for reports: {stack.bucket}")


def _tool(name: str) -> str:
    return os.path.join(cli.BENCH_DIR, "report", name)


def _judge(cfg, raw_path: str, detail_dir: str, keep_detail: bool) -> int:
    rc = 0
    if "mcast" in cfg.run_tools:
        print("\n>>> Run validity:", flush=True)
        p = subprocess.run([sys.executable, _tool("results.py"), "verdict", "--input", raw_path,
                            "--max-loss-pct", cfg.max_loss_pct, "--max-clock-offset-us", cfg.max_clock_offset_us])
        if p.returncode != 0:
            rc = EXIT_INVALID_RUN
    if cfg.save_report:
        args = [sys.executable, _tool("save_report.py"), "--input", raw_path, "--placement", cfg.placement,
                "--output-dir", cfg.runs_dir, "--max-loss-pct", cfg.max_loss_pct,
                "--max-clock-offset-us", cfg.max_clock_offset_us]
        if cfg.instance_type:
            args += ["--instance-type", cfg.instance_type]
        if cfg.notes:
            args += ["--notes", cfg.notes]
        if os.path.isdir(detail_dir):
            args += ["--detail-dir", detail_dir]
        print("\n>>> Generating markdown report...", flush=True)
        subprocess.run(args)
    if keep_detail and os.path.isdir(detail_dir):
        print(f"Histogram detail files kept in: {detail_dir}")
        print("  (pass --detail-dir to collect_results.py / save_report.py for pooled percentiles)")
    return rc


def main(argv: Optional[List[str]] = None) -> int:
    cfg = cli.parse_run_args(argv)
    run_id = f"{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-{os.getpid()}"
    work = tempfile.mkdtemp(prefix="tgw_bench.", dir=os.environ.get("TMPDIR") or "/tmp")
    raw_path = os.path.join(work, "raw.txt")
    console = Console(raw_path)
    keep_detail = bool(cfg.detail_dir) or cfg.save_report
    detail_dir = cfg.detail_dir or (os.path.join(cfg.runs_dir, f"{run_id}.details") if cfg.save_report
                                    else os.path.join(work, "details"))
    transport = None
    try:
        try:
            transport = make_transport(cfg.transport, console, cfg.ssh_user, cfg.ssh_key)
            ctx = Ctx(cfg, console, transport, cli.BENCH_DIR, cfg.state_dir)
            run_all(ctx, run_id, detail_dir)
            rc = 0
        except Abort as a:
            rc = a.rc
        if rc != 0:
            console.err(f"ERROR: benchmark aborted (exit {rc}); no results to judge.")
            return rc
        console.close()
        return _judge(cfg, raw_path, detail_dir, keep_detail)
    finally:
        if transport:
            transport.close()
        console.close()
        shutil.rmtree(work, ignore_errors=True)


def prepare_main(argv: Optional[List[str]] = None) -> int:
    cfg = cli.parse_prepare_args(argv)
    console = Console()
    transport = None
    try:
        transport = make_transport(cfg.transport, console, cfg.ssh_user, cfg.ssh_key)
        prepare(Ctx(cfg, console, transport, cli.BENCH_DIR, cfg.state_dir))
        return 0
    except Abort as a:
        return a.rc
    finally:
        if transport:
            transport.close()
