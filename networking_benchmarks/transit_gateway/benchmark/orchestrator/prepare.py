"""Stage 2: provision and verify each instance once, then record it in the manifest.

Done here, not per run: wait for the environment, read the native build, install the helper
scripts under /opt/benchmark/bin, check the static clock and NIC prerequisites, record the host
context. A run repeats none of it; it only confirms (boot id, helper hashes) that this still holds.
"""
from __future__ import annotations

import base64
import os
import re
from typing import Any, Dict, List, Optional

from . import manifest as mf
from .context import Ctx
from .model import load_outputs, public_ips
from .parallel import Outcome, all_ok, par_run
from .transport import SshTransport
from .util import Abort, sleep

READY_POLL_INTERVAL = 10
READY_POLL_MAX_ATTEMPTS = 60
READY_MAX_SILENT_ATTEMPTS = 12   # consecutive polls with no output at all: the instance is unreachable
# Context keys that change from second to second; the per-run clock blocks carry them instead.
DYNAMIC_CONTEXT_KEYS = ("chrony_system_time_us",)

READY_CMD = """if [ -f /opt/benchmark-env-ready ] && command -v sockperf >/dev/null 2>&1; then echo ENV=READY; else echo ENV=NOTREADY; fi
if [ -f /opt/benchmark-native-ready ]; then echo NATIVE=ready; echo "NATIVE_BUILD=$(cat /opt/benchmark-native/BUILD_INFO 2>/dev/null)"
elif [ -f /opt/benchmark-native-failed ]; then echo NATIVE=failed; else echo NATIVE=absent; fi
echo "BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)"
T=$(curl -fsS -m 2 -X PUT http://169.254.169.254/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 60' 2>/dev/null)
imds() { curl -fsS -m 2 -H "X-aws-ec2-metadata-token: $T" "http://169.254.169.254/latest/meta-data/$1" 2>/dev/null; }
echo "IID=$(imds instance-id)"; echo "AZ=$(imds placement/availability-zone)"; echo "PRIVATE_IP=$(imds local-ipv4)"
echo "AMI=$(imds ami-id)"; echo "ITYPE=$(imds instance-type)"
"""


def upload_command(bench_dir: str, name: str) -> str:
    """One file per command: a single SSM command stays well under the service's payload limit."""
    with open(os.path.join(bench_dir, mf.HELPER_SOURCES[name]), "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    dest = f"{mf.REMOTE_BIN}/{name}"
    return f"mkdir -p {mf.REMOTE_BIN} && echo '{b64}' | base64 -d > {dest} && chmod +x {dest} && echo UPLOADED"


STATIC_CMD = "\n".join(["echo '=====STATIC====='",
                        f"bash {mf.REMOTE_BIN}/preflight.sh --static",
                        "echo '=====CONTEXT====='",
                        f"bash {mf.REMOTE_BIN}/preflight.sh --context"])


def parse_kv(text: str) -> Dict[str, str]:
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def explain_preflight_failure(ctx: Ctx, stack_name: str) -> None:
    ctx.console.err("")
    ctx.console.err("ERROR: preflight failed. Either hardware timestamps are unavailable or the clock is not")
    ctx.console.err("       disciplined to the local PHC; the one-way measurement would be invalid.")
    ctx.console.err(f"       Run: python3 -m orchestrator check --stack-name {stack_name} for diagnostics.")


def _ready_worker(ctx: Ctx, iid: str) -> Outcome:
    silent = 0
    for attempt in range(1, READY_POLL_MAX_ATTEMPTS + 1):
        r = ctx.transport.run(iid, 30, READY_CMD)
        silent = silent + 1 if not r.out else 0
        if silent >= READY_MAX_SILENT_ATTEMPTS:
            return Outcome(iid, False, err=[f"ERROR: {iid} returned no output for {silent} consecutive polls; "
                                            "it is unreachable over the selected transport"])
        if "ENV=READY" in r.out:
            kv = parse_kv(r.out)
            if kv.get("IID") != iid:
                return Outcome(iid, False, err=[f"ERROR: {iid} reports instance id '{kv.get('IID', '')}' "
                                                "from its metadata service"])
            return Outcome(iid, True, out=[f"  {iid} - environment ready"],
                           data={"boot_id": kv.get("BOOT_ID", ""), "native_state": kv.get("NATIVE", "unknown"),
                                 "native_build": kv.get("NATIVE_BUILD", ""),
                                 "az": kv.get("AZ", ""), "private_ip": kv.get("PRIVATE_IP", ""),
                                 "ami": kv.get("AMI", ""), "itype": kv.get("ITYPE", "")})
        if attempt < READY_POLL_MAX_ATTEMPTS:
            sleep(READY_POLL_INTERVAL)
    return Outcome(iid, False, err=[f"ERROR: {iid} environment not ready after "
                                    f"{READY_POLL_MAX_ATTEMPTS * READY_POLL_INTERVAL}s"])


def _install_worker(ctx: Ctx, iid: str) -> Outcome:
    for name in mf.HELPERS:
        r = ctx.transport.run(iid, 30, upload_command(ctx.bench_dir, name))
        if "UPLOADED" not in r.out:
            return Outcome(iid, False, err=[f"ERROR: could not install {name} on {iid}"])
    r = ctx.transport.run(iid, 90, STATIC_CMD)
    static, _, context = r.out.partition("=====CONTEXT=====")
    static = static.partition("=====STATIC=====")[2]
    if not re.search(r"^PASS", static, re.M):
        return Outcome(iid, False, err=[f"  FAIL {iid} - preflight failed:"] + [f"    {x}" for x in static.splitlines()])
    ctxd = {k: v for k, v in parse_kv(context).items() if k not in DYNAMIC_CONTEXT_KEYS}
    return Outcome(iid, True, out=[f"  {iid} - helpers installed, static checks passed"], data=ctxd)


def bind_transport(ctx: Ctx, ids: List[str], public: Optional[Dict[str, Optional[str]]]) -> None:
    """Give the transport what it needs to reach the instances, and open it."""
    if isinstance(ctx.transport, SshTransport):
        ips = public or {}
        missing = [i for i in ids if not ips.get(i)]
        if missing:
            ctx.console.err(f"ERROR: {' '.join(missing)} has no public IP; use --transport ssm")
            raise Abort(1)
        ctx.transport.public_ips = dict(ips)
    ctx.transport.open(ids)


def prepare(ctx: Ctx) -> mf.Manifest:
    """Resolve the stack, verify and provision every instance, and save the manifest."""
    c = ctx.console
    c.out(">>> Preparing the stack (one-time setup per deployment)...")
    c.out(">>> Querying CloudFormation stack outputs...")
    out = load_outputs(ctx.cfg.stack_name)
    ids = out.all_ids
    public = public_ips(ids) if ctx.cfg.transport == "ssh" else None
    bind_transport(ctx, ids, public)
    ctx.transport.wait_online(ids)
    c.out(">>> Waiting for benchmark environment readiness on all instances...")
    ready = par_run(c, lambda i: _ready_worker(ctx, i), ids)
    if not all_ok(ready):
        raise Abort(1)
    c.out(">>> Installing helpers and checking clock and NIC prerequisites...")
    installed = par_run(c, lambda i: _install_worker(ctx, i), ids)
    if not all_ok(installed):
        explain_preflight_failure(ctx, ctx.cfg.stack_name)
        raise Abort(1)
    hashes = mf.local_hashes(ctx.bench_dir)
    instances: Dict[str, Dict[str, Any]] = {}
    for r, i in zip(ready, installed):
        d = r.data
        instances[r.id] = {"boot_id": d["boot_id"], "helpers": hashes, "context": i.data,
                           "native": {"state": d["native_state"], "build": d["native_build"]},
                           "az": d["az"], "private_ip": d["private_ip"], "ami": d["ami"], "itype": d["itype"],
                           "public_ip": (public or {}).get(r.id)}
    m = mf.new({"stack_id": out.stack_id, "region": out.region, "bucket": out.bucket, "group": out.group,
                "port": out.port, "publisher": out.publisher_id, "subscribers": out.subscriber_ids}, instances)
    mf.save(ctx.state_dir, ctx.cfg.stack_name, m)
    c.out(f"  Manifest saved: {mf.path_for(ctx.state_dir, ctx.cfg.stack_name)}")
    c.out("")
    ctx.mark("prepare")
    return m


def native_problems(m: mf.Manifest, ids: List[str]) -> Dict[str, str]:
    """{instance: state} for every instance whose native build is not ready."""
    return {i: m["instances"][i]["native"]["state"] for i in ids if m["instances"][i]["native"]["state"] != "ready"}
