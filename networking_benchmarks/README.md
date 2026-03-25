# Networking Benchmarks

A collection of low-latency networking tools for measuring and optimizing packet delivery on AWS EC2. Each project targets a different layer of the network stack — from kernel sockets to AF_XDP zero-copy to DPDK poll-mode — enabling precise latency characterization of HFT market data paths.

## Projects

| Project | Approach | Language | Latency | Use Case |
|---------|----------|----------|---------|----------|
| [ec2_timestamping_programs](#ec2-timestamping-programs) | Kernel sockets + HW timestamps | C | Baseline | Per-hop latency decomposition with Nitro PHC |
| [open_onload](#open_onload) | OpenOnload kernel bypass | C | — | Feed relay receiver with ENA AF_XDP integration |
| [af_xdp_zero_copy_perf_benchmark](#af_xdp-zero-copy-performance-benchmark) | AF_XDP + eBPF XDP filters | C++ | Sub-microsecond forwarding | Market data fan-out / packet replicator |
| [mcast2ucast](#mcast2ucast) | DPDK poll-mode driver | C | ~25 us RTT (metal) | Transparent multicast-over-unicast for AWS VPC |
| [phc_probe](#phc_probe) | SO_TIMESTAMPING + PHC | Python | — | HW vs SW timestamp diagnostic with live graph |

## Architecture Overview

```
Exchange / Market Data Source
         |
         | UDP multicast (or GRE-encapsulated on AWS VPC)
         v
+--------------------------------------------------+
|              Ingress / Feeder Host                |
|                                                   |
|  af_xdp_zero_copy  -- eBPF XDP at NIC driver     |
|  open_onload       -- OpenOnload kernel bypass    |
|  mcast2ucast       -- DPDK ENA PMD bypass         |
|  ec2_timestamping  -- kernel sockets (baseline)   |
+--------------------------------------------------+
         |
         | Unicast UDP fan-out to subscribers
         v
   Trading Strategy Instances
```

All projects share common design principles for latency-critical paths:
- CPU pinning with `SCHED_FIFO` real-time scheduling
- Lock-free ring buffers with atomic operations
- Cache-line aligned (64-byte) hot-path structures
- `-O3 -march=native -flto` compiler optimization
- Hardware timestamps via ENA PTP Hardware Clock
- Busy-poll receive loops (no `epoll`/`select` overhead)

---

## ec2 Timestamping Programs

**Path:** `ec2_timestamping_programs/`

A client/server toolkit for measuring one-way delay and round-trip time using three timestamp layers: Nitro hardware RX (PHC), kernel TX/RX, and application-level. Captures up to 10 timestamps per packet (T1-T10) for full per-hop latency decomposition.

### Key Features
- **Two modes:** `--one-way` (5 timestamps, 3 deltas) and `--round-trip` (10 timestamps, 7 deltas, 3 RTTs)
- **Batched TX** via `sendmmsg()` (128 packets/call), per-packet `recvmsg()` for RX timestamp precision
- **TSC calibration** with `rdtsc`/`rdtscp` for sub-microsecond busy-wait rate control
- **CSV join utility** for post-processing client + server logs by sequence number
- **Best-effort zero-copy TX** via `SO_ZEROCOPY` / `MSG_ZEROCOPY` (falls back gracefully if unsupported)

### Quick Start
```bash
cd ec2_timestamping_programs && make

# Server (one-way mode)
./timestamp_server --one-way --rx-interface eth0 --port 5000 --rx-cpu 4 \
  --stats=1M,bw=5,bn=100 --output-csv=svr.csv --time 60

# Client
./timestamp_client --one-way --dst-ip 10.0.0.2 --dst-port 5000 \
  --pps 10000 --pkt-size 64 --tx-interface eth0 --tx-cpu 4 \
  --output-csv=clt.csv --time 60

# Join CSVs for analysis
./timestamp_csvjoin --one-way --client-server \
  --clt-src-ip 10.0.0.1 --clt-src-port 45678 \
  --input-files clt.csv,clt_tx.csv,svr.csv --output-csv joined.csv
```

### Dependencies
- GCC, glibc, libpthread, librt
- EC2 instance with Nitro hardware timestamping + ENA driver
- PHC configured via chrony (`/dev/ptp*`)

---

## open_onload

**Path:** `open_onload/`

A one-way latency measurement receiver (`feed_relay_receiver`) that uses Xilinx OpenOnload kernel bypass with AF_XDP zero-copy on ENA NICs. Measures the delta between sender's application TX timestamp (embedded in payload) and receiver's hardware RX timestamp from the ENA PHC.

### Key Features
- **Hardware RX timestamps** from ENA PHC (`scm_timestamping64.ts[2]`) with automatic kernel fallback
- **Non-blocking hot loop** with `MSG_DONTWAIT` + `SO_BUSY_POLL` on dedicated CPU core
- **Lock-free stats + CSV ring buffers** with background writer thread on separate core
- **TSC-based timing** (`rdtsc`/`rdtscp`) for sub-nanosecond loop iteration and duration checks
- **Percentile statistics** (P25/P50/P75/P90/P95) with configurable histogram binning
- **8 property-based tests** covering packet parsing, latency computation, ring buffers, histograms, and CLI fuzz
- **Automated setup scripts** for patched ENA driver + OpenOnload installation on Nitro V5+ instances

### Quick Start
```bash
cd open_onload && make

# One-time setup (patches ENA + OpenOnload for AF_XDP)
sudo bash scripts/setup_onload_ena.sh enp40s0
sudo bash scripts/setup_ptp.sh enp40s0

# Run receiver under OpenOnload
EF_USE_HUGE_PAGES=0 EF_RX_TIMESTAMPING=1 EF_AF_XDP_ZEROCOPY=1 \
onload ./feed_relay_receiver \
  --rx-interface eth0 --port 12345 --rx-cpu 5 --time 60 \
  --stats=1M,bw=5,bn=100 --output-csv=latency.csv --log-cpu 0

# Verify AF_XDP is active
sudo ethtool -n enp40s0              # check n-tuple filter
sudo ./scripts/xdp_monitor.sh enp40s0 2   # watch rx_xdp_redirect

# Run tests
make test
```

### Architecture
- **Main thread** (pinned to `--rx-cpu`): non-blocking `recvmsg()` hot loop extracting HW timestamps from `SO_TIMESTAMPING` ancillary data
- **CSV writer thread** (pinned to `--log-cpu`): drains lock-free ring buffer, writes batches to disk
- **SIGALRM PPS reporter**: periodic stats display without impacting hot path
- **Sender**: any host, any language — just embed `(seq_num, tx_app_ns)` as first 12 bytes of UDP payload

### Dependencies
- GCC (GNU C99), glibc, libpthread, librt
- Patched ENA driver (`amzn/amzn-drivers` + `ena-onload-patches.tar`)
- Patched OpenOnload (Xilinx-CNS/onload at commit `2d4ec08aa`)
- EC2 Nitro V5+ instance (i7ie.*, m8i.*, r8i.*) with 2 ENIs

### Known Limitation
PTP hardware timestamping does not work with OpenOnload in `EF_AF_XDP_ZEROCOPY=1` mode. The receiver automatically falls back to kernel software timestamps with a warning.

---

## af_xdp Zero-Copy Performance Benchmark

**Path:** `af_xdp_zero_copy_perf_benchmark/`

A high-performance UDP packet replicator (fan-out engine) using AF_XDP kernel bypass with eBPF XDP filters. Intercepts market data at the NIC driver level and fans out unicast copies to registered subscribers with sub-microsecond forwarding latency.

### Key Features
- **Three ingress modes:** unicast, native multicast, GRE tunnel (for AWS VPC without multicast)
- **eBPF XDP filters** compiled to BPF bytecode for wire-speed packet classification
- **Multi-queue processing** (up to 8 NIC queues, one thread per queue)
- **Lock-free thread-local destination cache** refreshed every 100 ms
- **Self-healing ARP:** falls back to kernel socket, auto-promotes to AF_XDP once resolved (~100 ms)
- **Dynamic subscriber management** via UDP control protocol on port 12345
- **Optional GRE control proxy** for upstream producer notification
- **Built-in RTT benchmark client** with percentile histograms (P50/P90/P95/P99/P99.9)

### Quick Start
```bash
cd af_xdp_zero_copy_perf_benchmark && make all

# Start replicator (unicast mode)
sudo ./packet_replicator eth0 10.0.1.20 5000

# Add a subscriber
./control_client 10.0.1.20 add 10.0.1.30 9001

# Run latency benchmark
./market_data_provider_client 10.0.1.20 5000 10.0.1.30 9001 1000000 10000

# GRE tunnel mode (AWS VPC)
sudo ./packet_replicator eth0 224.0.31.50 5000 true --gre \
  --ctrl 224.0.31.51:5001 --producer 10.0.1.10:6000

# Cleanup XDP state
sudo ./cleanup.sh
```

### Dependencies
- g++ (C++17), clang (BPF target)
- libxdp (xdp-tools), libbpf, libelf, libpthread
- Linux kernel 5.10+ with AF_XDP zero-copy support
- AF_XDP-compatible NIC driver (ENA, i40e, ixgbe, ice, mlx5)

---

## mcast2ucast

**Path:** `mcast2ucast/`

A DPDK-based userspace daemon that provides transparent IP multicast service over AWS VPC's unicast-only fabric. Applications send/receive multicast normally through a TAP interface; the daemon intercepts, rewrites to unicast with a tunnel header, fans out to subscribers, and restores multicast on the receiving end. No application code changes required.

### Key Features
- **~25 us P50 RTT** on metal instances (cluster placement group), ~34 us on virtual instances
- **Zero-copy fan-out** using DPDK `rte_pktmbuf_clone()` / refcount-based mbuf sharing
- **Two-plane architecture:** poll-mode data plane (lcore 1) + IGMP/control plane (lcore 2)
- **Lock-free group table** using DPDK `rte_hash` with copy-on-write updates
- **Full IGMP snooping** (v1/v2/v3) for dynamic subscriber discovery
- **Custom 8-byte tunnel protocol** (`M2U\x01` magic + group IP + port)
- **Embedded web dashboard** on port 8080 with live stats JSON API
- **Comprehensive OS tuning script** (`tune_os.sh`): CPU isolation, C-state off, IRQ affinity, NIC offload tuning

### Quick Start
```bash
cd mcast2ucast

# Automated provisioning (installs DPDK 25.11, builds everything)
sudo ./bootstrap.sh

# Manual build
PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig meson setup build
meson compile -C build

# Setup ENA kernel bypass
sudo modprobe uio
sudo insmod ~/dpdk-kmods/linux/igb_uio/igb_uio.ko wc_activate=1
sudo dpdk-devbind.py -b igb_uio 0000:28:00.0
sudo sysctl -w vm.nr_hugepages=1024

# Run (ENA bypass mode)
echo "subscriber 224.0.0.101 <sub-ip> 5001 <sub-mac>" > deploy.conf
sudo LD_LIBRARY_PATH=/usr/local/lib64 ./build/mcast2ucast \
  -l 0-2 --allow=0000:28:00.0,llq_policy=1 --log-level=user1,7 \
  -- --rx-port 0 --tx-port 0 --tap mcast0 --config deploy.conf

# Post-start TAP config
sudo ip addr add 10.0.1.10/24 dev mcast0
sudo ip route add 224.0.0.0/4 dev mcast0
```

### Benchmarks (64-byte payload, 1 publisher -> 2 subscribers)

| Instance | Mode | P50 | P99 |
|----------|------|-----|-----|
| m8a.metal-24xl (cluster PG) | mcast2ucast e2e | 25.6 us | 28.2 us |
| m8a.metal-24xl (cluster PG) | Raw kernel UDP | 20.7 us | 24.1 us |
| m8a.2xlarge (same AZ) | mcast2ucast e2e | 33.7 us | 36.4 us |
| m8a.2xlarge (same AZ) | TAP-only (no bypass) | 100.0 us | 105.2 us |

### Dependencies
- DPDK 25.11 (ENA PMD, TAP PMD, rte_hash, rte_ring, rte_mempool)
- igb_uio kernel module (from dpdk-kmods)
- Meson + Ninja build system
- GCC (C11), g++ (C++17 for benchmarks)
- Linux hugepages (2 GB recommended)

---

## phc_probe

**Path:** `utilities/phc_probe/`

A two-host diagnostic tool that compares hardware RX timestamps from the ENA PTP Hardware Clock (`ts[2]`) with kernel software RX timestamps (`ts[0]`) for UDP packets. Validates that NIC hardware timestamping is working correctly and measures the HW-SW clock delta with a live rolling ASCII graph.

### Key Features
- **Two timestamp sources:** `ts[2]` (NIC PTP Hardware Clock) vs `ts[0]` (kernel `CLOCK_REALTIME`) from `SO_TIMESTAMPING` ancillary data
- **Live ASCII graph mode** (`--live`) showing HW-SW delta over a rolling 60-second window
- **Fixed-count mode** for quick validation with per-packet breakdown and summary statistics
- **Automatic fallback** from `SO_TIMESTAMPING_NEW` (option 65) to legacy `SO_TIMESTAMPING` (option 37)
- **Zero dependencies** beyond Python 3.9+ (receiver) and Python 3.6+ (sender)

### Quick Start
```bash
# On the receiver (EC2 with ENA PHC):
sudo python3 utilities/phc_probe/ts_receiver.py ens5 --port 9999 --count 20

# On the sender (any host):
python3 utilities/phc_probe/ts_sender.py 10.0.1.100 --port 9999 --count 20

# Live graph mode:
sudo python3 utilities/phc_probe/ts_receiver.py ens5 --port 9999 --live
```

### Dependencies
- Python 3.9+ (receiver), Python 3.6+ (sender)
- EC2 instance with ENA PHC enabled (`phc_enable=1`)
- Root access on the receiver for `SIOCSHWTSTAMP` ioctl

---

## Platform Requirements

All projects target **Amazon Linux 2023** on **EC2 instances with ENA NICs**. Recommended instance families:

| Capability | Instance Families |
|------------|-------------------|
| AF_XDP zero-copy + n-tuple steering | Nitro V5+: i7ie.*, m8i.*, r8i.* |
| DPDK ENA PMD | Any ENA-equipped instance |
| Hardware timestamping (PHC) | Any instance with latest ENA driver |
| Best latency (metal) | m8a.metal-24xl, i7ie.metal-* |

### Common OS Tuning

For reproducible low-latency benchmarks, apply these kernel parameters:
```
isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3
idle=poll processor.max_cstate=0 intel_idle.max_cstate=0
tsc=reliable clocksource=tsc
```

Pin NIC IRQs away from isolated cores and disable irqbalance, firewalld, and transparent hugepages. See `mcast2ucast/tune_os.sh` for a comprehensive automated script.

## License

All projects are licensed under **MIT No Attribution (MIT-0)**, copyright Amazon.com, Inc. or its affiliates.
