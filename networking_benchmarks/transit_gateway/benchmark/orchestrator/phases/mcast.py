"""Phase A: TGW multicast one-way latency. Receivers start, the publisher sends, results are collected."""
from __future__ import annotations

import json
import os
import re
from typing import Dict, List

from .. import emit
from .. import manifest as mf
from ..aws import AwsError, aws
from ..context import Ctx
from ..model import Stack
from ..parallel import Outcome, all_ok, par_run
from ..util import Abort

NATIVE_BIN = "/opt/benchmark-native/bin"

# The stop command is separate from the start command so that neither script text contains both
# the pkill pattern and the receiver path (pkill -f could otherwise match its own shell).
STOP_CMD = ("pkill -f '[m]cast_recv' 2>/dev/null && sleep 1; "
            "rm -f /tmp/mcast_recv.ready /tmp/mcast_result.json /tmp/mcast_recv_detail.json /tmp/mcast_recv.err; "
            "echo STOPPED")
READY_WAIT = ("for i in $(seq 1 50); do [ -f /tmp/mcast_recv.ready ] && { echo RECV=READY; exit 0; }; "
                     "pgrep -f '[m]cast_recv' >/dev/null || { echo RECV=DEAD; "
                     "cat /tmp/mcast_result.json /tmp/mcast_recv.err 2>/dev/null; exit 0; }; sleep 0.2; done; "
                     "echo RECV=TIMEOUT")


def _after(text: str, marker: str) -> str:
    return text.split(marker, 1)[1] if marker in text else text


def _before(text: str, marker: str) -> str:
    return text.split(marker, 1)[0]


def _recv_start_cmd(cfg, sender_timeout: int) -> str:
    recv = f"{NATIVE_BIN}/mcast_recv {cfg.group} {cfg.port} {sender_timeout}"
    start = f"nohup {recv} > /tmp/mcast_result.json 2> /tmp/mcast_recv.err < /dev/null & echo STARTED"
    return STOP_CMD + "\n" + start + "\n" + READY_WAIT


def _collect_cmd(cfg, detail_prefix: str, region: str) -> str:
    detail = ("if [ -f /tmp/mcast_recv_detail.json ]; then aws s3 cp /tmp/mcast_recv_detail.json "
              f"{detail_prefix}/@ID@.detail.json --region {region} --only-show-errors && echo DETAIL=UPLOADED; "
              "else echo DETAIL=MISSING; fi")
    return (
        "for i in $(seq 1 30); do pgrep -f '[m]cast_recv' >/dev/null || break; sleep 1; done\n"
        "if pgrep -f '[m]cast_recv' >/dev/null; then pkill -INT -f '[m]cast_recv'; sleep 3; echo RECV=INTERRUPTED; "
        "else echo RECV=DONE; fi\n"
        "echo '=====RESULT====='\n"
        "cat /tmp/mcast_result.json 2>/dev/null\n"
        "echo '=====DETAIL====='\n"
        f"{detail}\n"
        "echo '=====CLOCK====='\n"
        f"bash {mf.REMOTE_BIN}/preflight.sh --clock"
    )


def run_mcast(ctx: Ctx, stack: Stack, run_id: str, detail_dir: str) -> None:
    cfg, c, tp = ctx.cfg, ctx.console, ctx.transport
    pub = stack.publisher
    sub_ids = [s.id for s in stack.subscribers]
    sender_timeout = cfg.duration + 30
    c.out(f">>> Phase A: Multicast one-way latency ({cfg.rate} pps, {cfg.duration}s, "
          "native sender and receivers)...")

    recv_start = _recv_start_cmd(cfg, sender_timeout)
    detail_prefix = f"s3://{stack.bucket}/runs/{run_id}"
    collect_cmd = _collect_cmd(cfg, detail_prefix, stack.region)
    send_bin = f"{NATIVE_BIN}/mcast_send"
    if cfg.sender_cpu:
        send_bin = f"taskset -c {cfg.sender_cpu} {send_bin}"
    src_port_env = f"MCAST_SRC_PORT={cfg.src_port} " if cfg.src_port else ""
    send_cmd = f"{src_port_env}{send_bin} {cfg.group} {cfg.port} {cfg.rate} {cfg.duration} {pub.private_ip} {cfg.payload_bytes}"

    def recv_worker(iid: str) -> Outcome:
        """Start a subscriber's receiver and wait until it is listening; one retry, then fail."""
        notes: List[str] = []
        for attempt in (1, 2):
            r = tp.run(iid, 90, recv_start)
            m = re.search(r"^RECV=.*$", r.out, re.M)
            status = m.group(0) if m else ""
            if status == "RECV=READY":
                return Outcome(iid, True, out=notes + [f"  {iid} - multicast receiver ready"])
            if attempt == 2:
                return Outcome(iid, False, err=[f"ERROR: receiver on {iid} did not become ready after 2 attempts:"]
                               + [f"    {x}" for x in r.out.splitlines()])
            notes.append(f"  {iid} - receiver not ready ({status or 'no status'}); retrying once")
        raise AssertionError("unreachable")

    def collect_worker(iid: str) -> Outcome:
        """A subscriber prints its result, uploads its detail file and reads the clock; the
        publisher only reads the clock."""
        if iid == pub.id:
            r = tp.run(iid, 30, f"bash {mf.REMOTE_BIN}/preflight.sh --clock")
            return Outcome(iid, True, data={"res": "", "post": r.out})
        r = tp.run(iid, 120, collect_cmd.replace("@ID@", iid))
        out: List[str] = []
        if "RECV=INTERRUPTED" in r.out:
            out.append(f"  {iid} - receiver had to be interrupted (end marker never arrived?)")
        rest = _after(r.out, "=====RESULT=====")
        res = _before(rest, "=====DETAIL=====")
        rest = _after(rest, "=====DETAIL=====")
        det = _before(rest, "=====CLOCK=====")
        post = _after(rest, "=====CLOCK=====")
        if "DETAIL=UPLOADED" not in det:
            out.append(f"  WARN {iid} - detail file not uploaded; pooled percentiles will be unavailable")
        return Outcome(iid, True, out=out, data={"res": res.strip("\n"), "post": post.strip("\n")})

    # The whole batch (start receivers, send, collect) runs again once if the sender or any
    # subscriber produced no usable result; the second outcome is the one reported.
    batch = 0
    while True:
        batch += 1
        if not all_ok(par_run(c, recv_worker, sub_ids)):
            raise Abort(1)
        c.out(f"  Sending {cfg.rate} pps multicast for {cfg.duration}s...")
        r = tp.run(pub.id, sender_timeout, send_cmd)
        sender_ok = r.ok
        sender_out = r.out if r.ok else json.dumps(
            {"error": f"sender command did not complete (SSM status: {r.status or 'unknown'})"}, separators=(",", ":"))
        if sender_ok:
            c.out("  Multicast send complete.")
        else:
            c.err("  Multicast send FAILED.")
        c.out("")

        collected: Dict[str, Outcome] = {o.id: o for o in par_run(c, collect_worker, stack.all_ids)}
        os.makedirs(detail_dir, exist_ok=True)
        try:
            aws("s3", "cp", detail_prefix + "/", detail_dir + "/", "--recursive", "--region", stack.region,
                "--only-show-errors")
        except AwsError:
            c.out(f"  WARN could not download detail files from {detail_prefix}/")
        incomplete = (not sender_ok) or any(not emit.result_ok(collected[s].data["res"]) for s in sub_ids)
        if not incomplete or batch >= 2:
            break
        c.out("  Phase A incomplete (the sender or a subscriber produced no usable result); "
              "rerunning the whole batch once")

    ctx.mark(f"receiver start, send and collect (includes the {cfg.duration}s send)")
    c.out(f"--- SENDER {pub.id} ---")
    c.out(sender_out)
    c.out(f"--- END SENDER {pub.id} ---")
    for s in stack.subscribers:
        rel = "same-az" if s.az == pub.az else "cross-az"
        emit.mcast_block(c, s.id, s.az, rel, collected[s.id].data["res"])
    c.out("")
    # Clock state after the run was read together with the results.
    for iid in stack.all_ids:
        emit.clock_block(c, iid, "post", collected[iid].data["post"])
    c.out("")
    ctx.mark("post-run clock state")
