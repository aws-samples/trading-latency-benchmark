#!/usr/bin/env python3
"""Multicast receiver with NIC hardware RX timestamping (ENA PHC).

Mirrors the SO_TIMESTAMPING + SIOCSHWTSTAMP pattern used by the AWS
phc_probe sample (aws-samples/trading-latency-benchmark, networking_benchmarks/
utilities/phc_probe/ts_receiver.py) and applies it to multicast UDP for
one-way TGW latency measurement.

Requirements (enforced at runtime — exit non-zero if missing):
  * Linux kernel with SO_TIMESTAMPING (or SO_TIMESTAMPING_NEW)
  * ENA driver with phc_enable=1 (PHC-capable instance family)
  * Root / CAP_NET_ADMIN to issue SIOCSHWTSTAMP

Payload format (matches scripts/mcast_send.py and phc_probe ts_sender.py):
  Offset  Size  Field         Encoding
       0     4  seq_num       uint32_t, network byte order
       4     8  tx_ns         uint64_t, network byte order, ns since epoch
                              (CLOCK_REALTIME on the sender)

End-of-stream sentinel:
  seq == 0xFFFFFFFF, tx_ns slot carries total_sent count from sender.
  Used to derive the true expected packet count (avoids tail-loss undercount).

Usage:
  sudo python3 mcast_recv.py <group> <port> <timeout_secs> [iface]

Output:
  Single JSON object on stdout with latency stats and timestamp source counts.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import json
import os
import socket
import statistics
import struct
import subprocess
import sys

# ── Kernel constants (Linux) ────────────────────────────────────────────────
SO_TIMESTAMPING        = 37
SO_TIMESTAMPING_NEW    = 65
SCM_TIMESTAMPING       = 37
SCM_TIMESTAMPING_NEW   = 65
SO_BUSY_POLL           = 46

SOF_TIMESTAMPING_RX_HARDWARE  = 1 << 2
SOF_TIMESTAMPING_RX_SOFTWARE  = 1 << 3
SOF_TIMESTAMPING_SOFTWARE     = 1 << 4
SOF_TIMESTAMPING_RAW_HARDWARE = 1 << 6

SIOCSHWTSTAMP = 0x89B0

HWTSTAMP_TX_OFF       = 0
HWTSTAMP_FILTER_ALL   = 1
HWTSTAMP_FILTER_NONE  = 0

END_SEQ = 0xFFFFFFFF
PACKET_FMT = "!IQ"           # 4-byte seq + 8-byte ns timestamp (network order)
PACKET_LEN = struct.calcsize(PACKET_FMT)  # 12

# 3 × struct timespec64 = 6 × int64 = 48 bytes
SCM_TS_FMT = "qqqqqq"
SCM_TS_LEN = struct.calcsize(SCM_TS_FMT)


# ── ctypes structs (mirror phc_probe/ts_receiver.py) ────────────────────────
class HwtstampConfig(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_int),
        ("tx_type", ctypes.c_int),
        ("rx_filter", ctypes.c_int),
    ]


class Ifreq(ctypes.Structure):
    _fields_ = [
        ("ifr_name", ctypes.c_char * 16),
        ("ifr_data", ctypes.c_void_p),
    ]


def _libc():
    return ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6", use_errno=True)


def detect_iface() -> str:
    """Return the interface name of the default IPv4 route, or first UP non-lo iface."""
    try:
        out = subprocess.run(
            ["ip", "-o", "-4", "route", "show", "to", "default"],
            capture_output=True, text=True, timeout=3, check=False,
        )
        toks = out.stdout.split()
        if "dev" in toks:
            return toks[toks.index("dev") + 1]
    except Exception:
        pass

    # Fallback: first UP non-loopback iface
    try:
        for entry in os.listdir("/sys/class/net"):
            if entry == "lo":
                continue
            with open(f"/sys/class/net/{entry}/operstate") as f:
                if f.read().strip() == "up":
                    return entry
    except Exception:
        pass

    return "eth0"


def enable_hw_timestamping(sock_fd: int, iface: str, rx_filter: int = HWTSTAMP_FILTER_ALL) -> None:
    """Issue SIOCSHWTSTAMP ioctl. Raises OSError on failure."""
    config = HwtstampConfig(flags=0, tx_type=HWTSTAMP_TX_OFF, rx_filter=rx_filter)
    ifreq = Ifreq()
    ifreq.ifr_name = iface.encode()[:15].ljust(16, b"\x00")
    ifreq.ifr_data = ctypes.addressof(config)

    libc = _libc()
    rc = libc.ioctl(
        ctypes.c_int(sock_fd),
        ctypes.c_ulong(SIOCSHWTSTAMP),
        ctypes.byref(ifreq),
    )
    if rc < 0:
        errno_val = ctypes.get_errno()
        raise OSError(errno_val, f"SIOCSHWTSTAMP on {iface}: {os.strerror(errno_val)}")


def parse_scm_timestamps(ancdata):
    """Extract (sw_ns, hw_ns) from SCM_TIMESTAMPING ancillary data.

    Returns (0, 0) if no SCM_TIMESTAMPING cmsg present. ts[0] is kernel
    software (CLOCK_REALTIME); ts[2] is raw NIC hardware (ENA PHC).
    """
    sw_ns = 0
    hw_ns = 0
    for level, typ, cdata in ancdata:
        if level != socket.SOL_SOCKET:
            continue
        if typ not in (SCM_TIMESTAMPING_NEW, SCM_TIMESTAMPING):
            continue
        if len(cdata) < SCM_TS_LEN:
            continue
        ts = struct.unpack(SCM_TS_FMT, cdata[:SCM_TS_LEN])
        # ts[0]/ts[1] = software (CLOCK_REALTIME)
        # ts[2]/ts[3] = deprecated, always zero
        # ts[4]/ts[5] = raw hardware (PHC)
        if ts[0] >= 0:
            sw_ns = ts[0] * 1_000_000_000 + ts[1]
        if ts[4] >= 0:
            hw_ns = ts[4] * 1_000_000_000 + ts[5]
    return sw_ns, hw_ns


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: mcast_recv.py <group> <port> <timeout_secs> [iface]", file=sys.stderr)
        return 2

    group = sys.argv[1]
    port = int(sys.argv[2])
    timeout = int(sys.argv[3])
    iface = sys.argv[4] if len(sys.argv) > 4 else detect_iface()

    diagnostics: dict = {
        "iface": iface,
        "so_timestamping_variant": None,
        "hw_timestamping_enabled": False,
    }

    # --- Pin to CPU 1 to avoid scheduler jitter ---
    try:
        os.sched_setaffinity(0, {1})
    except (AttributeError, OSError):
        pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)

    try:
        sock.setsockopt(socket.SOL_SOCKET, SO_BUSY_POLL, 50)
    except OSError:
        pass

    # --- Enable SO_TIMESTAMPING (HW + SW) before the first recv ---
    flags = (
        SOF_TIMESTAMPING_RX_HARDWARE
        | SOF_TIMESTAMPING_RX_SOFTWARE
        | SOF_TIMESTAMPING_SOFTWARE
        | SOF_TIMESTAMPING_RAW_HARDWARE
    )
    try:
        sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING_NEW, struct.pack("I", flags))
        diagnostics["so_timestamping_variant"] = "SO_TIMESTAMPING_NEW"
    except OSError:
        sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING, struct.pack("I", flags))
        diagnostics["so_timestamping_variant"] = "SO_TIMESTAMPING"

    # --- Enable HW RX timestamping on the NIC (SIOCSHWTSTAMP) ---
    # Required: this benchmark is gated to PHC-capable instances.
    try:
        enable_hw_timestamping(sock.fileno(), iface)
        diagnostics["hw_timestamping_enabled"] = True
    except OSError as e:
        sock.close()
        result = {
            "error": "SIOCSHWTSTAMP failed",
            "detail": str(e),
            "iface": iface,
            "hint": "Instance must be PHC-capable (M7/R7/I8/C7/C8/M8/R8/X8 family) "
                    "with phc_enable=1 in the ENA driver. Check `ethtool -T %s`." % iface,
        }
        print(json.dumps(result))
        return 1

    # --- Bind + join multicast group ---
    sock.bind(("", port))
    mreq = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton("0.0.0.0"))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    sock.settimeout(timeout)

    # --- Receive loop ---
    latencies_ns: list[int] = []
    received = 0
    max_seq = -1
    hw_count = 0
    sender_total_sent: int | None = None

    try:
        while True:
            try:
                data, ancdata, _msg_flags, _addr = sock.recvmsg(2048, 1024)
            except socket.timeout:
                break

            sw_ns, hw_ns = parse_scm_timestamps(ancdata)

            # SOF_TIMESTAMPING_RAW_HARDWARE requires ts[2] (hw_ns) to be
            # populated by the NIC.  If it's zero, the ENA PHC setup is broken.
            # See kernel docs: networking/timestamping.html §SOF_TIMESTAMPING_RAW_HARDWARE
            if hw_ns == 0:
                err = {
                    "error": "RAW_HARDWARE timestamp (ts[2]) is zero",
                    "detail": (
                        "Received a packet without a hardware PHC timestamp. "
                        "This indicates a broken PTP/ENA setup: either phc_enable "
                        "is not active, the ENA driver was not reloaded, or the "
                        "instance family does not support RAW_HARDWARE timestamping."
                    ),
                    "iface": iface,
                    "sw_ns": sw_ns,
                    "packets_before_failure": received,
                    "hint": (
                        "Verify: cat /sys/module/ena/parameters/phc_enable == 1, "
                        "ethtool -T <iface> shows 'hardware-raw-clock', "
                        "and /dev/ptp_ena symlink exists."
                    ),
                }
                print(json.dumps(err))
                return 1

            rx_ns = hw_ns
            hw_count += 1

            if len(data) < PACKET_LEN:
                continue

            seq, tx_ns = struct.unpack(PACKET_FMT, data[:PACKET_LEN])

            if seq == END_SEQ:
                sender_total_sent = int(tx_ns)
                break

            received += 1
            if seq > max_seq:
                max_seq = seq

            latencies_ns.append(rx_ns - tx_ns)
    finally:
        try:
            enable_hw_timestamping(sock.fileno(), iface, rx_filter=HWTSTAMP_FILTER_NONE)
        except OSError:
            pass
        sock.close()

    # --- Stats ---
    if sender_total_sent is not None:
        expected = sender_total_sent
    elif max_seq >= 0:
        expected = max_seq + 1
    else:
        expected = 0
    loss = max(expected - received, 0)

    result: dict = {
        "iface": iface,
        "so_timestamping_variant": diagnostics["so_timestamping_variant"],
        "hw_timestamping_enabled": diagnostics["hw_timestamping_enabled"],
        "total_expected": expected,
        "total_received": received,
        "packet_loss_count": loss,
        "hw_timestamp_count": hw_count,
        "end_marker_received": sender_total_sent is not None,
    }

    if latencies_ns:
        latencies_ns.sort()
        n = len(latencies_ns)
        result.update({
            "min_latency_us": round(latencies_ns[0] / 1000, 3),
            "max_latency_us": round(latencies_ns[-1] / 1000, 3),
            "mean_latency_us": round(statistics.mean(latencies_ns) / 1000, 3),
            "median_latency_us": round(latencies_ns[n // 2] / 1000, 3),
            "p95_latency_us": round(latencies_ns[min(int(n * 0.95), n - 1)] / 1000, 3),
            "p99_latency_us": round(latencies_ns[min(int(n * 0.99), n - 1)] / 1000, 3),
        })

    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
