"""
Integration tests for the AF_XDP benchmark suite using the kernel-mode replicator.

Kernel mode (``replicator --kernel-mode``) implements the same control protocol
(ADD/REMOVE/LIST on the control port) and UDP echo as the AF_XDP replicator, but
over standard kernel sockets — no root, no XDP/BPF. That makes these tests
runnable in containers / CI / macOS. They exercise the *real* measurement client
(``rtt``) and the ``replicator_ctl`` / ``udp_send`` binaries end-to-end;
the echo/control server is a lightweight stand-in (src/KernelEcho.cpp), so this
is a functional/contract smoke test — the production AF_XDP datapath itself is
validated separately on EC2 (run_ucast).

Ports are off the production defaults (see conftest.py) so the suite does not
collide with a live replicator.service. The control port is exported via
AFXDP_CONTROL_PORT (conftest) and honoured by all binaries (src/ControlPort.hpp).

Usage:
  cd networking_benchmarks/af_xdp
  make all            # or: make kernel-mode
  pytest tests/ -v
"""

import json
import os
import signal
import socket
import struct
import subprocess
import time
from pathlib import Path

import pytest

# ── Paths ─────────────────────────────────────────────────────────────────────
# dev/tests/ → af_xdp root is three levels up (binaries built by `make` at the root).
AF_XDP_DIR = Path(__file__).parent.parent.parent
REPLICATOR = AF_XDP_DIR / "replicator"
RTT = AF_XDP_DIR / "rtt"
REPLICATOR_CTL = AF_XDP_DIR / "replicator_ctl"
UDP_SEND = AF_XDP_DIR / "udp_send"
MCAST_SEND = AF_XDP_DIR / "mcast_send"
MCAST_RECEIVE = AF_XDP_DIR / "mcast_receive"

# ── Non-production ports (must match conftest.py) ─────────────────────────────
LISTEN_IP = "127.0.0.1"
CONTROL_PORT = 23456          # prod: 12345
DATA_PORT = 29000             # prod ucast data: 5000
os.environ["AFXDP_CONTROL_PORT"] = str(CONTROL_PORT)


# ── Fixtures ──────────────────────────────────────────────────────────────────
@pytest.fixture(scope="module")
def replicator_process():
    """Start kernel-mode replicator for the test module."""
    if not REPLICATOR.exists():
        pytest.skip(f"Binary not found: {REPLICATOR}. Run 'make all' first.")

    proc = subprocess.Popen(
        [str(REPLICATOR), "--kernel-mode", LISTEN_IP, str(DATA_PORT)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env={**os.environ, "AFXDP_CONTROL_PORT": str(CONTROL_PORT)},
    )
    time.sleep(0.5)
    if proc.poll() is not None:
        output = proc.stdout.read()
        pytest.fail(f"Replicator exited immediately: {output}")

    yield proc

    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)


@pytest.fixture
def ctrl_socket():
    """UDP socket for control protocol testing."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    yield sock
    sock.close()


# ── Helpers ───────────────────────────────────────────────────────────────────
def _add_msg(ip: str, port: int) -> bytes:
    return bytes([1]) + socket.inet_aton(ip) + struct.pack("!H", port)


def _remove_msg(ip: str, port: int) -> bytes:
    return bytes([2]) + socket.inet_aton(ip) + struct.pack("!H", port)


def _ctrl_roundtrip(sock, msg) -> bytes:
    sock.sendto(msg, (LISTEN_IP, CONTROL_PORT))
    ack, _ = sock.recvfrom(64)
    return ack


def _make_receiver(port: int, timeout: float = 2.0) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.bind((LISTEN_IP, port))
    return s


def _register(ctrl_socket, port: int):
    assert _ctrl_roundtrip(ctrl_socket, _add_msg(LISTEN_IP, port)) == b"\x01"


# ── Test: Binary existence ────────────────────────────────────────────────────
class TestBinaries:
    def test_replicator_exists(self):
        assert REPLICATOR.exists(), f"Missing: {REPLICATOR}"

    def test_rtt_exists(self):
        assert RTT.exists(), f"Missing: {RTT}"

    def test_replicator_ctl_exists(self):
        assert REPLICATOR_CTL.exists(), f"Missing: {REPLICATOR_CTL}"

    def test_udp_send_exists(self):
        assert UDP_SEND.exists(), f"Missing: {UDP_SEND}"


# ── Test: Control protocol (happy path) ───────────────────────────────────────
class TestControlProtocol:
    def test_add_destination(self, replicator_process, ctrl_socket):
        assert _ctrl_roundtrip(ctrl_socket, _add_msg(LISTEN_IP, 29001)) == b"\x01"

    def test_add_duplicate_destination(self, replicator_process, ctrl_socket):
        assert _ctrl_roundtrip(ctrl_socket, _add_msg(LISTEN_IP, 29002)) == b"\x01"
        assert _ctrl_roundtrip(ctrl_socket, _add_msg(LISTEN_IP, 29002)) == b"\x01"

    def test_list_destinations(self, replicator_process, ctrl_socket):
        """LIST returns [1B count][per dest: 4B IP + 2B port] (prod wire format)."""
        ctrl_socket.sendto(bytes([3]), (LISTEN_IP, CONTROL_PORT))
        resp, _ = ctrl_socket.recvfrom(1024)
        assert len(resp) >= 1
        count = resp[0]
        assert len(resp) == 1 + 6 * count, f"malformed list: count={count} len={len(resp)}"
        assert count >= 1, "expected at least one registered destination"

    def test_remove_destination(self, replicator_process, ctrl_socket):
        assert _ctrl_roundtrip(ctrl_socket, _add_msg(LISTEN_IP, 29003)) == b"\x01"
        assert _ctrl_roundtrip(ctrl_socket, _remove_msg(LISTEN_IP, 29003)) == b"\x01"


# ── Test: Control protocol (negative / robustness) ────────────────────────────
class TestControlProtocolNegative:
    def test_remove_unknown_destination(self, replicator_process, ctrl_socket):
        """Removing a never-registered destination should NOT ack success."""
        ack = _ctrl_roundtrip(ctrl_socket, _remove_msg(LISTEN_IP, 29404))
        assert ack == b"\x00", f"Expected FAIL ack for unknown remove, got {ack!r}"

    def test_unknown_command(self, replicator_process, ctrl_socket):
        """An unknown command byte should be rejected (ack=0)."""
        ack = _ctrl_roundtrip(ctrl_socket, bytes([99]))
        assert ack == b"\x00", f"Expected FAIL ack for unknown cmd, got {ack!r}"

    def test_malformed_add_too_short(self, replicator_process, ctrl_socket):
        """An ADD missing the IP/port payload should be rejected (ack=0)."""
        ack = _ctrl_roundtrip(ctrl_socket, bytes([1, 0x7F]))  # cmd + 1 byte only
        assert ack == b"\x00", f"Expected FAIL ack for short ADD, got {ack!r}"


# ── Test: Data echo ───────────────────────────────────────────────────────────
class TestDataEcho:
    def test_echo_to_registered_destination(self, replicator_process, ctrl_socket):
        recv = _make_receiver(29010)
        _register(ctrl_socket, 29010)

        send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = b"TEST_ECHO_12345678"
        send.sendto(payload, (LISTEN_IP, DATA_PORT))

        data, _ = recv.recvfrom(2048)
        assert data == payload, f"Echo mismatch: sent {payload!r}, got {data!r}"
        recv.close()
        send.close()

    def test_no_echo_to_unregistered(self, replicator_process):
        recv = _make_receiver(29011, timeout=0.5)
        send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        send.sendto(b"SHOULD_NOT_ARRIVE", (LISTEN_IP, DATA_PORT))
        with pytest.raises(socket.timeout):
            recv.recvfrom(2048)
        recv.close()
        send.close()

    def test_multi_destination_fanout(self, replicator_process, ctrl_socket):
        """A single packet must be echoed to every registered destination."""
        r1 = _make_receiver(29012)
        r2 = _make_receiver(29013)
        _register(ctrl_socket, 29012)
        _register(ctrl_socket, 29013)

        send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = b"FANOUT_PAYLOAD"
        send.sendto(payload, (LISTEN_IP, DATA_PORT))

        assert r1.recvfrom(2048)[0] == payload
        assert r2.recvfrom(2048)[0] == payload
        r1.close()
        r2.close()
        send.close()

    def test_remove_stops_echo(self, replicator_process, ctrl_socket):
        """After REMOVE, a destination must stop receiving echoes."""
        recv = _make_receiver(29014, timeout=1.0)
        _register(ctrl_socket, 29014)

        send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        send.sendto(b"BEFORE_REMOVE", (LISTEN_IP, DATA_PORT))
        assert recv.recvfrom(2048)[0] == b"BEFORE_REMOVE"

        assert _ctrl_roundtrip(ctrl_socket, _remove_msg(LISTEN_IP, 29014)) == b"\x01"
        time.sleep(0.1)
        send.sendto(b"AFTER_REMOVE", (LISTEN_IP, DATA_PORT))
        with pytest.raises(socket.timeout):
            recv.recvfrom(2048)
        recv.close()
        send.close()

    def test_large_payload_echo(self, replicator_process, ctrl_socket):
        """A near-MTU binary payload must round-trip byte-for-byte."""
        recv = _make_receiver(29015)
        _register(ctrl_socket, 29015)

        send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        payload = bytes(range(256)) * 5 + b"END"  # 1283 bytes, full byte range
        send.sendto(payload, (LISTEN_IP, DATA_PORT))

        data, _ = recv.recvfrom(4096)
        assert data == payload, f"Large echo mismatch: {len(data)} vs {len(payload)} bytes"
        recv.close()
        send.close()


# ── Test: RTT binary ──────────────────────────────────────────────────────────
class TestRTTMeasurement:
    def _run_rtt(self, local_port, total, rate, warmup, timeout=30):
        json_path = "/tmp/rtt_results.json"
        if os.path.exists(json_path):
            os.remove(json_path)
        result = subprocess.run(
            [
                str(RTT),
                LISTEN_IP, str(DATA_PORT),
                LISTEN_IP, str(local_port),
                str(total), str(rate), str(warmup), "0", "1",
            ],
            capture_output=True, text=True, timeout=timeout,
            env={**os.environ, "AFXDP_CONTROL_PORT": str(CONTROL_PORT)},
        )
        return result, json_path

    def test_rtt_produces_json(self, replicator_process):
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result, json_path = self._run_rtt(29020, 1000, 1000, 100)
        assert result.returncode == 0, f"rtt failed: {result.stderr}\n{result.stdout}"
        assert os.path.exists(json_path), "JSON output not created"
        with open(json_path) as f:
            data = json.load(f)
        rtt = data["service_rtt_us"]
        for k in ("min", "p50", "p99", "max"):
            assert k in rtt, f"Missing {k} in {rtt.keys()}"
        assert rtt["p50"] > 0
        assert rtt["p99"] >= rtt["p50"]

    def test_rtt_json_schema_fields(self, replicator_process):
        """Top-level JSON must carry the run metadata the report relies on."""
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result, json_path = self._run_rtt(29023, 1000, 1000, 100)
        assert result.returncode == 0
        with open(json_path) as f:
            data = json.load(f)
        for key in ("messages", "warmup", "rate_mps", "lost", "loss_pct",
                    "timestamp_rx", "timestamp_tx"):
            assert key in data, f"Missing top-level key {key}: {list(data.keys())}"
        assert data["rate_mps"] == 1000
        assert data["timestamp_tx"] == "clock_realtime"

    def test_rtt_zero_loss_localhost(self, replicator_process):
        """Localhost kernel echo should not drop packets."""
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result, json_path = self._run_rtt(29024, 2000, 2000, 200)
        assert result.returncode == 0
        with open(json_path) as f:
            data = json.load(f)
        assert data["lost"] == 0, f"Unexpected loss on localhost: {data['lost']}"

    def test_rtt_respects_warmup(self, replicator_process):
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result, json_path = self._run_rtt(29021, 500, 1000, 400)
        assert result.returncode == 0
        with open(json_path) as f:
            data = json.load(f)
        assert data.get("messages", 0) in (500, 100)

    def test_rtt_localhost_latency_sanity(self, replicator_process):
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result, json_path = self._run_rtt(29022, 1000, 1000, 100)
        assert result.returncode == 0
        with open(json_path) as f:
            data = json.load(f)
        p50 = data["service_rtt_us"]["p50"]
        assert p50 < 5000, f"p50={p50}µs — too high for localhost kernel echo"

    def test_rtt_invalid_args(self):
        """rtt with too few args should exit non-zero and print usage."""
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result = subprocess.run([str(RTT), LISTEN_IP], capture_output=True, text=True, timeout=5)
        assert result.returncode != 0
        assert "Usage" in (result.stdout + result.stderr)

    def test_rtt_xdp_tx_requires_iface(self):
        """--xdp-tx must be rejected (exit != 0) without --iface. Validated before
        any socket/AF_XDP setup, so it is container-safe."""
        if not RTT.exists():
            pytest.skip("rtt binary not found")
        result = subprocess.run(
            [str(RTT), LISTEN_IP, str(DATA_PORT), LISTEN_IP, "29030", "100", "100", "--xdp-tx"],
            capture_output=True, text=True, timeout=5,
        )
        # kernel-mode builds reject --xdp-tx outright; full builds require --iface.
        assert result.returncode != 0
        out = (result.stdout + result.stderr).lower()
        assert "iface" in out or "kernel-mode" in out


# ── Test: replicator_ctl CLI round-trip ───────────────────────────────────────
class TestReplicatorCtl:
    def _ctl(self, *args, timeout=5):
        return subprocess.run(
            [str(REPLICATOR_CTL), LISTEN_IP, *args],
            capture_output=True, text=True, timeout=timeout,
            env={**os.environ, "AFXDP_CONTROL_PORT": str(CONTROL_PORT)},
        )

    def test_ctl_add_then_list(self, replicator_process):
        """replicator_ctl add should succeed and the dest should appear in list."""
        if not REPLICATOR_CTL.exists():
            pytest.skip("replicator_ctl binary not found")
        add = self._ctl("add", LISTEN_IP, "29055")
        assert add.returncode == 0, f"ctl add failed: {add.stderr}\n{add.stdout}"
        assert "successful" in add.stdout.lower()

        lst = self._ctl("list")
        assert lst.returncode == 0
        assert "29055" in lst.stdout, f"dest not listed: {lst.stdout}"

    def test_ctl_usage_on_no_command(self):
        if not REPLICATOR_CTL.exists():
            pytest.skip("replicator_ctl binary not found")
        result = subprocess.run([str(REPLICATOR_CTL)], capture_output=True, text=True, timeout=5)
        assert result.returncode != 0
        assert "Usage" in (result.stdout + result.stderr)


# ── Test: udp_send ────────────────────────────────────────────────────────────
class TestUdpSend:
    def test_udp_send_help(self):
        if not UDP_SEND.exists():
            pytest.skip("udp_send binary not found")
        result = subprocess.run([str(UDP_SEND)], capture_output=True, text=True, timeout=5)
        assert result.returncode != 0
        assert "Usage" in result.stdout or "Usage" in result.stderr


# ── Test: mcast_send / mcast_receive (CLI only) ───────────────────────────────
# The mcast tools require root + XDP + a real NIC (GRE datapath), so only their
# argument-parsing/usage layer is container-testable here. The GRE build,
# AF_XDP TX/RX, and per-hop (source→replicator→destination) latency are
# validated on EC2 via deploy/ansible/run_mcast.yaml.
class TestMcastBinaries:
    def test_mcast_send_exists(self):
        assert MCAST_SEND.exists(), f"Missing: {MCAST_SEND}"

    def test_mcast_receive_exists(self):
        assert MCAST_RECEIVE.exists(), f"Missing: {MCAST_RECEIVE}"


class TestMcastSendCli:
    """mcast_send arg parsing (runs before any AF_XDP/root/NIC access)."""

    def _run(self, *args, timeout=5):
        return subprocess.run([str(MCAST_SEND), *args],
                              capture_output=True, text=True, timeout=timeout)

    def test_help_exits_zero(self):
        if not MCAST_SEND.exists():
            pytest.skip("mcast_send binary not found")
        r = self._run("-h")
        assert r.returncode == 0
        assert "Usage" in (r.stdout + r.stderr)

    def test_missing_required_dst_fails(self):
        """-D <replicator-ip> is required; without it exit != 0 and usage shown."""
        if not MCAST_SEND.exists():
            pytest.skip("mcast_send binary not found")
        r = self._run()  # no args at all
        assert r.returncode != 0
        out = r.stdout + r.stderr
        assert "-D" in out or "required" in out.lower()

    def test_unknown_option_fails(self):
        if not MCAST_SEND.exists():
            pytest.skip("mcast_send binary not found")
        r = self._run("-Z")
        assert r.returncode != 0
        assert "Usage" in (r.stdout + r.stderr)


class TestMcastReceiveCli:
    """mcast_receive arg parsing (runs before XDP attach / AF_XDP)."""

    def _run(self, *args, timeout=5):
        return subprocess.run([str(MCAST_RECEIVE), *args],
                              capture_output=True, text=True, timeout=timeout)

    def test_help_exits_zero(self):
        if not MCAST_RECEIVE.exists():
            pytest.skip("mcast_receive binary not found")
        r = self._run("-h")
        assert r.returncode == 0
        assert "Usage" in (r.stdout + r.stderr)

    def test_missing_required_iface_fails(self):
        """-I <iface> is required; without it exit != 0 and usage shown."""
        if not MCAST_RECEIVE.exists():
            pytest.skip("mcast_receive binary not found")
        r = self._run()  # no args
        assert r.returncode != 0
        out = r.stdout + r.stderr
        assert "-I" in out or "required" in out.lower()

    def test_unknown_option_fails(self):
        if not MCAST_RECEIVE.exists():
            pytest.skip("mcast_receive binary not found")
        r = self._run("-Z")
        assert r.returncode != 0
        assert "Usage" in (r.stdout + r.stderr)
