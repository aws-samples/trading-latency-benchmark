"""End-to-end tests for save_report.py and collect_results.py on multicast raw output."""
from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys

import pytest

REPORT_DIR = pathlib.Path(__file__).resolve().parents[1] / "report"
sys.path.insert(0, str(REPORT_DIR))
sys.path.insert(0, str(pathlib.Path(__file__).parent))

from test_results import _hist_json, mcast_block, raw_run, recv  # noqa: E402


def _run(script, *args):
    return subprocess.run([sys.executable, str(REPORT_DIR / script), *args],
                          capture_output=True, text=True)


@pytest.fixture
def valid_raw(tmp_path):
    f = tmp_path / "run.raw.txt"
    f.write_text(raw_run())
    return f


@pytest.fixture
def invalid_raw(tmp_path):
    f = tmp_path / "bad.raw.txt"
    f.write_text(raw_run(subs=("i-a", "i-b"), blocks=[mcast_block("i-a"), mcast_block("i-b", error="boom")]))
    return f


def test_save_report_valid_run(valid_raw, tmp_path):
    out = tmp_path / "out"
    p = _run("save_report.py", "-i", str(valid_raw), "--output-dir", str(out), "--filename", "r.md")
    assert p.returncode == 0, p.stderr
    md = (out / "r.md").read_text()
    assert "Run status: VALID" in md
    assert "Path: same-az" in md
    assert "upper bound" in md
    assert "mean of" not in md.lower().replace("mean of per-subscriber", "")


def test_save_report_surfaces_failed_subscriber_and_exits_3(invalid_raw, tmp_path):
    out = tmp_path / "out"
    p = _run("save_report.py", "-i", str(invalid_raw), "--output-dir", str(out), "--filename", "r.md")
    assert p.returncode == 3
    md = (out / "r.md").read_text()
    assert "Run status: INVALID" in md and "FAILED" in md and "boom" in md
    # the failed subscriber is counted as total loss
    assert "1000/2000" in md


def test_save_report_pooled_with_details(valid_raw, tmp_path):
    d = tmp_path / "det"
    d.mkdir()
    for sid, v in (("i-a", 150_000), ("i-b", 160_000)):
        (d / f"{sid}.detail.json").write_text(json.dumps({"hist_hw_latency": _hist_json([v] * 50)}))
    out = tmp_path / "out"
    p = _run("save_report.py", "-i", str(valid_raw), "--detail-dir", str(d), "--output-dir", str(out), "--filename", "r.md")
    assert p.returncode == 0, p.stderr
    md = (out / "r.md").read_text()
    assert "pooled over 100 samples" in md


def test_save_report_without_details_says_so(valid_raw, tmp_path):
    out = tmp_path / "out"
    _run("save_report.py", "-i", str(valid_raw), "--output-dir", str(out), "--filename", "r.md")
    md = (out / "r.md").read_text()
    assert "Pooled percentiles | not available" in md and "Worst-subscriber P99" in md


def test_collect_accepts_mcast_and_refuses_invalid(valid_raw, invalid_raw, tmp_path):
    cw = pytest.importorskip("boto3")  # collect_results imports boto3
    del cw
    spec = importlib.util.spec_from_file_location("collect_results", REPORT_DIR / "collect_results.py")
    cr = importlib.util.module_from_spec(spec)
    sys.modules["collect_results"] = cr
    spec.loader.exec_module(cr)

    published = []
    cr.publish_mcast_to_cloudwatch = lambda rep, ns, region=None: published.append(rep)
    uploaded = []
    cr.upload_to_s3 = lambda rep, b, k, region=None: uploaded.append((b, k)) or True

    args = cr.parse_args(["--input", str(valid_raw), "--s3-bucket", "b"])
    assert cr.run_mcast(args, valid_raw.read_text()) == 0
    assert published and published[0]["valid"] is True and uploaded == [("b", "reports/r1.json")]

    published.clear()
    uploaded.clear()
    args = cr.parse_args(["--input", str(invalid_raw), "--s3-bucket", "b"])
    assert cr.run_mcast(args, invalid_raw.read_text()) == 3
    assert not published and not uploaded

    args = cr.parse_args(["--input", str(invalid_raw), "--s3-bucket", "b", "--allow-partial"])
    assert cr.run_mcast(args, invalid_raw.read_text()) == 0
    assert published[0]["valid"] is False
    assert any(f["code"] == "subscriber-failed" for f in published[0]["findings"])


def test_report_has_context_warmup_and_sendpath_sections(tmp_path):
    from test_results import _ctx_block
    sender = dict(SENDER_LIKE, sendto_median_us=3.1, sendto_p99_us=9.8, sendto_max_us=120.0)
    raw = raw_run(sender=sender, blocks=[mcast_block("i-a", recv(warmup_secs=2.0, warmup_skipped=2000)),
                                         mcast_block("i-b", recv(warmup_secs=2.0, warmup_skipped=2000))])
    raw += (_ctx_block("i-a", kernel="6.1", cpuidle_states="POLL:0us C2:800us")
            + _ctx_block("i-b", kernel="6.1", cpuidle_states="POLL:0us C2:800us"))
    f = tmp_path / "r.raw.txt"
    f.write_text(raw)
    out = tmp_path / "out"
    p = _run("save_report.py", "-i", str(f), "--output-dir", str(out), "--filename", "r.md")
    assert p.returncode == 0, p.stderr
    md = (out / "r.md").read_text()
    assert "### Run context" in md and "| kernel | 6.1 |" in md and "C2:800us" in md
    assert "exclude the first 2 s of each receiver (4000 packets)" in md
    assert "send-path cost" in md and "median 3.1 us" in md


def test_p999_row_explains_its_absence(valid_raw, tmp_path):
    d = tmp_path / "det"
    d.mkdir()
    for sid in ("i-a", "i-b"):
        (d / f"{sid}.detail.json").write_text(json.dumps({"hist_hw_latency": _hist_json([150_000] * 50)}))
    out = tmp_path / "out"
    _run("save_report.py", "-i", str(valid_raw), "--detail-dir", str(d), "--output-dir", str(out), "--filename", "r.md")
    assert "P99.9 | not reported" in (out / "r.md").read_text()


SENDER_LIKE = {"sent": 1000, "requested_pps": 1000, "achieved_pct": 100.0, "send_errors": 0, "resyncs": 0}


def _write_run(tmp, name, run_id, lat_us, n=200):
    d = tmp / f"{run_id}.details"
    d.mkdir(exist_ok=True)
    blocks = []
    for sid in ("i-a", "i-b"):
        blocks.append(mcast_block(sid, recv(median_latency_us=lat_us, p95_latency_us=lat_us + 8, p99_latency_us=lat_us + 15)))
        (d / f"{sid}.detail.json").write_text(json.dumps({"hist_hw_latency": _hist_json([int(lat_us * 1000)] * n)}))
    raw = raw_run(blocks=blocks).replace("Run-ID: r1", f"Run-ID: {run_id}")
    p = tmp / name
    p.write_text(raw)
    return p


def test_compare_runs_spread_interval_and_difference(tmp_path):
    runs = []
    for i, lat in enumerate((119.0, 119.4, 119.2, 119.1, 119.3)):
        runs.append("native:" + str(_write_run(tmp_path, f"n{i}.raw.txt", f"rn{i}", lat)))
    for i, lat in enumerate((124.0, 160.0, 201.0, 140.0, 175.0)):
        runs.append("python:" + str(_write_run(tmp_path, f"p{i}.raw.txt", f"rp{i}", lat)))
    args = [x for r in runs for x in ("--run", r)]
    p = _run("compare_runs.py", *args)
    assert p.returncode == 0, p.stderr
    out = p.stdout
    assert "## native (5 valid run(s))" in out and "## python (5 valid run(s))" in out
    assert "95% interval of the mean" in out
    assert "native minus python" in out and "the interval excludes 0" in out


def test_compare_runs_needs_five_runs_for_an_interval_and_skips_invalid(tmp_path):
    good = _write_run(tmp_path, "g.raw.txt", "rg", 120.0)
    bad = tmp_path / "bad.raw.txt"
    bad.write_text(raw_run(blocks=[mcast_block("i-a"), mcast_block("i-b", error="boom")]))
    p = _run("compare_runs.py", "--run", f"x:{good}", "--run", f"x:{bad}")
    assert p.returncode == 0
    assert "n/a (need 5+ runs)" in p.stdout
    assert "excluded: bad.raw.txt: subscriber-failed" in p.stdout


def test_compare_runs_is_reproducible(tmp_path):
    runs = [x for i, lat in enumerate((119.0, 130.0, 125.0)) for x in ("--run", f"a:{_write_run(tmp_path, f'a{i}.raw.txt', f'ra{i}', lat)}")]
    assert _run("compare_runs.py", *runs).stdout == _run("compare_runs.py", *runs).stdout


FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "sockperf_under_load.txt"


def test_shared_sockperf_parser_reads_real_output():
    import results as tr
    m = tr.parse_sockperf(FIXTURE.read_text())
    assert m["avg_rtt_us"] == pytest.approx(90.812)
    assert (m["p50_us"], m["p90_us"], m["p99_us"], m["p999_us"]) == (90.59, 97.17, 102.45, 103.57)
    assert (m["min_us"], m["max_us"]) == (76.02, 103.57)
    assert m["observations"] == 195 and m["dropped"] == 0
    assert tr.parse_sockperf("") == {}


def test_report_phase_b_uses_real_sockperf_text(tmp_path):
    raw = raw_run() + "--- SUBSCRIBER i-a ---\n" + FIXTURE.read_text() + "--- END i-a ---\n"
    f = tmp_path / "r.raw.txt"
    f.write_text(raw)
    out = tmp_path / "out"
    p = _run("save_report.py", "-i", str(f), "--output-dir", str(out), "--filename", "r.md")
    assert p.returncode == 0, p.stderr
    md = (out / "r.md").read_text()
    assert "| 90.8 | 90.6 | 97.2 | 102.5 | 76.0 | 103.6 |" in md


def test_report_compares_multicast_with_the_unicast_floor(tmp_path):
    # i-a: multicast median 150 against sockperf p50 90.59 (floor 45.3); i-b has no unicast result
    a = mcast_block("i-a", result=recv(median_latency_us=150.0))
    b = mcast_block("i-b", result=recv(median_latency_us=160.0))
    raw = raw_run(blocks=[a, b]) + "--- SUBSCRIBER i-a ---\n" + FIXTURE.read_text() + "--- END i-a ---\n"
    f = tmp_path / "r.raw.txt"
    f.write_text(raw)
    out = tmp_path / "out"
    p = _run("save_report.py", "-i", str(f), "--output-dir", str(out), "--filename", "r.md")
    assert p.returncode == 0, p.stderr
    md = (out / "r.md").read_text()
    assert "## Multicast against the unicast floor" in md
    assert "| same-az | 150.0 | 45.3 | +104.7 |" in md
    assert "i-b" not in md.split("## Multicast against the unicast floor")[1].split("Median difference")[0]
    assert "upper bound" in md.split("## Multicast against the unicast floor")[1]


def test_report_without_unicast_results_has_no_floor_section(valid_raw, tmp_path):
    out = tmp_path / "out"
    assert _run("save_report.py", "-i", str(valid_raw), "--output-dir", str(out), "--filename", "r.md").returncode == 0
    assert "unicast floor" not in (out / "r.md").read_text()
