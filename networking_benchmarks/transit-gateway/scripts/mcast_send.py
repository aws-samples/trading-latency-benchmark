#!/usr/bin/env python3
"""Multicast sender — binary payload matching the phc_probe pattern.

Payload format (matches scripts/mcast_recv.py and phc_probe ts_sender.py):
  Offset  Size  Field         Encoding
       0     4  seq_num       uint32_t, network byte order
       4     8  tx_ns         uint64_t, network byte order, ns since epoch
                              (CLOCK_REALTIME)

End-of-stream sentinel:
  seq == 0xFFFFFFFF, tx_ns repurposed to carry total_sent count so the
  receiver can report exact packet loss including tail losses.

Usage:
  python3 mcast_send.py <group> <port> <rate_pps> <duration_secs> <src_ip>
"""
from __future__ import annotations

import os
import socket
import struct
import sys
import time

END_SEQ = 0xFFFFFFFF
PACKET_FMT = "!IQ"  # 4-byte seq + 8-byte ns timestamp (network byte order)


def main() -> int:
    if len(sys.argv) != 6:
        print("usage: mcast_send.py <group> <port> <rate_pps> <duration_secs> <src_ip>",
              file=sys.stderr)
        return 2

    group = sys.argv[1]
    port = int(sys.argv[2])
    rate = int(sys.argv[3])
    duration = int(sys.argv[4])
    src_ip = sys.argv[5]

    # Pin to CPU 0 to avoid scheduler jitter
    try:
        os.sched_setaffinity(0, {0})
    except (AttributeError, OSError):
        pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(src_ip))
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)

    _clock_ns = time.clock_gettime_ns
    _CLOCK = time.CLOCK_REALTIME
    _sendto = sock.sendto
    _pack = struct.pack
    dest = (group, port)

    interval_ns = int(1_000_000_000 / rate) if rate > 0 else 0
    seq = 0
    end_ns = _clock_ns(_CLOCK) + duration * 1_000_000_000

    # Hot loop: pack timestamp + sequence and send.
    while _clock_ns(_CLOCK) < end_ns:
        if seq >= END_SEQ:
            # Stop sending data packets once we reach the END sentinel value
            # (extremely unlikely at any reasonable rate * duration, guard anyway).
            break
        tx_ns = _clock_ns(_CLOCK)
        _sendto(_pack(PACKET_FMT, seq, tx_ns), dest)
        seq += 1
        # Busy-wait for precise interval; sleep is too coarse at high rates.
        target = tx_ns + interval_ns
        while _clock_ns(_CLOCK) < target:
            pass

    # Send END sentinel (3x for resilience). tx_ns slot carries total_sent count
    # so the receiver can compute exact loss including tail losses.
    end_payload = _pack(PACKET_FMT, END_SEQ, seq)
    for _ in range(3):
        _sendto(end_payload, dest)
        time.sleep(0.05)

    sock.close()
    print(f"Sent {seq} packets in {duration}s at {rate} pps (target)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
