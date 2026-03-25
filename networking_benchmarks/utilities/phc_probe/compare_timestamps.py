#!/usr/bin/env python3
"""Compare hardware vs software (kernel) timestamps from SO_TIMESTAMPING.

Sends UDP packets through a real NIC and displays both timestamp sources
from the scm_timestamping64 ancillary data.

Reference: https://www.kernel.org/doc/html/latest/networking/timestamping.html
Validated against: https://github.com/aws-samples/trading-latency-benchmark

Usage:
    sudo python3 scripts/compare_timestamps.py <iface> <nic_ip> [num_packets]
"""

import ctypes
import os
import socket
import struct
import subprocess
import sys
import time

# ── constants (kernel docs §1.3.1 and §1.3.2) ───────────────────────
SOF_TIMESTAMPING_RX_HARDWARE  = 1 << 2   # §1.3.1: request HW RX timestamps
SOF_TIMESTAMPING_RX_SOFTWARE  = 1 << 3   # §1.3.1: request SW RX timestamps
SOF_TIMESTAMPING_SOFTWARE     = 1 << 4   # §1.3.2: report SW timestamps
SOF_TIMESTAMPING_RAW_HARDWARE = 1 << 6   # §1.3.2: report HW timestamps
SO_TIMESTAMPING_NEW = 65                 # §1.3: 64-bit safe variant
SCM_TIMESTAMPING_NEW = 65               # §2.1: cmsg type for scm_timestamping64
SIOCSHWTSTAMP = 0x89B0                   # §3: ioctl to configure NIC HW timestamping

# ── colours ──────────────────────────────────────────────────────────
GREEN  = "\033[0;32m"
RED    = "\033[0;31m"
YELLOW = "\033[1;33m"
CYAN   = "\033[0;36m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
NC     = "\033[0m"


# ── ctypes structs for SIOCSHWTSTAMP ioctl ───────────────────────────

class HwtstampConfig(ctypes.Structure):
    """hwtstamp_config from linux/net_tstamp.h (§3)"""
    _fields_ = [("flags", ctypes.c_int), ("tx_type", ctypes.c_int),
                ("rx_filter", ctypes.c_int)]


class Ifreq(ctypes.Structure):
    """struct ifreq — ifr_name[16] + ifr_data (void pointer)"""
    _fields_ = [("ifr_name", ctypes.c_char * 16),
                ("ifr_data", ctypes.c_void_p)]


def enable_ioctl(sock_fd: int, iface: str) -> bool:
    """Enable HW timestamping on NIC via SIOCSHWTSTAMP ioctl.

    Returns True if ioctl succeeded, False otherwise.
    """
    config = HwtstampConfig(flags=0, tx_type=0, rx_filter=1)
    ifreq = Ifreq()
    ifreq.ifr_name = iface.encode()[:15].ljust(16, b"\x00")
    ifreq.ifr_data = ctypes.addressof(config)

    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        ret = libc.ioctl(
            ctypes.c_int(sock_fd),
            ctypes.c_ulong(SIOCSHWTSTAMP),
            ctypes.byref(ifreq),
        )
        if ret < 0:
            errno = ctypes.get_errno()
            raise OSError(errno, os.strerror(errno))
        return True
    except OSError as e:
        print(f"  SIOCSHWTSTAMP ioctl: {RED}FAILED{NC} ({e})")
        print(f"  {DIM}(Need CAP_NET_ADMIN / sudo for hardware timestamps){NC}")
        return False


def detect_ena_phc(iface: str) -> bool:
    """Detect if ENA driver delivers PHC timestamps in ts[0]."""
    try:
        et = subprocess.run(
            ["ethtool", "-T", iface],
            capture_output=True, text=True, timeout=3,
        )
        return "hardware-receive" in et.stdout
    except Exception:
        return False


def collect_timestamps(iface: str, nic_ip: str, num_packets: int):
    """Send packets and collect both ts[0] and ts[2] timestamps."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    port = sock.getsockname()[1]

    # Enable SO_TIMESTAMPING_NEW on socket
    flags = (SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE
             | SOF_TIMESTAMPING_SOFTWARE  | SOF_TIMESTAMPING_RAW_HARDWARE)
    sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING_NEW, struct.pack("I", flags))

    # Enable HW timestamping on NIC via ioctl
    hw_ts_enabled = enable_ioctl(sock.fileno(), iface)
    if hw_ts_enabled:
        print(f"  SIOCSHWTSTAMP ioctl: {GREEN}OK{NC} — NIC hardware timestamping enabled")

    print(f"  Listening on port:  {port}")
    print()

    sock.setblocking(True)
    sock.settimeout(3.0)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    results = []
    for i in range(num_packets):
        payload = f"PKT_{i:04d}".encode()
        wall_before = time.time_ns()
        sender.sendto(payload, (nic_ip, port))

        try:
            data, ancdata, msg_flags, addr = sock.recvmsg(2048, 1024)
        except socket.timeout:
            results.append((i, payload, 0, 0, 0, 0))
            time.sleep(0.01)
            continue

        wall_after = time.time_ns()

        sw_ns = 0
        hw_ns = 0
        for level, typ, cdata in ancdata:
            if level == socket.SOL_SOCKET and typ == SCM_TIMESTAMPING_NEW and len(cdata) >= 48:
                ts = struct.unpack("qqqqqq", cdata[:48])
                sw_ns = ts[0] * 1_000_000_000 + ts[1] if ts[0] >= 0 else 0
                hw_ns = ts[4] * 1_000_000_000 + ts[5] if ts[4] >= 0 else 0

        results.append((i, data, sw_ns, hw_ns, wall_before, wall_after))
        time.sleep(0.01)

    sender.close()
    sock.close()
    return results, hw_ts_enabled


def display_results(results, hw_ts_enabled: bool, iface: str):
    """Display timestamp comparison table and summary."""
    ena_phc_in_ts0 = hw_ts_enabled and detect_ena_phc(iface)

    print(f"{BOLD}{'#':>3}  {'Payload':<12}  {'ts[0] (ns)':>22}  {'ts[2] (ns)':>22}  {'Wall-ts[0]':>12}  Source{NC}")
    print(f"{'─'*3}  {'─'*12}  {'─'*22}  {'─'*22}  {'─'*12}  {'─'*10}")

    hw_count = 0
    phc_ts0_count = 0
    sw_count = 0
    hw_sw_deltas = []
    wall_deltas = []

    for idx, payload, sw_ns, hw_ns, wall_before, wall_after in results:
        payload_str = payload.decode("utf-8", errors="replace")
        wall_mid = (wall_before + wall_after) // 2 if wall_before > 0 else 0

        if hw_ns > 0:
            source = f"{GREEN}HW ts[2]{NC}"
            hw_count += 1
            hw_sw_deltas.append(hw_ns - sw_ns)
        elif sw_ns > 0 and ena_phc_in_ts0:
            source = f"{GREEN}PHC ts[0]{NC}"
            phc_ts0_count += 1
        elif sw_ns > 0:
            source = f"{YELLOW}SOFTWARE{NC}"
            sw_count += 1
        else:
            source = f"{RED}NONE{NC}"

        if sw_ns > 0 and wall_mid > 0:
            wd = wall_mid - sw_ns
            wall_str = f"{wd:+d}"
            wall_deltas.append(wd)
        else:
            wall_str = "n/a"

        print(f"{idx:>3}  {payload_str:<12}  {sw_ns:>22}  {hw_ns:>22}  {wall_str:>12}  {source}")

    # ── summary ──────────────────────────────────────────────────────
    print()
    print(f"{BOLD}Summary:{NC}")
    print(f"  Total packets:    {len(results)}")
    if hw_count > 0:
        print(f"  HW ts[2] stamps:  {hw_count} {GREEN}✓{NC}")
    if phc_ts0_count > 0:
        print(f"  PHC ts[0] stamps: {phc_ts0_count} {GREEN}✓  (ENA driver — PHC timestamps delivered via ts[0]){NC}")
    if sw_count > 0:
        fallback = "(kernel fallback)" if hw_count == 0 and phc_ts0_count == 0 else ""
        print(f"  Software stamps:  {sw_count} {YELLOW}{fallback}{NC}")
    if hw_count == 0 and phc_ts0_count == 0 and sw_count == 0:
        print(f"  {RED}No timestamps received{NC}")

    if hw_sw_deltas:
        avg_d = sum(hw_sw_deltas) / len(hw_sw_deltas)
        print()
        print(f"{BOLD}HW-SW Delta (ts[2] - ts[0]):{NC}")
        print(f"  Min:  {min(hw_sw_deltas):>+12d} ns  ({min(hw_sw_deltas)/1000:>+10.1f} µs)")
        print(f"  Avg:  {avg_d:>+12.0f} ns  ({avg_d/1000:>+10.1f} µs)")
        print(f"  Max:  {max(hw_sw_deltas):>+12d} ns  ({max(hw_sw_deltas)/1000:>+10.1f} µs)")
        print()
        if all(d < 0 for d in hw_sw_deltas):
            print(f"  {CYAN}→ HW timestamps arrive BEFORE software timestamps{NC}")
            print(f"  {DIM}  (expected: NIC stamps at wire time, kernel stamps later){NC}")
        elif all(d > 0 for d in hw_sw_deltas):
            print(f"  {CYAN}→ HW timestamps arrive AFTER software timestamps{NC}")
            print(f"  {DIM}  (possible PTP clock offset or NIC queuing delay){NC}")
        else:
            print(f"  {CYAN}→ Mixed ordering (jitter in HW-SW delta){NC}")

    if wall_deltas:
        avg_wd = sum(wall_deltas) / len(wall_deltas)
        print()
        print(f"{BOLD}Wall Clock vs ts[0] (time.time_ns() - ts[0]):{NC}")
        print(f"  Min:  {min(wall_deltas):>+12d} ns  ({min(wall_deltas)/1000:>+10.1f} µs)")
        print(f"  Avg:  {avg_wd:>+12.0f} ns  ({avg_wd/1000:>+10.1f} µs)")
        print(f"  Max:  {max(wall_deltas):>+12d} ns  ({max(wall_deltas)/1000:>+10.1f} µs)")
        print()
        if abs(avg_wd) < 50_000:
            print(f"  {CYAN}→ ts[0] is within ~{abs(avg_wd)/1000:.0f}µs of wall clock{NC}")
            print(f"  {DIM}  (sub-50µs delta indicates PHC-sourced or high-res kernel timestamp){NC}")
        else:
            print(f"  {CYAN}→ ts[0] lags wall clock by ~{abs(avg_wd)/1000:.0f}µs{NC}")
            print(f"  {DIM}  (larger delta may indicate scheduling delay or clock skew){NC}")

    if hw_count == 0 and phc_ts0_count == 0 and sw_count > 0:
        print()
        print(f"  {YELLOW}⚠  No hardware timestamps received (ts[2]=0, no PHC detected in ts[0]).{NC}")
        print(f"  {DIM}  The sequencer is using plain kernel software timestamps.{NC}")
        if hw_ts_enabled:
            print(f"  {DIM}  The SIOCSHWTSTAMP ioctl succeeded (§3), but the NIC driver did not{NC}")
            print(f"  {DIM}  populate ts[2] and ethtool does not report hardware-receive.{NC}")
        else:
            print(f"  {DIM}  To get HW timestamps, run with sudo.{NC}")

    if phc_ts0_count > 0:
        print()
        print(f"  {GREEN}✓  PHC timestamps confirmed via ts[0] (ENA driver pattern).{NC}")
        print(f"  {DIM}  The ENA driver delivers Nitro PHC timestamps in the software slot (ts[0]){NC}")
        print(f"  {DIM}  rather than the raw hardware slot (ts[2]).{NC}")
        print(f"  {DIM}  Ref: https://github.com/amzn/amzn-drivers/tree/master/kernel/linux/ena{NC}")

    if hw_count > 0:
        print()
        print(f"  {GREEN}✓  Hardware timestamps confirmed in ts[2] (raw NIC PTP clock).{NC}")
        print(f"  {DIM}  Per kernel docs §2.1.2: ts[2] holds the hardware timestamp from the NIC.{NC}")

    print()
    print(f"{DIM}References:{NC}")
    print(f"{DIM}  Kernel: https://www.kernel.org/doc/html/latest/networking/timestamping.html{NC}")
    print(f"{DIM}  AWS:    https://github.com/aws-samples/trading-latency-benchmark{NC}")
    print()


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <iface> <nic_ip> [num_packets]", file=sys.stderr)
        sys.exit(1)

    iface = sys.argv[1]
    nic_ip = sys.argv[2]
    num_packets = int(sys.argv[3]) if len(sys.argv) > 3 else 5

    results, hw_ts_enabled = collect_timestamps(iface, nic_ip, num_packets)
    display_results(results, hw_ts_enabled, iface)


if __name__ == "__main__":
    main()
