#!/usr/bin/env python3
"""Timestamp Receiver — listens for UDP packets and displays HW vs SW timestamps.

Run this on the EC2 instance with ENA PHC / hardware timestamping enabled.
Packets must arrive from a REMOTE host (not loopback) to get HW timestamps.

Usage:
    sudo python3 scripts/ts_receiver.py <interface> [--port PORT] [--count N]
    sudo python3 scripts/ts_receiver.py <interface> [--port PORT] --live

Example:
    sudo python3 scripts/ts_receiver.py ens5 --port 9999 --count 20
    sudo python3 scripts/ts_receiver.py ens5 --port 9999 --live
"""

import ctypes
import collections
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import argparse

# ── kernel constants ─────────────────────────────────────────────────
SOF_TIMESTAMPING_RX_HARDWARE  = 1 << 2
SOF_TIMESTAMPING_RX_SOFTWARE  = 1 << 3
SOF_TIMESTAMPING_SOFTWARE     = 1 << 4
SOF_TIMESTAMPING_RAW_HARDWARE = 1 << 6
SO_TIMESTAMPING_NEW = 65
SO_TIMESTAMPING     = 37
SCM_TIMESTAMPING_NEW = 65
SCM_TIMESTAMPING     = 37
SIOCSHWTSTAMP = 0x89B0

# ── colours ──────────────────────────────────────────────────────────
GREEN  = "\033[0;32m"
RED    = "\033[0;31m"
YELLOW = "\033[1;33m"
CYAN   = "\033[0;36m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
NC     = "\033[0m"


class HwtstampConfig(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_int), ("tx_type", ctypes.c_int),
                ("rx_filter", ctypes.c_int)]

class Ifreq(ctypes.Structure):
    _fields_ = [("ifr_name", ctypes.c_char * 16),
                ("ifr_data", ctypes.c_void_p)]


def enable_hw_timestamping(sock_fd: int, iface: str) -> bool:
    config = HwtstampConfig(flags=0, tx_type=0, rx_filter=1)
    ifreq = Ifreq()
    ifreq.ifr_name = iface.encode()[:15].ljust(16, b"\x00")
    ifreq.ifr_data = ctypes.addressof(config)
    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        ret = libc.ioctl(ctypes.c_int(sock_fd), ctypes.c_ulong(SIOCSHWTSTAMP),
                         ctypes.byref(ifreq))
        if ret < 0:
            raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))
        return True
    except OSError as e:
        print(f"  SIOCSHWTSTAMP: {RED}FAILED{NC} ({e})")
        return False


def detect_ena_phc(iface: str) -> bool:
    try:
        et = subprocess.run(["ethtool", "-T", iface],
                            capture_output=True, text=True, timeout=3)
        return "hardware-receive" in et.stdout
    except Exception:
        return False


def parse_scm_timestamps(ancdata):
    """Extract sw (ts[0]) and hw (ts[2]) nanosecond timestamps from ancillary data."""
    sw_ns = 0
    hw_ns = 0
    for level, typ, cdata in ancdata:
        if level == socket.SOL_SOCKET and typ in (SCM_TIMESTAMPING_NEW, SCM_TIMESTAMPING):
            if len(cdata) >= 48:
                # scm_timestamping64: 3 x (tv_sec, tv_nsec) = 6 x int64
                ts = struct.unpack("qqqqqq", cdata[:48])
                sw_ns = ts[0] * 1_000_000_000 + ts[1] if ts[0] >= 0 else 0
                hw_ns = ts[4] * 1_000_000_000 + ts[5] if ts[4] >= 0 else 0
            elif len(cdata) >= 24:
                # scm_timestamping (legacy): 3 x struct timespec (could be 32 or 64 bit)
                # Try 64-bit first (most common on 64-bit kernels)
                try:
                    ts = struct.unpack("llllll", cdata[:48])
                    sw_ns = ts[0] * 1_000_000_000 + ts[1] if ts[0] >= 0 else 0
                    hw_ns = ts[4] * 1_000_000_000 + ts[5] if ts[4] >= 0 else 0
                except struct.error:
                    pass
    return sw_ns, hw_ns


def create_socket(iface, port):
    """Create and configure a timestamping UDP socket."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))

    flags = (SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE
             | SOF_TIMESTAMPING_SOFTWARE  | SOF_TIMESTAMPING_RAW_HARDWARE)
    try:
        sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING_NEW, struct.pack("I", flags))
        print(f"  SO_TIMESTAMPING_NEW: {GREEN}OK{NC}")
    except OSError:
        sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING, struct.pack("I", flags))
        print(f"  SO_TIMESTAMPING (legacy): {GREEN}OK{NC}")

    hw_ok = enable_hw_timestamping(sock.fileno(), iface)
    if hw_ok:
        print(f"  SIOCSHWTSTAMP:       {GREEN}OK{NC}")

    return sock


def run_live_graph(sock, iface):
    """Run infinite live ASCII graph of HW-SW delta (ts[2] - ts[0]) over a 60s rolling window."""
    WINDOW_SECS = 60
    GRAPH_HEIGHT = 20
    # Store (wall_time, delta_us) tuples
    samples = collections.deque()

    sock.settimeout(1.0)
    start_time = time.monotonic()
    pkt_count = 0
    no_hw_warned = False

    print()
    print(f"  {BOLD}Live HW-SW delta graph{NC} (last {WINDOW_SECS}s, Ctrl+C to stop)")
    print(f"  Waiting for packets from a REMOTE sender...")
    print()

    # Hide cursor and clear screen once at start
    sys.stdout.write("\033[?25l\033[2J\033[H")
    sys.stdout.flush()

    try:
        while True:
            try:
                data, ancdata, msg_flags, addr = sock.recvmsg(2048, 1024)
            except socket.timeout:
                # Still redraw graph on timeout so the window scrolls
                _draw_graph(samples, WINDOW_SECS, GRAPH_HEIGHT, pkt_count, start_time)
                continue

            sw_ns, hw_ns = parse_scm_timestamps(ancdata)
            pkt_count += 1
            now = time.monotonic()

            if hw_ns > 0 and sw_ns > 0:
                delta_us = (hw_ns - sw_ns) / 1000.0
                samples.append((now, delta_us))
            elif not no_hw_warned and sw_ns > 0:
                no_hw_warned = True

            # Prune samples older than window
            cutoff = now - WINDOW_SECS
            while samples and samples[0][0] < cutoff:
                samples.popleft()

            _draw_graph(samples, WINDOW_SECS, GRAPH_HEIGHT, pkt_count, start_time)

    except KeyboardInterrupt:
        # Show cursor again, move below graph
        sys.stdout.write("\033[?25h\n")
        sys.stdout.flush()
        print()
        if samples:
            deltas = [d for _, d in samples]
            avg = sum(deltas) / len(deltas)
            print(f"{BOLD}Final stats ({len(deltas)} samples in last {WINDOW_SECS}s):{NC}")
            print(f"  Closest:  {max(deltas):>+10.1f} µs")
            print(f"  Avg:      {avg:>+10.1f} µs")
            print(f"  Furthest: {min(deltas):>+10.1f} µs")
        print(f"  Total packets: {pkt_count}")
        print()


def _draw_graph(samples, window_secs, height, pkt_count, start_time):
    """Redraw the ASCII graph in-place (cursor home, overwrite lines)."""
    term_cols = shutil.get_terminal_size((80, 24)).columns
    term_rows = shutil.get_terminal_size((80, 24)).lines
    graph_width = min(term_cols - 14, 120)

    # Build all output lines first, then write in one shot
    lines = []

    if not samples:
        elapsed = time.monotonic() - start_time
        lines.append(f"{BOLD}HW-SW Delta (ts[2] - ts[0]) — Live{NC}  [{elapsed:.0f}s elapsed, {pkt_count} pkts]")
        lines.append("")
        if pkt_count > 0:
            lines.append(f"  {YELLOW}Packets received but no HW timestamps (ts[2]=0){NC}")
            lines.append(f"  {DIM}Packets must come from a REMOTE host for HW timestamps{NC}")
        else:
            lines.append(f"  {DIM}Waiting for packets...{NC}")
    else:
        now = time.monotonic()
        elapsed = now - start_time
        deltas = [d for _, d in samples]
        y_min = min(deltas)
        y_max = max(deltas)
        y_avg = sum(deltas) / len(deltas)

        y_range = y_max - y_min
        if y_range < 1.0:
            y_range = 1.0
            y_min = y_avg - 0.5
            y_max = y_avg + 0.5
        pad = y_range * 0.1
        y_min -= pad
        y_max += pad
        y_range = y_max - y_min

        grid = [[' '] * graph_width for _ in range(height)]
        for t, d in samples:
            age = now - t
            col = int((1.0 - age / window_secs) * (graph_width - 1))
            col = max(0, min(graph_width - 1, col))
            row = int((1.0 - (d - y_min) / y_range) * (height - 1))
            row = max(0, min(height - 1, row))
            grid[row][col] = '█'

        lines.append(f"{BOLD}HW-SW Delta (ts[2] - ts[0]) — Live{NC}  [{elapsed:.0f}s elapsed, {pkt_count} pkts, {len(samples)} in window]")
        lines.append(f"  closest={y_max - pad:.1f}µs  avg={y_avg:.1f}µs  furthest={y_min + pad:.1f}µs")
        lines.append("")

        for row in range(height):
            y_val = y_max - (row / (height - 1)) * y_range
            label = f"{y_val:>+8.1f}µs"
            line = ''.join(grid[row])
            if row == 0:
                lines.append(f"  {label} ┤{line}┐")
            elif row == height - 1:
                lines.append(f"  {label} ┤{line}┘")
            else:
                lines.append(f"  {label} │{line}│")

        x_pad = ' ' * 11
        lines.append(f"{x_pad} └{'─' * graph_width}┘")
        left_label = f"-{window_secs}s"
        right_label = "now"
        gap = graph_width - len(left_label) - len(right_label) + 1
        lines.append(f"{x_pad}  {left_label}{' ' * max(gap, 1)}{right_label}")

    # Pad each line to terminal width to overwrite stale content
    buf = "\033[H"  # cursor home (no clear)
    for line in lines:
        buf += line.ljust(term_cols) + "\n"
    # Clear any remaining lines from previous draw
    remaining = term_rows - len(lines) - 1
    for _ in range(max(remaining, 0)):
        buf += " " * term_cols + "\n"

    sys.stdout.write(buf)
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description="Timestamp Receiver — HW vs SW timestamp comparison")
    parser.add_argument("interface", help="Network interface (e.g. ens5, enp40s0)")
    parser.add_argument("--port", type=int, default=9999, help="UDP port to listen on (default: 9999)")
    parser.add_argument("--count", type=int, default=10, help="Number of packets to capture (default: 10)")
    parser.add_argument("--live", action="store_true",
                        help="Live rolling graph of HW-SW delta (last 60s, runs until Ctrl+C)")
    args = parser.parse_args()

    if os.geteuid() != 0:
        print(f"{RED}ERROR{NC}: Must run as root (sudo)")
        sys.exit(1)

    iface = args.interface
    port = args.port

    nic_ip = None
    try:
        import subprocess as sp
        out = sp.run(["ip", "-4", "addr", "show", iface], capture_output=True, text=True)
        for line in out.stdout.splitlines():
            if "inet " in line:
                nic_ip = line.strip().split()[1].split("/")[0]
                break
    except Exception:
        pass

    print()
    print(f"{BOLD}Timestamp Receiver{NC}")
    print(f"  Interface:  {CYAN}{iface}{NC} ({nic_ip or 'unknown'})")
    print(f"  Port:       {CYAN}{port}{NC}")
    if args.live:
        print(f"  Mode:       {CYAN}Live graph{NC} (Ctrl+C to stop)")
    else:
        print(f"  Packets:    {CYAN}{args.count}{NC}")
    print()

    sock = create_socket(iface, port)
    ena_phc = detect_ena_phc(iface)

    if args.live:
        run_live_graph(sock, iface)
        sock.close()
        return

    # ── Fixed-count mode (original behavior) ─────────────────────────
    num_packets = args.count
    print()
    print(f"  Waiting for {num_packets} packets from a REMOTE sender...")
    print(f"  {DIM}(Sender: python3 scripts/ts_sender.py <this-ip> --port {port} --count {num_packets}){NC}")
    print()

    sock.settimeout(30.0)

    print(f"{BOLD}{'#':>3}  {'From':<22}  {'ts[0] sw (ns)':>22}  {'ts[2] hw (ns)':>22}  {'hw-sw (us)':>12}  Source{NC}")
    print(f"{'─'*3}  {'─'*22}  {'─'*22}  {'─'*22}  {'─'*12}  {'─'*10}")

    results = []
    for i in range(num_packets):
        try:
            data, ancdata, msg_flags, addr = sock.recvmsg(2048, 1024)
        except socket.timeout:
            print(f"{i:>3}  {'TIMEOUT':<22}  {'—':>22}  {'—':>22}  {'—':>12}  {RED}TIMEOUT{NC}")
            continue

        sw_ns, hw_ns = parse_scm_timestamps(ancdata)
        src = f"{addr[0]}:{addr[1]}"

        if hw_ns > 0:
            source = f"{GREEN}HW ts[2]{NC}"
            delta_us = (hw_ns - sw_ns) / 1000.0
            delta_str = f"{delta_us:+.1f}"
        elif sw_ns > 0 and ena_phc:
            source = f"{GREEN}PHC ts[0]{NC}"
            delta_str = "n/a"
        elif sw_ns > 0:
            source = f"{YELLOW}SW only{NC}"
            delta_str = "n/a"
        else:
            source = f"{RED}NONE{NC}"
            delta_str = "n/a"

        print(f"{i:>3}  {src:<22}  {sw_ns:>22}  {hw_ns:>22}  {delta_str:>12}  {source}")
        results.append((sw_ns, hw_ns))

    sock.close()

    # Summary
    hw_count = sum(1 for s, h in results if h > 0)
    sw_count = sum(1 for s, h in results if s > 0 and h == 0)
    hw_sw_deltas = [(h - s) for s, h in results if h > 0 and s > 0]

    print()
    print(f"{BOLD}Summary:{NC}")
    print(f"  Received:         {len(results)}")
    if hw_count > 0:
        print(f"  HW timestamps:    {hw_count} {GREEN}✓{NC}")
    if sw_count > 0:
        print(f"  SW-only:          {sw_count} {YELLOW}(no HW){NC}")
    if hw_count == 0 and sw_count > 0:
        print(f"  {YELLOW}⚠  No hardware timestamps received.{NC}")
        print(f"  {DIM}  Packets must arrive from a REMOTE host (not loopback) for HW timestamps.{NC}")
        print(f"  {DIM}  Verify: sudo ethtool -T {iface} | grep hardware-receive{NC}")

    if hw_sw_deltas:
        avg = sum(hw_sw_deltas) / len(hw_sw_deltas)
        print()
        print(f"{BOLD}HW-SW Delta (ts[2] - ts[0]):{NC}")
        print(f"  Closest:  {max(hw_sw_deltas)/1000:>+10.1f} µs")
        print(f"  Avg:      {avg/1000:>+10.1f} µs")
        print(f"  Furthest: {min(hw_sw_deltas)/1000:>+10.1f} µs")
    print()


if __name__ == "__main__":
    main()
