# AF_XDP Zero-Copy Performance Benchmark

High-performance, low-latency UDP packet replicator using AF_XDP kernel bypass technology for market data distribution.

## Architecture

```
Exchange / Mock Exchange
    │  UDP multicast  224.0.31.50:5000
    │  (wrapped in GRE unicast on AWS VPC — see GRE mode below)
    ▼
PacketReplicator  (feeder host, eth0)
    │  AF_XDP zero-copy RX  →  eBPF XDP program intercepts matching frames
    │  Extracts UDP payload in userspace
    │  Fan-out: AF_XDP zero-copy TX to each subscriber
    ▼
Trading strategy instances  (unicast UDP, any port)
```

Three ingress modes are supported, selected automatically at startup:

| Mode | BPF program | When used |
|------|------------|-----------|
| Unicast | `unicast_filter.o` | `listen_ip` is a regular unicast address |
| Native multicast | `multicast_filter.o` | `listen_ip` is a multicast group (224.0.0.0/4); real multicast routing available (TGW Multicast Domain or same-subnet) |
| GRE tunnel | `gre_filter.o` | `--gre` flag; AWS VPC without native multicast routing; mock exchange wraps inner multicast UDP in GRE unicast |

Egress is always AF_XDP zero-copy unicast UDP to each registered subscriber, with automatic fallback to a kernel socket.

## Quick Start

### Build
```bash
# Install dependencies (Amazon Linux 2023)
sudo yum install clang libbpf-devel elfutils-libelf-devel -y
sudo yum groupinstall "Development Tools" -y

# Clone and build xdp-tools
git clone https://github.com/xdp-project/xdp-tools.git
cd xdp-tools && ./configure && make && sudo make install

# Build all binaries and eBPF programs
make all
```

Produced binaries: `packet_replicator`, `control_client`, `test_client`, `market_data_provider_client`.
Produced eBPF objects: `unicast_filter.o`, `multicast_filter.o`, `gre_filter.o`.

---

## Usage

### Mode 1 — Unicast (original benchmark mode)

**Feeder host**
```bash
sudo ./packet_replicator eth0 10.0.1.20 5000
```

**Subscriber / benchmark client**
```bash
# Self-registers via control protocol, then runs RTT benchmark
./market_data_provider_client 10.0.1.20 5000 10.0.1.30 9001 1000000 10000
```

### Mode 2 — GRE tunnel (AWS VPC POC: simulated multicast)

AWS VPC does not support IP multicast routing between subnets. GRE mode works around this:
the mock exchange encapsulates inner multicast UDP inside a GRE unicast packet sent to the feeder.
The feeder's `gre_filter.o` intercepts the GRE frame before the kernel `ip_gre` module, strips
the headers in userspace, and fans out the inner UDP payload to subscribers.

**Mock exchange host — configure kernel GRE tunnel once**
```bash
# Create GRE tunnel pointing at feeder unicast IP
sudo ip tunnel add gre_feed mode gre remote <feeder_ip> local <exchange_ip> ttl 64
sudo ip link set gre_feed up
# Route multicast group into the tunnel
sudo ip route add 224.0.0.0/4 dev gre_feed
# Allow GRE (IP protocol 47) in security group: exchange → feeder
```

**Feeder host**
```bash
# GRE mode + upstream control forwarding (subscribers → feeder → producer)
sudo ./packet_replicator eth0 224.0.31.50 5000 true --gre \
    --ctrl 224.0.31.51:5001 --producer 10.0.1.10:6000

# GRE only (no control forwarding)
sudo ./packet_replicator eth0 224.0.31.50 5000 true --gre
```

`--ctrl <group>:<port>` and `--producer <ip>:<port>` must be used together. The feeder joins the
control multicast group, receives subscriber control messages, and forwards them verbatim to the
producer's unicast endpoint.

**Mock exchange — send test traffic**
```bash
# test_client auto-detects multicast target and sets IP_MULTICAST_TTL=8
./test_client 224.0.31.50 5000 1 "trade" --iface eth0

# Or run the full RTT benchmark (--feeder-ip separates data dst from control dst)
./market_data_provider_client 224.0.31.50 5000 10.0.1.10 9001 1000000 10000 \
    --feeder-ip 10.0.1.20
```

**Subscribers — register with feeder**
```bash
./control_client 10.0.1.20 add 10.0.1.30 9001
./control_client 10.0.1.20 list
```

### Mode 3 — Native multicast (TGW Multicast Domain / co-location)

```bash
# Feeder joins the multicast group via IGMP and loads multicast_filter.o automatically
sudo ./packet_replicator eth0 224.0.31.50 5000

# Exchange sends directly to the multicast group (no GRE tunnel needed)
./test_client 224.0.31.50 5000 1 "trade" --iface eth0
```

Requires a TGW Multicast Domain (cross-subnet) or same-subnet multicast routing.

---

## Control Protocol (port 12345)

```bash
./control_client <feeder_ip> add    <dest_ip> <dest_port>   # register subscriber
./control_client <feeder_ip> remove <dest_ip> <dest_port>   # deregister
./control_client <feeder_ip> list                           # inspect active subscribers
```

The control client has a 5-second receive timeout — it will fail fast if the feeder is unreachable
rather than blocking indefinitely.

---

## Verification

**Confirm AF_XDP zero-copy is active (on feeder after traffic starts)**
```bash
ethtool -S eth0 | grep -E 'xsk|xdp_redirect'
# queue_1_tx_xsk_cnt:      10000   # AF_XDP zero-copy TX frames
# queue_1_rx_xdp_redirect: 10000   # XDP program redirected to AF_XDP
```

**GRE mode — confirm encapsulated frames arrive**
```bash
sudo tcpdump -i eth0 proto gre -c 5
```

**ARP resolution (required for AF_XDP TX on ENA/VPC)**
```bash
cat /proc/net/arp | grep <subscriber_ip>
# If broadcast FF:FF:FF:FF:FF:FF appears, the feeder falls back to kernel socket
# and self-heals within 100 ms once ARP resolves
```

---

## Components

| Binary / Object | Role |
|----------------|------|
| `packet_replicator` | AF_XDP ingress + zero-copy fan-out engine |
| `control_client` | CLI to add/remove/list subscribers at runtime |
| `test_client` | UDP traffic generator; supports unicast and multicast targets |
| `market_data_provider_client` | RTT benchmark: self-registers, sends trade messages, reports latency percentiles |
| `unicast_filter.o` | eBPF XDP: matches target unicast IP:port → AF_XDP |
| `multicast_filter.o` | eBPF XDP: matches 224.0.0.0/4 range + target port → AF_XDP |
| `gre_filter.o` | eBPF XDP: matches GRE unicast frames carrying inner multicast UDP → AF_XDP |

---

## Performance Features

- **Sub-microsecond latency** packet forwarding via AF_XDP zero-copy
- **Zero hot-path syscalls** — interface IP/MAC cached at init, destination MACs cached at `add` time
- **Multi-queue** processing with one dedicated thread per NIC queue
- **Lock-free** thread-local destination cache refreshed every 100 ms
- **CPU affinity** binding for cache-resident hot paths
- **Self-healing MAC resolution** — broadcast-MAC destinations fall back to kernel socket while ARP resolves, then automatically switch to AF_XDP fast path

## HFT Optimizations

- Branch prediction hints (`__builtin_expect`) on cold paths
- Cache-aligned data structures (64-byte alignment on all hot-path structs)
- Busy polling with `_mm_pause` / `XDP_USE_NEED_WAKEUP` instead of blocking sleep
- RFC 6864 compliant IP ID=0 on DF-set atomic datagrams
- ARP polling (1 ms interval, 50 ms cap) instead of fixed sleep in `addDestination()`

---

## Troubleshooting

**Permission errors**: Run `packet_replicator` with `sudo` (AF_XDP requires CAP_NET_ADMIN).

**Device busy / XDP program conflict**:
```bash
sudo ./cleanup.sh
# or manually: sudo ip link set eth0 xdp off
```

**Zero-copy fails**: Driver falls back to copy mode automatically. Check driver support:
```bash
ethtool -i eth0   # ENA (AWS), i40e, ixgbe, mlx5_core all support XDP_ZEROCOPY
```

**GRE packets not arriving at feeder**:
- Check security group: IP protocol 47 (GRE) must be allowed from exchange to feeder.
- Verify tunnel: `ip tunnel show` on exchange host.
- Confirm route: `ip route get 224.0.31.50` on exchange host — should show `dev gre_feed`.

**Subscribers not receiving data**:
```bash
./control_client <feeder_ip> list   # confirm subscriber is registered
cat /proc/net/arp | grep <subscriber_ip>   # confirm ARP resolved
```

**market_data_provider_client hangs at startup**: The feeder must be running before the client
starts — `addSelfAsSubscriber()` calls `control_client` which blocks up to 5 seconds for a
response from port 12345.

---

## Requirements

- Linux kernel 5.10+ (AF_XDP zero-copy; `XDP_USE_NEED_WAKEUP` available from 5.11)
- AF_XDP compatible NIC driver (ENA on AWS, i40e, ixgbe, mlx5_core)
- Root privileges for AF_XDP and XDP program loading
- `clang` with BPF target support for eBPF compilation

## License

MIT-0 License
