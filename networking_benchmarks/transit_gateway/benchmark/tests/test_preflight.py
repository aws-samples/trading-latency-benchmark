"""preflight.sh against stubbed /sys, chronyc, ethtool and ip."""
from __future__ import annotations

import os
import pathlib
import stat
import subprocess

import pytest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "host" / "preflight.sh"

SOURCES_OK = """MS Name/IP address         Stratum Poll Reach LastRx Last sample
===============================================================================
#* PHC0                          0   0   377     1    +12ns[  +15ns] +/- 1823ns
^- 169.254.169.123               3   4   377    10   +120us[ +120us] +/-  900us
"""
TRACKING = """Reference ID    : 50484330 (PHC0)
Stratum         : 1
System time     : {sys} seconds {dir} of NTP time
Last offset     : +0.000000012 seconds
RMS offset      : 0.000000030 seconds
Leap status     : Normal
"""


@pytest.fixture
def lab(tmp_path):
    root = tmp_path / "root"
    (root / "sys/module/ena/parameters").mkdir(parents=True)
    (root / "sys/module/ena/parameters/phc_enable").write_text("1\n")
    (root / "sys/class/ptp/ptp0").mkdir(parents=True)
    (root / "sys/class/ptp/ptp0/clock_name").write_text("ena-ptp-00\n")
    fx = tmp_path / "fx"
    fx.mkdir()
    (fx / "sources").write_text(SOURCES_OK)
    (fx / "tracking").write_text(TRACKING.format(sys="0.000000062", dir="fast"))
    (fx / "ethtool").write_text("Capabilities:\n  hardware-transmit\n  hardware-receive\n  hardware-raw-clock\n")
    bin_ = tmp_path / "bin"
    bin_.mkdir()
    log = tmp_path / "calls.log"

    def stub(name, body):
        p = bin_ / name
        p.write_text("#!/bin/bash\n" + body)
        p.chmod(p.stat().st_mode | stat.S_IEXEC)

    stub("chronyc", f'echo "chronyc $*" >> {log}\n'
         f'case "$1" in sources) cat {fx}/sources;; tracking) cat {fx}/tracking;; *) exit 0;; esac\n')
    stub("ethtool", f'cat {fx}/ethtool\n')
    stub("ip", 'echo "default via 10.0.0.1 dev ens5 proto dhcp src 10.0.0.5 metric 100"\n')
    env = dict(os.environ, SYSROOT=str(root), PATH=f"{bin_}:{os.environ['PATH']}", PREFLIGHT_SETTLE="0")
    return {"root": root, "fx": fx, "env": env, "log": log}


def run(lab, *args):
    return subprocess.run(["bash", str(SCRIPT), *args], env=lab["env"], capture_output=True, text=True)


def test_pass_and_forces_a_clock_step(lab):
    p = run(lab)
    assert p.returncode == 0, p.stdout
    assert "PASS" in p.stdout and "system_time_us=0.062" in p.stdout
    calls = lab["log"].read_text()
    assert "chronyc makestep" in calls and "chronyc burst" not in calls
    assert calls.index("makestep") < calls.index("tracking")


def test_phc_disabled(lab):
    (lab["root"] / "sys/module/ena/parameters/phc_enable").write_text("0\n")
    p = run(lab)
    assert p.returncode == 1 and "FAIL:phc_not_enabled" in p.stdout and "PASS" not in p.stdout


def test_no_ena_ptp_device(lab):
    (lab["root"] / "sys/class/ptp/ptp0/clock_name").write_text("other\n")
    assert "FAIL:no_ena_ptp_device" in run(lab).stdout


def test_no_hw_receive(lab):
    (lab["fx"] / "ethtool").write_text("Capabilities:\n  software-receive\n")
    assert "FAIL:no_hw_receive" in run(lab).stdout


def test_two_refclocks(lab):
    (lab["fx"] / "sources").write_text(SOURCES_OK + "#- PHC1       0   0   377     1   +1ns[+1ns] +/- 1us\n")
    assert "FAIL:refclock_count_not_1" in run(lab).stdout


def test_phc_not_selected(lab):
    (lab["fx"] / "sources").write_text(SOURCES_OK.replace("#* PHC0", "#- PHC0").replace("^- 169", "^* 169"))
    assert "FAIL:phc_not_selected" in run(lab).stdout


def test_offset_over_threshold_either_direction(lab):
    (lab["fx"] / "tracking").write_text(TRACKING.format(sys="0.000025000", dir="slow"))
    p = run(lab)
    assert p.returncode == 1 and "clock_offset_exceeds_10us" in p.stdout and "system_time_us=-25.000" in p.stdout


def test_threshold_is_configurable(lab):
    (lab["fx"] / "tracking").write_text(TRACKING.format(sys="0.000025000", dir="fast"))
    assert run(lab, "--max-offset-us", "50").returncode == 0


def test_unreadable_tracking(lab):
    (lab["fx"] / "tracking").write_text("503 Not authorised\n")
    assert "FAIL:clock_unreadable" in run(lab).stdout


def test_clock_mode_is_read_only(lab):
    p = run(lab, "--clock")
    assert p.returncode == 0 and "system_time_us=0.062" in p.stdout and "leap=Normal" in p.stdout
    assert "makestep" not in lab["log"].read_text()


def test_multiple_failures_are_all_reported(lab):
    (lab["root"] / "sys/module/ena/parameters/phc_enable").write_text("0\n")
    (lab["fx"] / "ethtool").write_text("nothing\n")
    out = run(lab).stdout
    assert "FAIL:phc_not_enabled" in out and "FAIL:no_hw_receive" in out


def test_context_mode_records_host_and_nic_state(lab, tmp_path):
    root = lab["root"]
    (root / "proc").mkdir()
    (root / "proc/cpuinfo").write_text("processor : 0\nmodel name : AMD EPYC 9R14\nprocessor : 1\nmodel name : AMD EPYC 9R14\n")
    (root / "proc/cmdline").write_text("BOOT_IMAGE=x root=UUID=1 ro isolcpus=1-3 quiet nohz_full=1-3\n")
    (root / "sys/devices/system/cpu/cpuidle").mkdir(parents=True)
    (root / "sys/devices/system/cpu/cpuidle/current_driver").write_text("acpi_idle\n")
    for n, (name, lat) in enumerate((("POLL", 0), ("C1", 1), ("C2", 800))):
        d = root / f"sys/devices/system/cpu/cpu0/cpuidle/state{n}"
        d.mkdir(parents=True)
        (d / "name").write_text(name + "\n")
        (d / "latency").write_text(f"{lat}\n")
    (root / "sys/class/net/ens5").mkdir(parents=True)
    (root / "sys/class/net/ens5/mtu").write_text("9001\n")
    (root / "sys/class/net/ens5/napi_defer_hard_irqs").write_text("2\n")
    (root / "etc/chrony.d").mkdir(parents=True)
    (root / "etc/chrony.d/aws-phc.conf").write_text("refclock PHC /dev/ptp_ena poll 0 dpoll -2 delay 0.000010 prefer\n")
    bin_ = pathlib.Path(lab["env"]["PATH"].split(":")[0])
    stub = bin_ / "ethtool"
    stub.write_text("""#!/bin/bash
case "$1" in
  -i) printf 'driver: ena\\nversion: 2.14.1g\\n';;
  -c) printf 'Adaptive RX: on  TX: n/a\\nrx-usecs: 20\\ntx-usecs: 64\\n';;
  -l) printf 'Pre-set maximums:\\nCombined:\\t8\\nCurrent hardware settings:\\nCombined:\\t2\\n';;
  -k) printf 'generic-receive-offload: on\\n';;
  *) printf 'hardware-receive\\n';;
esac
""")
    stub.chmod(stub.stat().st_mode | stat.S_IEXEC)
    p = run(lab, "--context")
    assert p.returncode == 0, p.stderr
    kv = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
    assert kv["cpu_model"] == "AMD EPYC 9R14" and kv["cpu_count"] == "2"
    assert kv["ena_driver"] == "ena 2.14.1g"
    assert kv["coalescing"] == "adaptive_rx=on rx_usecs=20 tx_usecs=64"
    assert kv["channels"] == "2" and kv["gro"] == "on" and kv["mtu"] == "9001"
    assert kv["cpuidle_states"] == "POLL:0us C1:1us C2:800us" and kv["cpuidle_driver"] == "acpi_idle"
    assert kv["cmdline_tuning"] == "isolcpus=1-3 nohz_full=1-3"
    assert kv["napi_defer_hard_irqs"] == "2"
    assert "dpoll -2" in kv["chrony_refclock"]
    assert kv["chrony_system_time_us"] == "0.062"
    assert "makestep" not in lab["log"].read_text()   # read-only


def test_static_mode_checks_hardware_and_never_steps_the_clock(lab):
    p = run(lab, "--static")
    assert p.returncode == 0, p.stdout
    assert "PASS" in p.stdout and "refclocks=1" in p.stdout
    assert "system_time_us" not in p.stdout and "selected=" not in p.stdout
    assert "makestep" not in lab["log"].read_text()


def test_static_mode_reports_static_failures_only(lab):
    (lab["root"] / "sys/module/ena/parameters/phc_enable").write_text("0\n")
    (lab["fx"] / "sources").write_text(SOURCES_OK.replace("#* PHC0", "#- PHC0"))   # not selected: a gate matter
    out = run(lab, "--static").stdout
    assert "FAIL:phc_not_enabled" in out and "phc_not_selected" not in out


def test_gate_mode_steps_the_clock_and_skips_static_checks(lab):
    (lab["root"] / "sys/module/ena/parameters/phc_enable").write_text("0\n")      # a static matter
    p = run(lab, "--gate")
    assert p.returncode == 0, p.stdout
    assert "PASS" in p.stdout and "system_time_us=0.062" in p.stdout and "phc_enable" not in p.stdout
    assert "makestep" in lab["log"].read_text()


def test_gate_mode_fails_when_the_phc_is_not_selected_or_the_offset_is_large(lab):
    (lab["fx"] / "sources").write_text(SOURCES_OK.replace("#* PHC0", "#- PHC0").replace("^- 169", "^* 169"))
    assert "FAIL:phc_not_selected" in run(lab, "--gate").stdout
    (lab["fx"] / "sources").write_text(SOURCES_OK)
    (lab["fx"] / "tracking").write_text(TRACKING.format(sys="0.000025000", dir="slow"))
    assert "clock_offset_exceeds_10us" in run(lab, "--gate").stdout
