# mcast2ucast Live Demo

A browser-based dashboard that demonstrates multicast pub/sub across EC2 instances via mcast2ucast. One publisher sends multicast messages, two subscribers receive them in real-time — the entire DPDK kernel-bypass pipeline is exercised transparently.

## Architecture

```
Your Laptop (browser)
  │
  ├── localhost:8881 ──SSH tunnel──► Instance 1 (publisher :8888)
  ├── localhost:8882 ──SSH tunnel──► Instance 2 (subscriber :8888)
  └── localhost:8883 ──SSH tunnel──► Instance 3 (subscriber :8888)
```

Message flow when you click Send:

```
Browser ─POST─► server.py ─UDP mcast─► mcast0 TAP ─DPDK─► ENA (igb_uio)
                                                            │
                                                      unicast over wire
                                                            │
         Browser ◄─SSE─ server.py ◄─UDP mcast─ mcast0 ◄─DPDK─ ENA
```

## Prerequisites

- mcast2ucast running on all instances (see [main README](../README.md))
- TAP interfaces (`mcast0`) configured with IPs and multicast routes
- SSH access to all instances

## Quick Start

### 1. Start demo servers on each instance

**Instance 1 — Publisher:**
```bash
python3 ~/mcast2ucast/demo/server.py --mode publisher --mcast-iface <mcast0-ip>
```

**Instance 2 — Subscriber:**
```bash
python3 ~/mcast2ucast/demo/server.py --mode subscriber --mcast-iface <mcast0-ip>
```

**Instance 3 — Subscriber:**
```bash
python3 ~/mcast2ucast/demo/server.py --mode subscriber --mcast-iface <mcast0-ip>
```

The `--mcast-iface` IP must match the IP assigned to `mcast0` on that instance (`ip addr show mcast0`).

### 2. Open SSH tunnels from your laptop

```bash
ssh -i ~/.ssh/<key>.pem -L 8881:localhost:8888 ec2-user@<publisher-ip> -N &
ssh -i ~/.ssh/<key>.pem -L 8882:localhost:8888 ec2-user@<subscriber1-ip> -N &
ssh -i ~/.ssh/<key>.pem -L 8883:localhost:8888 ec2-user@<subscriber2-ip> -N &
```

Add `-o ServerAliveInterval=15` to keep tunnels from dropping.

### 3. Open the dashboard

Open `dashboard.html` in your browser:

```
dashboard.html?pub=localhost:8881&sub1=localhost:8882&sub2=localhost:8883
```

Or fill in the connection fields manually and click **Connect**.

## Dashboard Features

- **Live latency stats**: Real-time per-message latency (one-way, using `SO_TIMESTAMPNS` kernel timestamps), avg, min/max
- **Interactive terminal walkthrough**: Step through `ip link show`, Python listener/sender code, then send live messages
- **Send buttons**: BTC Quote, ETH Quote, Order, Fill, Trade, Burst x10 — each generates Python `sendto()` code in the publisher terminal and shows received output with latency in subscriber terminals
- **Real message delivery**: Every button click sends a real multicast message through the DPDK pipeline
- **Connection indicators**: Green = connected, orange = connecting, red = disconnected
- **URL persistence**: Connection settings are saved in the URL for easy bookmarking

## Files

| File | Description |
|------|-------------|
| `server.py` | HTTP + multicast bridge with latency measurement, runs on each instance (zero dependencies) |
| `dashboard.html` | Single-file browser dashboard with interactive terminal demo (no build step, no dependencies) |
| `tunnels.sh` | Helper script to manage SSH tunnels (edit variables for your deployment) |

## Troubleshooting

- **Messages sent but subscribers don't receive**: Check mcast2ucast is running (`pgrep mcast2ucast`) and TAP is configured (`ip addr show mcast0`).
- **Dashboard won't connect**: Verify tunnels are up: `curl -s http://localhost:8881/api/status` should return JSON.
- **Subscriber shows "No such device"**: The `--mcast-iface` IP doesn't match mcast0. Use `ip addr show mcast0` to find the correct IP.
- **Tunnel keeps dropping**: Add `-o ServerAliveInterval=15 -o ServerAliveCountMax=20` to the SSH command. Consider using a dedicated SSH ENI.
