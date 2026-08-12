# tools/

Measurement instruments and control utilities.

## Binaries

| File | Binary | Description |
|------|--------|-------------|
| `rtt.cpp` | `rtt` | High-precision RTT measurement client. Subscribes to replicator, sends UDP packets, measures round-trip via kernel-SW `SO_TIMESTAMPING` (CLOCK_REALTIME) RX + `clock_gettime(CLOCK_REALTIME)` TX (single clock domain). Optional `--xdp-tx` (AF_XDP send) and `--xdp-rx` (XDP-stamped RX). Outputs JSON with p50/p90/p95/p99/p999/max. |
| `mcast_send.cpp` | `mcast_send` | Multicast sender — timestamps packets, sends to the replicator (m2u) or a multicast group. Used as the "exchange" in multicast scenarios. |
| `mcast_receive.cpp` | `mcast_receive` | Multicast receiver — attaches `mcast.o`, seeds its `config_map` with the target group+port (`-g`/`-p`) so the XDP filter redirects to the AF_XDP socket, captures packets, computes one-way + per-hop latency from sender/replicator timestamps. Requires PHC clock sync between hosts. |
| `replicator_ctl.cpp` | `replicator_ctl` | Control protocol client. Sends ADD/REMOVE/LIST commands to replicator's control port (12345). |
| `udp_send.cpp` | `udp_send` | Simple UDP connectivity probe. Sends packets to a target and reports reachability. Supports multicast groups. |

## rtt usage

```bash
sudo ./rtt <replicator_ip> <data_port> <listen_ip> <listen_port> \
           <count> <rate_per_sec> <warmup> <tx_cpu> <rx_cpu> \
           [--xdp-tx[=queue]] [--iface <name>] [--xdp-rx]
```

Requires `sudo` for SCHED_FIFO and mlockall. Runs without root but degrades gracefully
(warnings printed, continues with SCHED_OTHER).

### `--xdp-tx` (AF_XDP zero-copy send)

By default the probe is sent with `sendto()` (kernel UDP stack). Pass `--xdp-tx[=queue]`
+ `--iface <name>` to send via a TX-only AF_XDP socket instead, building the
`Eth|IPv4|UDP|payload` frame in userspace and stamping the TX timestamp immediately
before `submit` (sfence-ordered). This removes the ~3-5µs kernel TX stack from the
measured send leg — expect a few µs lower p50 and tighter tails; RX stays kernel-socket
(`SO_TIMESTAMPING`), same clock domain, so the RTT stays valid.

- Binds a **dedicated queue (default 1)**: queue 0 is owned by the local ucast
  `replicator.service` AF_XDP socket (RSS is pinned to queue 0), so the sender must use a
  different queue. TX egress is independent of RSS. Falls back to kernel `sendto` (with a
  warning) if AF_XDP init fails.
- Only available in `make full` builds (needs libxdp); `make kernel-mode` rejects the flag.
- JSON gains `"tx_path": "af_xdp" | "kernel"`. Enable fleet-wide via
  `run_ucast.yaml -e xdp_tx=true [-e xdp_tx_queue=1]`.
- Same AF_XDP zero-copy TX mechanism as `mcast_send` and the replicator's forward path
  (`sendSinglePacketDirect`, XSK TX ring), so the send leg is measured consistently across the
  ucast and mcast tools.

### `--xdp-rx` (XDP-stamped RX — *not* an AF_XDP RX datapath)

By default RX is the kernel software timestamp (`SOF_TIMESTAMPING_RX_SOFTWARE`, CLOCK_REALTIME,
NAPI `netif_receive_skb`). Pass `--xdp-rx` to instead read an RX time stamped even earlier — at
the **XDP ingress hook** — by the ucast XDP program: for packets carrying the rtt magic it writes
`bpf_ktime_get_ns()` (CLOCK_MONOTONIC) into the echo payload and `XDP_PASS`es it up. The client
reads that value and stamps TX with CLOCK_MONOTONIC to match; `rtt = rx_mono − tx_mono`.

**This is not an AF_XDP RX datapath.** `--xdp-rx` does not bind an XSK or read a ring — the echo
still traverses the full kernel RX stack to the UDP socket (`recvmsg`); only the *timestamp
source* moves to the XDP hook. This is unlike `mcast_receive`, which uses a **true AF_XDP RX**
(XSK) and takes `rx_ns` at ring dequeue (kernel-bypass). A true XSK-RX rtt client would match
`mcast_receive`'s methodology, but on a replicator-colocated host it needs its own steered RX
queue (queue 0 is the replicator's XSK), so `--xdp-rx` is the pragmatic "early stamp without a
second XSK".

- Requires the extended `ucast.o` loaded by `replicator.service` (it stamps the magic packets and
  zeroes the UDP checksum, since the payload changed).
- The rtt listen port must differ from the local replicator's listen port, so the echo falls
  through to the stamp-and-PASS branch instead of being redirected to the replicator's XSK.
- JSON: `"timestamp_rx": "xdp_ktime_mono"`, `"timestamp_tx": "clock_monotonic"`. Enable
  fleet-wide via `run_ucast.yaml -e xdp_rx=true`.
- Measured **no lower** than the kernel-SW path on ENA — the kernel-SW stamp is already near-wire
  in the NAPI path and the datapath is unchanged, so the XDP hook has little to shave. Kept as an
  optional, validated mode.


Output: JSON at `/tmp/rtt_results.json` with `service_rtt_us` and `response_rtt_us` percentiles.

### Latency optimizations

The `rtt` binary implements the following optimizations to minimize measurement overhead
and isolate the true network/replicator latency from OS noise:

| Optimization | Mechanism | Impact |
|---|---|---|
| **SCHED_FIFO (priority 80)** | `sched_setscheduler(SCHED_FIFO, 80)` on both sender and receiver threads | Eliminates preemption by other processes. Removes 5-15µs scheduler jitter. |
| **mlockall** | `mlockall(MCL_CURRENT \| MCL_FUTURE)` | Prevents page faults during measurement. First-touch faults can add 50-100µs spikes. |
| **CPU pinning** | `sched_setaffinity()` — sender and receiver on separate isolated cores | Eliminates cross-core cache migration. Works best with `isolcpus` kernel boot param. |
| **SO_BUSY_POLL (100µs)** | `setsockopt(SO_BUSY_POLL, 100)` on receive socket | Kernel spins polling NIC queue instead of sleeping for IRQ wakeup. Removes 8-12µs IRQ→schedule→wakeup path. |
| **SO_PREFER_BUSY_POLL** | Forces busy-poll path even under moderate load | Prevents kernel from falling back to interrupt-driven receive. |
| **SO_BUSY_POLL_BUDGET (256)** | Packets processed per busy-poll NAPI cycle | Prevents early exit from poll loop when multiple packets queued. |
| **TX timestamp** | `clock_gettime(CLOCK_REALTIME)` right before the send (kernel `sendto`, or AF_XDP TX with `--xdp-tx`) | Single CLOCK_REALTIME domain with the kernel-SW RX; `--xdp-tx` removes the kernel TX stack. No TSC. |
| **Kernel RX timestamp** | `SO_TIMESTAMPING` with `SOF_TIMESTAMPING_RX_SOFTWARE` | CLOCK_REALTIME, stamped by the stack in the NAPI `netif_receive_skb` path (before the socket queue), not at `recvmsg()` return. |
| **Lock-free slot array** | Pre-allocated array indexed by sequence ID | No allocation, no mutex, no map lookup in hot path. Cache-line aligned (64B). |
| **Spin-wait receive** | `recvmsg(MSG_DONTWAIT)` + `_mm_pause()` loop | No `poll()`/`select()` syscall overhead. `_mm_pause` hints CPU for SMT-friendly spinning. |
| **Safety alarm** | `alarm(runtime + 30s)` with `_exit(2)` handler | Prevents SCHED_FIFO busy-loop from locking up the system if the benchmark hangs. |
| **Absolute-time pacing** | `clock_nanosleep(TIMER_ABSTIME)` | Prevents drift accumulation — each send targets exact wall-clock deadline. |

### Expected latency by optimization level

| Configuration | p50 (CPG, c7i.xlarge) | Notes |
|---|---|---|
| Default (no RT, no tuning) | ~35-46µs | OS scheduler + IRQ wakeup dominate |
| + SO_BUSY_POLL only | ~28-35µs | Removes IRQ wakeup, still has scheduler jitter |
| + SCHED_FIFO + CPU pin | ~22-28µs | No preemption, deterministic scheduling |
| + mlockall + isolcpus | ~20-25µs | No page faults, no kernel housekeeping on measurement cores |
| All combined (current) | ~18-24µs | Kernel UDP path floor |

### Limitations (kernel path floor)

Even with all optimizations, the kernel UDP path has an irreducible ~15-20µs floor:
- `sendto()` syscall: ~3-5µs (context switch + kernel lock + skb alloc + copy)
- `recvmsg()` kernel→user copy: ~2-3µs
- NIC DMA + wire propagation: ~3-5µs

To go below this floor, the sender would need AF_XDP TX (bypass kernel entirely),
which would reduce p50 to ~12-15µs.

### Timestamp modes

| Mode | Source (where taken) | Clock domain |
|---|---|---|
| Default RX — kernel software (`SOF_TIMESTAMPING_RX_SOFTWARE`) | stack, NAPI RX path (`netif_receive_skb` / `net_timestamp_check`), right after the ENA driver builds the skb — before the socket queue | CLOCK_REALTIME |
| Default RX fallback — userspace | `clock_gettime` after `recvmsg` (only if no cmsg) | CLOCK_REALTIME |
| Default TX | `clock_gettime` before the send (kernel `sendto`, or AF_XDP frame build with `--xdp-tx`) | CLOCK_REALTIME |
| `--xdp-rx` (RX + TX) | RX = XDP ingress `bpf_ktime_get_ns()` into the payload; TX = `clock_gettime(CLOCK_MONOTONIC)` to match | CLOCK_MONOTONIC |
| Hardware PHC (`SOF_TIMESTAMPING_RX_HARDWARE`) | Nitro engine — **multicast one-way only**, not the RTT | PHC epoch (needs phc2sys) |

The default RTT is a single **CLOCK_REALTIME** delta on one host: TX = `clock_gettime` immediately
before the send; RX = the kernel software timestamp stamped by the stack in the NAPI receive path
just after the ENA driver pulls the frame off the RX ring — before the socket queue — so it
excludes socket-queue + poll/schedule jitter. **No TSC and no PHC are used for the RTT** (ENA has
no TX hardware timestamp; PHC lives in a separate epoch and is used only for the one-way multicast
path, `mcast_receive`). `--xdp-rx` moves the RX stamp to the XDP ingress hook (CLOCK_MONOTONIC) —
see above.

## replicator_ctl usage

```bash
./replicator_ctl <replicator_ip> add <dest_ip> <dest_port>
./replicator_ctl <replicator_ip> remove <dest_ip> <dest_port>
./replicator_ctl <replicator_ip> list
./replicator_ctl <replicator_ip> mcast <multicast_group>
./replicator_ctl <replicator_ip> mcast-leave <multicast_group>
```

## Multicast (mcast_send / mcast_receive) usage

```bash
# Receiver (destination): attach mcast.o, listen for group:port fan-out
sudo ./mcast_receive -I <iface> -g <group> -p <port> -c <count> -t <timeout_s> [-q <queue>]

# Sender (source): m2u-tagged unicast to the replicator, group in the header
sudo ./mcast_send -I <iface> -D <replicator_ip> -g <group> -p <port> -c <count> -i <interval_us>
```

Interface flag is `-I` in both tools (`mcast_send` uses `-i` for interval). `mcast_receive`
seeds `config_map[0] = {group, port}` so `mcast.o` redirects matching packets; without a
matching entry the filter `XDP_PASS`es everything and the AF_XDP socket sees nothing.

For the multi-group / multi-destination capability and the orchestration roadmap, see
[deploy/ansible/README.md → "Multicast: groups & destinations"](../deploy/ansible/README.md).

## Dependencies

All tools link only `-lpthread` in kernel-mode builds. Full builds add `-lxdp -lbpf -lelf` (unused at runtime by tools, but pulled in by the shared Makefile LDLIBS).
