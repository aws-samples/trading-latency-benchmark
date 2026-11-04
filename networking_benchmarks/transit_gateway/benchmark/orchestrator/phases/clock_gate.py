"""Per-run setup, one call per instance: confirm the prepared state still holds, then gate the clock.

The clock gate is never cached. It forces a chrony step and requires System time within the
threshold, because it validates the measurement that follows.
"""
from __future__ import annotations

import re
from typing import Dict, List, Tuple

from .. import manifest as mf
from ..context import Ctx
from ..model import Stack
from ..parallel import Outcome, par_run

MARK = "=====GATE====="


def gate_command(max_offset_us: str) -> str:
    names = " ".join(mf.HELPERS)
    return "\n".join([
        'echo "BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)"',
        f"(cd {mf.REMOTE_BIN} 2>/dev/null && sha256sum {names} 2>/dev/null) "
        "| awk '{print \"SHA_\" $2 \"=\" $1}'",
        f"echo '{MARK}'",
        f"bash {mf.REMOTE_BIN}/preflight.sh --gate --max-offset-us {max_offset_us}",
    ])


def _worker(ctx: Ctx, iid: str) -> Outcome:
    r = ctx.transport.run(iid, 90, gate_command(ctx.cfg.max_clock_offset_us))
    head, _, gate = r.out.partition(MARK)
    boot = next((m.group(1) for m in re.finditer(r"^BOOT_ID=(.*)$", head, re.M)), "")
    hashes = {m.group(1): m.group(2) for m in re.finditer(r"^SHA_(\S+)=(\S+)$", head, re.M)}
    passed = bool(re.search(r"^PASS", gate, re.M))
    return Outcome(iid, passed, data={"boot_id": boot, "hashes": hashes, "gate": gate})


def run_gate(ctx: Ctx, stack: Stack, manifest: mf.Manifest) -> Tuple[List[Outcome], Dict[str, List[str]]]:
    """Returns the per-instance outcomes and {instance: reasons} for instances that need prepare."""
    outcomes = par_run(ctx.console, lambda i: _worker(ctx, i), stack.all_ids)
    stale: Dict[str, List[str]] = {}
    for o in outcomes:
        reasons = mf.instance_problems(manifest["instances"][o.id], o.data["boot_id"], o.data["hashes"])
        if reasons:
            stale[o.id] = reasons
    return outcomes, stale
