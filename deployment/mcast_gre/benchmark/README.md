# mcast_gre Latency Benchmark

End-to-end one-way latency measurement for the GRE-based market data distribution pipeline:

```
Exchange  ──GRE──►  Feeder (AF_XDP)  ──GRE unicast──►  Subscriber
sender.cpp                                           receiver.cpp
```

Each packet carries a 24-byte header: `seq(8) + tx_ns(8) + feeder_ns(8)`.
The exchange stamps `tx_ns` at send time.  The feeder (`packet_replicator`) overwrites
`feeder_ns` in transit using `CLOCK_REALTIME` before forwarding.  The receiver stamps
`rx_ns` on dequeue and reports three latency sections: Hop 1 (exchange→feeder),
Hop 2 (feeder→subscriber), and Total.  All three clocks must be synchronised — run
`ptp.sh` on every node and activate `refclock PHC /dev/ptp0` for ±200–500 ns accuracy.

---

## Build

```bash
make all
# produces: sender  receiver  subscriber_filter.o
```

> **Note:** `subscriber_filter.o` is an eBPF program and must be compiled with
> `clang` (not gcc).  The Makefile hardcodes `clang` for the BPF rule; if your
> environment overrides `CC=gcc`, the BPF target is unaffected.

---

## Tools

### `sender`

Builds and injects GRE packets directly at Layer 2 via AF_XDP (zero-copy TX),
bypassing the kernel GRE tunnel (`gre_feed`) and the kernel IP stack entirely.
The full `Eth | outer IPv4 | GRE | inner IPv4 | UDP | payload` frame is constructed
in userspace; `tx_ns` is stamped into the UMEM frame just before the TX ring submission.
`feeder_ns` is zeroed by the sender; the feeder overwrites it in transit.

```
Usage: sudo ./sender [options]
  -I <iface>        real NIC (e.g. eth0, NOT gre_feed)   (default: eth0)
  -D <feeder-ip>    outer GRE destination — feeder private IP  (REQUIRED)
  -g <group>        inner multicast group                 (default: 224.0.31.50)
  -p <port>         inner UDP destination port            (default: 5000)
  -c <count>        number of packets                     (default: 10000)
  -i <interval_us>  inter-packet gap µs                   (default: 1000 = 1 ms)
                    < 1000: busy-wait spin (warm cache, best floor)
                    ≥ 1000: nanosleep (realistic, poll-wakeup overhead visible)
  -s <size>         payload bytes                         (default: 64, min: 24)
```

**Requires root** (`CAP_NET_ADMIN` for XDP attach, `CAP_NET_RAW` for AF_XDP).

### `receiver`

Attaches `subscriber_filter.o` (XDP program) to the NIC, opens an AF_XDP socket,
and polls the RX ring directly — no `recv()` syscall, no kernel IP stack after the
XDP redirect.  Parses `Eth | outer IPv4 | GRE | inner IPv4 | UDP | payload` and
measures one-way latency from the embedded timestamp.

```
Usage: sudo ./receiver [options]
  -i <iface>   network interface (e.g. eth0)           (REQUIRED)
  -B <path>    path to subscriber_filter.o             (default: ./subscriber_filter.o)
  -p <port>    inner UDP dst port to match             (default: 5000)
  -c <count>   packets to receive                      (default: 10000)
  -t <timeout> seconds before giving up                (default: 60)
  -q <queue>   XDP/AF_XDP RX queue index               (default: 0)
  -r           also print raw latencies (ns)
```

**Requires root** (`CAP_NET_ADMIN` for XDP attach, `CAP_NET_RAW` for AF_XDP).

---

## Running a test

### Prerequisites

- `packet_replicator` running on feeder (`systemctl status packet-replicator`)
- Subscriber registered with feeder (`control_client <feeder-ip> list` shows subscriber IP)
- Clocks synchronised — run `ptp.sh` on **all three nodes** (exchange, feeder, subscriber).
  With `refclock PHC /dev/ptp0` active (ENA `phc_enable=1`), expect ±200–500 ns inter-node offset.
  With NTP-only fallback, expect ±1–5 µs — still acceptable for P50 but Hop 2 may show
  negative samples when feeder clock leads subscriber by more than the transit time.
- Know the feeder's **private IP** (from CloudFormation outputs or `stack-outputs.json`)
- **ENA NIC prepared on subscriber** — `configure.yaml` handles this, but verify manually if needed:
  ```bash
  # Must be 1 combined queue (ensures GRE RSS-hashes to queue 0 where AF_XDP socket lives)
  ethtool -l $IFACE | grep -A5 'Current hardware'
  # Combined: 1

  # Must be MTU 3498 (ENA native XDP page-size limit; higher MTU forces xdpgeneric/SKB mode)
  ip link show $IFACE | grep mtu
  # mtu 3498

  # Interrupt coalescing must be 0 (default rx-usecs=20 tx-usecs=64 adds ~20–84 µs)
  ethtool -c $IFACE | grep usecs
  # rx-usecs: 0   tx-usecs: 0
  ```


### Step 1 — Start receiver on each subscriber (run BEFORE sender)

```bash
ssh ec2-user@<subscriber-public-ip>
cd ~/gre-benchmark/deployment/mcast_gre/benchmark
sudo ./receiver -i $IFACE -B ./subscriber_filter.o -p 5000 -c 10000
```

The receiver blocks and prints a rolling average every 100 packets.

### Step 2 — Send traffic from exchange

```bash
ssh ec2-user@<exchange-public-ip>
cd ~/gre-benchmark/deployment/mcast_gre/benchmark
sudo ./sender -I $IFACE -D <feeder-private-ip> -g 224.0.31.50 -p 5000 -c 10000 -i 1000
```

### Step 3 — Read the report

The receiver prints a three-section report when it finishes.
Example from c7i.4xlarge, same-AZ CPG, `-i 1000 -c 10000`:

```
==================================================
  GRE Latency Report (AF_XDP)
==================================================
  Interface:     eth0  queue 0
  Received:      10000/10000 packets (10.1s)
  Lost:          0 packets
  Out of order:  0

  Hop 1 — Exchange → Feeder (usec):
    Min:     13.2   Avg:     14.1   Max:     28.4
    P0        13.2
    P50       14.0
    P90       15.3
    P99       18.7

  Hop 2 — Feeder → Subscriber (usec):
    Min:     16.8   Avg:     17.2   Max:     31.0
    P0        16.8
    P50       17.1
    P90       18.6
    P99       22.4

  Total — Exchange → Subscriber (usec):
    Min:     30.0   Avg:     31.3   Max:     59.4
    P0        30.0
    P50       31.3
    P90       33.3
    P95       35.1
    P99       39.1
==================================================
```

Hop 2 is consistently ~3–5 µs higher than Hop 1.  This is expected: `feeder_ns` is
stamped before the feeder's TX path (descriptor build + NIC kick ~1–2 µs) and the
subscriber always pays a `poll()` wakeup cost (~1–3 µs).  The delta is real overhead
in the subscriber's latency budget, not a measurement artefact.

---

## Test cases and expected results

Three standard test cases on c7i.4xlarge, same-AZ CPG with PHC sync active:

| Case | Flags | Rate | P50 Total | Notes |
|------|-------|------|-----------|-------|
| Burst / hardware floor | `-i 100 -c 50000` | 10 kpps | ~31 µs | `busy_wait_ns` spin; warm cache; minimal poll overhead |
| Moderate / baseline | `-i 1000 -c 10000` | 1 kpps | ~34 µs | `nanosleep`; representative workload; stable percentiles |
| Sparse / cold | `-i 10000 -c 1000` | 100 pps | ~43 µs | `nanosleep`; poll wakeup adds ~8–12 µs; wide tail |

The gap between Sparse and Moderate is entirely software overhead (`poll()` wakeup +
cold cache), not network.

## Tuning tips

| Goal | Change |
|------|--------|
| Hardware floor measurement | `-i 100 -c 50000` (busy_wait, warm cache) |
| Realistic sparse feed | `-i 10000 -c 1000` |
| Larger payload | `-s 256` (matches typical market data frame) |
| Raw ns values for histogram | add `-r` to receiver |
| Multiple subscribers | run `receiver` on each before starting sender |

> **Note:** ENA does not support RSS flow steering by protocol
> (`ethtool -N flow-type ip4 proto 47 action <n>` returns an error on ENA).
> Use `ethtool -L combined 1` instead to collapse all queues to queue 0.

## Latency expectations

| Setup | P50 Total | Hop 1 | Hop 2 | Floor |
|-------|-----------|-------|-------|-------|
| c7i.4xlarge, same AZ, CPG, `-i 100` | ~30–33 µs | ~14 µs | ~17 µs | ~30 µs |
| c7i.4xlarge, same AZ, CPG, `-i 1000` | ~32–35 µs | ~14 µs | ~19 µs | ~30 µs |

Hop 2 is structurally ~3–5 µs higher than Hop 1: `feeder_ns` is stamped before the feeder's TX
path, and the subscriber always pays a `poll()` wakeup cost.

## Interpreting results

- **No packets received / receiver unresponsive** — almost always an RSS mismatch: GRE is hashing to a queue other than the one the AF_XDP socket is on.  Verify `ethtool -l $IFACE` shows `Combined: 1`.  Also check `ethtool -S $IFACE | grep xdp` — `queue_N_rx_zc_queue_pkt_copy` incrementing means packets arrive but copy-mode fallback is active (fill ring starved or needs_wakeup logic).
- **Lost packets** — feeder dropped frames (check `ethtool -S $IFACE | grep xdp` on feeder) or subscriber fill ring depleted
- **High P99 vs P50** — CPU jitter; apply `tune_os.yaml` + `tune_feeder.yaml` tuning
- **Hop 2 shows `[!] N/M samples negative`** — feeder clock leads subscriber by more than the Hop 2 transit time.  Run `chronyc sources -v` on all nodes and confirm `PHC0` is the selected source (marked `*`).  If `phc_enable=0`, reboot the instance and re-run `ptp.sh`.
- **Out of order** — feeder RSS spreading ingress GRE across threads; does not affect P50/P99 with sufficient packet count
- **XDP attached as `xdpgeneric` instead of `xdp`** — MTU exceeds 3498; ENA falls back to SKB mode (not kernel bypass).  Set `ip link set $IFACE mtu 3498`
