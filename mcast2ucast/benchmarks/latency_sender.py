#!/usr/bin/env python3
"""
Latency test sender — sends multicast UDP packets with embedded nanosecond
timestamps through the mcast2ucast pipeline.

Each packet contains:
  - 8 bytes: sequence number (uint64, big-endian)
  - 8 bytes: send timestamp in nanoseconds since epoch (uint64, big-endian)
  - N bytes: padding to reach desired packet size

Requires PTP-grade clock sync (run end_to_end_ptp.sh first).

Usage:
  sudo python3 latency_sender.py [interface] [group] [port] [count] [interval_us] [size]
"""
import socket
import struct
import sys
import time

IFACE    = "mcast0"
GROUP    = "224.0.0.101"
PORT     = 5001
COUNT    = 1000
INTERVAL = 1000      # microseconds between packets
PKT_SIZE = 64        # payload size (min 16 for header)

def main():
    iface      = sys.argv[1] if len(sys.argv) > 1 else IFACE
    group      = sys.argv[2] if len(sys.argv) > 2 else GROUP
    port       = int(sys.argv[3]) if len(sys.argv) > 3 else PORT
    count      = int(sys.argv[4]) if len(sys.argv) > 4 else COUNT
    interval   = int(sys.argv[5]) if len(sys.argv) > 5 else INTERVAL
    pkt_size   = int(sys.argv[6]) if len(sys.argv) > 6 else PKT_SIZE

    pkt_size = max(pkt_size, 16)  # minimum: seq(8) + ts(8)
    padding = b'\x00' * (pkt_size - 16)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
    # SO_BINDTODEVICE = 25
    sock.setsockopt(socket.SOL_SOCKET, 25, (iface + '\0').encode())

    interval_s = interval / 1_000_000.0

    print(f"Sending {count} packets to {group}:{port} via {iface}")
    print(f"  payload={pkt_size}B  interval={interval}us")
    print()

    for seq in range(count):
        ts_ns = time.clock_gettime_ns(time.CLOCK_REALTIME)
        payload = struct.pack('!QQ', seq, ts_ns) + padding
        sock.sendto(payload, (group, port))

        if seq % 100 == 0:
            print(f"  sent seq={seq}/{count}", end='\r')

        # Busy-wait for sub-millisecond intervals
        if interval < 1000:
            target = time.monotonic_ns() + interval * 1000
            while time.monotonic_ns() < target:
                pass
        else:
            time.sleep(interval_s)

    print(f"\nDone. Sent {count} packets.")

if __name__ == "__main__":
    main()
