#!/usr/bin/env python3
"""Send multicast UDP test packets to a specified group via a given interface."""
import socket
import struct
import sys
import time

GROUP = "224.0.0.101"
PORT = 5001
IFACE = "mcast0"
INTERVAL = 1.0  # seconds between packets

def main():
    iface = sys.argv[1] if len(sys.argv) > 1 else IFACE
    group = sys.argv[2] if len(sys.argv) > 2 else GROUP
    port = int(sys.argv[3]) if len(sys.argv) > 3 else PORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)

    # Bind to the specified interface by name
    sock.setsockopt(socket.SOL_SOCKET, 25, (iface + '\0').encode())  # SO_BINDTODEVICE

    seq = 0
    print(f"Sending multicast to {group}:{port} via {iface}")
    while True:
        msg = f"mcast2ucast test packet seq={seq} time={time.time():.6f}".encode()
        sock.sendto(msg, (group, port))
        print(f"  sent seq={seq} ({len(msg)} bytes)")
        seq += 1
        time.sleep(INTERVAL)

if __name__ == "__main__":
    main()
