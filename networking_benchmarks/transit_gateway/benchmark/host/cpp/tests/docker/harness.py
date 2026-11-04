#!/usr/bin/env python3
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
"""Container load harness for the native TGW sender/receiver.

Runs the C++ sender and receiver on real multicast between containers on a user-defined
bridge, under several load profiles. Results are checked against independent ground
truth (kernel interface counters, SO_RXQ_OVFL / UdpRcvbufErrors, the netem qdisc's own
packet counts, the sender's own count) rather than only against the receiver's output.

Scope and limits (also printed in the report):
  * veth has no hardware timestamps. The receiver runs with a SYNTHETIC hardware
    stamp (hw = sw - 700 ns): the C++ test build (make synth). This validates the pipeline, accounting and capacity. It says nothing about the
    real ENA hardware stamp or real latency.
  * Container latencies are a shared-VM artifact. Do not read them as transit time.

Usage: harness.py [--only NAME[,NAME...]] [--out DIR]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent.parent            # host/cpp
REPORT = NATIVE.parent.parent / "report"   # benchmark/report
IMAGE = os.environ.get("TGW_TEST_IMAGE", "tgw-native-test")
PLATFORM = os.environ.get("TGW_TEST_PLATFORM", "")   # e.g. linux/amd64 (emulated)
NET = "tgwnet"
SUBNET = "10.77.0.0/24"
GROUP, PORT = "239.1.1.1", 5001
RX_TIMEOUT = 5                         # receiver silence timeout (s)
SYNTH_DIV_US = 0.700

sys.path.insert(0, str(REPORT))        # the real downstream parser: save_report.py


def sh(args, timeout=None, check=True):
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, args))[:200]}\n{r.stdout}\n{r.stderr}")
    return r


def dexec(container, cmd, detach=False, timeout=None, check=True):
    a = ["docker", "exec"] + (["-d"] if detach else []) + [container, "sh", "-c", cmd]
    return sh(a, timeout=timeout, check=check)


class Lab:
    """Containers on one bridge network. Each gets its own CPU set so they do not compete."""

    def __init__(self, out: Path):
        self.out = out
        self.names = []
        self.ip = {}

    def up(self, name, cpuset, cpus=None):
        sh(["docker", "rm", "-f", name], check=False)
        cmd = ["docker", "run", "-d", "--name", name, "--network", NET, "--cap-add", "NET_ADMIN"]
        cmd += ["--platform", PLATFORM] if PLATFORM else []
        cmd += [

               "--cpuset-cpus", cpuset, "-v", f"{NATIVE}:/native:ro"
               "-v", f"{self.out}:/out"]
        if cpus:
            cmd += ["--cpus", str(cpus)]
        sh(cmd + [IMAGE, "sleep", "infinity"])
        self.names.append(name)
        self.ip[name] = sh(["docker", "inspect", "-f",
                            f'{{{{(index .NetworkSettings.Networks "{NET}").IPAddress}}}}', name]).stdout.strip()

    def down(self, *names):
        for n in names or list(self.names):
            sh(["docker", "rm", "-f", n], check=False)
            if n in self.names:
                self.names.remove(n)

    # -- receivers ---------------------------------------------------------
    def rx_cmd(self, impl, timeout=RX_TIMEOUT):
        return f"env MCAST_WARMUP_SECS={getattr(self, 'warmup', 0)} /out/bin/mcast_recv_synth {GROUP} {PORT} {timeout} eth0"

    def rx_start(self, c, impl, tag, timeout=RX_TIMEOUT):
        dexec(c, "rm -f /tmp/mcast_recv.ready /tmp/mcast_recv_detail.json")
        self.rx_pkts0 = getattr(self, "rx_pkts0", {})
        self.rx_pkts0[c] = self.iface_rx(c)
        cmd = f'/usr/bin/time -f "%U %S %e" -o /out/{tag}_{c}.time {self.rx_cmd(impl, timeout)} > /out/{tag}_{c}.json 2>&1'
        dexec(c, cmd, detach=True)

    def rx_ready(self, c, impl, wait=8.0):
        t0 = time.time()
        while time.time() - t0 < wait:
            if dexec(c, "test -f /tmp/mcast_recv.ready", check=False).returncode == 0:
                return True
            time.sleep(0.05)
        return False

    def rx_wait(self, c, tag, limit):
        f = self.out / f"{tag}_{c}.json"
        t0 = time.time()
        while time.time() - t0 < limit:
            if f.exists() and f.stat().st_size > 0 and f.read_text().endswith("\n"):
                time.sleep(0.2)
                return True
            time.sleep(0.2)
        return False

    def iface_rx(self, c):
        j = json.loads(dexec(c, "ip -s -j link show eth0").stdout)
        return j[0]["stats64"]["rx"]["packets"]

    def rx_collect(self, c, impl, tag):
        res = {"container": c, "impl": impl, "tag": tag}
        f = self.out / f"{tag}_{c}.json"
        raw = f.read_text() if f.exists() else ""
        res["raw_stdout"] = raw
        res["lines"] = len([x for x in raw.splitlines() if x.strip()])
        try:
            res["json"] = json.loads(raw.splitlines()[0]) if raw.strip() else None
        except json.JSONDecodeError:
            res["json"] = None
        tf = self.out / f"{tag}_{c}.time"
        if tf.exists():
            try:
                u, s, e = tf.read_text().split()[-3:]
                res["cpu_s"] = float(u) + float(s)
                res["wall_s"] = float(e)
            except ValueError:
                pass
        res["iface_rx_delta"] = self.iface_rx(c) - self.rx_pkts0.get(c, 0)
        if impl == "cpp":
            r = dexec(c, f"cat /tmp/mcast_recv_detail.json", check=False)
            try:
                res["detail"] = json.loads(r.stdout)
            except json.JSONDecodeError:
                res["detail"] = None
        return res

    # -- senders -----------------------------------------------------------
    def tx_cmd(self, impl, rate, dur, payload=12, src=None):
        src = src or self.ip["tx"]
        extra = f" {payload}" if payload != 12 else ""
        return f"/out/bin/mcast_send {GROUP} {PORT} {rate} {dur} {src}{extra}"

    def tx_run(self, impl, rate, dur, tag, payload=12):
        cmd = f'/usr/bin/time -f "%U %S %e" -o /out/{tag}_tx.time {self.tx_cmd(impl, rate, dur, payload)}'
        r = dexec("tx", cmd, timeout=dur + 60, check=False)
        return self.tx_parse(impl, r.stdout, tag, rate, dur, r.returncode)

    def tx_start(self, impl, rate, dur, tag, payload=12):
        cmd = f'/usr/bin/time -f "%U %S %e" -o /out/{tag}_tx.time {self.tx_cmd(impl, rate, dur, payload)} > /out/{tag}_tx.json 2>&1'
        dexec("tx", cmd, detach=True)

    def tx_finish(self, impl, rate, dur, tag, wait):
        f = self.out / f"{tag}_tx.json"
        t0 = time.time()
        while time.time() - t0 < wait:
            if f.exists() and f.stat().st_size > 0 and f.read_text().endswith("\n"):
                break
            time.sleep(0.2)
        return self.tx_parse(impl, f.read_text() if f.exists() else "", tag, rate, dur, 0)

    def tx_parse(self, impl, stdout, tag, rate, dur, rc):
        res = {"impl": impl, "rate": rate, "duration": dur, "rc": rc}
        try:
            res.update(json.loads(stdout.strip().splitlines()[-1]))
        except (ValueError, IndexError):
            res["parse_error"] = stdout[:200]
        tf = self.out / f"{tag}_tx.time"
        if tf.exists():
            try:
                u, s, e = tf.read_text().split()[-3:]
                res["cpu_s"] = float(u) + float(s)
                res["wall_s"] = float(e)
            except ValueError:
                pass
        if "sent" in res and dur:
            res["avg_pps"] = res["sent"] / dur
            if rate:
                res["avg_pct"] = 100.0 * res["sent"] / (rate * dur)
        return res


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

class Checks:
    def __init__(self):
        self.rows = []

    def add(self, scope, name, ok, detail=""):
        self.rows.append({"scope": scope, "check": name, "ok": bool(ok), "detail": detail})

    def failures(self):
        return [r for r in self.rows if not r["ok"]]


def check_cpp_rx(ch, scope, rx, tx=None, expect_clean=True):
    j = rx["json"]
    ch.add(scope, "single flat JSON line", rx["lines"] == 1 and j is not None,
           f"lines={rx['lines']}")
    if not j or "error" in j:
        ch.add(scope, "no error", False, str(j)[:200])
        return
    line = rx["raw_stdout"].splitlines()[0]
    try:
        import save_report
        import results
        block = ("--- MCAST i-0test ---\n" + json.dumps({"subscriber_id": "i-0test", "az": "a", "az_relation": "same-az",
                                                          "result": json.loads(line)}) + "\n--- END MCAST i-0test ---\n")
        parsed = results.parse_subscribers(block)
        rep = save_report.generate_report({"stack_name": "t"}, parsed, [], "unit", "single-az-cpg", "",
                                          __import__("datetime").datetime.now())
        ch.add(scope, "save_report.py parses and renders it", len(parsed) == 1 and parsed[0].ok and "Phase A" in rep)
    except Exception as e:  # noqa: BLE001
        ch.add(scope, "save_report.py parses and renders it", False, repr(e))
    ch.add(scope, "accounting: received + loss == expected",
           j["total_received"] + j["packet_loss_count"] == j["total_expected"] or j["received_exceeds_expected"],
           f"{j['total_received']}+{j['packet_loss_count']} vs {j['total_expected']}")
    ch.add(scope, "divergence plumbing: sw-hw == 0.700 us on every sample",
           j.get("div_min_us") == SYNTH_DIV_US and j.get("div_max_us") == SYNTH_DIV_US,
           f"min={j.get('div_min_us')} max={j.get('div_max_us')}")
    # Both ends share one VM clock, so a negative latency should not occur. The Docker Desktop VM clock
    # is disciplined by its host and was measured stepping by 0.1-0.7 ms, in both directions, roughly every
    # 10-30 s while idle, so an occasional negative sample is the environment, not the code: a parsing
    # or offset bug would make negatives common. Tolerate up to 1 per 100,000 samples and say so.
    neg = j["negative_hw_samples"]
    allowed = max(1, j["latency_samples"] // 100000)
    ch.add(scope, f"negative latencies are rare (<= {allowed}; VM clock steps, see README)",
           neg <= allowed and j["negative_sw_samples"] <= allowed,
           f"hw={neg} sw={j['negative_sw_samples']} of {j['latency_samples']}; most negative {j['most_negative_hw_us']} us")
    raw = j["packets_received_raw"] + j["end_markers_seen"] + j["short_packets"]
    # Ground truth: the kernel's own interface counter. It also counts IGMP and ARP, hence the slack.
    # Packets dropped at the socket buffer are in the interface counter but not in `raw`.
    ch.add(scope, "interface RX == receiver raw + socket drops (+ small non-benchmark traffic)",
           0 <= rx["iface_rx_delta"] - raw - j["socket_drops"] <= 40,
           f"iface={rx['iface_rx_delta']} raw={raw} drops={j['socket_drops']}")
    d = rx.get("detail")
    ch.add(scope, "detail file valid; histogram mass == sample count",
           bool(d) and sum(c for _, c in d["hist_hw_latency"]["pos"]) + sum(c for _, c in d["hist_hw_latency"]["neg"])
           == j["latency_samples"])
    if tx and "sent" in tx and j["end_marker_received"]:
        ch.add(scope, "receiver expected == sender sent", j["total_expected"] == tx["sent"],
               f"{j['total_expected']} vs {tx['sent']}")
    if expect_clean:
        ch.add(scope, "no duplicates / reordering without impairment",
               j["duplicate_count"] == 0 and j["out_of_order_count"] == 0,
               f"dup={j['duplicate_count']} ooo={j['out_of_order_count']}")


# ---------------------------------------------------------------------------
# Profiles
# ---------------------------------------------------------------------------

def run_pair_profile(lab, ch, name, rate, dur, payload=12, senders=("cpp",), netem=None, rx_names=None):
    """One profile: the C++ sender and the receivers in rx_names (default one)."""
    rx_names = rx_names or [("rxc1", "cpp")]
    out = []
    for si in senders:
        tag = f"{name}_{si}"
        scope = f"{name}/{si}-sender"
        dexec("tx", "tc qdisc del dev eth0 root 2>/dev/null; true", check=False)
        if netem:
            dexec("tx", f"tc qdisc add dev eth0 root netem {netem}")
        for c, impl in rx_names:
            lab.rx_start(c, impl, tag)
        for c, impl in rx_names:
            lab.rx_ready(c, impl)
        tx = lab.tx_run(si, rate, dur, tag, payload)
        tcs = dexec("tx", "tc -s qdisc show dev eth0", check=False).stdout if netem else ""
        rxs = []
        for c, impl in rx_names:
            lab.rx_wait(c, tag, RX_TIMEOUT + 8)
            rxs.append(lab.rx_collect(c, impl, tag))
        if netem:
            dexec("tx", "tc qdisc del dev eth0 root", check=False)
        for rx in rxs:
            if rx["impl"] == "cpp":
                check_cpp_rx(ch, f"{scope}/{rx['container']}", rx, tx, expect_clean=not netem)
        if si == "cpp" and rate and rate <= 20000 and tx.get("sent"):
            ch.add(scope, "C++ sender holds the requested rate (>=99%)", tx.get("achieved_pct", 0) >= 99.0,
                   f"{tx.get('achieved_pct')}%")
        out.append({"profile": name, "sender": tx, "receivers": rxs, "tc": tcs, "scope": scope, "netem": netem})
        time.sleep(1.0)
    return out


def run_late_join(lab, ch):
    out = []
    for gated in (False, True):
        tag = f"late_join_{'gated' if gated else 'ungated'}"
        scope = f"late_join/{'gated' if gated else 'ungated'}"
        rate, dur, join_delay = 2000, 8, 2.0
        lab.rx_pkts0 = getattr(lab, "rx_pkts0", {})
        if gated:
            lab.rx_start("rxc1", "cpp", tag)
            ok = lab.rx_ready("rxc1", "cpp")
            ch.add(scope, "ready marker appears", ok)
            lab.tx_start("cpp", rate, dur, tag)
        else:
            lab.tx_start("cpp", rate, dur, tag)
            time.sleep(join_delay)
            lab.rx_start("rxc1", "cpp", tag)
            lab.rx_ready("rxc1", "cpp")
        tx = lab.tx_finish("cpp", rate, dur, tag, wait=dur + 15)
        lab.rx_wait("rxc1", tag, RX_TIMEOUT + 8)
        rx = lab.rx_collect("rxc1", "cpp", tag)
        j = rx["json"] or {}
        if gated:
            ch.add(scope, "gated start: zero loss", j.get("packet_loss_count") == 0, f"loss={j.get('packet_loss_count')}")
        else:
            lost = j.get("packet_loss_count", -1)
            want = rate * join_delay
            ch.add(scope, "ungated start: startup loss is counted (~ rate x join delay)",
                   0.7 * want <= lost <= 1.3 * want, f"loss={lost}, expected ~{int(want)}")
        out.append({"profile": "late_join", "sender": tx, "receivers": [rx], "scope": scope, "tc": "", "netem": None,
                    "note": "gated" if gated else f"receiver joined {join_delay}s after the sender started"})
        time.sleep(1.0)
    return out


def run_join_live_stream(lab, ch, trials=30):
    """Regression: a receiver started while traffic is already flowing must not abort.

    Linux can deliver the first packets of a new timestamping socket without stamps. One
    attempt hid this (it fired in roughly 10-17% of starts), so this repeats the join."""
    scope = "join_live_stream"
    lab.tx_start("cpp", 2000, 200, "join_live")
    time.sleep(1.5)
    aborts, invalid, valid, skipped_runs = 0, 0, 0, 0
    errs = {}
    for _ in range(trials):
        r = dexec("rxc1", f"env MCAST_WARMUP_SECS=0 timeout -s TERM 2 /out/bin/mcast_recv_synth {GROUP} {PORT} 30 eth0", check=False)
        line = (r.stdout.strip().splitlines() or [""])[0]
        try:
            j = json.loads(line)
        except ValueError:
            invalid += 1
            continue
        if "error" in j:
            aborts += 1
            errs[j["error"]] = errs.get(j["error"], 0) + 1
        elif j.get("latency_samples", 0) < 500:
            invalid += 1          # saw no traffic: proves nothing, must not count as a pass
        else:
            valid += 1
            skipped_runs += 1 if j.get("startup_unstamped_skipped") else 0
        time.sleep(0.25)
    dexec("tx", "pkill -f '^/out/bin/mcast_send'", check=False)
    ch.add(scope, f"{trials} joins of a live stream: no aborts", aborts == 0, f"aborts={aborts} {errs or ''}")
    ch.add(scope, "every trial actually saw traffic (so a pass is meaningful)", invalid == 0, f"invalid={invalid}, valid={valid}")
    return [{"profile": "join_live_stream", "sender": {"impl": "cpp"}, "receivers": [], "scope": scope, "tc": "", "netem": None,
             "note": f"{valid} valid, {aborts} aborted, {skipped_runs} needed to skip startup packets"}]


def run_no_end(lab, ch):
    tag, scope = "no_end_marker", "no_end_marker"
    lab.rx_start("rxc1", "cpp", tag, timeout=3)
    lab.rx_ready("rxc1", "cpp")
    lab.tx_start("cpp", 2000, 30, tag)
    time.sleep(4)
    dexec("tx", "pkill -9 -f '^/out/bin/mcast_send'", check=False)
    killed_at = time.time()
    lab.rx_wait("rxc1", tag, 20)
    rx = lab.rx_collect("rxc1", "cpp", tag)
    j = rx["json"] or {}
    ch.add(scope, "no end marker: reported as not received, timed out", j.get("end_marker_received") is False and j.get("timed_out") is True,
           f"end={j.get('end_marker_received')} timed_out={j.get('timed_out')}")
    ch.add(scope, "expected falls back to max_seq+1 (no loss claimed for a clean prefix)",
           j.get("total_expected") == j.get("total_received") and j.get("packet_loss_count") == 0,
           f"expected={j.get('total_expected')} received={j.get('total_received')}")
    ch.add(scope, "exits after ~timeout of silence", 2.5 <= time.time() - killed_at <= 8, f"{time.time() - killed_at:.1f}s")
    return [{"profile": "no_end_marker", "sender": {"impl": "cpp", "note": "SIGKILL after 4s"}, "receivers": [rx],
             "scope": scope, "tc": "", "netem": None}]


def run_sigterm(lab, ch):
    tag, scope = "sigterm", "sigterm"
    lab.rx_start("rxc1", "cpp", tag, timeout=30)
    lab.rx_ready("rxc1", "cpp")
    lab.tx_start("cpp", 2000, 10, tag)
    time.sleep(3)
    dexec("rxc1", "pkill -TERM -f '^/out/bin/mcast_recv_synth'", check=False)
    time.sleep(1.5)
    ready_gone = dexec("rxc1", "test -f /tmp/mcast_recv.ready", check=False).returncode != 0
    rxc = lab.rx_collect("rxc1", "cpp", tag)
    lab.tx_finish("cpp", 2000, 10, tag, wait=15)
    j = rxc["json"] or {}
    ch.add(scope, "C++ receiver prints its results on SIGTERM", j.get("interrupted") is True and j.get("latency_samples", 0) > 1000,
           f"interrupted={j.get('interrupted')} samples={j.get('latency_samples')}")
    ch.add(scope, "C++ receiver removes the ready marker", ready_gone)
    return [{"profile": "sigterm", "sender": {"impl": "cpp"}, "receivers": [rxc], "scope": scope, "tc": "", "netem": None}]


def run_slow_rx(lab, ch):
    lab.up("rxsc", "10", cpus=0.02)
    rate, dur = 50000, 10
    tag, scope = "slow_rx_50k", "slow_rx_50k/cpp-sender"
    lab.rx_start("rxsc", "cpp", tag, timeout=8)
    lab.rx_ready("rxsc", "cpp", wait=20)
    tx = lab.tx_run("cpp", rate, dur, tag)
    rxs = []
    for c, impl in (("rxsc", "cpp"),):
        lab.rx_wait(c, tag, 90)
        rxs.append(lab.rx_collect(c, impl, tag))
    j = rxs[0]["json"] or {}
    if j:
        drops, rb = j.get("socket_drops", 0), j.get("udp_rcvbuf_errors_delta", -1)
        ch.add(scope, "slow receiver really dropped packets", drops > 0, f"socket_drops={drops}")
        ch.add(scope, "SO_RXQ_OVFL drops == UdpRcvbufErrors (two independent kernel counters)",
               abs(drops - rb) <= max(5, 0.001 * drops), f"{drops} vs {rb}")
        gap = j["total_expected"] - j["total_received"]
        ch.add(scope, "reported loss attributed to the receiver (loss ~= socket drops)",
               abs(gap - drops) <= max(10, 0.01 * drops), f"loss={gap} drops={drops}")
        ch.add(scope, "interface RX == raw + drops", 0 <= rxs[0]["iface_rx_delta"] - (j["packets_received_raw"] + j["end_markers_seen"] + j["short_packets"]) - drops <= 60,
               f"iface={rxs[0]['iface_rx_delta']} raw={j['packets_received_raw']} drops={drops}")
    lab.down("rxsc")
    return [{"profile": "slow_rx_50k", "sender": tx, "receivers": rxs, "scope": scope, "tc": "", "netem": None,
             "note": "receiver limited to 0.02 CPU"}]


def run_multi(lab, ch):
    lab.up("rxc2", "8")
    names = [("rxc1", "cpp"), ("rxc2", "cpp")]
    res = run_pair_profile(lab, ch, "multi_sub_10k", 10000, 10, rx_names=names)
    for r in res:
        cp = [x["json"] for x in r["receivers"] if x["impl"] == "cpp" and x["json"]]
        if len(cp) == 2:
            ch.add(r["scope"], "both C++ subscribers got the same stream (same expected, loss within 1%)",
                   cp[0]["total_expected"] == cp[1]["total_expected"]
                   and abs(cp[0]["packet_loss_count"] - cp[1]["packet_loss_count"]) <= max(5, 0.01 * cp[0]["total_expected"]),
                   f"loss {cp[0]['packet_loss_count']} / {cp[1]['packet_loss_count']}")
    lab.down("rxc2")
    return res


def run_netem(lab, ch):
    res = run_pair_profile(lab, ch, "netem_5k", 5000, 15, netem="delay 2ms 1ms loss 2% duplicate 1% reorder 5% 50%")
    for r in res:
        for rx in r["receivers"]:
            j = rx["json"]
            if rx["impl"] != "cpp" or not j:
                continue
            sc = f"{r['scope']}/{rx['container']}"
            n = j["total_expected"]
            m = re.search(r"Sent \d+ bytes (\d+) pkt \(dropped (\d+)", r["tc"])
            if m:
                out_pkts = int(m.group(1))
                got = j["total_received"] + j["duplicate_count"] + j["end_markers_seen"]
                ch.add(sc, "EXACT: unique + duplicates + end markers == packets that left the netem qdisc",
                       got == out_pkts, f"{j['total_received']}+{j['duplicate_count']}+{j['end_markers_seen']}={got} vs tc {out_pkts}")
            ch.add(sc, "loss rate near the configured 2%", 0.012 <= j["packet_loss_count"] / n <= 0.028,
                   f"{100.0 * j['packet_loss_count'] / n:.2f}%")
            ch.add(sc, "duplicates detected (configured 1%)", 0.003 * n <= j["duplicate_count"] <= 0.02 * n,
                   f"dup={j['duplicate_count']} ({100.0 * j['duplicate_count'] / n:.2f}%)")
            ch.add(sc, "reordering detected", j["out_of_order_count"] > 0, f"ooo={j['out_of_order_count']}")
    return res


def run_max(lab, ch):
    res = []
    for si in ("cpp",):
        tag = f"unpaced_{si}"
        for c, impl in (("rxc1", "cpp"),):
            lab.rx_start(c, impl, tag)
        for c, impl in (("rxc1", "cpp"),):
            lab.rx_ready(c, impl)
        tx = lab.tx_run(si, 0, 5, tag)
        rxs = []
        for c, impl in (("rxc1", "cpp"),):
            lab.rx_wait(c, tag, RX_TIMEOUT + 10)
            rxs.append(lab.rx_collect(c, impl, tag))
        for rx in rxs:
            if rx["impl"] == "cpp":
                check_cpp_rx(ch, f"unpaced/{si}-sender/{rx['container']}", rx, tx, expect_clean=False)
        res.append({"profile": "unpaced_max", "sender": tx, "receivers": rxs, "scope": f"unpaced/{si}-sender", "tc": "",
                    "netem": None})
        time.sleep(1.0)
    return res


def run_warmup(lab, ch):
    """Default warm-up: the first seconds leave the latency statistics, not the loss accounting."""
    tag, scope = "warmup_default", "warmup_default"
    lab.warmup = 2
    try:
        lab.rx_start("rxc1", "cpp", tag, timeout=30)
        lab.rx_ready("rxc1", "cpp")
        tx = lab.tx_run("cpp", 2000, 8, tag)
        lab.rx_wait("rxc1", tag, 15)
        rx = lab.rx_collect("rxc1", "cpp", tag)
    finally:
        lab.warmup = 0
    j = rx["json"] or {}
    ch.add(scope, "receiver reports the warm-up it applied", j.get("warmup_secs") == 2.0, str(j.get("warmup_secs")))
    ch.add(scope, "about 2 s of packets left out of the latency stats (2000 pps)",
           3500 <= j.get("warmup_skipped", 0) <= 4500, str(j.get("warmup_skipped")))
    ch.add(scope, "loss accounting still sees every packet",
           j.get("total_received") == tx.get("sent") and j.get("packet_loss_count") == 0,
           f"received={j.get('total_received')} sent={tx.get('sent')}")
    ch.add(scope, "latency samples + skipped == packets received",
           j.get("latency_samples", 0) + j.get("warmup_skipped", 0) == j.get("total_received"),
           f"{j.get('latency_samples')}+{j.get('warmup_skipped')} vs {j.get('total_received')}")
    d = rx.get("detail") or {}
    h = d.get("hist_hw_latency", {})
    mass = sum(c for _, c in h.get("pos", [])) + sum(c for _, c in h.get("neg", []))
    ch.add(scope, "histogram mass == latency samples (warm-up excluded)", mass == j.get("latency_samples"), str(mass))
    return [{"profile": "warmup_default", "sender": {"impl": "cpp"}, "receivers": [rx], "scope": scope, "tc": "", "netem": None}]


PROFILES = [
    ("warmup_default", run_warmup),
    ("base_1k", lambda lab, ch: run_pair_profile(lab, ch, "base_1k", 1000, 15)),
    ("rate_10k", lambda lab, ch: run_pair_profile(lab, ch, "rate_10k", 10000, 15)),
    ("rate_50k", lambda lab, ch: run_pair_profile(lab, ch, "rate_50k", 50000, 10)),
    ("rate_100k", lambda lab, ch: run_pair_profile(lab, ch, "rate_100k", 100000, 10)),
    ("payload_1400", lambda lab, ch: run_pair_profile(lab, ch, "payload_1400_10k", 10000, 10, payload=1400)),
    ("netem", run_netem),
    ("multi_sub", run_multi),
    ("slow_rx", run_slow_rx),
    ("unpaced", run_max),
    ("late_join", run_late_join),
    ("join_live", run_join_live_stream),
    ("no_end", run_no_end),
    ("sigterm", run_sigterm),
]


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def f(v, nd=1):
    return "-" if v is None else (f"{v:.{nd}f}" if isinstance(v, float) else str(v))


def report(runs, ch, out: Path):
    L = ["# Native sender/receiver: container load results", "",
         "C++ sender and receiver (`host/cpp/`). The hardware stamp is SYNTHETIC (sw - 700 ns). Container latencies are a",
         "shared-VM artifact; read counts, accounting and capacity, not microseconds.", "",
         "## Sender", "", "| profile | sender | requested pps | sent | achieved % | CPU s (user+sys) | late max us |",
         "|---|---|---|---|---|---|---|"]
    for r in runs:
        t = r["sender"]
        if not t or t.get("impl") is None:
            continue
        pct = None if not t.get("rate") else t.get("achieved_pct", t.get("avg_pct"))
        L.append(f"| {r['scope'].split('/')[0]} | {t['impl']} | {t.get('rate', '-') or 'unpaced'} | {f(t.get('sent'))} | "
                 f"{f(pct)} | {f(t.get('cpu_s'), 2)} | {f(t.get('late_max_us') if t.get('rate') else None)} |")
    L += ["", "## Receivers", "",
          "| profile / sender | rx | expected | unique | loss | sock drops | dup | ooo | p50 us | p99 us | p99.9 us | max us | div p50 us | CPU s |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in runs:
        for rx in r["receivers"]:
            j = rx["json"] or {}
            lab = f"{r['scope']} / {rx['container']}"
            if j.get("error") or not j:
                L.append(f"| {lab} | {rx['impl']} | (no result) | | | | | | | | | | | |")
                continue
            L.append(f"| {lab} | {rx['impl']} | {j.get('total_expected')} | {j.get('total_received')} | {j.get('packet_loss_count')} | "
                     f"{f(j.get('socket_drops'))} | {f(j.get('duplicate_count'))} | {f(j.get('out_of_order_count'))} | "
                     f"{f(j.get('median_latency_us'))} | {f(j.get('p99_latency_us'))} | {f(j.get('p999_latency_us'))} | "
                     f"{f(j.get('max_latency_us'), 0)} | {f(j.get('div_median_us'), 3)} | {f(rx.get('cpu_s'), 2)} |")
    L += ["", "## Checks", "", f"{sum(1 for c in ch.rows if c['ok'])} passed, {len(ch.failures())} failed of {len(ch.rows)}.", ""]
    if ch.failures():
        L += ["### Failures", "", "| scope | check | detail |", "|---|---|---|"]
        L += [f"| {c['scope']} | {c['check']} | {c['detail']} |" for c in ch.failures()]
        L.append("")
    L += ["### All checks", "", "| result | scope | check | detail |", "|---|---|---|---|"]
    L += [f"| {'PASS' if c['ok'] else '**FAIL**'} | {c['scope']} | {c['check']} | {c['detail']} |" for c in ch.rows]
    (out / "summary.md").write_text("\n".join(L) + "\n")
    slim = [{k: v for k, v in r.items() if k != "receivers"} | {"receivers": [
        {k: v for k, v in rx.items() if k not in ("raw_stdout", "detail")} for rx in r["receivers"]]} for r in runs]
    (out / "results.json").write_text(json.dumps({"runs": slim, "checks": ch.rows}, indent=1, default=str))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    out = Path(a.out or f"/tmp/native_results/{time.strftime('%Y%m%d-%H%M%S')}")
    (out / "bin").mkdir(parents=True, exist_ok=True)
    sh(["docker", "network", "inspect", NET], check=False).returncode == 0 or sh(
        ["docker", "network", "create", "--subnet", SUBNET, NET])
    lab = Lab(out)
    ch, runs = Checks(), []
    only = {x for x in a.only.split(",") if x}
    try:
        lab.up("tx", "2-3")
        lab.up("rxc1", "4-5")
        b = dexec("tx", "cd /native && make -s BUILD=/out/bin all synth 2>&1", check=False)
        ch.add("build", "native sources compile warning-free on Linux", b.returncode == 0 and not b.stdout.strip(), b.stdout[:300])
        ut = dexec("tx", "cd /native && make -s BUILD=/out/bin stats_test 2>&1 | tail -1", check=False)
        ch.add("build", "unit tests pass on Linux", " 0 failed" in ut.stdout, ut.stdout.strip())
        # The shipped receiver must refuse to run without a real hardware stamp.
        ns = dexec("rxc1", f"/out/bin/mcast_recv {GROUP} {PORT} 2 eth0", check=False)
        ch.add("build", "shipped receiver (no synthetic hw) refuses to run on veth",
               ns.returncode == 1 and "SIOCSHWTSTAMP failed" in ns.stdout, ns.stdout[:120])
        for name, fn in PROFILES:
            if only and name not in only:
                continue
            print(f"== {name}", flush=True)
            runs += fn(lab, ch)
            report(runs, ch, out)
    finally:
        lab.down()
    report(runs, ch, out)
    print(f"\nresults: {out}\n{sum(1 for c in ch.rows if c['ok'])}/{len(ch.rows)} checks passed")
    return 0 if not ch.failures() else 1


if __name__ == "__main__":
    sys.exit(main())
