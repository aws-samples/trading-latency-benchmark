#!/usr/bin/env python3
"""Timestamp Sender — sends UDP packets to a remote receiver for HW timestamp testing.

Run this on ANY host (no special drivers or privileges needed).
The receiver (ts_receiver.py) runs on the EC2 instance with HW timestamping.

Usage:
    python3 scripts/ts_sender.py <receiver-ip> [--port PORT] [--count N] [--interval MS]

Example:
    python3 scripts/ts_sender.py 10.0.1.100 --port 9999 --count 20 --interval 100
"""

import argparse
import socket
import struct
import time


def main():
    parser = argparse.ArgumentParser(description="Timestamp Sender — send UDP packets for HW timestamp testing")
    parser.add_argument("receiver_ip", help="IP address of the receiver host")
    parser.add_argument("--port", type=int, default=9999, help="UDP port (default: 9999)")
    parser.add_argument("--count", type=int, default=10, help="Number of packets (default: 10)")
    parser.add_argument("--interval", type=int, default=100, help="Interval between packets in ms (default: 100)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.receiver_ip, args.port)
    interval_s = args.interval / 1000.0

    print(f"Sending {args.count} packets to {dest[0]}:{dest[1]} (interval: {args.interval}ms)")

    for i in range(args.count):
        # Payload: 4-byte seq + 8-byte timestamp (network byte order)
        tx_ns = int(time.clock_gettime(time.CLOCK_REALTIME) * 1e9)
        pkt = struct.pack("!IQ", i, tx_ns)
        sock.sendto(pkt, dest)
        print(f"  [{i:>4}] sent {len(pkt)} bytes, tx_ns={tx_ns}")
        if i < args.count - 1:
            time.sleep(interval_s)

    sock.close()
    print(f"Done — sent {args.count} packets")


if __name__ == "__main__":
    main()
