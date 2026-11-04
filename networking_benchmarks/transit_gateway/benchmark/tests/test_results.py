"""Tests for report/results.py: parsing, pooling, and the validity gates."""
from __future__ import annotations

import importlib.util
import json
import pathlib
import random
import shutil
import subprocess
import sys

import pytest

BENCH_DIR = pathlib.Path(__file__).resolve().parents[1]
REPORT_DIR = BENCH_DIR / "report"
spec = importlib.util.spec_from_file_location("results", REPORT_DIR / "results.py")
tr = importlib.util.module_from_spec(spec)
sys.modules["results"] = tr
spec.loader.exec_module(tr)


# ---------------------------------------------------------------------------
# Histogram: Python port against the real C++ implementation
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def hist_dump(tmp_path_factory):
    cxx = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler")
    out = tmp_path_factory.mktemp("hd") / "hist_dump"
    src = BENCH_DIR / "host" / "cpp" / "tests" / "hist_dump.cpp"
    subprocess.run([cxx, "-std=c++17", "-O1", "-o", str(out), str(src)], check=True)
    return str(out)


def _cpp_hist(hist_dump, values):
    p = subprocess.run([hist_dump], input="\n".join(map(str, values)) + "\n",
                       capture_output=True, text=True, check=True)
    return tr.SignedHistogram.from_json(json.loads(p.stdout))


def _exact(values, q):
    s = sorted(values)
    return s[min(int(len(s) * q), len(s) - 1)]


def _within_bucket(approx, exact):
    # relative width of a bucket is at most 2**-SUB_BITS; small values are exact
    return abs(approx - exact) <= max(1, abs(exact) * 2.0 / tr.SUB)


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_pooled_quantiles_from_cpp_match_exact(hist_dump, seed):
    rng = random.Random(seed)
    parts = [[int(rng.lognormvariate(12, 0.4)) for _ in range(rng.randint(500, 3000))]
             for _ in range(4)]
    parts[1] = [v - 300_000 for v in parts[1]]  # force negatives in one subscriber
    pooled = tr.SignedHistogram()
    for p in parts:
        pooled.merge(_cpp_hist(hist_dump, p))
    allv = [v for p in parts for v in p]
    assert pooled.count == len(allv)
    assert pooled.min == min(allv) and pooled.max == max(allv)
    assert pooled.mean() == pytest.approx(sum(allv) / len(allv))
    for q in (0.0, 0.01, 0.5, 0.95, 0.99, 0.999, 1.0):
        assert _within_bucket(pooled.quantile(q), _exact(allv, q)), q


def test_mean_of_p99_differs_from_pooled_p99(hist_dump):
    # One quiet and one noisy subscriber: averaging their p99s is not the population p99.
    quiet = [100_000] * 9000
    noisy = [100_000] * 500 + [900_000] * 500
    pooled = tr.SignedHistogram()
    for p in (quiet, noisy):
        pooled.merge(_cpp_hist(hist_dump, p))
    mean_of_p99 = (_exact(quiet, 0.99) + _exact(noisy, 0.99)) / 2
    assert _within_bucket(pooled.quantile(0.99), _exact(quiet + noisy, 0.99))
    assert abs(pooled.quantile(0.99) - mean_of_p99) > 100_000


def test_histogram_rejects_wrong_layout_and_inconsistent_mass():
    good = {"sub_bits": 10, "max_bits": 44, "count": 1, "sum": 5, "min": 5, "max": 5,
            "pos": [[5, 1]], "neg": []}
    tr.SignedHistogram.from_json(good)
    with pytest.raises(ValueError):
        tr.SignedHistogram.from_json({**good, "sub_bits": 8})
    with pytest.raises(ValueError):
        tr.SignedHistogram.from_json({**good, "count": 2})


def test_empty_histogram_quantile_is_zero():
    assert tr.SignedHistogram().quantile(0.99) == 0


# ---------------------------------------------------------------------------
# Raw-output builders
# ---------------------------------------------------------------------------

def recv(**kw):
    r = {"total_expected": 1000, "total_received": 1000, "packet_loss_count": 0,
         "duplicate_count": 0, "socket_drops": 0, "end_marker_received": True,
         "negative_hw_samples": 0, "min_latency_us": 150.0, "max_latency_us": 400.0,
         "mean_latency_us": 170.0, "p99_latency_us": 250.0, "latency_samples": 1000,
         "div_median_us": 0.7}
    r.update(kw)
    return r


def mcast_block(sid, result=None, az="use1-az1", rel="same-az", error=None):
    doc = {"subscriber_id": sid, "az": az, "az_relation": rel}
    if error:
        doc["error"] = error
    else:
        doc["result"] = result if result is not None else recv()
    return f"--- MCAST {sid} ---\n{json.dumps(doc)}\n--- END MCAST {sid} ---\n"


def clock_block(sid, phase, sys_us):
    return f"--- CLOCK {sid} {phase} ---\nsystem_time_us={sys_us}\n--- END CLOCK ---\n"


def raw_run(subs=("i-a", "i-b"), sender=None, blocks=None, clocks=True):
    head = ("Stack: S\nRun-ID: r1\nTools: mcast\nRate: 1000\nDuration: 1s\n"
            f"Subscribers: {' '.join(subs)}\nImpl: sender=native receiver=native\n")
    s = sender if sender is not None else {"sent": 1000, "requested_pps": 1000,
                                           "achieved_pct": 100.0, "send_errors": 0, "resyncs": 0}
    out = head + f"--- SENDER i-p ---\n{json.dumps(s)}\n--- END SENDER i-p ---\n"
    out += "".join(blocks if blocks is not None else [mcast_block(x) for x in subs])
    if clocks:
        for x in subs:
            out += clock_block(x, "pre", 0.2) + clock_block(x, "post", 0.3)
    return out


def codes(raw, **kw):
    f, _ = tr.evaluate(raw, **kw)
    return {(x.level, x.code) for x in f}


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def test_parse_header_accepts_tool_and_tools():
    assert tr.parse_header("Tool: mcast\n")["tools"] == "mcast"
    assert tr.parse_header("Tools: mcast sockperf\n")["tools"] == "mcast sockperf"


def test_parse_subscribers_and_clock():
    raw = raw_run()
    subs = tr.parse_subscribers(raw)
    assert [s.subscriber_id for s in subs] == ["i-a", "i-b"]
    assert all(s.ok and s.az_relation == "same-az" for s in subs)
    clk = tr.parse_clock(raw)
    assert clk["i-a"]["pre"]["system_time_us"] == "0.2"
    assert clk["i-a"]["post"]["system_time_us"] == "0.3"


def test_error_and_empty_subscribers_are_kept_as_failures():
    raw = raw_run(blocks=[
        mcast_block("i-a"),
        mcast_block("i-b", error="no result returned"),
    ])
    subs = {s.subscriber_id: s for s in tr.parse_subscribers(raw)}
    assert subs["i-a"].ok and not subs["i-b"].ok
    raw2 = raw_run(blocks=["--- MCAST i-a ---\n\n--- END MCAST i-a ---\n",
                           "--- MCAST i-b ---\nnot json\n--- END MCAST i-b ---\n"])
    assert not any(s.ok for s in tr.parse_subscribers(raw2))


def test_receiver_level_error_is_a_failure():
    raw = raw_run(blocks=[mcast_block("i-a", result={"error": "no_hw_timestamps", "detail": "x"}),
                          mcast_block("i-b")])
    s = tr.parse_subscribers(raw)[0]
    assert not s.ok and "no_hw_timestamps" in s.error


# ---------------------------------------------------------------------------
# Gates
# ---------------------------------------------------------------------------

def test_clean_run_is_valid():
    f, _ = tr.evaluate(raw_run())
    assert [x for x in f if x.level == "FAIL"] == []


def test_missing_subscriber_fails():
    raw = raw_run(subs=("i-a", "i-b"), blocks=[mcast_block("i-a")])
    assert ("FAIL", "subscriber-missing") in codes(raw)


def test_errored_subscriber_fails():
    raw = raw_run(blocks=[mcast_block("i-a"), mcast_block("i-b", error="boom")])
    assert ("FAIL", "subscriber-failed") in codes(raw)


def test_no_results_fails():
    assert ("FAIL", "no-results") in codes("Tools: mcast\n")


def test_loss_gate_and_threshold_override():
    lossy = recv(total_received=950, packet_loss_count=50)
    raw = raw_run(blocks=[mcast_block("i-a", lossy), mcast_block("i-b")])
    assert ("FAIL", "loss-gate") in codes(raw)
    assert ("FAIL", "loss-gate") not in codes(raw, max_loss_pct=10.0)


def test_socket_drops_are_called_out():
    r = recv(total_received=900, packet_loss_count=100, socket_drops=100)
    f, _ = tr.evaluate(raw_run(blocks=[mcast_block("i-a", r), mcast_block("i-b")]))
    msg = next(x.message for x in f if x.code == "loss-gate")
    assert "socket buffer" in msg


def test_rate_shortfall_fails():
    s = {"sent": 800, "requested_pps": 1000, "achieved_pct": 80.0}
    r = recv(total_expected=800, total_received=800)
    raw = raw_run(sender=s, blocks=[mcast_block("i-a", r), mcast_block("i-b", r)])
    assert ("FAIL", "rate-shortfall") in codes(raw)


def test_interrupted_sender_fails():
    s = {"sent": 1000, "requested_pps": 1000, "achieved_pct": 100.0, "interrupted": True}
    assert ("FAIL", "sender-interrupted") in codes(raw_run(sender=s))


def test_count_mismatch_fails():
    raw = raw_run(blocks=[mcast_block("i-a", recv(total_expected=1200)), mcast_block("i-b")])
    assert ("FAIL", "count-mismatch") in codes(raw)


def test_loss_hidden_by_missing_end_marker_is_still_caught():
    # receiver never saw the end marker, so it believes nothing is missing
    r = recv(total_expected=900, total_received=900, packet_loss_count=0, end_marker_received=False)
    raw = raw_run(blocks=[mcast_block("i-a", r), mcast_block("i-b")])
    c = codes(raw)
    assert ("FAIL", "loss-gate") in c and ("WARN", "no-end-marker") in c


def test_negative_latency_fails_the_run():
    assert ("FAIL", "negative-latency") in codes(
        raw_run(blocks=[mcast_block("i-a", recv(negative_hw_samples=3)), mcast_block("i-b")]))


def test_clock_gate_pre_and_post_and_unreadable():
    raw = raw_run().replace("system_time_us=0.2", "system_time_us=25.0", 1)
    assert ("FAIL", "clock-offset-pre") in codes(raw)
    raw = raw_run().replace("system_time_us=0.3", "system_time_us=-40.0", 1)
    assert ("FAIL", "clock-offset-post") in codes(raw)
    raw = raw_run().replace("system_time_us=0.3", "system_time_us=abc", 1)
    assert ("FAIL", "clock-unreadable") in codes(raw)
    assert ("FAIL", "clock-offset-pre") not in codes(
        raw_run().replace("system_time_us=0.2", "system_time_us=25.0", 1),
        max_clock_offset_us=50.0)


def test_divergence_warns_but_does_not_fail():
    c = codes(raw_run(blocks=[mcast_block("i-a", recv(div_median_us=55.0)), mcast_block("i-b")]))
    assert ("WARN", "stamp-divergence") in c
    assert not any(level == "FAIL" for level, _ in c)


def test_verdict_text_and_cli(tmp_path):
    bad = tmp_path / "bad.txt"
    bad.write_text(raw_run(blocks=[mcast_block("i-a")]))
    good = tmp_path / "good.txt"
    good.write_text(raw_run())
    cmd = ["python3", str(REPORT_DIR / "results.py"), "verdict", "--input"]
    p = subprocess.run(cmd + [str(bad)], capture_output=True, text=True)
    assert p.returncode == 1 and "RUN INVALID" in p.stdout
    p = subprocess.run(cmd + [str(good)], capture_output=True, text=True)
    assert p.returncode == 0 and "RUN VALID" in p.stdout


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

def _hist_json(values):
    h = tr.SignedHistogram()
    for v in values:
        # build via the public wire shape using the module's own bucket math
        idx = _index(abs(v))
        d = h.pos if v >= 0 else h.neg
        d[idx] = d.get(idx, 0) + 1
        h.count += 1
        h.total += v
        h.min = v if h.min is None else min(h.min, v)
        h.max = v if h.max is None else max(h.max, v)
    return {"sub_bits": 10, "max_bits": 44, "count": h.count, "sum": h.total,
            "min": h.min, "max": h.max,
            "pos": sorted(h.pos.items()), "neg": sorted(h.neg.items())}


def _index(v):
    if v < tr.SUB:
        return v
    b = v.bit_length() - 1
    shift = b - tr.SUB_BITS
    return (shift + 1) * tr.SUB + ((v >> shift) - tr.SUB)


def test_python_index_helper_inverts_hist_lower():
    for v in (0, 5, 1023, 1024, 5000, 170_000, 10**9):
        i = _index(v)
        assert tr.hist_lower(i) <= v < tr.hist_lower(i) + tr.hist_width(i)


def test_groups_are_not_mixed_and_exact_mean_is_weighted():
    a = tr.Subscriber("i-a", az_relation="same-az", result=recv(mean_latency_us=100.0, latency_samples=9000))
    b = tr.Subscriber("i-b", az_relation="cross-az", result=recv(mean_latency_us=300.0, latency_samples=1000))
    groups = tr.group_subscribers([a, b])
    assert set(groups) == {"same-az", "cross-az"}
    c = tr.Subscriber("i-c", az_relation="same-az", result=recv(mean_latency_us=200.0, latency_samples=1000))
    agg = tr.aggregate_group([a, c])
    assert agg["mean_us"] == pytest.approx((100 * 9000 + 200 * 1000) / 10000)


def test_pooled_only_when_every_histogram_present():
    vals_a = [100_000] * 100
    vals_b = [200_000] * 100
    sa = tr.Subscriber("i-a", result=recv(), detail={"hist_hw_latency": _hist_json(vals_a)})
    sb = tr.Subscriber("i-b", result=recv(), detail={"hist_hw_latency": _hist_json(vals_b)})
    agg = tr.aggregate_group([sa, sb])
    assert agg["pooled"] and agg["pooled_samples"] == 200
    assert 100.0 <= agg["median_us"] <= 200.5
    sb.detail = None
    agg = tr.aggregate_group([sa, sb])
    assert agg["pooled"] is False and "p99_us" not in agg
    assert agg["worst_subscriber_p99_us"] == 250.0


def test_aggregate_of_all_failed_group_reports_zero_ok():
    s = tr.Subscriber("i-a", error="x")
    assert tr.aggregate_group([s]) == {"subscribers": 1, "subscribers_ok": 0}


# ---------------------------------------------------------------------------
# Context, p99.9 rule, warm-up
# ---------------------------------------------------------------------------

def _ctx_block(iid, **kv):
    body = "\n".join(f"{k}={v}" for k, v in kv.items())
    return f"--- CONTEXT {iid} ---\n{body}\n--- END CONTEXT ---\n"


def test_context_parse_and_split_common_from_differing():
    raw = (_ctx_block("i-a", kernel="6.1", cpuidle_states="POLL:0us C2:800us", ami="ami-1")
           + _ctx_block("i-b", kernel="6.1", cpuidle_states="POLL:0us", ami="ami-1"))
    ctx = tr.parse_context(raw)
    assert set(ctx) == {"i-a", "i-b"}
    common, differing = tr.context_summary(ctx)
    assert common == {"ami": "ami-1", "kernel": "6.1"}
    assert differing == {"cpuidle_states": {"i-a": "POLL:0us C2:800us", "i-b": "POLL:0us"}}
    assert tr.context_summary({}) == ({}, {})


def test_p999_is_withheld_below_the_sample_minimum():
    few = [tr.Subscriber("i-a", result=recv(), detail={"hist_hw_latency": _hist_json([100_000 + i for i in range(5000)])})]
    agg = tr.aggregate_group(few)
    assert "p999_us" not in agg and "100000" in agg["p999_omitted"].replace(",", "").replace("_", "")
    assert "p99_us" in agg
    many = [tr.Subscriber("i-a", result=recv(), detail={"hist_hw_latency": _hist_json([100_000 + i % 500 for i in range(tr.MIN_SAMPLES_FOR_P999)])})]
    assert "p999_us" in tr.aggregate_group(many)


def test_warmup_fields_are_aggregated():
    a = tr.Subscriber("i-a", result=recv(warmup_secs=2.0, warmup_skipped=2000))
    b = tr.Subscriber("i-b", result=recv(warmup_secs=2.0, warmup_skipped=1990))
    agg = tr.aggregate_group([a, b])
    assert agg["warmup_secs"] == 2.0 and agg["warmup_skipped"] == 3990
    assert tr.aggregate_group([tr.Subscriber("i-c", result=recv())])["warmup_secs"] is None
