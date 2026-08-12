# `tools/` — measurement instruments & control CLI

The instruments that drive and measure the replicator. Two measurement
topologies are supported, each with its own timing methodology:

- **Unicast RTT** (`rtt`) — a **round-trip** probe on a single host's clock →
  no clock sync needed. Sends UDP to the replicator, which echoes it back.
- **Multicast one-way** (`mcast_send` → replicator → `mcast_receive`) — a
  **one-way** fan-out path; timing travels in the payload and both ends read a
  PHC/NTP-disciplined `CLOCK_REALTIME`.

Plus two utilities: `replicator_ctl` (control-protocol CLI) and `udp_send`
(connectivity probe).

---

## Measurement paths (with the tech used at each step)

### A. Unicast RTT (`rtt`) — single-clock round trip

```
  ┌──────────────────────────── rtt CLIENT (one host) ────────────────────────────┐
  │                                                                                │
  │ [R1] register self as a destination        control protocol (UDP :12345,       │
  │      over the control channel               AFXDP_CONTROL_PORT); replicator     │
  │                                              echoes back to this client         │
  │ [R2] pace next send                         clock_nanosleep(TIMER_ABSTIME) —     │
  │                                              jitter-free absolute cadence        │
  │ [R3] stamp TX + transmit  ───────────┐      TX time = clock_gettime(CLOCK_       │
  │      seq → lock-free slot array       │     REALTIME) taken right before send    │
  │      default: kernel sendto()         │     SCHED_FIFO(80) + mlockall + CPU pin  │
  │      --xdp-tx: TX-only AF_XDP frame    │     rtt.cpp (inlined): frame pre-built    │
  │                (build once, patch 10   │     every UMEM frame at startup; per pkt │
  │                 seq digits, submit)    │     writes 10 digits + TX stamp, submits │
  └────────────────────────────────────────┼───────────────────────────────────────┘
                                            ▼ wire
                           ┌──────────── REPLICATOR ────────────┐
                           │ ucast.o (XDP) → AF_XDP → fan-out    │  (see src/Replicator/README.md
                           │ echoes the frame back to the client │   steps [1]–[11])
                           └──────────────────┬─────────────────┘
                                            ▼ wire
  ┌──────────────────────────── rtt CLIENT ───────────────────────────────────────┐
  │ [R4] receive echo + stamp RX               default RX = SO_TIMESTAMPING +        │
  │      seq → slot → retrieve TX stamp          SOF_TIMESTAMPING_RX_SOFTWARE, stamped│
  │                                              in the NAPI netif_receive_skb path   │
  │                                              (CLOCK_REALTIME, pre socket-queue);   │
  │                                              --xdp-rx = bpf_ktime_get_ns stamped   │
  │                                              at the XDP ingress hook (CLOCK_       │
  │                                              MONOTONIC), read from the payload     │
  │ [R5] RTT = RX − TX  (same clock domain, single host → NO clock sync)             │
  │ [R6] aggregate: drop warmup; min/mean/p50/p90/p95/p99/p99.9/max → service_rtt_us │
  └────────────────────────────────────────────────────────────────────────────────┘
```

### B. Multicast one-way (`mcast_send` → replicator → `mcast_receive`)

```
  ┌───────────── SOURCE: mcast_send ─────────────┐
  │ [M1] build m2u frame in UMEM                  Eth/IPv4(17)/UDP/ m2u{magic="M2CU",group} /
  │      Eth|IP|UDP|m2u|payload{seq,ts_ns,0}      payload[0..23]={seq(8),ts_ns(8),replicator_ns(8=0)}
  │ [M2] pace (clock_nanosleep ABSTIME)           absolute-time cadence at -i interval_us
  │ [M3] stamp ts_ns + AF_XDP TX  ───────────┐    ts_ns=CLOCK_REALTIME just before submit;
  │      (unicast to replicator -D)           │   TX-only AF_XDP zero-copy (libxdp), ARP-resolved dst MAC
  └────────────────────────────────────────────┼──────────────────────────────────────────────┘
                                              ▼ wire (unicast)
                    ┌──────────────────── REPLICATOR (--mcast) ────────────────────┐
                    │ mcast.o (XDP): parse Eth/IP/UDP + m2u, match {group,port} in  │
                    │ config_map → AF_XDP redirect → fan-out. Stamps replicator_ns  │
                    │ (RX) and replicator_tx_ns (TX). See src/Replicator/README.md. │
                    └───────────────────────────────┬──────────────────────────────┘
                                              ▼ wire (unicast ×K)
  ┌───────────── DEST: mcast_receive ─────────────┐
  │ [M4] own mcast.o + config_map[0]={group,port}  seeds its own filter so the NIC redirects
  │      attaches XDP, seeds the group             the group to its AF_XDP socket (zero-copy)
  │ [M5] AF_XDP RX (busy-poll NAPI) + stamp rx_ns  rx_ns=CLOCK_REALTIME at XSK dequeue;
  │      read seq, ts_ns, replicator_ns, repl_tx   SO_BUSY_POLL; frames peeked from RX ring
  │ [M6] per-hop latency:                          hop1 = replicator_ns − ts_ns  (src→replicator)
  │      needs cross-host clock sync               hop2 = rx_ns − replicator_tx_ns (replicator→dest)
  │      (Nitro PHC /dev/ptp0 + chrony, ~µs)       total = rx_ns − ts_ns          (end-to-end)
  │ [M7] percentiles p50…max; counts negative hop2 (clock-skew guard) → JSON (-j) + report
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Step-by-step

- **[R1] rtt self-registration** — `rtt` sends `CTRL_ADD_DESTINATION` to the
  replicator's control port (`AFXDP_CONTROL_PORT`, default 12345) so the echo
  comes back to it. On exit it removes itself.
- **[R2]/[M2] pacing** — `clock_nanosleep(TIMER_ABSTIME)` fires each send at an
  absolute deadline (target rate), eliminating cumulative drift/jitter that a
  relative `sleep` would accrue.
- **[R3] rtt TX** — the send timestamp is `clock_gettime(CLOCK_REALTIME)` read
  *immediately* before the send. Two backends:
  - **kernel `sendto()`** (default): simple, but the full kernel TX stack
    (~3–5 µs) is inside the measured leg.
  - **`--xdp-tx[=queue]` + `--iface`** (inlined AF_XDP TX sender in `rtt.cpp`, full builds only): a
    **TX-only AF_XDP socket** opened with `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD`
    (no XDP program — egress is independent of RSS). The whole
    `Eth|IPv4|UDP|payload` frame is built **once into every UMEM frame** at
    startup (dst MAC via ARP); per packet it writes only the 10 ASCII sequence
    digits and stamps TX right before `submit`. Removes the kernel TX stack from
    the measured leg (measured A/B: p99.9 −14.5 µs ≈ 21%, max 488→82 µs). Binds a
    **dedicated queue (default 1)** because queue 0 is owned by the local
    `replicator.service`.
- **[R3] hot-path perf** — thread pinned to `send_cpu`, `SCHED_FIFO` prio 80,
  `mlockall` (no page faults mid-measurement). Each probe's send time is stored
  in a **lock-free, sequence-indexed slot array** (no map, no mutex).
- **[R4] rtt RX** — default RX stamp is kernel software `SO_TIMESTAMPING`
  (`SOF_TIMESTAMPING_RX_SOFTWARE`), recorded in the **NAPI `netif_receive_skb`
  path** (`CLOCK_REALTIME`, before the socket receive-queue → excludes
  socket-queue + scheduler jitter). `--xdp-rx` instead reads a `bpf_ktime_get_ns`
  (`CLOCK_MONOTONIC`) stamp written by `ucast.o` at the XDP ingress hook into the
  payload; `rtt` then also stamps TX with `CLOCK_MONOTONIC` to match the domain.
- **[R5]/[R6]** — `RTT = RX − TX` on one host's clock (no sync). Warmup samples
  are discarded, then min/mean/p50/p90/p95/p99/p99.9/max are written as
  `service_rtt_us` alongside `messages, warmup, rate_mps, lost, loss_pct,
  timestamp_rx, timestamp_tx, tx_path`.
- **[M1] m2u framing** — `mcast_send` builds `Eth|IPv4(proto 17)|UDP|m2u{magic
  "M2CU"=0x4D324355, group}|payload`; `payload[0..23] = {seq, ts_ns,
  replicator_ns=0}`. It's a **plain unicast** frame to the replicator's private
  IP (`-D`); the group lives only in the m2u tag (ENA has no L2 multicast).
- **[M3]** — `ts_ns = CLOCK_REALTIME` stamped in place just before AF_XDP TX
  (zero-copy). dst MAC ARP-resolved at startup.
- **[M4]/[M5] mcast_receive** — attaches its **own** `mcast.o` and seeds
  `config_map[0] = {group, port}` (`-g`/`-p`) so the NIC redirects the group to
  its AF_XDP socket; busy-polls the RX ring (`SO_BUSY_POLL`), stamps `rx_ns`
  (`CLOCK_REALTIME`) at dequeue, and reads `ts_ns`/`replicator_ns`/
  `replicator_tx_ns` back out of the payload.
- **[M6]/[M7]** — **hop1** = `replicator_ns − ts_ns`, **hop2** =
  `rx_ns − replicator_tx_ns`, **total** = `rx_ns − ts_ns`. Because it's one-way
  across two hosts, both must share a UTC-aligned clock (Nitro PHC `/dev/ptp0` +
  chrony, ~µs). `mcast_receive` counts **negative hop2** samples as a live
  clock-skew diagnostic and reports percentiles (p50…max) to stdout and `-j` JSON.

---

## Binaries & every launch option

| File | Binary | Build |
|------|--------|-------|
| `rtt.cpp` | `rtt` | full (AF_XDP TX) or echo-mode (`--xdp-tx` rejected) |
| `mcast_send.cpp` | `mcast_send` | full only (`make full`) — needs libxdp |
| `mcast_receive.cpp` | `mcast_receive` | full only (`make full`) — needs libxdp |
| `replicator_ctl.cpp` | `replicator_ctl` | full or echo-mode |
| `udp_send.cpp` | `udp_send` | full or echo-mode |

### `rtt` — unicast RTT probe
```
rtt <replicator_ip> <data_port> <listen_ip> <listen_port> \
    <count> <rate_per_sec> <warmup> <tx_cpu> <rx_cpu> \
    [--xdp-tx[=queue]] [--iface <name>] [--xdp-rx]
```
- **Positional (all required):** replicator IP + data port to probe; local
  listen IP + port for echoes; `count` measured msgs; `rate_per_sec`; `warmup`
  msgs to discard; `tx_cpu`/`rx_cpu` core pins.
- **`--xdp-tx[=queue]`** — send via TX-only AF_XDP (default queue 1). **Requires
  `--iface`.** Rejected in echo-mode builds (`"--xdp-tx not available in this
  (echo-mode) build"`). JSON `tx_path` becomes `"af_xdp"`.
- **`--iface <name>`** — NIC for `--xdp-tx`.
- **`--xdp-rx`** — read the XDP-ingress `bpf_ktime` RX stamp from the payload
  (needs `ucast.o` stamping upstream); switches the clock domain to
  `CLOCK_MONOTONIC` on both TX and RX.
- Too few positionals → prints `Usage:` and exits non-zero.

### `mcast_send` — m2u multicast source (`getopt I:D:g:p:c:i:s:q:h`)
| Flag | Meaning | Default |
|------|---------|---------|
| `-D <replicator-ip>` | unicast tunnel destination — **REQUIRED** | — |
| `-I <iface>` | real NIC | eth0-class default |
| `-g <group>` | multicast group (carried in the m2u header) | `224.0.31.50` |
| `-p <port>` | UDP dst port | `5000` |
| `-c <count>` | packets to send | (default in help) |
| `-i <interval_us>` | inter-packet gap (µs) | (default in help) |
| `-s <pkt_size>` | payload size | — |
| `-q <tx_queue>` | AF_XDP TX queue | — |
| `-h` | usage, exit 0 | — |

Missing `-D` → `error: -D <replicator-ip> is required` + usage, exit 1. Unknown
option → usage, exit 1.

### `mcast_receive` — AF_XDP multicast sink (`getopt I:g:p:c:t:q:rj:h`)
| Flag | Meaning | Default |
|------|---------|---------|
| `-I <iface>` | network interface — **REQUIRED** | — |
| `-g <group>` | inner multicast group to match | `224.0.31.50` |
| `-p <port>` | inner UDP dst port to match | `5000` |
| `-c <count>` | packets to receive | (default in help) |
| `-t <timeout>` | seconds before giving up | (default in help) |
| `-q <queue>` | AF_XDP queue | 0 |
| `-r` | raw mode | off |
| `-j <path>` | write JSON results | — |
| `-h` | usage, exit 0 | — |

Missing `-I` → `error: -I <iface> is required` + usage, exit 1.

### `replicator_ctl` — control-protocol CLI
```
replicator_ctl <replicator_ip> <command> [args...]
  add <dest_ip> <dest_port>     Register destination (CTRL_ADD_DESTINATION)
  remove <dest_ip> <dest_port>  Deregister (CTRL_REMOVE_DESTINATION)
  list                          List destinations (CTRL_LIST_DESTINATIONS)
  mcast <group>                 Join group (CTRL_MCAST_JOIN) — mcast mode only
  mcast-leave <group>           Leave group (CTRL_MCAST_LEAVE)
```
No command → usage, exit 1. `mcast`/`mcast-leave` need `CAP_NET_RAW` (sudo); the
join is sent unicast to `<replicator_ip>`, so it crosses VPC peering without
native multicast routing.

### `udp_send` — connectivity probe
```
udp_send <target_ip> <target_port> [interval_ms] [message] [--iface <name>]
```
Unicast to a replicator, or a multicast group (`--iface` sets `IP_MULTICAST_IF`,
required for native multicast). No args → usage, exit 1.

---

## Timestamping & clocks (summary)

| Path | TX clock | RX clock | Sync needed? |
|------|----------|----------|:---:|
| `rtt` default | `CLOCK_REALTIME` before send | `SO_TIMESTAMPING` SW (NAPI, `CLOCK_REALTIME`) | No (single host round-trip) |
| `rtt --xdp-rx` | `CLOCK_MONOTONIC` | `bpf_ktime_get_ns` at XDP ingress (`CLOCK_MONOTONIC`) | No |
| mcast one-way | `ts_ns` `CLOCK_REALTIME` (source) | `rx_ns` `CLOCK_REALTIME` (dest, XSK dequeue) | **Yes** — PHC/chrony ~µs |

ENA provides **no TX hardware timestamp**; no TSC is used. The replicator's
`replicator_ns`/`replicator_tx_ns` stamps (see `src/Replicator/README.md`) split
the one-way total into source→replicator and replicator→dest hops.

## Build modes

- `make full` builds all five binaries + the AF_XDP `--xdp-tx` backend (needs
  libxdp/libbpf; `mcast_send`/`mcast_receive` are full-only).
- `make echo-mode` builds `rtt`, `replicator_ctl`, `udp_send` without libxdp;
  `rtt --xdp-tx` is compiled out and rejected at runtime.

The real AF_XDP datapaths run on EC2 (`deploy/ansible/run_ucast.yaml`,
`run_mcast.yaml`); the container test suite (`tests/`) exercises the CLI/arg
surface and the control protocol against a echo-mode replicator.
