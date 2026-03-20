#!/usr/bin/env python3
"""
mcast2ucast demo server — lightweight HTTP + multicast bridge.
Zero dependencies (stdlib only). Runs on each EC2 instance.

Usage:
  Publisher:    python3 server.py --mode publisher --mcast-iface <mcast0-ip>
  Subscriber:   python3 server.py --mode subscriber --mcast-iface <mcast0-ip>

Opens port 8888 for the dashboard to connect to.
"""

import argparse
import json
import socket
import struct
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from collections import deque

# Shared state
messages = deque(maxlen=200)
sse_clients = []
sse_lock = threading.Lock()
stats = {"sent": 0, "received": 0, "mode": "unknown", "started": time.time()}

MCAST_GROUP = "224.0.0.101"
MCAST_PORT = 5001


def broadcast_sse(event_type, data):
    """Send an SSE event to all connected dashboard clients."""
    payload = f"event: {event_type}\ndata: {json.dumps(data)}\n\n".encode()
    with sse_lock:
        dead = []
        for wfile in sse_clients:
            try:
                wfile.write(payload)
                wfile.flush()
            except Exception:
                dead.append(wfile)
        for w in dead:
            sse_clients.remove(w)


def multicast_listener(mcast_iface_ip):
    """Listen for multicast on mcast0 and record received messages with latency."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, 25, b"mcast0\0")  # SO_BINDTODEVICE
    except Exception:
        pass

    # Enable kernel-level nanosecond receive timestamps (SO_TIMESTAMPNS = 35)
    try:
        sock.setsockopt(socket.SOL_SOCKET, 35, 1)
        print("[subscriber] SO_TIMESTAMPNS enabled — using kernel receive timestamps")
    except Exception:
        print("[subscriber] SO_TIMESTAMPNS not available — using userspace timestamps")

    sock.bind(("", MCAST_PORT))
    mreq = struct.pack("4s4s",
                        socket.inet_aton(MCAST_GROUP),
                        socket.inet_aton(mcast_iface_ip))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    print(f"[subscriber] Listening on {MCAST_GROUP}:{MCAST_PORT} via mcast0")

    while True:
        try:
            # Use recvmsg to get kernel timestamp from ancillary data
            data, ancdata, flags, addr = sock.recvmsg(4096, 1024)
            recv_ts_ns = time.clock_gettime_ns(time.CLOCK_REALTIME)  # fallback

            # Parse kernel timestamp from ancillary data (SCM_TIMESTAMPNS)
            # Level=SOL_SOCKET(1), Type=SCM_TIMESTAMPNS(35), Data=timespec(sec,nsec)
            for cmsg_level, cmsg_type, cmsg_data in ancdata:
                if cmsg_level == socket.SOL_SOCKET and cmsg_type == 35:
                    sec, nsec = struct.unpack("ll", cmsg_data[:struct.calcsize("ll")])
                    recv_ts_ns = sec * 1_000_000_000 + nsec
                    break

            raw = data.decode("utf-8", errors="replace")

            # Parse embedded send timestamp: "TS:<nanoseconds>|<text>"
            latency_us = None
            if raw.startswith("TS:"):
                pipe_idx = raw.index("|")
                send_ts_ns = int(raw[3:pipe_idx])
                text = raw[pipe_idx + 1:]
                latency_us = round((recv_ts_ns - send_ts_ns) / 1000, 1)
            else:
                text = raw

            stats["received"] += 1
            msg = {
                "id": stats["received"],
                "text": text,
                "from": addr[0],
                "time": time.time(),
                "time_str": time.strftime("%H:%M:%S"),
            }
            if latency_us is not None:
                msg["latency_us"] = latency_us
            messages.append(msg)
            broadcast_sse("message", msg)
            lat_str = f" [{latency_us}µs]" if latency_us is not None else ""
            print(f"[recv] #{stats['received']} from {addr[0]}: {text}{lat_str}")
        except Exception as e:
            print(f"[subscriber] Error: {e}")


def send_multicast(text, mcast_iface_ip):
    """Send a multicast message via mcast0 with embedded nanosecond timestamp."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                    socket.inet_aton(mcast_iface_ip))
    # Embed send timestamp (nanoseconds) right before sendto for accuracy
    send_ts = time.clock_gettime_ns(time.CLOCK_REALTIME)
    payload = f"TS:{send_ts}|{text}".encode("utf-8")
    sock.sendto(payload, (MCAST_GROUP, MCAST_PORT))
    sock.close()
    stats["sent"] += 1
    msg = {
        "id": stats["sent"],
        "text": text,
        "time": time.time(),
        "time_str": time.strftime("%H:%M:%S"),
    }
    messages.append(msg)
    broadcast_sse("message", msg)
    print(f"[sent] #{stats['sent']}: {text}")
    return msg


class DemoHandler(BaseHTTPRequestHandler):
    mcast_iface_ip = "0.0.0.0"

    def log_message(self, format, *args):
        pass  # silence request logs

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        if self.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors()
            self.end_headers()
            body = json.dumps({
                "mode": stats["mode"],
                "sent": stats["sent"],
                "received": stats["received"],
                "uptime": int(time.time() - stats["started"]),
                "message_count": len(messages),
            })
            self.wfile.write(body.encode())

        elif self.path == "/api/messages":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self._cors()
            self.end_headers()

            # Send existing messages as initial batch
            for msg in messages:
                self.wfile.write(f"event: message\ndata: {json.dumps(msg)}\n\n".encode())
            self.wfile.write(f"event: sync\ndata: {json.dumps({'count': len(messages)})}\n\n".encode())
            self.wfile.flush()

            # Register for future events
            with sse_lock:
                sse_clients.append(self.wfile)

            # Keep connection alive
            try:
                while True:
                    time.sleep(5)
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except Exception:
                with sse_lock:
                    if self.wfile in sse_clients:
                        sse_clients.remove(self.wfile)

        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path == "/api/send":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            try:
                data = json.loads(body)
                text = data.get("message", "")
            except Exception:
                text = body.decode("utf-8", errors="replace")

            if not text:
                self.send_response(400)
                self._cors()
                self.end_headers()
                self.wfile.write(b'{"error":"empty message"}')
                return

            msg = send_multicast(text, self.mcast_iface_ip)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors()
            self.end_headers()
            self.wfile.write(json.dumps(msg).encode())
        else:
            self.send_response(404)
            self.end_headers()


def main():
    parser = argparse.ArgumentParser(description="mcast2ucast demo server")
    parser.add_argument("--mode", choices=["publisher", "subscriber"], required=True)
    parser.add_argument("--mcast-iface", required=True,
                        help="IP address of mcast0 on this instance")
    parser.add_argument("--port", type=int, default=8888,
                        help="HTTP port (default: 8888)")
    args = parser.parse_args()

    stats["mode"] = args.mode
    DemoHandler.mcast_iface_ip = args.mcast_iface

    if args.mode == "subscriber":
        t = threading.Thread(target=multicast_listener, args=(args.mcast_iface,),
                             daemon=True)
        t.start()

    class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True

    server = ThreadedHTTPServer(("0.0.0.0", args.port), DemoHandler)
    print(f"[{args.mode}] Demo server on http://0.0.0.0:{args.port}")
    print(f"[{args.mode}] mcast0 IP: {args.mcast_iface}")
    print(f"[{args.mode}] Multicast group: {MCAST_GROUP}:{MCAST_PORT}")
    server.serve_forever()


if __name__ == "__main__":
    main()
