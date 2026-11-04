"""`orchestrator check` against the fake `aws` CLI (see fake_aws.py)."""
from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_run_benchmark import PUB, SUBS, Lab  # noqa: E402  (Lab builds the fake aws and ssh)

DEFAULT_CHECK = """=== ptp ===
ptp0 ena-ptp-00
=== symlink ===
lrwxrwxrwx 1 root root 4 Jan  1 00:00 /dev/ptp_ena -> ptp0
=== phc_enable ===
1
=== hw_ts ===
0000:00:05.0 0
=== ethtool ===
iface ens5
Capabilities:
        hardware-transmit
        hardware-receive
        hardware-raw-clock
=== sources ===
MS Name/IP address         Stratum Poll Reach LastRx Last sample
===============================================================================
#* PHC0                          0   0   377     1    +12ns[  +15ns] +/- 1823ns
^- 169.254.169.123               3   4   377    10   +120us[ +120us] +/-  900us
=== tracking ===
Reference ID    : 50484330 (PHC0)
System time     : 0.000000062 seconds fast of NTP time
RMS offset      : 0.000000030 seconds
Leap status     : Normal
"""


class Diag(Lab):
    def __init__(self, tmp):
        super().__init__(tmp)
        self.scenario["check"] = {i: DEFAULT_CHECK for i in (PUB, *SUBS)}

    def command(self, name, *args):
        cmd = ["python3", "-m", "orchestrator", name, "--stack-name", "S", *args]
        return subprocess.run(cmd, env=self._env(), capture_output=True, text=True)


@pytest.fixture
def diag(tmp_path):
    return Diag(tmp_path)


def test_check_reports_a_table_and_succeeds_when_synced(diag):
    p = diag.command("check")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "PTP SUMMARY" in p.stdout
    for iid in (PUB, *SUBS):
        row = next(line for line in p.stdout.splitlines() if line.startswith(iid))
        assert "/dev/ptp0" in row and "PTP" in row and "0.062" in row
    assert "publisher" in p.stdout and p.stdout.count("subscriber") >= 2


def test_check_fails_when_an_instance_is_on_ntp(diag):
    diag.scenario["check"][SUBS[0]] = DEFAULT_CHECK.replace("#* PHC0", "#- PHC0").replace("^- 169", "^* 169")
    p = diag.command("check")
    assert p.returncode == 1 and "NTP" in p.stdout and SUBS[0] in p.stderr


def test_check_fails_above_the_system_time_limit_and_the_limit_is_configurable(diag):
    diag.scenario["check"][PUB] = DEFAULT_CHECK.replace("0.000000062 seconds fast", "0.000025000 seconds slow")
    p = diag.command("check")
    assert p.returncode == 1 and "-25.000" in p.stdout
    assert diag.command("check", "--max-sys-us", "50").returncode == 0


def test_check_verbose_prints_the_raw_sections(diag):
    p = diag.command("check", "--verbose")
    assert p.returncode == 0 and "=== tracking ===" in p.stdout and f"Instance: {PUB} (publisher)" in p.stdout


def test_check_treats_an_instance_that_returns_nothing_as_failed(diag):
    diag.scenario["check"][SUBS[1]] = ""
    p = diag.command("check")
    assert p.returncode == 1 and SUBS[1] in p.stderr


@pytest.mark.parametrize("args", [["--max-sys-us", "x"], ["--transport", "telnet"]])
def test_check_rejects_bad_arguments_before_any_aws_call(diag, args):
    p = diag.command("check", *args)
    assert p.returncode != 0 and not diag.calls()
