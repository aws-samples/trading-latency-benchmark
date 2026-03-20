#!/usr/bin/env python3
"""
Latency test receiver — receives unicast UDP packets from the mcast2ucast
pipeline, extracts embedded timestamps, and calculates one-way latency
statistics with percentiles (inspired by dpdk_latency).

Expected packet format (from latency_sender.py):
  - 8 bytes: sequence number (uint64, big-endian)
  - 8 bytes: send timestamp in nanoseconds since epoch (uint64, big-endian)

Requires PTP-grade clock sync (run end_to_end_ptp.sh first).

Usage:
  python3 latency_receiver.py [port] [count] [timeout_s]
"""
import socket
import struct
import sys
import time

PORT    = 5001
COUNT   = 1000
TIMEOUT = 30  # seconds to wait for packets

def percentile(data, p):
    """Return the p-th percentile value from sorted data."""
    if not data:
        return 0
    idx = int(len(data) * p / 100)
    idx = min(idx, len(data) - 1)
    return data[idx]

def main():
    port    = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    count   = int(sys.argv[2]) if len(sys.argv) > 2 else COUNT
    timeout = int(sys.argv[3]) if len(sys.argv) > 3 else TIMEOUT

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Enable kernel RX timestamps
    SO_TIMESTAMPNS = 35
    sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
    sock.bind(('', port))
    sock.settimeout(timeout)

    latencies_ns = []
    received = 0
    lost_seq = set()
    out_of_order = 0
    last_seq = -1
    min_lat = float('inf')
    max_lat = 0

    print(f"Listening on UDP port {port}, expecting {count} packets (timeout={timeout}s)")
    print()

    start_time = time.monotonic()

    try:
        while received < count:
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout:
                print(f"Timeout after {timeout}s")
                break

            rx_ns = time.clock_gettime_ns(time.CLOCK_REALTIME)

            if len(data) < 16:
                continue

            seq, tx_ns = struct.unpack('!QQ', data[:16])

            latency_ns = rx_ns - tx_ns

            # Sanity check: negative latency means clock skew > latency
            if latency_ns < 0:
                latency_ns = 0

            latencies_ns.append(latency_ns)
            received += 1

            if latency_ns < min_lat:
                min_lat = latency_ns
            if latency_ns > max_lat:
                max_lat = latency_ns

            # Track ordering
            if seq != last_seq + 1 and last_seq >= 0:
                if seq < last_seq:
                    out_of_order += 1
                else:
                    for s in range(last_seq + 1, seq):
                        lost_seq.add(s)
            last_seq = max(last_seq, seq)

            if received % 100 == 0:
                avg = sum(latencies_ns[-100:]) / min(100, len(latencies_ns[-100:]))
                print(f"  [{received}/{count}] last={latency_ns/1000:.1f}us avg(100)={avg/1000:.1f}us", end='\r')

    except KeyboardInterrupt:
        print("\nInterrupted.")

    elapsed = time.monotonic() - start_time
    sock.close()

    if received == 0:
        print("No packets received.")
        return

    # Sort for percentile calculation
    latencies_ns.sort()
    avg_ns = sum(latencies_ns) / len(latencies_ns)

    print(f"\n")
    print(f"{'='*50}")
    print(f"  mcast2ucast Latency Report")
    print(f"{'='*50}")
    print(f"  Received:      {received}/{count} packets ({elapsed:.1f}s)")
    print(f"  Lost:          {len(lost_seq)} packets")
    print(f"  Out of order:  {out_of_order}")
    print()
    print(f"  Latency (usec):")
    print(f"    Min:    {min_lat/1000:>10.1f}")
    print(f"    Avg:    {avg_ns/1000:>10.1f}")
    print(f"    Max:    {max_lat/1000:>10.1f}")
    print()
    print(f"  Percentiles (usec):")
    for p in [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99]:
        val = percentile(latencies_ns, p)
        print(f"    P{p:<3d}  {val/1000:>10.1f}")
    print(f"{'='*50}")

    # Optional: dump raw data
    if '--raw' in sys.argv:
        print("\nRaw latencies (ns):")
        for i, lat in enumerate(latencies_ns):
            print(f"  {i}: {lat}")

if __name__ == "__main__":
    main()
