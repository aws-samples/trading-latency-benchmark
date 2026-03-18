# mcast2ucast

A DPDK-based daemon that converts multicast UDP traffic into unicast UDP streams (and back) with zero-copy payload duplication. Runs on each host in a cluster to provide transparent multicast over unicast-only networks (e.g. AWS VPC).

Each host runs one instance of mcast2ucast. Local applications send and receive multicast normally via a TAP interface (`mcast0`). The daemon intercepts multicast, fans it out as unicast to subscribers, and on the receiving end restores the original multicast destination before delivering to local apps.

## Architecture

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │  HOST A (Producer Side)                                             │
 │                                                                     │
 │  App sends multicast ──► mcast0 (TAP) ──► mcast2ucast              │
 │                                              │                      │
 │                                              │ rewrite to unicast   │
 │                                              │ + m2u tunnel header  │
 │                                              ▼                      │
 │                                           ENA (igb_uio)             │
 └───────────────────────────────────────────────┬─────────────────────┘
                                                 │
                                                 │  unicast UDP over wire / VPC
                                                 │
 ┌───────────────────────────────────────────────┼─────────────────────┐
 │  HOST B (Subscriber Side)                     ▼                     │
 │                                           ENA (igb_uio)             │
 │                                              │                      │
 │                                              │ detect m2u tunnel    │
 │                                              │ strip header         │
 │                                              │ restore multicast    │
 │                                              ▼                      │
 │  App receives multicast ◄── mcast0 (TAP) ◄── mcast2ucast           │
 └─────────────────────────────────────────────────────────────────────┘
```

## End-to-End Data Flow

### Forward Path (Multicast → Unicast)

```
 Multicast sender (local app)
       │
       │ sendto(224.0.0.101:5001)
       ▼
 ┌───────────┐
 │  mcast0   │  DPDK TAP PMD (kernel sees it as a regular netdev)
 │  (TAP RX) │  multicast route: 224.0.0.0/4 → mcast0
 └───────────┘
       │
       │ RX via DPDK poll-mode (rte_eth_rx_burst)
       ▼
 ┌──────────────────┐
 │   Data Plane      │  Classifies packets:
 │   (lcore 1)       │    • Multicast UDP  → rewrite engine
 │                    │    • IGMP           → punt to control plane via rte_ring
 │                    │    • Other          → drop
 └──────────────────┘
       │
       │ rewrite_mcast_to_ucast()
       ▼
 ┌──────────────────┐  Per subscriber:
 │  Rewrite Engine   │    1. Alloc header mbuf (50B: Eth + IP + UDP + tunnel hdr)
 │  (zero-copy)      │    2. Clone/refcnt original UDP payload (zero-copy)
 │                    │    3. Rewrite dst IP to subscriber's unicast IP
 │                    │    4. Insert 8-byte m2u tunnel header (magic + group IP)
 │                    │    5. Recalculate IP checksum, zero UDP checksum
 │                    │    6. Chain: header → payload (multi-segment mbuf)
 └──────────────────┘
       │
       │ TX via DPDK (rte_eth_tx_burst)
       ▼
 ┌───────────┐
 │  NIC TX   │  ENA PMD (igb_uio): direct to wire, kernel bypass
 │           │  TAP mode: kernel routes via IP stack + ARP
 └───────────┘
       │
       │ wire / VPC
       ▼
   Remote subscriber host
```

### Reverse Path (Unicast → Multicast)

On the subscriber host, incoming unicast packets with the m2u tunnel header are detected, the tunnel is stripped, and the original multicast destination is restored:

```
 Unicast packet from producer host
       │
       │ dst = subscriber_ip:5001, payload = [m2u_hdr][original UDP data]
       ▼
 ┌───────────┐
 │  NIC RX   │  ENA PMD (igb_uio) or AF_PACKET PMD
 └───────────┘
       │
       │ try_reverse_path(): check first 4 bytes of UDP payload for M2U_TUNNEL_MAGIC
       ▼
 ┌──────────────────┐
 │  Reverse Path     │    1. Extract group_ip from tunnel header
 │  (in data plane)  │    2. Strip 8-byte tunnel header (memmove + trim)
 │                    │    3. Rewrite IP dst → original multicast group
 │                    │    4. Set Ethernet dst → 01:00:5e multicast MAC
 │                    │    5. Adjust IP total_length and UDP dgram_len (-8)
 │                    │    6. Recalculate IP checksum, zero UDP checksum
 └──────────────────┘
       │
       │ TX to TAP port
       ▼
 ┌───────────┐
 │  mcast0   │  Kernel receives multicast frame on TAP
 │  (TAP TX) │  Delivers to sockets subscribed to the multicast group
 └───────────┘
       │
       ▼
 Local app receives original multicast on 224.0.0.101:5001
```

### m2u Tunnel Header

An 8-byte header prepended to the UDP payload during the forward path:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic (0x4D325501 "M2U\x01")               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              Original Multicast Group IP (NBO)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

The subscriber side uses the magic to identify encapsulated packets and the group IP to reconstruct the original multicast destination.

### Control Plane

```
 Data Plane ──[IGMP packets]──► rte_ring ──► Control Plane (lcore 2)
                                                  │
                           ┌──────────────────────┼───────────────────┐
                           │                      │                   │
                      Parse IGMP v1/v2/v3    Poll ctrl port       Periodic
                      Update group table     for remote            30s keepalives
                      (lock-free hash)       SUBSCRIBE/            90s subscriber
                                             UNSUBSCRIBE/          expiry
                                             KEEPALIVE
                           │                      │                   │
                           ▼                      ▼                   │
                    Notify producers         Add/remove/refresh       │
                    via kernel UDP           dynamic subscribers      │
```

The control protocol uses a 16-byte `notify_msg` over unicast UDP:

| Field | Size | Description |
|-------|------|-------------|
| type | 1B | SUBSCRIBE (0x01), UNSUBSCRIBE (0x02), KEEPALIVE (0x03) |
| reserved | 1B | — |
| subscriber_port | 2B | Subscriber's UDP port (NBO) |
| group_ip | 4B | Multicast group address (NBO) |
| subscriber_ip | 4B | Subscriber's unicast IP (NBO) |
| source_ip | 4B | Source filter IP (0 = ASM, any source) |

## Zero-Copy Strategy

Adapted from DPDK's `examples/ipv4_multicast`:

| Fan-out | Segments | Strategy | Cost per subscriber |
|---------|----------|----------|---------------------|
| ≤ 2 | ≤ 2 | `rte_pktmbuf_clone()` | Header alloc (50B) + clone metadata |
| > 2 | any | `rte_pktmbuf_refcnt_update()` | Header alloc (50B) only |

Three dedicated memory pools:
- **packet_pool** (8192 mbufs) — full RX buffers
- **header_pool** (8192 mbufs, 256B) — Eth+IP+UDP+tunnel headers for rewritten packets
- **clone_pool** (8192 mbufs, 0B data) — indirect mbufs for clone path

`MBUF_FAST_FREE` is explicitly disabled since it assumes refcnt==1 and a single pool.

## Provisioning

### Prerequisites

- Two (or more) Linux hosts with network connectivity (physical, VPC, etc.)
- DPDK 24.11+ built and installed
- 2GB+ hugepages allocated

### AWS EC2 Setup

#### 1. Launch Instances

Launch two EC2 instances (e.g. `m8a.2xlarge`) in the same VPC and subnet. Ensure the security group allows UDP traffic between instances.

#### 2. Install DPDK on Both Instances

```bash
# Install build dependencies
sudo yum install -y numactl-devel elfutils-libelf-devel libpcap-devel \
  meson ninja-build python3-pip libatomic
pip3 install pyelftools

# Download and build DPDK
cd ~
curl -sLO https://fast.dpdk.org/rel/dpdk-25.11.tar.xz
tar xf dpdk-25.11.tar.xz
cd dpdk-25.11
meson setup build
ninja -C build -j$(nproc)
sudo ninja -C build install
sudo ldconfig
```

#### 3. Allocate Hugepages on Both Instances

```bash
sudo sysctl -w vm.nr_hugepages=1024
# Verify:
grep HugePages_Free /proc/meminfo
```

#### 4. Build mcast2ucast on Both Instances

```bash
cd ~/mcast2ucast
PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig meson setup build
meson compile -C build
```

#### 5. (Optional) ENA Kernel Bypass for Lower Latency

Binding a secondary ENI to igb_uio gives mcast2ucast direct NIC access, bypassing the kernel for wire-facing TX/RX. Combined with immediate TX flush and OS tuning, this achieves ~34µs P50 e2e latency.

```bash
# From a machine with AWS CLI (not the instance):
./create_eni.sh <instance-id> <subnet-id> <sg-id> <region>

# On the instance:
sudo ./build_igb_uio.sh       # Build igb_uio kernel module
sudo ./setup_ena_bypass.sh     # Bind secondary ENI to DPDK
```

**Why igb_uio?** AWS Nitro has no hardware IOMMU, so:
- `vfio-pci` (no-IOMMU): no write-combine → LLQ disabled → ENA SQ creation fails
- `uio_pci_generic`: no MSI-X → ENA admin command timeouts
- `igb_uio` with `wc_activate=1`: supports MSI-X + write-combine → works

Must also pass `llq_policy=1` devarg (defaults to 0 due to zero-init):
```
--allow=0000:28:00.0,llq_policy=1
```

### Configuration

#### Config File Format

```conf
# Producer map: which upstream producer to notify for each multicast group
# producer <multicast-group> <producer-ip> <producer-port>
producer 224.0.0.101 10.0.1.5 9000

# Static subscribers: always forward this group to these unicast destinations
# subscriber <multicast-group> <unicast-ip> <unicast-port> [dst-mac]
subscriber 224.0.0.101 10.0.1.10 5001 02:00:00:00:00:01
```

#### CLI Options

```
mcast2ucast [EAL options] -- [app options]

App options:
  --rx-port <id>       RX port ID (default: 0)
  --tx-port <id>       TX port ID (default: 0)
  --config <file>      Path to config file
  --tap <name>         Create TAP interface with this name (enables TAP mode)
  --local-ip <ip>      Local IP for producer notifications
  --ctrl-port <port>   Control port for SUBSCRIBE/UNSUBSCRIBE (default: 9000)
```

## Running

### ENA Kernel Bypass Mode (Recommended — ~34µs P50)

Both hosts use a secondary ENI bound to igb_uio for wire-facing traffic, while keeping the primary NIC for SSH. Local apps communicate via a TAP interface (`mcast0`).

```bash
# Prerequisites (both instances):
sudo modprobe uio
sudo insmod /path/to/igb_uio.ko wc_activate=1
sudo ip link set <secondary-nic> down
sudo dpdk-devbind.py -b igb_uio <pci-address>
sudo sysctl -w vm.nr_hugepages=1024
```

**Producer Host (Instance 1):**

```bash
cat > deploy.conf << 'EOF'
subscriber 224.0.0.101 <subscriber-eni-ip> 5001 <subscriber-eni-mac>
EOF

sudo LD_LIBRARY_PATH=/usr/local/lib64 \
  ./build/mcast2ucast \
    -l 0-2 \
    --allow=0000:28:00.0,llq_policy=1 \
    --log-level=user1,7 \
    -- \
    --rx-port 0 --tx-port 0 \
    --tap mcast0 \
    --config deploy.conf \
    > /tmp/mcast2ucast.log 2>&1 &
```

**Subscriber Host (Instance 2):**

```bash
cat > subscriber.conf << 'EOF'
# Subscriber side: receives unicast, restores multicast via TAP
EOF

sudo LD_LIBRARY_PATH=/usr/local/lib64 \
  ./build/mcast2ucast \
    -l 0-2 \
    --allow=0000:28:00.0,llq_policy=1 \
    --log-level=user1,7 \
    -- \
    --rx-port 0 --tx-port 0 \
    --tap mcast0 \
    --config subscriber.conf \
    > /tmp/mcast2ucast.log 2>&1 &
```

**Post-start TAP configuration (both hosts):**

```bash
sleep 3
sudo ip addr add <local-ip>/24 dev mcast0
sudo ip route add 224.0.0.0/4 dev mcast0
sudo sysctl -qw net.ipv4.conf.all.accept_local=1
sudo sysctl -qw net.ipv4.conf.all.rp_filter=0
sudo sysctl -qw net.ipv4.conf.mcast0.rp_filter=0

# Pin primary NIC IRQs away from DPDK lcores (critical for kernel UDP latency)
for irq in $(grep <primary-nic> /proc/interrupts | awk '{print $1}' | tr -d ':'); do
    echo f0 | sudo tee /proc/irq/$irq/smp_affinity > /dev/null
done

# If mcast0 shares the same subnet as <primary-nic>, add host routes for unicast peers:
sudo ip route replace <remote-ip>/32 dev <primary-nic> src <local-ip>

# iptables (if FORWARD policy is DROP)
sudo iptables -P FORWARD ACCEPT
```

### TAP-Only Mode (No NIC Unbinding — ~100µs P50)

For simpler setups where you don't want to dedicate a NIC to DPDK. Uses `--no-pci` and creates a TAP-only configuration.

**Producer Host:**

```bash
sudo LD_LIBRARY_PATH=/usr/local/lib64 \
  ./build/mcast2ucast \
    -l 0-2 \
    --vdev=net_tap0,iface=mcast0 \
    --no-pci \
    --log-level=user1,7 \
    -- \
    --rx-port 0 --tx-port 0 \
    --config deploy.conf \
    > /tmp/mcast2ucast.log 2>&1 &
```

**Subscriber Host:**

```bash
sudo LD_LIBRARY_PATH=/usr/local/lib64 \
  ./build/mcast2ucast \
    -l 0-2 \
    --vdev=net_af_packet0,iface=<primary-nic> \
    --log-level=user1,7 \
    -- \
    --rx-port 0 --tx-port 0 \
    --tap mcast0 \
    --config subscriber.conf \
    > /tmp/mcast2ucast.log 2>&1 &
```

Post-start TAP configuration is the same as above.

**Important**: Do NOT pass `--vdev=net_tap0,iface=mcast0` in EAL options together with `--tap mcast0`. The `--tap` flag creates the TAP device internally. Passing both creates a conflict. For TAP-only mode on the producer, use `--vdev` without `--tap`.

## Testing

### End-to-End Multicast Test

On the **subscriber host** (Instance 2), start a multicast listener bound to the TAP interface:

```python
# recv_mcast.py — run with: sudo python3 recv_mcast.py
import socket, struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.setsockopt(socket.SOL_SOCKET, 25, b"mcast0\0")  # SO_BINDTODEVICE
sock.bind(("", 5001))

# Join multicast group on mcast0 IP specifically
mreq = struct.pack("4s4s",
    socket.inet_aton("224.0.0.101"),
    socket.inet_aton("10.0.1.10"))  # mcast0 IP on subscriber
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
sock.settimeout(30)

print("Listening on mcast0 for 224.0.0.101:5001...")
while True:
    try:
        data, addr = sock.recvfrom(4096)
        print(f"Received {len(data)} bytes from {addr}: {data[:60]}")
    except socket.timeout:
        break
```

On the **producer host** (Instance 1), send multicast:

```python
# send_mcast.py
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
    socket.inet_aton("10.0.1.5"))  # mcast0 IP on producer

for i in range(5):
    msg = f"Hello multicast test {i}".encode()
    sock.sendto(msg, ("224.0.0.101", 5001))
    print(f"Sent: {msg}")
```

Expected output on subscriber:

```
Listening on mcast0 for 224.0.0.101:5001...
Received 22 bytes from ('10.0.1.5', 40852): b'Hello multicast test 0'
Received 22 bytes from ('10.0.1.5', 40852): b'Hello multicast test 1'
Received 22 bytes from ('10.0.1.5', 40852): b'Hello multicast test 2'
Received 22 bytes from ('10.0.1.5', 40852): b'Hello multicast test 3'
Received 22 bytes from ('10.0.1.5', 40852): b'Hello multicast test 4'
```

The payload is clean (no tunnel header), the source IP is preserved, and the multicast group is correctly restored.

### Gotchas

- **Multicast route conflict**: The kernel may add a specific route like `224.0.0.101 dev <primary-nic>` when a socket joins a multicast group. This overrides the general `224.0.0.0/4 dev mcast0` route and prevents TAP delivery. Delete it: `sudo ip route del 224.0.0.101 dev <primary-nic>`.
- **Multicast group must be joined on mcast0**: Use the mcast0 IP in `IP_ADD_MEMBERSHIP`, not `0.0.0.0`, to ensure the multicast MAC filter is added to the TAP interface.
- **AF_PACKET captures copies**: On the subscriber side, the kernel still processes unicast packets normally. Applications listening on the raw unicast port will receive packets with the tunnel header in the payload. Use `SO_BINDTODEVICE` to mcast0 for clean multicast-only reception.

## Latency Benchmarks

Measured on AWS EC2 `m8a.2xlarge` instances in the same AZ, 1000 iterations, 64-byte packets.

### Comparison

| Setup | P0 | P50 | P90 | P99 | Notes |
|-------|-----|-----|-----|-----|-------|
| **DPDK kernel bypass** | 25.2µs | 26.8µs | 28.1µs | 29.9µs | igb_uio + ENA LLQ, dpdk_latency |
| **Standard UDP sockets** | 28.3µs | 29.9µs | 31.1µs | 32.4µs | Linux sendto/recvfrom, latency_test |
| **mcast2ucast e2e (ENA bypass)** | 31.7µs | 33.7µs | 34.9µs | 36.4µs | TAP RX → ENA TX, immediate flush |
| **mcast2ucast e2e (TAP only)** | 89.9µs | 100.0µs | 102.9µs | 105.2µs | TAP RX + TAP TX, no kernel bypass |
| **DPDK AF_PACKET** | 60.9µs | 130.6µs | 132.1µs | 263.0µs | dpdk_latency via AF_PACKET PMD |

All benchmarks run with full OS tuning (see [OS Tuning](#os-tuning-for-low-latency) below): kernel boot params (`isolcpus`, `nohz_full`, `idle=poll`), NIC IRQ isolation, workqueue affinity, THP disabled, and sysctl tuning.

### Analysis

- **DPDK kernel bypass** (26.8µs P50) is the raw floor — packets never touch the kernel, going directly from userspace to NIC hardware via igb_uio and ENA's Low Latency Queue mode.
- **Standard UDP sockets** (29.9µs P50) with full OS tuning is remarkably fast. Kernel boot params (`isolcpus`, `nohz_full`, `idle=poll`) eliminate timer ticks and scheduling jitter on DPDK cores, while `busy_poll`/`busy_read` sysctl makes the kernel spin-poll NIC queues instead of interrupt-driven wakeup. Tail latency improved dramatically (P99: 32.4µs vs 75.2µs before tuning).
- **mcast2ucast e2e with ENA bypass** (33.7µs P50) is only ~7µs above raw DPDK and ~4µs above kernel UDP. The overhead comes from the TAP→DPDK→ENA hop on the TX side. P99 of 36.4µs shows excellent tail consistency from DPDK's deterministic poll-mode. Key optimizations: (1) wire-facing NIC bound to igb_uio for kernel-bypass TX/RX, (2) immediate `flush_tx()` after packet production, (3) full OS tuning with CPU isolation and C-state elimination.
- **mcast2ucast e2e TAP-only** (100.0µs P50) uses AF_PACKET or TAP for the wire-facing path, adding kernel overhead on both TX and RX.
- **DPDK AF_PACKET** (130.6µs P50) is the slowest — the MMAP ring buffer synchronization adds more overhead than native kernel sockets.

**Note**: The first run after mcast2ucast restart shows ~2× higher latency due to cold caches (ENA interrupt coalescing, CPU caches). Results above are from warmed-up runs (preceded by a 100-iteration warmup pass).

### OS Tuning for Low Latency

The `tune_os.sh` script applies comprehensive OS-level tuning inspired by [aws-samples/trading-latency-benchmark](https://github.com/aws-samples/trading-latency-benchmark). It reduced mcast2ucast e2e P50 from 38.7µs to 33.7µs and kernel UDP P99 from 75.2µs to 32.4µs.

```bash
# Apply runtime tuning only (no reboot needed):
sudo ./tune_os.sh

# Apply runtime tuning + kernel boot parameters (requires reboot):
sudo ./tune_os.sh --grub --reboot
```

**What it does:**

| Category | Tuning | Effect |
|----------|--------|--------|
| **Kernel boot** | `isolcpus=0-2 nohz_full=0-2 rcu_nocbs=0-2` | Removes DPDK cores from scheduler, stops timer ticks, offloads RCU |
| **C-state elimination** | `cpuidle.off=1 processor.max_cstate=0 idle=poll` | Prevents CPU sleep states (eliminates wakeup latency) |
| **Sysctl (network)** | `busy_poll=50`, `busy_read=50`, `noqueue` qdisc | Spin-poll NIC queue, eliminate TX queueing delay |
| **Sysctl (kernel)** | `numa_balancing=0`, `timer_migration=0`, `swappiness=0` | Reduce scheduling jitter and memory management overhead |
| **THP** | `transparent_hugepage=never` | Prevents compaction latency spikes at P99+ |
| **NIC** | Interrupt coalescing off, GRO/GSO/TSO off | Minimize per-packet latency |
| **IRQ affinity** | <primary-nic> IRQs pinned to CPUs 3-7 | Prevents DPDK poll-mode from starving kernel softirqs |
| **Workqueues** | All kernel workqueues pinned to CPUs 3-7 | Keeps kernel work off DPDK cores |
| **Services** | Disable irqbalance, firewalld | Prevents IRQ rebalancing and per-packet firewall overhead |

The `--grub` flag provides the biggest latency improvement (CPU isolation and C-state elimination). Runtime tuning alone is useful but less impactful.

### NIC IRQ Isolation (Critical)

DPDK's poll-mode loop busy-spins at 100% CPU on its assigned lcores (e.g. CPUs 0–2 with `-l 0-2`). If the primary NIC's (<primary-nic>) interrupts land on those same CPUs, kernel softirq processing is starved, inflating kernel UDP latency from ~40µs to 250+µs.

**Fix**: Pin <primary-nic> IRQs to CPUs not used by DPDK:

```bash
# If mcast2ucast uses -l 0-2 (CPUs 0,1,2), pin NIC IRQs to CPUs 4-7:
for irq in $(grep <primary-nic> /proc/interrupts | awk '{print $1}' | tr -d ':'); do
    echo f0 > /proc/irq/$irq/smp_affinity   # 0xf0 = CPUs 4-7
done
```

This must be done on **both** instances after boot. The `setup_ena_bypass.sh` script does this automatically.

### Reproducing Benchmarks

Full step-by-step guide to reproduce the latency numbers from scratch on two AWS EC2 instances.

#### Step 1: Provision Two EC2 Instances

Launch two `m8a.2xlarge` (8 vCPUs, no SMT) in the same VPC/subnet/AZ. Security group must allow all UDP between instances.

```bash
# Note your instance IDs, subnet ID, and security group ID
INST1=i-xxx   # Producer
INST2=i-yyy   # Subscriber
SUBNET=subnet-xxx
SG=sg-xxx
```

#### Step 2: Install DPDK and Build mcast2ucast (Both Instances)

```bash
# Install DPDK (see "Install DPDK on Both Instances" above)
# Then build mcast2ucast:
cd ~/mcast2ucast
PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig meson setup build
meson compile -C build

# Build benchmark tools:
cd benchmarks && make
```

#### Step 3: Create Secondary ENIs for Kernel Bypass (From Your Workstation)

```bash
./create_eni.sh $INST1 $SUBNET $SG eu-central-1
./create_eni.sh $INST2 $SUBNET $SG eu-central-1
# Note the ENI IPs and MACs printed by each command
```

#### Step 4: Apply OS Tuning (Both Instances)

```bash
# Copy tune_os.sh to each instance, then:
sudo ./tune_os.sh --grub --reboot
# Instances reboot. Wait for them to come back up.

# After reboot, verify kernel params took effect:
cat /proc/cmdline | tr ' ' '\n' | grep -E 'isolcpus|nohz_full|idle'
```

#### Step 5: Set Up ENA Bypass (Both Instances, After Reboot)

```bash
# Build igb_uio (needed after each kernel update):
sudo ./build_igb_uio.sh

# Bind secondary ENI to DPDK:
sudo ./setup_ena_bypass.sh 28:00.0
# This also pins primary NIC IRQs away from DPDK lcores
```

#### Step 6: Start mcast2ucast (Both Instances)

```bash
# Producer (instance 1):
cat > deploy.conf << 'EOF'
subscriber 224.0.0.101 <inst2-eni-ip> 5001 <inst2-eni-mac>
EOF

sudo LD_LIBRARY_PATH=/usr/local/lib64 ./build/mcast2ucast \
  -l 0-2 --allow=0000:28:00.0,llq_policy=1 --log-level=user1,7 \
  -- --rx-port 0 --tx-port 0 --tap mcast0 --config deploy.conf &

# Subscriber (instance 2):
sudo LD_LIBRARY_PATH=/usr/local/lib64 ./build/mcast2ucast \
  -l 0-2 --allow=0000:28:00.0,llq_policy=1 --log-level=user1,7 \
  -- --rx-port 0 --tx-port 0 --tap mcast0 --config subscriber.conf &
```

#### Step 7: Configure TAP Interface (Both Instances)

```bash
sleep 3
sudo ip addr add <local-ip>/24 dev mcast0
sudo ip route add 224.0.0.0/4 dev mcast0
sudo sysctl -qw net.ipv4.conf.all.accept_local=1
sudo sysctl -qw net.ipv4.conf.all.rp_filter=0
sudo sysctl -qw net.ipv4.conf.mcast0.rp_filter=0
sudo iptables -P FORWARD ACCEPT

# Host route to prevent mcast0 from capturing unicast traffic:
sudo ip route replace <remote-ip>/32 dev <primary-nic> src <local-ip>
```

#### Step 8: Run Benchmarks

```bash
# On subscriber (instance 2) — start reflector servers:
./benchmarks/start_servers.sh <mcast0-ip>

# On producer (instance 1) — run all benchmarks:
./benchmarks/run_benchmarks.sh <remote-ip> <mcast0-local-ip> 1000 64
```

The `run_benchmarks.sh` script runs both direct UDP and mcast2ucast e2e tests with proper CPU pinning (CPU 6, away from DPDK lcores 0-2).

#### Step 9: DPDK Kernel Bypass Benchmark (Optional)

This measures raw DPDK-to-DPDK latency without mcast2ucast in the path. Requires stopping mcast2ucast and running `dpdk_latency` directly on the secondary ENIs.

```bash
# Stop mcast2ucast on both instances first
sudo pkill mcast2ucast

# Server (instance 2):
sudo LD_LIBRARY_PATH=/usr/local/lib64 dpdk-latency \
  --allow=0000:28:00.0,llq_policy=1 -l 0 -- -u

# Client (instance 1):
sudo LD_LIBRARY_PATH=/usr/local/lib64 dpdk-latency \
  --allow=0000:28:00.0,llq_policy=1 -l 0 -- \
  -c -u -m <server-eni-mac> -s <client-eni-ip> -d <server-eni-ip> -i 1000 -l 64
```

#### Cleanup Between Tests

```bash
# Kill all benchmark processes
pkill -f latency_test
pkill -f dpdk-latency

# Wait for ports to release
sleep 2

# Re-pin IRQs if irqbalance moved them (shouldn't happen if disabled)
for irq in $(grep <primary-nic> /proc/interrupts | awk '{print $1}' | tr -d ':'); do
    echo f0 > /proc/irq/$irq/smp_affinity
done
```

#### Troubleshooting

- **Inflated kernel UDP latency (250+µs)**: NIC IRQs on DPDK cores. Run `cat /proc/interrupts | grep <primary-nic>` — if CPUs 0-2 show high counts, re-pin IRQs. `tune_os.sh` and `setup_ena_bypass.sh` both handle this.
- **Cold-cache first run**: First mcast2ucast e2e test after restart shows ~2× higher latency. Run a 100-iteration warmup first, then the real measurement.
- **mcast0 route conflict**: If mcast0 and <primary-nic> share a subnet, mcast0 wins (metric 0). Add host routes: `ip route replace <remote-ip>/32 dev <primary-nic>`.
- **Multiple servers interfering**: Old `latency_test` processes from previous runs may still be running. Always `pkill -f latency_test` before starting new ones.
- **igb_uio build fails after reboot**: Kernel headers mismatch. Run `sudo ./build_igb_uio.sh` to rebuild against the current kernel.

## Project Structure

```
mcast2ucast/
  meson.build               Build configuration
  deploy.conf               Example producer-side config
  src/
    main.c                  EAL init, port setup, TAP creation, lcore assignment
    config.c / config.h     CLI parsing, config file loading, producer map
    dataplane.c / .h        Poll-mode RX loop, classification, forward + reverse path
    rewrite.c / .h          Zero-copy multicast→unicast engine, m2u tunnel header
    group_table.c / .h      Lock-free hash: multicast group → subscriber list
    control.c / .h          IGMP snooping, ctrl port listener, keepalives, expiry
    producer_notify.c / .h  Kernel UDP socket for SUBSCRIBE/UNSUBSCRIBE/KEEPALIVE
    stats.c / stats.h       Atomic counters for RX/TX/drop/IGMP
    web.c / web.h           HTTP dashboard on port 8080
  benchmarks/
    Makefile                Build all benchmark tools
    run_benchmarks.sh       Client-side: run all latency tests (pinned to CPU 6)
    start_servers.sh        Server-side: start reflector servers (pinned to CPU 6)
    latency_test.cpp        4-mode latency tool: server, mcast-server, direct, mcast
    latency_sender.cpp      Standalone multicast sender with timestamps
    latency_receiver.cpp    Standalone multicast receiver with latency reporting
    test_sender.py          Python multicast test sender
    latency_sender.py       Python latency sender
    latency_receiver.py     Python latency receiver
  create_eni.sh             AWS: create + attach secondary ENI for DPDK bypass
  setup_ena_bypass.sh       AWS: bind ENI to igb_uio for kernel-bypass TX
  build_igb_uio.sh          Build igb_uio kernel module from dpdk-kmods
  tune_os.sh                Ultra-low latency OS tuning (sysctl, GRUB, IRQ, THP, etc.)
  end_to_end_ptp.sh         Configure PTP-grade time sync for latency testing
```
