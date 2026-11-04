"""The orchestrator (`python3 -m orchestrator run|prepare`) end to end against a fake `aws` CLI (see fake_aws.py).

These check the orchestrator's control flow and the raw stream it produces. They do not
exercise a real instance, the SSM service or the NIC.
"""
from __future__ import annotations

import json
import os
import pathlib
import stat
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
BENCH_DIR = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(BENCH_DIR / "report"))

import results as tr  # noqa: E402
from test_results import _hist_json, recv  # noqa: E402

PUB = "i-pub0000000000001"
SUBS = ["i-sub0000000000001", "i-sub0000000000002"]
SENDER_OK = {"tool": "mcast_send", "impl": "cpp", "requested_pps": 1000, "duration_s": 1, "sent": 1000,
             "send_errors": 0, "achieved_pps": 1000.0, "achieved_pct": 100.0, "late_max_us": 40.0,
             "resyncs": 0, "interrupted": False}


class Lab:
    def __init__(self, tmp: pathlib.Path):
        self.tmp = tmp
        self.state = tmp / "state"
        self.state.mkdir()
        self.bin = tmp / "bin"
        self.bin.mkdir()
        self.runs = tmp / "runs"
        self.manifests = tmp / "manifests"
        self.scenario = {
            "outputs": {"PublisherInstanceId": PUB, "SubscriberInstanceIds": ",".join(SUBS),
                        "S3BucketName": "bkt", "MulticastGroup": "239.1.1.1", "MulticastPort": "5001"},
            "az": {PUB: "us-east-1a", SUBS[0]: "us-east-1a", SUBS[1]: "us-east-1a"},
            "ips": {PUB: "10.0.0.5"},
            "public_ips": {PUB: "54.0.0.1", SUBS[0]: "54.0.0.2", SUBS[1]: "54.0.0.3"},
            "sender": json.dumps(SENDER_OK),
            "results": {s: json.dumps(recv()) for s in SUBS},
            "details": {s: {"hist_hw_latency": _hist_json([150_000 + 1000 * i for i in range(200)])} for s in SUBS},
        }
        for name, body in (
            ("aws", f'#!/bin/sh\nexec "{sys.executable}" "{HERE / "fake_aws.py"}" "$@"\n'),
            ("ssh", f'#!/bin/sh\nexec "{sys.executable}" "{HERE / "fake_aws.py"}" __ssh__ "$@"\n'),
            ("sleep", "#!/bin/sh\nexit 0\n"),
        ):
            p = self.bin / name
            p.write_text(body)
            p.chmod(p.stat().st_mode | stat.S_IEXEC)
        (self.bin / "python3").symlink_to(sys.executable)

    def _env(self):
        sc = self.tmp / "scenario.json"
        sc.write_text(json.dumps(self.scenario))
        return dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", FAKE_AWS_SCENARIO=str(sc),
                    FAKE_AWS_STATE=str(self.state), TMPDIR=str(self.tmp), TGW_BENCH_NO_SLEEP="1",
                    PYTHONPATH=str(BENCH_DIR))

    def run(self, *args, tools="mcast"):
        cmd = ["python3", "-m", "orchestrator", "run", "--stack-name", "S", "--tool", tools,
               "--duration", "1", "--runs-dir", str(self.runs), "--state-dir", str(self.manifests), *args]
        return subprocess.run(cmd, env=self._env(), capture_output=True, text=True)

    def prepare(self, *args):
        cmd = ["python3", "-m", "orchestrator", "prepare", "--stack-name", "S",
               "--state-dir", str(self.manifests), *args]
        return subprocess.run(cmd, env=self._env(), capture_output=True, text=True)

    def calls(self):
        f = self.state / "calls.jsonl"
        return [json.loads(x) for x in f.read_text().splitlines()] if f.exists() else []

    def commands(self, needle):
        return [c for c in self.calls() if needle in c["command"]]


@pytest.fixture
def lab(tmp_path):
    return Lab(tmp_path)


def verdict(stdout):
    f, _ = tr.evaluate(stdout)
    return f


def test_happy_path_native(lab):
    p = lab.run()
    assert p.returncode == 0, p.stdout + (p.stdout + p.stderr)
    out = p.stdout
    assert "Native-Build: commit=abc123" in out
    assert out.count("--- MCAST ") == 2 and out.count("--- CLOCK ") == 3 * 2
    assert "RUN VALID" in out
    subs = tr.parse_subscribers(out)
    assert [s.az_relation for s in subs] == ["same-az", "same-az"]
    assert tr.parse_sender(out)["sent"] == 1000
    # the native binaries were started
    assert lab.commands("/opt/benchmark-native/bin/mcast_recv 239.1.1.1 5001")
    assert lab.commands("/opt/benchmark-native/bin/mcast_send 239.1.1.1 5001 1000 1 10.0.0.5")


def test_preflight_runs_before_any_receiver_starts(lab):
    lab.run()
    order = [c["command"] for c in lab.calls()]
    first_recv = next(i for i, c in enumerate(order) if "nohup" in c and "mcast_recv" in c)
    last_preflight = max(i for i, c in enumerate(order) if "preflight.sh --gate" in c)
    assert last_preflight < first_recv


def test_cross_az_is_labelled(lab):
    lab.scenario["az"][SUBS[1]] = "us-east-1b"
    p = lab.run()
    rel = {s.subscriber_id: s.az_relation for s in tr.parse_subscribers(p.stdout)}
    assert rel == {SUBS[0]: "same-az", SUBS[1]: "cross-az"}


def test_details_are_downloaded_for_pooling(lab, tmp_path):
    d = tmp_path / "det"
    p = lab.run("--detail-dir", str(d))
    assert p.returncode == 0, p.stdout
    assert sorted(x.name for x in d.iterdir()) == sorted(f"{s}.detail.json" for s in SUBS)


def test_native_build_failure_aborts_loudly_and_does_not_fall_back(lab):
    lab.scenario["native"] = {SUBS[0]: "failed"}
    p = lab.run()
    assert p.returncode != 0
    assert "no native build" in (p.stdout + p.stderr)
    assert "make: *** [mcast_recv] Error 1" in (p.stdout + p.stderr)
    assert not lab.commands("nohup")


def test_commit_mismatch_between_instances_aborts(lab):
    lab.scenario["commit"] = {SUBS[1]: "def456"}
    p = lab.run()
    assert p.returncode != 0 and "different native commits" in (p.stdout + p.stderr)


def test_preflight_failure_stops_before_receivers(lab):
    lab.scenario["preflight"] = {SUBS[0]: "phc_enable=1\nsystem_time_us=55.0\nFAIL:clock_offset_exceeds_10us"}
    p = lab.run()
    assert p.returncode != 0 and "preflight failed" in (p.stdout + p.stderr)
    assert not lab.commands("nohup")


def test_receiver_gets_one_retry(lab):
    lab.scenario["recv_ready"] = {SUBS[0]: ["RECV=DEAD\nbind failed", "RECV=READY"]}
    p = lab.run()
    assert p.returncode == 0, p.stdout + (p.stdout + p.stderr)
    assert len([c for c in lab.commands("nohup") if c["instance"] == SUBS[0]]) == 2


def test_receiver_that_never_starts_aborts(lab):
    lab.scenario["recv_ready"] = {SUBS[0]: ["RECV=DEAD\nno hw timestamps"]}
    p = lab.run()
    assert p.returncode != 0 and "did not become ready" in (p.stdout + p.stderr)
    assert not lab.commands("/opt/benchmark-native/bin/mcast_send")


def test_silent_subscriber_makes_the_run_invalid(lab):
    lab.scenario["results"][SUBS[1]] = ""
    p = lab.run()
    assert p.returncode == 3, p.stdout + (p.stdout + p.stderr)
    assert "RUN INVALID" in p.stdout
    # the block is present, as an error, rather than the subscriber disappearing
    subs = {s.subscriber_id: s for s in tr.parse_subscribers(p.stdout)}
    assert not subs[SUBS[1]].ok and subs[SUBS[0]].ok


def test_whole_batch_is_retried_once_when_a_result_is_missing(lab):
    good = json.dumps(recv())
    lab.scenario["results"][SUBS[1]] = ["", good]
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert "rerunning the whole batch once" in p.stdout
    assert all(s.ok for s in tr.parse_subscribers(p.stdout))
    assert p.stdout.count("--- SENDER ") == 1          # only the final attempt is reported
    assert len(lab.commands("/opt/benchmark-native/bin/mcast_send")) == 2


def test_batch_retry_is_bounded_to_one(lab):
    lab.scenario["results"][SUBS[1]] = ""
    p = lab.run()
    assert p.returncode == 3
    assert len(lab.commands("/opt/benchmark-native/bin/mcast_send")) == 2


def test_context_is_recorded_per_instance(lab):
    p = lab.run()
    ctx = tr.parse_context(p.stdout)
    assert set(ctx) == {PUB, *SUBS}
    assert ctx[PUB]["ami"] == "ami-0123456789abcdef0" and ctx[PUB]["cpuidle_states"].endswith("C2:800us")


def test_loss_over_threshold_invalid_and_threshold_configurable(lab):
    lab.scenario["results"][SUBS[0]] = json.dumps(recv(total_received=900, packet_loss_count=100))
    assert lab.run().returncode == 3
    assert lab.run("--max-loss-pct", "15").returncode == 0


def test_sender_failure_is_invalid(lab):
    lab.scenario["sender_status"] = "Failed"
    p = lab.run()
    assert p.returncode == 3
    assert "sender-failed" in p.stdout


def test_post_run_clock_drift_is_caught(lab):
    lab.scenario["post_clock"] = {SUBS[0]: "88.0"}
    p = lab.run()
    assert p.returncode == 3 and "clock-offset-post" in p.stdout


def test_save_report_writes_report_with_pooled_percentiles(lab):
    p = lab.run("--save-report")
    assert p.returncode == 0, p.stdout + (p.stdout + p.stderr)
    md = next(lab.runs.glob("*.md")).read_text()
    assert "pooled over 400 samples" in md and "Run status: VALID" in md
    assert any(lab.runs.glob("*.details"))


def test_invalid_run_still_saves_a_report_and_exits_3(lab):
    lab.scenario["results"][SUBS[1]] = ""
    p = lab.run("--save-report")
    assert p.returncode == 3
    assert "Run status: INVALID" in next(lab.runs.glob("*.md")).read_text()


@pytest.mark.parametrize("args", [
    ["--port", "5001;id"], ["--rate", "0"], ["--duration", "x"], ["--group", "239.1.1.1;id"],
    ["--impl", "python"], ["--max-loss-pct", "-1"], ["--stack-name", "a b"],
])
def test_bad_arguments_rejected_before_any_aws_call(lab, args):
    p = lab.run(*args)
    assert p.returncode != 0
    assert not lab.calls()


def test_sender_can_be_pinned_to_a_cpu(lab):
    p = lab.run("--sender-cpu", "1")
    assert p.returncode == 0, p.stdout + p.stderr
    assert lab.commands("taskset -c 1 /opt/benchmark-native/bin/mcast_send 239.1.1.1")
    assert not lab.run("--sender-cpu", "x").returncode == 0


def test_unicast_tools_still_produce_subscriber_blocks(lab):
    p = lab.run(tools="sockperf")
    assert p.returncode == 0, p.stdout + (p.stdout + p.stderr)
    assert p.stdout.count("--- SUBSCRIBER ") == 2 and "avg-rtt=80.0" in p.stdout


# ---------------------------------------------------------------------------
# Merged command groups and the transport
# ---------------------------------------------------------------------------

def _runs(lab):
    return [c for c in lab.calls() if c["command"] != "__push_key__"]


def test_first_run_prepares_and_a_later_run_only_gates_and_measures(lab):
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    # prepare: readiness, the helper upload and the static checks, once per instance. run: gate once per instance, receiver start
    # (stop, start and ready-wait together) once per subscriber, the sender once, collection per instance
    assert len(_runs(lab)) == (3 + 3 * 2) + 3 + 2 + 1 + 3
    assert (lab.manifests / "S.json").exists() and "Preparing the stack first: no manifest" in p.stdout
    before = len(_runs(lab))
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert "Preparing the stack" not in p.stdout
    assert len(_runs(lab)) - before == 3 + 2 + 1 + 3
    assert not [c for c in _runs(lab)[before:] if "ENV=READY" in c["command"] or "base64 -d" in c["command"]]


def test_collection_command_returns_result_detail_and_clock_together(lab):
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    sub_collect = [c for c in _runs(lab) if "=====RESULT=====" in c["command"]]
    assert {c["instance"] for c in sub_collect} == set(SUBS)         # the publisher only reads its clock
    assert all(f"/{c['instance']}.detail.json" in c["command"] for c in sub_collect)
    clocks = tr.parse_clock(p.stdout)
    assert set(clocks) == {PUB, *SUBS} and all("post" in v for v in clocks.values())


def test_ssh_transport_runs_the_same_flow_without_ssm(lab):
    p = lab.run("--transport", "ssh")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "Transport: ssh" in p.stdout and "RUN VALID" in p.stdout
    runs = _runs(lab)
    assert all(c.get("transport") == "ssh" for c in runs)
    assert len(runs) == 18                                              # the same commands as over SSM
    pushes = [c for c in lab.calls() if c["command"] == "__push_key__"]
    assert {c["instance"] for c in pushes} == {PUB, *SUBS}
    assert len(tr.parse_subscribers(p.stdout)) == 2 and all(s.ok for s in tr.parse_subscribers(p.stdout))


def test_ssh_transport_pushes_the_key_again_after_a_failed_connection(lab):
    lab.scenario["ssh_fail_first"] = {SUBS[0]: 1}
    p = lab.run("--transport", "ssh")
    assert p.returncode == 0, p.stdout + p.stderr
    pushes = [c for c in lab.calls() if c["command"] == "__push_key__" and c["instance"] == SUBS[0]]
    assert len(pushes) == 2


def test_ssh_transport_refuses_instances_without_a_public_ip(lab):
    del lab.scenario["public_ips"][SUBS[1]]
    p = lab.run("--transport", "ssh")
    assert p.returncode != 0 and "no public IP" in p.stdout + p.stderr and "--transport ssm" in p.stdout + p.stderr


def test_ssh_transport_leaves_no_control_sockets_behind(lab):
    import glob
    before = set(glob.glob("/tmp/tgwcm.*"))
    assert lab.run("--transport", "ssh").returncode == 0
    assert set(glob.glob("/tmp/tgwcm.*")) == before


def test_bad_transport_and_key_arguments_are_rejected_early(lab):
    assert lab.run("--transport", "telnet").returncode != 0
    assert not lab.calls()
    p = lab.run("--transport", "ssh", "--ssh-key", "/nonexistent/key")
    assert p.returncode != 0 and "--ssh-key needs both" in p.stdout + p.stderr
    assert lab.run("--ssh-user", "root; id").returncode != 0


# ---------------------------------------------------------------------------
# The prepared-state manifest
# ---------------------------------------------------------------------------

def _prepared_again(p):
    return "Preparing the stack" in p.stdout or "preparing again" in p.stdout


def test_prepare_stack_runs_alone_and_the_next_run_does_not_repeat_it(lab):
    p = lab.prepare()
    assert p.returncode == 0, p.stdout + p.stderr
    assert (lab.manifests / "S.json").exists() and not lab.commands("nohup")
    n = len(_runs(lab))
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert not _prepared_again(p) and len(_runs(lab)) - n == 3 + 2 + 1 + 3


def test_manifest_records_boot_id_hashes_native_build_and_static_context(lab):
    lab.prepare()
    rec = json.loads((lab.manifests / "S.json").read_text())["instances"][SUBS[0]]
    assert rec["boot_id"] == "boot-1" and rec["native"] == {"state": "ready", "build": "commit=abc123 repo=r ref=main"}
    assert set(rec["helpers"]) == {"preflight.sh"}
    assert rec["context"]["kernel"] == "6.1.0" and "chrony_system_time_us" not in rec["context"]


def test_a_rebooted_instance_triggers_prepare_and_the_run_still_succeeds(lab):
    assert lab.run().returncode == 0
    lab.scenario["boot_ids"] = {SUBS[1]: "boot-2"}
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert "Prepared state is stale" in p.stdout and f"{SUBS[1]}: rebooted" in p.stdout
    assert json.loads((lab.manifests / "S.json").read_text())["instances"][SUBS[1]]["boot_id"] == "boot-2"
    assert "RUN VALID" in p.stdout


def test_changed_helper_on_an_instance_triggers_prepare(lab):
    assert lab.run().returncode == 0
    (lab.state / "files" / f"{PUB}.json").write_text(json.dumps({"preflight.sh": "0" * 64}))
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert "helper files differ" in p.stdout


def test_changed_helper_on_this_machine_triggers_prepare(lab):
    assert lab.run().returncode == 0
    m = json.loads((lab.manifests / "S.json").read_text())
    for rec in m["instances"].values():
        rec["helpers"]["preflight.sh"] = "f" * 64
    (lab.manifests / "S.json").write_text(json.dumps(m))
    p = lab.run()
    assert p.returncode == 0 and "helper files changed on this machine" in p.stdout


def test_a_replaced_instance_is_detected_and_the_new_one_prepared(lab):
    assert lab.run().returncode == 0
    new = "i-sub0000000000009"
    lab.scenario["gone"] = [SUBS[1]]
    lab.scenario["results"][new] = lab.scenario["results"][SUBS[1]]
    lab.scenario["details"][new] = lab.scenario["details"][SUBS[1]]
    lab.scenario["outputs"]["SubscriberInstanceIds"] = f"{SUBS[0]},{new}"
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert f"{SUBS[1]}: unreachable" in p.stdout and new in p.stdout
    assert set(json.loads((lab.manifests / "S.json").read_text())["instances"]) == {PUB, SUBS[0], new}


def test_a_repeat_run_makes_no_cloudformation_or_ec2_calls(lab):
    assert lab.run().returncode == 0
    (lab.state / "api.log").unlink()
    assert lab.run().returncode == 0
    calls = {l.split()[0] for l in (lab.state / "api.log").read_text().splitlines()}
    assert not calls & {"cloudformation", "ec2"}


def test_instance_facts_come_from_the_instance_metadata_and_are_kept_in_the_manifest(lab):
    lab.scenario["az"][SUBS[1]] = "us-east-1c"
    lab.prepare()
    rec = json.loads((lab.manifests / "S.json").read_text())
    assert rec["instances"][PUB]["private_ip"] == "10.0.0.5" and rec["instances"][SUBS[1]]["az"] == "us-east-1c"
    assert rec["stack"]["bucket"] == "bkt" and rec["stack"]["subscribers"] == SUBS


def test_an_instance_that_reports_another_id_stops_prepare(lab):
    lab.scenario["iid_override"] = {SUBS[0]: "i-somebody-else"}
    p = lab.prepare()
    assert p.returncode != 0 and "reports instance id 'i-somebody-else'" in p.stdout + p.stderr
    assert not (lab.manifests / "S.json").exists()


def test_force_prepare_repeats_the_setup(lab):
    assert lab.run().returncode == 0
    n = len(lab.commands("ENV=READY"))
    p = lab.run("--force-prepare")
    assert p.returncode == 0 and len(lab.commands("ENV=READY")) == 2 * n


def test_corrupt_manifest_is_rebuilt(lab):
    lab.manifests.mkdir()
    (lab.manifests / "S.json").write_text("{not json")
    p = lab.run()
    assert p.returncode == 0, p.stdout + p.stderr
    assert json.loads((lab.manifests / "S.json").read_text())["version"] == 2


def test_static_check_failure_aborts_prepare_and_saves_no_manifest(lab):
    lab.scenario["static"] = {SUBS[0]: "phc_enable=0\nFAIL:phc_not_enabled"}
    p = lab.run()
    assert p.returncode != 0 and "preflight failed" in p.stdout + p.stderr
    assert "FAIL:phc_not_enabled" in p.stdout + p.stderr
    assert not (lab.manifests / "S.json").exists() and not lab.commands("nohup")


def test_the_clock_gate_runs_on_every_run_even_when_prepared(lab):
    assert lab.run().returncode == 0
    assert lab.run().returncode == 0
    assert len([c for c in lab.commands("preflight.sh --gate") if c["instance"] == PUB]) == 2


def test_native_state_is_read_from_the_manifest_without_asking_the_instances_again(lab):
    assert lab.run().returncode == 0
    ready = len(lab.commands("ENV=READY"))
    lab.scenario["native"] = {SUBS[0]: "failed"}      # a change after prepare is not seen until the next prepare
    assert lab.run().returncode == 0 and len(lab.commands("ENV=READY")) == ready
    p = lab.run("--force-prepare")
    assert p.returncode != 0 and "no native build" in p.stdout + p.stderr


def test_src_port_is_passed_to_the_sender_environment(lab):
    p = lab.run("--src-port", "45000")
    assert p.returncode == 0, p.stdout + p.stderr
    assert lab.commands("MCAST_SRC_PORT=45000 /opt/benchmark-native/bin/mcast_send 239.1.1.1")


@pytest.mark.parametrize("bad", ["0", "65536", "x", "45000;id", "45010-45000", "1000-3000", "45000-"])
def test_bad_src_port_is_rejected_before_any_aws_call(lab, bad):
    p = lab.run("--src-port", bad)
    assert p.returncode != 0 and not lab.calls()


def test_src_port_range_is_passed_through(lab):
    p = lab.run("--src-port", "45000-45015")
    assert p.returncode == 0, p.stdout + p.stderr
    assert lab.commands("MCAST_SRC_PORT=45000-45015 /opt/benchmark-native/bin/mcast_send")


def test_sockperf_is_opt_in_and_iperf3_is_gone(lab):
    cmd = ["python3", "-m", "orchestrator", "run", "--stack-name", "S", "--duration", "1",
           "--runs-dir", str(lab.runs), "--state-dir", str(lab.manifests)]
    p = subprocess.run(cmd, env=lab._env(), capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "Tools:    mcast\n" in p.stdout and not lab.commands("sockperf under-load")
    assert lab.run(tools="iperf3").returncode != 0


def test_sockperf_client_port_pins_the_unicast_flow(lab):
    p = lab.run("--client-port", "46000", tools="mcast,sockperf")
    assert p.returncode == 0, p.stdout + p.stderr
    assert lab.commands("sockperf under-load --ip 10.0.0.9 --port 5001 --client_port 46000 --mps 1000")
    assert "Phase B: Unicast latency floor" in p.stdout


@pytest.mark.parametrize("bad", ["0", "65536", "x", "46000;id"])
def test_bad_client_port_is_rejected_before_any_aws_call(lab, bad):
    p = lab.run("--client-port", bad, tools="sockperf")
    assert p.returncode != 0 and not lab.calls()
