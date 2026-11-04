"""`python3 -m orchestrator check`: PTP and clock diagnostic for every instance.

Standalone and read-only. It does not need `prepare`, so it can run on a freshly deployed stack or
between benchmark runs. The instance side only dumps raw facts; everything is interpreted here.
"""
from __future__ import annotations

import re
from typing import Dict, List, Optional

from . import cli
from .context import Ctx
from .parallel import Outcome, par_run
from .session import connect, session
from .util import Abort

# Raw sections, one per fact. Nothing is interpreted on the instance.
REMOTE_CMD = r"""section() { printf '=== %s ===\n' "$1"; }
section ptp; for f in /sys/class/ptp/*/clock_name; do [ -f "$f" ] && echo "$(basename "$(dirname "$f")") $(cat "$f")"; done
section symlink; ls -la /dev/ptp_ena 2>&1
section phc_enable; cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo 0
section hw_ts; for d in /sys/bus/pci/devices/*/hw_packet_timestamping_state; do [ -f "$d" ] && echo "$(basename "$(dirname "$d")") $(cat "$d")"; done
section ethtool; IFACE=$(ip -o -4 route show to default | awk '{print $5}' | head -1); echo "iface $IFACE"; ethtool -T "$IFACE" 2>&1
section sources; chronyc sources 2>&1
section tracking; chronyc tracking 2>&1
true"""

COLUMNS = (("Instance", 21), ("Role", 11), ("PHC dev", 10), ("phc_en", 7), ("/dev/ena", 9), ("hw_ts", 9),
           ("hw_rx", 7), ("Active source", 16), ("Sync", 8), ("SysTime us", 13), ("RMS offset", 13))


def split_sections(text: str) -> Dict[str, str]:
    """{section name: body} from the '=== name ===' markers."""
    out: Dict[str, str] = {}
    name: Optional[str] = None
    for line in text.splitlines():
        m = re.fullmatch(r"=== (\w+) ===", line)
        if m:
            name = m.group(1)
            out[name] = ""
        elif name:
            out[name] += line + "\n"
    return out


def summarize(text: str) -> Dict[str, str]:
    """The table row for one instance, from the raw sections."""
    sec = split_sections(text)
    ptp = [ln.split(None, 1) for ln in sec.get("ptp", "").splitlines() if ln.strip()]
    phc_dev = next((f"/dev/{d}" for d, *n in ptp if n and "ena-ptp" in n[0]), "")
    hw_ts = next((ln.split()[1] for ln in sec.get("hw_ts", "").splitlines() if len(ln.split()) > 1), "?")
    ethtool = sec.get("ethtool", "")
    active_line = next((ln for ln in sec.get("sources", "").splitlines() if re.match(r"[#^]\*", ln)), "")
    active = active_line.split()[1] if len(active_line.split()) > 1 else "none"
    sync = "PTP" if "PHC" in active_line else "NTP" if "169.254" in active_line else "UNKNOWN"
    tracking = sec.get("tracking", "")
    m = re.search(r"^System time\s*:\s*([0-9.]+) seconds (\w+)", tracking, re.M)
    sys_us = f"{float(m.group(1)) * 1e6 * (-1 if m.group(2) == 'slow' else 1):.3f}" if m else "?"
    r = re.search(r"^RMS offset\s*:\s*([0-9.]+)", tracking, re.M)
    return {
        "phc_dev": phc_dev, "phc_enable": sec.get("phc_enable", "").strip() or "?",
        "symlink": "no" if (not sec.get("symlink", "").strip() or "No such file" in sec["symlink"]) else "yes",
        "hw_ts": hw_ts, "hw_rx": "yes" if "hardware-receive" in ethtool else "no",
        "active": active, "sync": sync, "sys_us": sys_us, "rms": r.group(1) if r else "?",
    }


def exceeds(sys_us: str, limit: float) -> bool:
    """True when the System time is unreadable or its magnitude is above the limit."""
    try:
        return abs(float(sys_us)) > limit
    except ValueError:
        return True


def _worker(ctx: Ctx, iid: str) -> Outcome:
    r = ctx.transport.run(iid, 30, REMOTE_CMD)
    return Outcome(iid, r.ok, data=r.out)


def check_main(argv: Optional[List[str]] = None) -> int:
    cfg = cli.parse_check_args(argv)
    try:
        with session(cfg) as ctx:
            c = ctx.console
            out = connect(ctx)
            roles = {out.publisher_id: "publisher", **{i: "subscriber" for i in out.subscriber_ids}}
            c.out(f"Probing {len(out.all_ids)} instances...")
            outcomes = par_run(c, lambda i: _worker(ctx, i), out.all_ids)
            rows = []
            for o in outcomes:
                if cfg.verbose:
                    c.out("=" * 44)
                    c.out(f"Instance: {o.id} ({roles[o.id]})")
                    c.out("=" * 44)
                    c.out(o.data or "(no output - command failed or timed out)")
                    c.out("")
                s = summarize(o.data or "")
                rows.append((o.id, roles[o.id], s))
            width = sum(w + 1 for _, w in COLUMNS) - 1
            c.out("")
            c.out(" PTP SUMMARY ".center(width, "="))
            c.out(" ".join(f"{name:<{w}}" for name, w in COLUMNS))
            c.out(" ".join("-" * (w - 1) for _, w in COLUMNS))
            for iid, role, s in rows:
                cells = (iid, role, s["phc_dev"] or "?", s["phc_enable"], s["symlink"], s["hw_ts"], s["hw_rx"],
                         s["active"], s["sync"], s["sys_us"], s["rms"])
                c.out(" ".join(f"{v:<{w}}" for v, (_, w) in zip(cells, COLUMNS)))
            c.out("")
            c.out("Legend:")
            c.out("  hw_ts   - /sys/bus/pci/devices/*/hw_packet_timestamping_state (ENA driver)")
            c.out("  hw_rx   - ethtool -T advertises 'hardware-receive' on the primary NIC")
            c.out("  Sync=PTP means chronyc shows a PHC reference as the active source.")
            c.out(f"  SysTime = chrony System time: the clock error right now. Exit status is non-zero above "
                  f"{cfg.max_sys_us} us.")
            c.out("")
            # hw_ts of '0' or '?' is not a failure: the receiver enables hardware stamping when it starts.
            limit = float(cfg.max_sys_us)
            bad = [iid for iid, _, s in rows if s["sync"] != "PTP" or exceeds(s["sys_us"], limit)]
            if bad:
                c.err(f"FAIL: not synced to the PHC within {cfg.max_sys_us} us: {' '.join(bad)}")
            return 1 if bad else 0
    except Abort as a:
        return a.rc
