"""Smoke tests for scripts/mcast_recv.py and scripts/mcast_send.py.

These run on the developer's machine (macOS / Linux) without root and
without an ENA NIC. They verify:

  * The module imports cleanly (catches syntax errors)
  * Public constants match phc_probe's values
  * The cmsg parser correctly extracts ts[0] (SW) and ts[2] (HW)
  * The packet format matches between sender and receiver
  * Default-route iface detection returns *something* (best-effort)

Actual SIOCSHWTSTAMP behavior is not exercised here — that requires an
ENA-backed PHC-enabled EC2 instance. The end-to-end CDK deployment test
covers that path.
"""
from __future__ import annotations

import importlib.util
import pathlib
import struct
import sys

# Load the receiver module directly from disk so we don't depend on
# package layout / __init__.py shenanigans.
SCRIPTS_DIR = pathlib.Path(__file__).resolve().parents[1]


def _load_module(name: str, filename: str):
    path = SCRIPTS_DIR / filename
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    # Linux-only modules in mcast_recv shouldn't fail to import on macOS;
    # only ctypes/socket/struct are imported at module load time.
    spec.loader.exec_module(module)
    return module


def test_mcast_recv_imports():
    mod = _load_module("mcast_recv_under_test", "mcast_recv.py")
    assert mod is not None


def test_mcast_send_imports():
    mod = _load_module("mcast_send_under_test", "mcast_send.py")
    assert mod is not None


def test_constants_match_phc_probe():
    mod = _load_module("mcast_recv_under_test", "mcast_recv.py")
    # Mirror the constants in phc_probe/ts_receiver.py
    assert mod.SO_TIMESTAMPING_NEW == 65
    assert mod.SO_TIMESTAMPING == 37
    assert mod.SCM_TIMESTAMPING_NEW == 65
    assert mod.SCM_TIMESTAMPING == 37
    assert mod.SIOCSHWTSTAMP == 0x89B0
    assert mod.SOF_TIMESTAMPING_RX_HARDWARE == 1 << 2
    assert mod.SOF_TIMESTAMPING_RX_SOFTWARE == 1 << 3
    assert mod.SOF_TIMESTAMPING_SOFTWARE == 1 << 4
    assert mod.SOF_TIMESTAMPING_RAW_HARDWARE == 1 << 6
    assert mod.HWTSTAMP_FILTER_ALL == 1
    assert mod.HWTSTAMP_TX_OFF == 0


def test_packet_format_consistent():
    """Sender and receiver must agree on the wire format."""
    recv = _load_module("mcast_recv_under_test", "mcast_recv.py")
    send = _load_module("mcast_send_under_test", "mcast_send.py")
    assert recv.PACKET_FMT == send.PACKET_FMT == "!IQ"
    assert recv.END_SEQ == send.END_SEQ == 0xFFFFFFFF
    assert recv.PACKET_LEN == 12


def test_cmsg_parser_extracts_hw_and_sw():
    """parse_scm_timestamps decodes a 48-byte scm_timestamping64 buffer."""
    import socket as _socket
    mod = _load_module("mcast_recv_under_test", "mcast_recv.py")

    # Build a synthetic scm_timestamping64 buffer:
    #   ts[0] (SW) = 1234567890 s + 100_000_000 ns
    #   ts[1] (deprecated) = 0
    #   ts[2] (HW) = 1234567890 s + 100_000_500 ns  (500ns later than SW)
    sw_sec, sw_nsec = 1234567890, 100_000_000
    hw_sec, hw_nsec = 1234567890, 100_000_500
    cdata = struct.pack("qqqqqq", sw_sec, sw_nsec, 0, 0, hw_sec, hw_nsec)

    ancdata = [(_socket.SOL_SOCKET, mod.SCM_TIMESTAMPING_NEW, cdata)]
    sw_ns, hw_ns = mod.parse_scm_timestamps(ancdata)
    assert sw_ns == sw_sec * 1_000_000_000 + sw_nsec
    assert hw_ns == hw_sec * 1_000_000_000 + hw_nsec
    assert hw_ns - sw_ns == 500


def test_cmsg_parser_returns_zero_when_no_ts():
    """parse_scm_timestamps returns (0, 0) when no SCM_TIMESTAMPING cmsg present."""
    import socket as _socket
    mod = _load_module("mcast_recv_under_test", "mcast_recv.py")

    # Empty ancillary data
    sw_ns, hw_ns = mod.parse_scm_timestamps([])
    assert sw_ns == 0
    assert hw_ns == 0

    # Wrong cmsg type — should still return zeros
    ancdata = [(_socket.SOL_SOCKET, 999, b"\x00" * 48)]
    sw_ns, hw_ns = mod.parse_scm_timestamps(ancdata)
    assert sw_ns == 0
    assert hw_ns == 0


def test_cmsg_parser_handles_zero_hw_ts():
    """When NIC HW ts is missing (loopback, pre-routing), ts[2] is 0."""
    import socket as _socket
    mod = _load_module("mcast_recv_under_test", "mcast_recv.py")

    # SW present, HW missing (typical of loopback or NIC without HW timestamping)
    cdata = struct.pack("qqqqqq", 1234567890, 100_000_000, 0, 0, 0, 0)
    ancdata = [(_socket.SOL_SOCKET, mod.SCM_TIMESTAMPING_NEW, cdata)]
    sw_ns, hw_ns = mod.parse_scm_timestamps(ancdata)
    assert sw_ns > 0
    assert hw_ns == 0


def test_detect_iface_returns_string():
    """detect_iface must return a non-empty string (best-effort across platforms)."""
    mod = _load_module("mcast_recv_under_test", "mcast_recv.py")
    iface = mod.detect_iface()
    assert isinstance(iface, str)
    assert len(iface) > 0
