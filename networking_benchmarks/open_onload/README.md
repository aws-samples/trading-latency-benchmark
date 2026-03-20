# Feed Relay Receiver (OpenOnload)

## Overview

The Feed Relay Receiver (`feed_relay_receiver`) measures one-way network latency from a Feed Relay Server to an EC2 receiver. It listens for UDP packets containing embedded TX application timestamps, extracts hardware RX timestamps via the ENA PTP Hardware Clock, and computes one-way latency (HW_RX - TX_App). It reports configurable percentile statistics, logs per-packet CSV data, and displays real-time PPS rates.

The receiver is designed to run under OpenOnload kernel bypass using only standard POSIX/Linux socket APIs. OpenOnload is required only on the receiver side — the sender (Feed Relay Server) does not require OpenOnload and can use any standard networking stack.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Build Instructions](#build-instructions)
- [Sender Usage](#sender-usage)
- [Receiver Usage](#receiver-usage)
- [Example End-to-End Latency Measurement](#example-end-to-end-latency-measurement)
- [Output](#output)
- [Validating AF_XDP and Troubleshooting](#validating-af_xdp-and-troubleshooting)
- [License](#license)

## Architecture

```
                        SENDER                              RECEIVER
                  (any host/stack)                  (EC2 + ENA + OpenOnload)
               ┌─────────────────────┐          ┌──────────────────────────────┐
               │  Feed Relay Server  │          │    feed_relay_receiver       │
               │                     │          │                              │
               │  Embeds in payload: │   UDP    │  1. recvmsg() via OpenOnload │
               │  ┌───────────────┐  │ ──────>  │  2. ENA NIC HW RX Timestamp │
               │  │ 4B seq_num    │  │  network │  3. Extract payload TX_App   │
               │  │ 8B TX_App_ts  │  │          │  4. Latency = HW_RX - TX_App│
               │  └───────────────┘  │          │  5. Stats / CSV / PPS output │
               │                     │          │                              │
               │  No OpenOnload      │          │  OpenOnload + ENA PHC        │
               │  No special drivers │          │  HW RX timestamping          │
               └─────────────────────┘          └──────────────────────────────┘
```

Key points:
- HW RX timestamping occurs only on the receiver side via the ENA PTP Hardware Clock
- The sender embeds an application-layer TX timestamp in the packet payload using `clock_gettime(CLOCK_REALTIME)` — no hardware TX timestamping is needed
- One-way latency is computed as `HW_RX_Timestamp - TX_App_Timestamp` in microseconds
- If HW RX timestamps are unavailable (e.g., loopback/same-host), the receiver falls back to kernel RX timestamps automatically

## Project Structure

```
open_onload/
├── LICENSE                    # MIT-0 license file
├── README.md                  # This file
├── Makefile                   # Build configuration
├── build/                     # Build output directory
├── include/                   # Header files
│   ├── timestamp_common.h     # Core data structures and function declarations
│   └── timestamp_logging.h    # Logging system definitions
├── scripts/                   # Utility scripts
│   ├── setup_onload_ena.sh    # Automated OpenOnload + ENA setup for AL2023
│   ├── setup_ptp.sh           # PTP hardware timestamping setup and validation
│   └── xdp_monitor.sh        # AF_XDP stats monitoring with change highlighting
├── src/                       # Source code
│   ├── feed_relay_receiver.c  # Feed relay receiver implementation
│   ├── timestamp_common.c     # Shared infrastructure (sockets, stats, CSV, scheduling)
│   └── timestamp_logging.c    # Logging system implementation
└── tests/                     # Property-based tests
    ├── test_property1_packet_parsing_roundtrip.c
    ├── test_property2_oneway_latency.c
    ├── test_property3_power_of_2.c
    ├── test_property4_ring_buffer_order.c
    ├── test_property5_percentile_computation.c
    ├── test_property6_histogram_bins.c
    ├── test_property7_csv_timestamp_roundtrip.c
    └── test_property8_argument_parsing.c
```

## Prerequisites

**Receiver host (EC2 instance):**
- x86 EC2 instance with n-tuple flow steering support (e.g. i7ie.*, m8i.*, r8i.* — Nitro V5+)
- Amazon Linux 2023 with 6.1 kernel
- 2 ENIs: one for SSH, one for OpenOnload (in different subnets)
- ENA driver with AF_XDP patches applied
- OpenOnload installed and configured for AF_XDP

**Important: OpenOnload requires patched ENA driver and patched Onload.** The standard upstream ENA driver and Onload do not support AF_XDP zero-copy together. The patches applied by `scripts/setup_onload_ena.sh` enable this integration.

**Important: PTP hardware timestamping does not currently work with OpenOnload AF_XDP zero-copy mode.** When running under `onload` with `EF_AF_XDP_ZEROCOPY=1`, the receiver will not receive hardware RX timestamps (`ts[2]` will be zero). The receiver automatically falls back to kernel software timestamps (`ts[0]`) in this case and logs a one-time warning. To get hardware RX timestamps, run the receiver without OpenOnload (standard kernel path). This is a known limitation of the current OpenOnload AF_XDP implementation with ENA.

**Automated setup scripts:**

1. OpenOnload + ENA setup (patches, builds, and configures everything):
```bash
sudo bash scripts/setup_onload_ena.sh <onload-interface>
# Example: sudo bash scripts/setup_onload_ena.sh enp40s0
# Resume from a specific step: sudo bash scripts/setup_onload_ena.sh enp40s0 --from 7
```

2. PTP hardware timestamping setup and validation (for non-Onload use or verification):
```bash
sudo bash scripts/setup_ptp.sh [interface]
# Example: sudo bash scripts/setup_ptp.sh enp40s0
# Auto-detect interface: sudo bash scripts/setup_ptp.sh
```
This script verifies ENA PHC is enabled (`phc_enable=1`), checks `/dev/ptpN` exists, enables HW RX timestamping via `hwstamp_ctl`, and validates the configuration through sysfs and `ethtool -T`. Use this when running the receiver without OpenOnload to confirm hardware timestamps are available.

**Sender host:**
- No special requirements — any Linux host with standard UDP networking
- No OpenOnload, no ENA driver, no PHC needed
- Only needs `clock_gettime(CLOCK_REALTIME)` for embedding TX timestamps

**Build host:**
- GCC compiler with GNU C99 support
- Standard C library with GNU extensions (`glibc`)
- POSIX threads library (`libpthread`)
- Real-time library (`librt`)

## Build Instructions

```bash
cd ptp_solutions/open_onload

# Build the feed_relay_receiver binary
make clean all

# Run property tests
make test

# Debug build (enables --log-level DEBUG)
make clean debug
```

## Sender Usage

The Feed Relay Server sends UDP packets with the following payload format:

```
Offset  Size    Field               Encoding
──────  ──────  ──────────────────  ─────────────────────────────────────
0       4       seq_num             uint32_t, network byte order (htonl)
4       8       tx_app_timestamp    uint64_t, network byte order (htobe64), nanoseconds
12+     N       (optional payload)  ignored by receiver
```

Minimum valid packet size: 12 bytes.

The sender does NOT require OpenOnload. Any program that sends UDP packets with this format will work. Example sender snippet in C:

```c
#include <arpa/inet.h>
#include <endian.h>
#include <time.h>
#include <string.h>

/* Build a feed relay packet */
void build_feed_relay_packet(char *buf, uint32_t seq_num) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t tx_app_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    uint32_t net_seq = htonl(seq_num);
    uint64_t net_ts  = htobe64(tx_app_ns);

    memcpy(buf,     &net_seq, 4);
    memcpy(buf + 4, &net_ts,  8);
}

/* Send: sendto(sockfd, buf, 12, 0, ...) */
```

## Receiver Usage

Launch the receiver under OpenOnload with the required environment variables:

```
EF_USE_HUGE_PAGES=0 EF_RX_TIMESTAMPING=1 EF_AF_XDP_ZEROCOPY=1 \
onload ./feed_relay_receiver --rx-interface <interface> [OPTIONS]
```

**Command-line flags:**

| Flag | Required | Default | Description |
|------|----------|---------|-------------|
| `--rx-interface <iface>` | Yes | — | Network interface name for receiving packets |
| `--port <port>` | No | 12345 | UDP port to listen on |
| `--rx-cpu <cpu>` | No | 5 | CPU core for receive processing |
| `--time <seconds>` | No | unlimited | Run duration in seconds |
| `--stats[=config]` | No | disabled | Enable statistics collection. Format: `[buffer_size],bw=[bin_width_us],bn=[max_bins]`. Defaults: 5M,bw=10,bn=1000 |
| `--output-csv[=filename]` | No | disabled | Enable CSV logging to file |
| `--log-cpu <cpu>` | No | 0 | CPU core for CSV logger thread (requires `--output-csv`) |
| `--log-level <level>` | No | INFO | Set logging level (DEBUG\|INFO\|WARN\|ERROR) |
| `--log-component <comp>` | No | ALL | Enable specific log components (comma-separated: MAIN\|STATS\|CSV\|NETWORK\|SIGNAL) |
| `--help` | No | — | Show usage message |

## Example End-to-End Latency Measurement

**Step 1: Start the receiver** on the EC2 instance with OpenOnload:

```bash
EF_USE_HUGE_PAGES=0 EF_RX_TIMESTAMPING=1 EF_AF_XDP_ZEROCOPY=1 \
onload ./feed_relay_receiver \
  --rx-interface eth0 \
  --port 12345 \
  --rx-cpu 5 \
  --time 60 \
  --stats=1M,bw=5,bn=100 \
  --output-csv=feed_relay_latency.csv \
  --log-cpu 0
```

**Step 2: Start the sender** on any host (no OpenOnload needed):

The sender is your own application that sends UDP packets with the 12-byte feed relay format. For example, using Python:

```bash
python3 -c "
import socket, struct, time
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for seq in range(100000):
    tx_ns = int(time.clock_gettime(time.CLOCK_REALTIME) * 1e9)
    sock.sendto(struct.pack('!IQ', seq, tx_ns), ('10.0.1.100', 12345))
    time.sleep(0.0001)  # ~10k pps
sock.close()
"
```

The receiver will display real-time PPS rates during the run and print percentile statistics upon completion.

## Output

**Percentile statistics** (displayed when `--stats` is enabled):

```
[INFO] TX_App->HW_RX:
[INFO] Packets Evaluated: 9773
[INFO] Percentiles (us): P25=70.386, P50=70.720, P75=71.147, P90=71.659, P95=72.112
[INFO] Histogram (bin_width=5us):
[INFO]   65-70us: 500
[INFO]   70-75us: 9132
[INFO]   75-80us: 29
[INFO]   80-85us: 12
```

**CSV file format** (generated when `--output-csv` is enabled):

Header:
```
seq_num,src_ip,src_port,hw_rx_ts,ker_rx_ts,app_rx_ts,tx_app_ts,one_way_latency_us
```

Example rows:
```
1,10.0.1.50,54321,1234567891.123456789,1234567891.123457000,1234567891.123458000,1234567891.123440000,16.789
2,10.0.1.50,54321,1234567891.223456789,1234567891.223457000,1234567891.223458000,1234567891.223440000,16.789
```

All timestamps are in `seconds.nanoseconds` format. The `one_way_latency_us` column is the computed latency in microseconds: `(hw_rx_ts - tx_app_ts) / 1000.0`.

## Validating AF_XDP and Troubleshooting

When running the receiver under OpenOnload with `EF_AF_XDP_ZEROCOPY=1`, verify that AF_XDP is actually being used with the following checks. Run these commands in a separate terminal while the receiver is active.

### 1. Check n-tuple filter

Onload should create an n-tuple filter to steer matching traffic to a specific RX queue for AF_XDP processing:

```bash
sudo ethtool -n enp40s0
```

Expected output:

```
4 RX rings available
Total 1 rules

Filter: 1008
  Rule Type: UDP over IPv4
  Src IP addr: 0.0.0.0 mask: 255.255.255.255
  Dest IP addr: 172.31.3.178 mask: 0.0.0.0
  TOS: 0x0 mask: 0xff
  Src port: 0 mask: 0xffff
  Dest port: 12345 mask: 0x0
  Action: Direct to queue 0
```

Key things to verify:
- `Dest port: 12345` matches your `--port` value
- `Action: Direct to queue 0` means traffic is steered to a specific RX queue for AF_XDP
- If no filter is shown, Onload is not using AF_XDP — check your `EF_AF_XDP_ZEROCOPY` setting and Onload version

### 2. Monitor XDP queue stats

```bash
watch "sudo ethtool -S enp40s0 | egrep 'xsk|xdp'"
```

Expected output while traffic is flowing:

```
queue_0_rx_xdp_redirect: 3575214    <-- packets going through AF_XDP to Onload (should increase)
queue_0_rx_xdp_pass: 1556957        <-- non-matching packets passed to kernel (SSH, ARP, etc.)
queue_0_rx_xdp_aborted: 0           <-- should be 0
queue_0_rx_xdp_drop: 0              <-- should be 0
```

- `rx_xdp_redirect` incrementing confirms AF_XDP is active for your traffic
- `rx_xdp_pass` incrementing is normal — this is other traffic (SSH, DNS, NTP) going through the kernel stack
- `rx_xdp_aborted` or `rx_xdp_drop` incrementing indicates a problem

A helper script `scripts/xdp_monitor.sh` is included that highlights changed counters with deltas:

```bash
sudo ./scripts/xdp_monitor.sh enp40s0 2
```

### 3. Check for Onload fallback to kernel stack

In the receiver's startup output, you should NOT see:

```
oo:feed_relay_recei[...]: citp_udp_socket: failed (errno:22 strerror:Invalid argument) - PASSING TO OS
```

If you see "PASSING TO OS", Onload failed to accelerate the socket and fell back to the kernel networking stack. Common causes:
- `EF_AF_XDP_ZEROCOPY=1` not set
- Onload version doesn't support AF_XDP on your kernel
- Interface doesn't support XDP (check `ethtool -i enp40s0` for ENA driver)

### 4. Expected kernel log messages on shutdown

When the receiver exits, you may see messages like these in `dmesg`:

```
[sfc efrm] __efrm_vi_resource_issue_flush: tx queue 0 flush requested for nic 0 rc -95
[sfc efhw] af_xdp_flush_rx_dma_channel: FIXME AF_XDP
```

These are harmless. The `rc -95` (`EOPNOTSUPP`) means Onload's AF_XDP backend doesn't implement hardware DMA queue flushing (ENA doesn't expose that interface). The VI resources still get freed properly. These only appear during teardown and do not affect the data path.

### 5. Debug logging

For verbose output, build with the debug target and use `--log-level DEBUG`:

```bash
make clean debug

EF_USE_HUGE_PAGES=0 EF_RX_TIMESTAMPING=1 EF_AF_XDP_ZEROCOPY=1 \
onload ./feed_relay_receiver \
  --rx-interface enp40s0 --port 12345 --rx-cpu 2 --time 10 \
  --log-level DEBUG
```

This shows detailed socket setup, timestamping configuration, buffer allocation, and per-subsystem initialization messages.

## License

See the [LICENSE](LICENSE) file in this repo.
