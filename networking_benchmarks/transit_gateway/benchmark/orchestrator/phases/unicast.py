"""Phase B: the unicast floor, sockperf round-trip latency between the same hosts without the TGW."""
from __future__ import annotations

from typing import List

from ..context import Ctx
from ..model import Instance, Stack
from ..parallel import Outcome, all_ok, par_run
from ..util import Abort

# The floor needs far fewer samples than the multicast run: 10 s at the default rate is 10,000 round trips.
SOCKPERF_MAX_SECS = 10


def _server_cmd(sub: Instance, port: int) -> str:
    """Free the port, start a sockperf server bound to the subscriber's IP, and wait until it listens."""
    return (f"pkill -f '[m]cast_recv' 2>/dev/null; pkill -f 'sockperf server' 2>/dev/null; "
            f"fuser -k {port}/udp 2>/dev/null; "
            f"nohup sockperf server --ip {sub.private_ip} --port {port} > /tmp/sockperf_server.txt 2>&1 < /dev/null &\n"
            f"for i in $(seq 1 50); do ss -uln | grep -q ':{port} ' && {{ echo SERVER=READY; exit 0; }}; sleep 0.1; done\n"
            "echo SERVER=TIMEOUT; cat /tmp/sockperf_server.txt 2>/dev/null")


def run_sockperf(ctx: Ctx, stack: Stack) -> None:
    cfg, c, tp = ctx.cfg, ctx.console, ctx.transport
    secs = min(cfg.duration, SOCKPERF_MAX_SECS)
    client_port = f"--client_port {cfg.client_port} " if cfg.client_port else ""
    c.out(f">>> Phase B: Unicast latency floor (sockperf under-load, {secs}s per subscriber)...")

    def start(iid: str) -> Outcome:
        sub = next(s for s in stack.subscribers if s.id == iid)
        r = tp.run(iid, 60, _server_cmd(sub, cfg.port))
        if "SERVER=READY" in r.out:
            return Outcome(iid, True, out=[f"  {iid} ({sub.private_ip}) - sockperf server listening"])
        return Outcome(iid, False, err=[f"ERROR: sockperf server on {iid} did not start:"]
                       + [f"    {x}" for x in r.out.splitlines()])

    ids: List[str] = [s.id for s in stack.subscribers]
    if not all_ok(par_run(c, start, ids)):
        raise Abort(1)
    try:
        for s in stack.subscribers:
            c.out(f"  Running sockperf under-load -> {s.id} ({s.private_ip})...")
            r = tp.run(stack.publisher.id, secs + 30,
                       f"sockperf under-load --ip {s.private_ip} --port {cfg.port} {client_port}--mps {cfg.rate} "
                       f"--time {secs} --reply-every 1 --full-rtt 2>&1")
            text = r.out if r.ok else f"ERROR: sockperf did not complete (status: {r.status or 'unknown'})"
            c.out(f"--- SUBSCRIBER {s.id} ---")
            c.out(text)
            c.out(f"--- END {s.id} ---")
            c.out("")
    finally:
        # A sockperf server left bound to the port would block the next multicast receiver.
        par_run(c, lambda iid: Outcome(iid, True, data=tp.run(iid, 30, "pkill -f 'sockperf server' || true")), ids)
