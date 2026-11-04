#!/usr/bin/env python3
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
"""Shared parsing, validation and aggregation for TGW multicast benchmark output.

The orchestrator prints a raw text stream. save_report.py (markdown report) and
collect_results.py (S3 / CloudWatch) both read it through this module, so the two
cannot disagree about what a run contains or whether it is valid.

Raw format:

    Run-ID: <id>
    >>> Phase A: Multicast ...
    --- SENDER <instance> ---           one JSON object (the native sender's summary)
    --- END SENDER <instance> ---
    --- MCAST <instance> ---            one JSON object per subscriber, ALWAYS present
    {"subscriber_id": ..., "az": ..., "az_relation": ..., "result": {...receiver json...}}
    --- END MCAST <instance> ---
    --- CLOCK <instance> <pre|post> ---  key=value lines (chrony System time, ...)
    --- END CLOCK ---
    --- CONTEXT <instance> ---          key=value lines: kernel, driver, CPU, idle states, ...
    --- END CONTEXT ---


What counts as a failed or invalid run, and why, is in `evaluate()`.

Percentiles of a population come from merging the receivers' histograms, never from
averaging per-subscriber percentiles. The histogram layout must match
host/cpp/lat_stats.hpp (LogHist / SignedHist).
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Thresholds. Each has a reason; none is a precision knob.
# ---------------------------------------------------------------------------

# rejects pairs above 2% loss: percentiles are computed from the datagrams that
# returned, so a lossy run reports the distribution of a non-random subset.
DEFAULT_MAX_LOSS_PCT = 2.0
# multicast clock gate. Converged nodes measure tens to hundreds of
# nanoseconds of chrony "System time"; 10 us is ~100x that and ~6% of this
# benchmark's ~170 us signal.
DEFAULT_MAX_CLOCK_OFFSET_US = 10.0
# Below this share of the requested rate the offered load is not the load requested
DEFAULT_MIN_ACHIEVED_PCT = 90.0
# Provisional. Healthy hosts on the tuned fleet show a software-minus-hardware
# gap of 0.1-2.5 us; this stack is untuned and unmeasured, so this only warns.
DEFAULT_MAX_DIV_MEDIAN_US = 20.0
# A p99.9 rests on the slowest 0.1% of samples; with fewer than this many it is ~100 points
# or fewer and mostly noise, so it is not reported.
MIN_SAMPLES_FOR_P999 = 100_000


# ---------------------------------------------------------------------------
# Histogram (must match host/cpp/lat_stats.hpp)
# ---------------------------------------------------------------------------

SUB_BITS = 10
SUB = 1 << SUB_BITS
MAX_BITS = 44


def hist_lower(idx: int) -> int:
    if idx < SUB:
        return idx
    shift = idx // SUB - 1
    top = SUB + (idx % SUB)
    return top << shift


def hist_width(idx: int) -> int:
    return 1 if idx < SUB else 1 << (idx // SUB - 1)


def hist_representative(idx: int) -> int:
    w = hist_width(idx)
    return hist_lower(idx) + (w // 2 if w > 1 else 0)


@dataclass
class SignedHistogram:
    """Mergeable signed histogram in nanoseconds (the detail file's wire form)."""

    pos: Dict[int, int] = field(default_factory=dict)
    neg: Dict[int, int] = field(default_factory=dict)
    count: int = 0
    total: float = 0.0
    min: Optional[int] = None
    max: Optional[int] = None

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "SignedHistogram":
        if d.get("sub_bits") != SUB_BITS or d.get("max_bits") != MAX_BITS:
            raise ValueError(f"histogram layout {d.get('sub_bits')}/{d.get('max_bits')} "
                             f"does not match {SUB_BITS}/{MAX_BITS}")
        h = cls()
        h.pos = {int(i): int(c) for i, c in d.get("pos", [])}
        h.neg = {int(i): int(c) for i, c in d.get("neg", [])}
        h.count = int(d.get("count", 0))
        h.total = float(d.get("sum", 0))
        if h.count:
            h.min, h.max = int(d["min"]), int(d["max"])
        if sum(h.pos.values()) + sum(h.neg.values()) != h.count:
            raise ValueError("histogram bucket mass does not equal its sample count")
        return h

    def merge(self, other: "SignedHistogram") -> None:
        for src, dst in ((other.pos, self.pos), (other.neg, self.neg)):
            for i, c in src.items():
                dst[i] = dst.get(i, 0) + c
        self.count += other.count
        self.total += other.total
        if other.min is not None:
            self.min = other.min if self.min is None else min(self.min, other.min)
            self.max = other.max if self.max is None else max(self.max, other.max)

    def mean(self) -> float:
        return self.total / self.count if self.count else 0.0

    def value_at_rank(self, k: int) -> int:
        nneg = sum(self.neg.values())
        if k < nneg:
            m = nneg - 1 - k  # ascending value == descending magnitude
            acc = 0
            for i in sorted(self.neg):
                acc += self.neg[i]
                if acc > m:
                    v = -hist_representative(i)
                    break
            else:  # pragma: no cover - guarded by the mass check
                v = -hist_representative(max(self.neg))
        else:
            r = k - nneg
            acc = 0
            for i in sorted(self.pos):
                acc += self.pos[i]
                if acc > r:
                    v = hist_representative(i)
                    break
            else:  # pragma: no cover
                v = hist_representative(max(self.pos))
        return max(self.min, min(self.max, v))

    def quantile(self, q: float) -> int:
        """Rank rule: sorted[min(int(n*q), n-1)]."""
        if not self.count:
            return 0
        k = min(int(self.count * q), self.count - 1)
        return self.value_at_rank(k)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

_BLOCK = r"--- {kind} (?P<id>\S+)(?: (?P<tag>\S+))? ---\n(?P<body>.*?)--- END {kind}(?: (?P=id))?(?: (?P=tag))? ---"


def _blocks(raw: str, kind: str) -> List[Tuple[str, Optional[str], str]]:
    pat = re.compile(_BLOCK.format(kind=kind), re.DOTALL)
    return [(m.group("id"), m.group("tag"), m.group("body").strip()) for m in pat.finditer(raw)]


def parse_header(raw: str) -> Dict[str, Any]:
    cfg: Dict[str, Any] = {}

    def grab(key: str, pattern: str, cast=str):
        m = re.search(pattern, raw, re.M)
        if m:
            cfg[key] = cast(m.group(1))

    grab("stack_name", r"^Stack:\s+(\S+)")
    grab("run_id", r"^Run-ID:\s+(\S+)")
    grab("tools", r"^Tools?:\s+(.+?)\s*$")
    grab("multicast_group", r"^Group:\s+([\d.]+)")
    grab("port", r"^Port:\s+(\d+)", int)
    grab("rate", r"^Rate:\s+(\d+)", int)
    grab("duration", r"^Duration:\s+(\d+)s", int)
    grab("payload_bytes", r"^Payload:\s+(\d+)\s+bytes", int)
    grab("publisher", r"^\s*Publisher:\s+(\S+)")
    grab("subscribers", r"^\s*Subscribers:\s+(.+?)\s*$")
    grab("instance_type", r"^Instance-Type:\s+(\S+)")
    grab("native_build", r"^Native-Build:\s+(.+?)\s*$")
    if isinstance(cfg.get("subscribers"), str):
        cfg["subscribers"] = cfg["subscribers"].split()
    return cfg


@dataclass
class Subscriber:
    subscriber_id: str
    az: Optional[str] = None
    az_relation: Optional[str] = None       # "same-az" | "cross-az" | None (unknown)
    result: Optional[Dict[str, Any]] = None  # receiver's flat JSON, if it produced one
    error: Optional[str] = None             # why there is no usable result
    detail: Optional[Dict[str, Any]] = None  # nested detail document (histograms), if fetched

    @property
    def ok(self) -> bool:
        return self.error is None and self.result is not None


def parse_subscribers(raw: str) -> List[Subscriber]:
    """Subscribers from MCAST blocks. A block with no usable result is kept as a failure."""
    out: List[Subscriber] = []
    for sid, _tag, body in _blocks(raw, "MCAST"):
        sub = Subscriber(subscriber_id=sid)
        try:
            doc = json.loads(body) if body else {}
        except json.JSONDecodeError:
            sub.error = "result is not valid JSON"
            out.append(sub)
            continue
        sub.az = doc.get("az")
        sub.az_relation = doc.get("az_relation")
        res = doc.get("result")
        if doc.get("error"):
            sub.error = str(doc["error"])
        elif not isinstance(res, dict) or not res:
            sub.error = "no result returned"
        elif res.get("error"):
            sub.error = f"{res['error']}: {res.get('detail', '')}".strip(": ")
        else:
            sub.result = res
        out.append(sub)
    return out


def parse_sender(raw: str) -> Optional[Dict[str, Any]]:
    """The sender's JSON summary, or None when it is missing or not JSON."""
    for _sid, _tag, body in _blocks(raw, "SENDER"):
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            return None
    return None


def parse_clock(raw: str) -> Dict[str, Dict[str, Dict[str, str]]]:
    """{instance: {"pre"|"post": {key: value}}} from CLOCK blocks."""
    out: Dict[str, Dict[str, Dict[str, str]]] = {}
    for iid, phase, body in _blocks(raw, "CLOCK"):
        kv = dict(line.split("=", 1) for line in body.splitlines() if "=" in line)
        out.setdefault(iid, {})[phase or "pre"] = kv
    return out


def parse_sockperf(text: str) -> Dict[str, Any]:
    """Metrics from `sockperf under-load` output (the sender-side statistics).

    sockperf 3.10 prints the round-trip summary as `Summary: Round trip is X usec`, the
    average as `avg-rtt=X`, percentiles as `---> percentile P = V` (no unit), and, by default,
    replies to only every 100th message, so SentMessages minus ReceivedMessages is not loss;
    `# dropped messages` is. Keys are absent when the text does not contain them.
    """
    m: Dict[str, Any] = {}

    def num(pattern: str) -> Optional[float]:
        r = re.search(pattern, text)
        return float(r.group(1)) if r else None

    avg = num(r"avg-rtt=([\d.]+)")
    if avg is None:
        avg = num(r"Summary:\s*(?:Round trip|Latency) is ([\d.]+) usec")
    if avg is not None:
        m["avg_rtt_us"] = avg
    pct = {float(a): float(b) for a, b in re.findall(r"percentile\s+([\d.]+)\s*=\s*([\d.]+)", text)}
    for p, key in ((50.0, "p50_us"), (90.0, "p90_us"), (99.0, "p99_us"), (99.9, "p999_us")):
        if p in pct:
            m[key] = pct[p]
    for label, key in (("MIN", "min_us"), ("MAX", "max_us")):
        v = num(rf"<{label}> observation\s*=\s*([\d.]+)")
        if v is not None:
            m[key] = v
    obs = num(r"Total (\d+) observations")
    if obs is not None:
        m["observations"] = int(obs)
    dropped = num(r"# dropped messages = (\d+)")
    if dropped is not None:
        m["dropped"] = int(dropped)
    sent = re.search(r"\[Valid Duration\][^\n]*SentMessages=(\d+);\s*ReceivedMessages=(\d+)", text)
    if sent:
        m["valid_sent"], m["valid_received"] = int(sent.group(1)), int(sent.group(2))
    return m


def parse_context(raw: str) -> Dict[str, Dict[str, str]]:
    """{instance: {key: value}} from CONTEXT blocks (the state each measurement was taken under)."""
    out: Dict[str, Dict[str, str]] = {}
    for iid, _tag, body in _blocks(raw, "CONTEXT"):
        out[iid] = dict(line.split("=", 1) for line in body.splitlines() if "=" in line)
    return out


def context_summary(ctx: Dict[str, Dict[str, str]]) -> Tuple[Dict[str, str], Dict[str, Dict[str, str]]]:
    """Split context into (keys identical on every instance, {key: {instance: value}} for the rest)."""
    if not ctx:
        return {}, {}
    keys = sorted({k for kv in ctx.values() for k in kv})
    common: Dict[str, str] = {}
    differing: Dict[str, Dict[str, str]] = {}
    for k in keys:
        vals = {iid: kv.get(k, "") for iid, kv in ctx.items()}
        if len(set(vals.values())) == 1:
            common[k] = next(iter(vals.values()))
        else:
            differing[k] = vals
    return common, differing


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

def _hist(sub: Subscriber, key: str = "hist_hw_latency") -> Optional[SignedHistogram]:
    if not sub.detail or key not in sub.detail:
        return None
    try:
        return SignedHistogram.from_json(sub.detail[key])
    except (ValueError, KeyError, TypeError):
        return None


def group_subscribers(subs: Iterable[Subscriber]) -> Dict[str, List[Subscriber]]:
    """Subscribers of unlike paths (same-AZ vs cross-AZ) are never merged into one number."""
    groups: Dict[str, List[Subscriber]] = {}
    for s in subs:
        groups.setdefault(s.az_relation or "unknown-path", []).append(s)
    return groups


def aggregate_group(subs: List[Subscriber]) -> Dict[str, Any]:
    """Summary of one homogeneous group of subscribers.

    Pooled percentiles need every subscriber's histogram. If any is missing, no
    percentile is reported for the group (rather than a wrong one): only the exact
    quantities (counts, sample-weighted mean, worst subscriber tail) are given.
    """
    ok = [s for s in subs if s.ok]
    out: Dict[str, Any] = {"subscribers": len(subs), "subscribers_ok": len(ok)}
    if not ok:
        return out
    r = [s.result for s in ok]
    out["expected"] = sum(int(x.get("total_expected", 0)) for x in r)
    out["received"] = sum(int(x.get("total_received", 0)) for x in r)
    out["lost"] = sum(int(x.get("packet_loss_count", 0)) for x in r)
    out["duplicates"] = sum(int(x.get("duplicate_count", 0)) for x in r)
    out["socket_drops"] = sum(int(x.get("socket_drops", 0)) for x in r)
    warm = [float(x["warmup_secs"]) for x in r if "warmup_secs" in x]
    out["warmup_secs"] = max(warm) if warm else None
    out["warmup_skipped"] = sum(int(x.get("warmup_skipped", 0)) for x in r)
    ns = [int(x.get("latency_samples", x.get("total_received", 0))) for x in r]
    means = [float(x.get("mean_latency_us", 0.0)) for x in r]
    if sum(ns):
        out["mean_us"] = sum(m * n for m, n in zip(means, ns)) / sum(ns)  # exact, weighted
    out["min_us"] = min(float(x["min_latency_us"]) for x in r if "min_latency_us" in x) if any("min_latency_us" in x for x in r) else None
    out["max_us"] = max(float(x["max_latency_us"]) for x in r if "max_latency_us" in x) if any("max_latency_us" in x for x in r) else None
    p99s = [float(x["p99_latency_us"]) for x in r if "p99_latency_us" in x]
    out["worst_subscriber_p99_us"] = max(p99s) if p99s else None

    hists = [_hist(s) for s in ok]
    if all(h is not None for h in hists) and hists:
        pooled = SignedHistogram()
        for h in hists:
            pooled.merge(h)
        out["pooled"] = True
        out["pooled_samples"] = pooled.count
        for q, name in ((0.50, "median"), (0.95, "p95"), (0.99, "p99"), (0.999, "p999")):
            if name == "p999" and pooled.count < MIN_SAMPLES_FOR_P999:
                out["p999_omitted"] = f"fewer than {MIN_SAMPLES_FOR_P999} samples"
                continue
            out[f"{name}_us"] = pooled.quantile(q) / 1000.0
        sw = [_hist(s, "hist_sw_latency") for s in ok]
        dv = [_hist(s, "hist_div_sw_minus_hw") for s in ok]
        if all(h is not None for h in sw) and all(h is not None for h in dv):
            psw, pdv = SignedHistogram(), SignedHistogram()
            for h in sw:
                psw.merge(h)
            for h in dv:
                pdv.merge(h)
            out["sw_median_us"] = psw.quantile(0.5) / 1000.0
            out["sw_p99_us"] = psw.quantile(0.99) / 1000.0
            out["div_median_us"] = pdv.quantile(0.5) / 1000.0
            out["div_p99_us"] = pdv.quantile(0.99) / 1000.0
    else:
        out["pooled"] = False
    return out


def load_details(subs: Iterable[Subscriber], detail_dir: str) -> int:
    """Attach `<detail_dir>/<instance>.detail.json` to each subscriber. Returns how many loaded."""
    import os
    n = 0
    for s in subs:
        path = os.path.join(detail_dir, f"{s.subscriber_id}.detail.json")
        try:
            with open(path) as f:
                s.detail = json.load(f)
            n += 1
        except (OSError, json.JSONDecodeError):
            s.detail = None
    return n


def loss_totals(subs: Iterable[Subscriber], sender: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Run-level loss. A failed subscriber received nothing, so it counts as total loss.

    Without a sender count a failed subscriber's denominator is unknown, so it is reported
    in `unmeasured` rather than silently dropped from the sums.
    """
    sent = int(sender["sent"]) if sender and sender.get("sent") is not None else None
    lost = expected = unmeasured = 0
    for s in subs:
        if s.ok:
            r = s.result
            exp = max(int(r.get("total_expected", 0)), sent or 0)
            ls = int(r.get("packet_loss_count", 0))
            if sent is not None:
                ls = max(ls, sent - int(r.get("total_received", 0)))
            lost += ls
            expected += exp
        elif sent is not None:
            lost += sent
            expected += sent
        else:
            unmeasured += 1
    return {"lost": lost, "expected": expected,
            "pct": 100.0 * lost / expected if expected else 0.0, "unmeasured": unmeasured}


# ---------------------------------------------------------------------------
# Validity
# ---------------------------------------------------------------------------

@dataclass
class Finding:
    level: str   # "FAIL" invalidates the run; "WARN" is reported
    code: str
    message: str


def _chrony_offset_us(kv: Dict[str, str]) -> Optional[float]:
    v = kv.get("system_time_us")
    try:
        return abs(float(v)) if v is not None else None
    except ValueError:
        return None


def evaluate(raw: str,
             max_loss_pct: float = DEFAULT_MAX_LOSS_PCT,
             max_clock_offset_us: float = DEFAULT_MAX_CLOCK_OFFSET_US,
             min_achieved_pct: float = DEFAULT_MIN_ACHIEVED_PCT,
             max_div_median_us: float = DEFAULT_MAX_DIV_MEDIAN_US,
             expected_subscribers: Optional[List[str]] = None) -> Tuple[List[Finding], Dict[str, Any]]:
    """Decide whether a run's numbers can be trusted. Returns (findings, summary)."""
    cfg = parse_header(raw)
    subs = parse_subscribers(raw)
    sender = parse_sender(raw)
    clocks = parse_clock(raw)
    F: List[Finding] = []

    # 1. Every expected subscriber must be present and healthy.
    expected = expected_subscribers or cfg.get("subscribers") or []
    seen = {s.subscriber_id for s in subs}
    for sid in expected:
        if sid not in seen:
            F.append(Finding("FAIL", "subscriber-missing", f"{sid}: no result block at all"))
    for s in subs:
        if not s.ok:
            F.append(Finding("FAIL", "subscriber-failed", f"{s.subscriber_id}: {s.error}"))
    if not subs and "mcast" in str(cfg.get("tools", "mcast")):
        F.append(Finding("FAIL", "no-results", "no multicast results found in the output"))

    ok = [s for s in subs if s.ok]

    # 2. Sender: did it deliver the load that was requested?
    sent = None
    if sender and sender.get("error"):
        F.append(Finding("FAIL", "sender-failed", f"sender produced no summary: {sender['error']}"))
        sender = None
    if sender:
        sent = sender.get("sent")
        pct = sender.get("achieved_pct")
        if pct is not None and pct < min_achieved_pct:
            F.append(Finding("FAIL", "rate-shortfall",
                             f"sender achieved {pct:.1f}% of the requested {sender.get('requested_pps')} pps "
                             f"(minimum {min_achieved_pct:.0f}%)"))
        if sender.get("interrupted"):
            F.append(Finding("FAIL", "sender-interrupted", "sender was interrupted before the full duration"))
        if sender.get("send_errors"):
            F.append(Finding("WARN", "send-errors", f"sender reported {sender['send_errors']} send errors"))
        if sender.get("resyncs"):
            F.append(Finding("WARN", "sender-stalled", f"sender stalled and shifted its schedule {sender['resyncs']} time(s)"))
    elif "mcast" in str(cfg.get("tools", "mcast")):
        F.append(Finding("WARN", "no-sender-summary", "no sender summary; the achieved rate is unknown"))

    # 3. Per-subscriber consistency and loss.
    for s in ok:
        r = s.result
        sid = s.subscriber_id
        exp = int(r.get("total_expected", 0))
        if sent is not None and r.get("end_marker_received") and exp != int(sent):
            F.append(Finding("FAIL", "count-mismatch", f"{sid}: receiver expected {exp} but the sender sent {sent}"))
        if not r.get("end_marker_received", True):
            F.append(Finding("WARN", "no-end-marker", f"{sid}: end marker never arrived; tail loss is not counted"))
        denom = max(exp, int(sent or 0), 1)
        lost = max(int(r.get("packet_loss_count", 0)), (int(sent) - int(r.get("total_received", 0))) if sent else 0)
        pct = 100.0 * lost / denom
        if pct > max_loss_pct:
            drops = int(r.get("socket_drops", 0))
            why = f"; {drops} were dropped at this receiver's socket buffer" if drops else ""
            F.append(Finding("FAIL", "loss-gate",
                             f"{sid}: {pct:.2f}% loss exceeds {max_loss_pct:.1f}%{why} "
                             "(percentiles describe only the packets that arrived)"))
        elif r.get("socket_drops"):
            F.append(Finding("WARN", "receiver-drops", f"{sid}: {r['socket_drops']} packets dropped at the receiver, not the network"))
        if r.get("duplicate_count"):
            F.append(Finding("WARN", "duplicates", f"{sid}: {r['duplicate_count']} duplicate packets"))

        # 4. Clock sanity from the data itself.
        neg = r.get("negative_hw_samples", 0)
        if neg:
            F.append(Finding("FAIL", "negative-latency",
                             f"{sid}: {neg} packet(s) with a receive time earlier than the send time "
                             f"(most negative {r.get('most_negative_hw_us', r.get('min_latency_us'))} us): "
                             "clocks disagree"))
        div = r.get("div_median_us")
        if div is not None and abs(float(div)) > max_div_median_us:
            F.append(Finding("WARN", "stamp-divergence",
                             f"{sid}: software-minus-hardware stamp median is {div} us "
                             f"(warn above {max_div_median_us:.0f} us): receive-path delay or clock-domain offset"))

    # 5. Clock state on every node, before and after.
    for iid, phases in clocks.items():
        for phase, kv in phases.items():
            off = _chrony_offset_us(kv)
            if off is None:
                F.append(Finding("FAIL", "clock-unreadable", f"{iid} ({phase}): could not read chrony System time"))
            elif off > max_clock_offset_us:
                F.append(Finding("FAIL", f"clock-offset-{phase}",
                                 f"{iid} ({phase}): chrony System time {off:.1f} us exceeds {max_clock_offset_us:.0f} us"))

    summary = {
        "config": cfg,
        "sender": sender,
        "subscribers": len(subs),
        "subscribers_ok": len(ok),
        "groups": {g: aggregate_group(v) for g, v in group_subscribers(subs).items()},
    }
    return F, summary


def verdict_text(findings: List[Finding]) -> str:
    fails = [f for f in findings if f.level == "FAIL"]
    lines = []
    for f in findings:
        lines.append(f"  {f.level} [{f.code}] {f.message}")
    head = "RUN INVALID" if fails else "RUN VALID"
    if fails:
        head += f": {len(fails)} blocking finding(s)"
    return head + ("\n" + "\n".join(lines) if lines else "")


if __name__ == "__main__":  # python3 results.py verdict < raw
    import argparse
    import sys

    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["verdict"])
    ap.add_argument("--input", "-i")
    ap.add_argument("--max-loss-pct", type=float, default=DEFAULT_MAX_LOSS_PCT)
    ap.add_argument("--max-clock-offset-us", type=float, default=DEFAULT_MAX_CLOCK_OFFSET_US)
    ap.add_argument("--min-achieved-pct", type=float, default=DEFAULT_MIN_ACHIEVED_PCT)
    a = ap.parse_args()
    text = open(a.input).read() if a.input else sys.stdin.read()
    fnd, _ = evaluate(text, a.max_loss_pct, a.max_clock_offset_us, a.min_achieved_pct)
    print(verdict_text(fnd))
    sys.exit(1 if any(f.level == "FAIL" for f in fnd) else 0)
