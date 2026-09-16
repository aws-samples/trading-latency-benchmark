# src/ — AF_XDP replicator engine

Core of the benchmark: an **AF_XDP zero-copy UDP packet replicator** plus a
echo-mode fallback echo server. Paired with the `tools/` clients (`rtt`,
`mcast_send`, `mcast_receive`, `replicator_ctl`) it measures point-to-point
(unicast) and fan-out (multicast-over-m2u) latency between EC2 instances with
sub-microsecond timing resolution.

## Files

| File | Description |
|------|-------------|
| `Replicator/Main.cpp` | Entry point — CLI parsing, mode dispatch (`--echo-mode` vs AF_XDP) |
| `Replicator/Replicator.hpp` | AF_XDP replicator — single class declaration (multi-queue RX, per-group fan-out, control protocol) |
| `Replicator/Internal.hpp` | Shared internal header for the `Replicator/*.cpp` units (common includes + debug macros) |
| `Replicator/Core.cpp` | Replicator impl — lifecycle (ctor/dtor/move), thread start/stop, statistics, CPU affinity |
| `Replicator/Init.cpp` | Replicator impl — XDP program load, per-queue AF_XDP socket setup, `config_map` seeding |
| `Replicator/Groups.cpp` | Replicator impl — dynamic BPF group slots (ref-counted) + kernel XDP_TX forward target |
| `Replicator/Control.cpp` | Replicator impl — control protocol thread + message handling + upstream forwarding |
| `Replicator/Destinations.cpp` | Replicator impl — `Destination` type, destination registry, thread-local fan-out cache |
| `Replicator/DataPath.cpp` | Replicator impl — RX busy-poll, replicate/fan-out, UDP/m2u parse, zero-copy TX, packet build |
| `Replicator/Net.cpp` | Replicator impl — IP/MAC helpers, interface discovery, ARP/gateway resolution |
| `Replicator/XdpSocket.cpp/hpp` | AF_XDP socket wrapper (replicator datapath): UMEM, fill/comp/rx/tx rings, batched zero-copy TX/RX |
| `Replicator/KernelEcho.cpp` | Echo-mode replicator backend — UDP echo server, same control protocol, no root/XDP (CI, tests) |
| `common/ControlPort.hpp` | Shared control-port resolver (`AFXDP_CONTROL_PORT`, default 12345) — used by the replicator and `tools/` |
| `xdp/ucast.c` | eBPF XDP program — unicast filter (steers matching UDP to the AF_XDP socket) |
| `xdp/mcast.c` | eBPF XDP program — intercepts m2u-tagged multicast UDP, steers to AF_XDP |

## Build modes

```bash
make all          # full AF_XDP build (needs libxdp, libbpf) — EC2
make full         # all + multicast tools (mcast_send/receive)
make echo-mode  # -DECHO_MODE_ONLY, no libxdp — containers / CI / macOS
```
`#ifdef ECHO_MODE_ONLY` guards exclude all XDP/BPF code paths (and the AF_XDP TX sender inlined in `tools/rtt.cpp`)
so the control-protocol + echo logic can be unit-tested without root or a NIC.

---

## Key data structures

**UMEM & rings** (`XdpSocket.hpp`) — a UMEM is a contiguous mmap'd region carved
into fixed frames shared with the kernel:
- `DEFAULT_TX_FRAMES = 2048`, `DEFAULT_RX_FRAMES = 2048` → `UMEM = 4096` frames;
  RX frames begin at index 2048 (`UMEM_RX_FIRST_FRAME_IX`), TX uses 0..2047.
- Four rings per socket: **FILL** (userspace → kernel: empty RX frames),
  **RX** (kernel → userspace: filled frames), **TX** (userspace → kernel: frames
  to send), **COMPLETION** (kernel → userspace: sent frames freed).
- `TX_BATCH_SIZE = 64` — TX is submitted in batches to amortize the syscall/kick.
- Attach flags: `XDP_FLAGS_DRV_MODE | XDP_FLAGS_ZERO_COPY` (native driver + ZC).

**Destination** (`Replicator/Replicator.hpp`) — one fan-out target:
`{ ip_address, port, sockaddr_in addr, uint8_t mac[6] }`. The **MAC is resolved
via ARP at registration time** so fan-out frames carry a real unicast dst MAC
(otherwise ENA drops broadcast-dst frames).

**Per-group BPF state** (mcast mode), keyed by group IP in network byte
order, guarded by `group_mutex_`:
- `group_slots_`  : group → `config_map` slot index
- `group_ref_counts_` : group → number of joined destinations
- `free_slots_` : available slots (`MAX_GROUPS = 16`)
- `group_destinations_` : group → { dest IP → `Destination` }

**BPF maps** (in the `.c` XDP programs):
- `xsks_map` (`BPF_MAP_TYPE_XSKMAP`) — queue index → AF_XDP socket, target of `bpf_redirect_map`.
- `config_map` — `{target_ip, target_port}` entries the filter matches on (sparse; `target_ip==0` = unused slot).
- `stats` — per-action counters.

**Lock-free timing slots** (`rtt.cpp`) — a preallocated array indexed by
sequence id, each holding the send timestamp(s), intended-send time, and RX
timestamp for one probe. No hash map, no mutex on the hot path.

---

## Unicast workflow (step by step)

Topology: every node runs `replicator` (AF_XDP) and, to measure a peer, runs the
`rtt` client against that peer's replicator.

1. **Replicator startup** — bind the listen IP/port, bring up the NIC for XDP
   (MTU 3498, RSS→queue 0), allocate the UMEM, create one AF_XDP socket per RX
   queue, and **load `ucast.o`**, attaching it to the interface in DRV+ZC mode.
2. **Seed `config_map`** — the replicator inserts its own `{listen_ip, listen_port}`
   so the XDP program knows which packets to steer. It keeps `config_map_fd_`
   open for runtime updates.
3. **Datapath (kernel, per packet):** `ucast.c` runs at the driver before the
   normal stack: parse Ethernet → require `ETH_P_IP` → require `IPPROTO_UDP` →
   look up `{dst ip, dst port}` in `config_map`. Match → `bpf_redirect_map` into
   `xsks_map` (packet lands in the AF_XDP RX ring, zero-copy). No match →
   `XDP_PASS` (normal kernel path, e.g. SSH).
4. **Replicator RX/echo:** a per-queue `packet_processor_thread_` busy-polls the
   RX ring, reads the frame in-place from the UMEM, and for the echo/probe case
   swaps src/dst and transmits from the TX ring (zero-copy, `sendSinglePacketDirect`) — or
   fans out to all registered `Destination`s (rewriting dst IP/MAC/port and UDP/IP checksums).
   The AF_XDP zero-copy TX ring is the primary forward path; a kernel `sendto` fallback is used
   only when a destination MAC is unresolved (ARP) or the direct AF_XDP send fails.
5. **`rtt` client:** registers itself as a destination over the control channel
   so the replicator returns packets to it, then sends `N` sequenced probes (plus
   warmup), timestamping each, and records RX timestamps of the echoes. See
   *latency measurement* below.
6. **Control channel:** a `control_thread_` serves the binary UDP protocol
   (`AFXDP_CONTROL_PORT`, default 12345) for add/remove/list/mcast-join.

## Multicast workflow (m2u tunnel, step by step)

ENA (see below) has **no native L2 multicast**, so multicast is carried inside a
plain unicast UDP packet tagged with a light **8-byte m2u header** `{magic, group}`
to the replicator, intercepted by XDP. No kernel tunnel device is involved.

1. **Node prep** — `configure_mcast.yaml` stops the ucast `replicator.service`
   on source/destination (frees AF_XDP queue 0) and detaches stale XDP.
2. **m2u framing** — `mcast_send` builds the frame in userspace and sends it via
   AF_XDP zero-copy straight to the replicator (`-D <replicator_ip>`). Frame on
   the wire: `Eth / IPv4 (proto 17=UDP) / UDP / m2u{magic(4), group(4)} / payload`
   (50 B of headers, vs 66 B for the old encapsulation). Inspired by [mcast2ucast](../../mcast2ucast/).
3. **Replicator (mcast mode)** — loads `mcast.o` instead of `ucast.o`. It parses
   `Eth/IP/UDP` + the 8-byte m2u tag, reads the group from the header, and matches
   `{group, dst port}` against `config_map`; match → redirect to the AF_XDP socket
   (zero-copy). No outer-IP proto-47, no variable-length tunnel header, no inner IP.
4. **Group registration** — each destination sends `CTRL_MCAST_JOIN` (`[0x04][4B
   group]`) over the control channel; the replicator allocates a `config_map`
   slot (up to `MAX_GROUPS=16`), ref-counts it, ARP-resolves the destination, and
   fans the group out to all joined destinations. `CTRL_MCAST_LEAVE` reverses it.
5. **Measurement** — `mcast_send` injects the stream; `mcast_receive` (with
   `-g <group>`) seeds its own `config_map[0]` and receives via the AF_XDP
   socket. Multicast latency is **one-way** (source→dest), so it requires
   synchronized clocks (see accuracy).

## Latency-critical RX/TX paths

- **In-app NAPI busy-poll** (`XdpSocket::receive`) — on an empty RX peek the loop
  issues `recvfrom(fd, …, MSG_DONTWAIT)`; with `SO_PREFER_BUSY_POLL`+`SO_BUSY_POLL`
  set on the XSK fd this runs the NAPI poll in *this* (pinned) thread, so the
  NIC→ring fill doesn't wait on the deferred hard IRQ. This is what makes hop1
  (source→replicator) independent of `gro_flush_timeout`. The small gro (10µs) is
  only the backstop for the windows when the thread is busy fanning-out rather than
  polling — **not** the primary delivery path. (`gro=0` breaks this → multi-second
  bursts; `gro=200µs` makes the backstop the primary path → ~200µs/hop.)
- **Fan-out hot path** (`Replicator::replicatePacket` → `createUdpPacket`) — the
  source IP is parsed once at `initialize()` (`cached_iface_saddr_nbo_`), not per
  packet (`inet_aton` was on the hot path); TX completions are drained once per
  fan-out batch (not per destination); one driver kick covers all K destinations.
- **CPU layout** — the busy-poll thread is pinned to an isolated CPU and the ENA
  hard IRQ to a *different* isolated CPU (see `deploy/ansible/run_mcast.yaml` /
  `ena-irq-affinity.service`), so the IRQ never preempts the poll loop.

---

## How packets are sent (`rtt` TX)

Two send backends, selected by `rtt --xdp-tx`:

- **Kernel `sendto`** (default): a normal UDP socket. Simple; incurs the full
  kernel TX stack (~3–5 µs) inside the measured send leg.
- **AF_XDP TX** (inlined in `tools/rtt.cpp`, `--xdp-tx[=queue] --iface`): a **TX-only**
  AF_XDP socket opened with `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD` (no XDP program
  loaded — egress is independent of RSS). The full `Eth|IPv4|UDP|payload` frame
  is built **once into every UMEM frame** at startup (dst MAC via ARP); per
  packet it writes only the 10 ASCII sequence digits and stamps the TX timestamp
  immediately before submitting to the TX ring. This removes the kernel TX stack
  from the measured leg. A/B measured: p99.9 −14.5 µs (~21%), max 488→82 µs.

---

## How latency is measured

**TX timestamp — `CLOCK_REALTIME`.** On the send hot path the tool reads
`clock_gettime(CLOCK_REALTIME)` immediately before the send (kernel `sendto`, or the
AF_XDP TX frame build with `--xdp-tx`). This is the value differenced in the RTT and it
matches the kernel-SW RX domain. ENA provides **no TX hardware timestamp**, and no TSC
is used.

**RX timestamp — kernel software, stamped early.**
1. **Kernel software** (`SO_TIMESTAMPING` + `SOF_TIMESTAMPING_RX_SOFTWARE`) —
   `CLOCK_REALTIME` (`skb->tstamp = ktime_get_real`), recorded by the stack in the
   **NAPI receive path** (`netif_receive_skb` / `net_timestamp_check`) right after the
   ENA driver pulls the frame off the RX ring and builds the skb, **before the socket
   receive-queue enqueue**, so it excludes socket-queue + scheduler/wakeup jitter. This
   is the default on ENA.
2. **Userspace fallback** — `clock_gettime(CLOCK_REALTIME)` after `recvmsg` if no cmsg.

Hardware PHC RX timestamps are **not** used for the RTT — they live in a separate
wall-clock epoch; PHC is only for the one-way multicast path below.

**`--xdp-rx` (optional).** The RX time is instead stamped at the **XDP ingress hook**
by the ucast XDP program via `bpf_ktime_get_ns()` (`CLOCK_MONOTONIC`), written into the
echo payload; the client reads it and stamps TX with `CLOCK_MONOTONIC` to match. **This is
not an AF_XDP RX datapath** — the echo still traverses the full kernel stack to the UDP
socket (`recvmsg`); only the timestamp *source* moves to the XDP hook. (Contrast
`mcast_receive`, which uses a true AF_XDP RX and reads `rx_ns` at XSK dequeue.) On ENA it
measured no lower than kernel-SW — that stamp is already near-wire and the datapath is
unchanged — so it is off by default.

**RTT computation.** Each echo carries its sequence id → index into the lock-free slot
array → retrieve the send timestamp. RTT = RX − TX, both in a single clock domain
(`CLOCK_REALTIME` by default, `CLOCK_MONOTONIC` under `--xdp-rx`). Because unicast RTT is
round-trip on one host, **no clock synchronization is required**.

**Aggregation.** A configurable **warmup** count is discarded (cache/JIT/ARP
warm-up), then min/mean/p50/p90/p95/p99/p99.9/max are computed and written as
`service_rtt_us` JSON, along with `messages`, `lost`, `loss_pct`, `tx_path`.

---

## How multicast (one-way) latency is measured

Multicast latency is **one-way** (source → replicator → destination), so — unlike
the unicast round trip — it cannot use a single host's clock. Instead the timestamp
**travels in the packet payload** and both hosts read a **shared, PHC-disciplined
UTC clock** (`CLOCK_REALTIME`, aligned to UTC to ~µs via the Nitro PHC + chrony).

**Payload header** (`mcast_send.cpp`, 24 bytes):

| bytes | field | written by |
|-------|-------|-----------|
| 0..7  | `seq` | sender |
| 8..15 | `ts_ns` — send time | sender hot path (`CLOCK_REALTIME`), just before TX |
| 16..23 | `replicator_ns` — replicator RX time | the replicator as it fans the packet out |

**Flow:**
1. `mcast_send` (AF_XDP TX, zero-copy, m2u-tagged unicast) writes `seq` + `ts_ns`
   in place into each frame on the hot path (16 bytes) and transmits.
2. The replicator stamps `replicator_ns` into the payload as it receives and fans
   the packet out — this yields a per-hop split.
3. `mcast_receive` peeks each frame off the AF_XDP RX ring, takes
   `rx_ns = CLOCK_REALTIME`, and reads `ts_ns`/`replicator_ns` back out of the payload.

**Computed metrics** (`mcast_receive.cpp`), reported as percentiles (p50…max):
- **total one-way** = `rx_ns − ts_ns`   (source → destination, end-to-end)
- **hop 2** = `rx_ns − replicator_ns`    (replicator → destination)
- **hop 1** = `replicator_ns − ts_ns`    (source → replicator; by difference)

When no replicator timestamps are present it falls back to a single-hop (total-only) view.

**Why payload-carried, not the unicast slot array:** the receiver is a *different
host*, so a host-local clock value would be meaningless there — the stamp must
accompany the packet and be compared against a UTC-aligned clock on both ends.

**Accuracy & clock-skew guard:** one-way accuracy is bounded by each host's PHC
discipline to UTC (~µs), independent of the path distance. `mcast_receive` counts
**negative hop-2 samples** (`rx_ns < replicator_ns` — the replicator's clock
leading the destination's) as a live skew diagnostic: a nonzero count means the
clocks aren't tight enough and the one-way figures should be distrusted. This is
why one-way is used only where the signal (cross-AZ / cross-region, tens of µs–ms)
is well above the µs-scale sync error; sub-µs same-rack latency stays on the
unicast round-trip (single-host `CLOCK_REALTIME`) path.

## Optimizations

- **Zero-copy AF_XDP** (DRV mode) — packets DMA'd straight into the UMEM; the
  replicator reads/echoes in place, no copies.
- **Lock-free slot array** — sequence-indexed timing store; no map/mutex on the
  hot path.
- **Busy-poll receive** (`SO_BUSY_POLL`) — the RX thread spins the NIC queue
  instead of sleeping on `poll()/select()`, removing IRQ→wakeup latency; XDP
  threads use `_mm_pause` spin.
- **CPU pinning + isolation** — send/recv/replicator threads pinned to dedicated
  physical cores; `isolcpus`/`nohz_full`/`rcu_nocbs`/`nosmt` keep those cores
  free of the scheduler tick and OS work (core 0 handles OS + NIC IRQs).
- **`SCHED_FIFO` priority 80** — real-time scheduling on the measurement threads.
- **`mlockall`** — locks pages so there are no page faults mid-measurement.
- **Absolute-time pacing** (`clock_nanosleep(TIMER_ABSTIME)`) — jitter-free send
  cadence at the target rate.
- **Batched TX** (`TX_BATCH_SIZE=64`) — amortizes the ring kick/syscall.
- **TX-only AF_XDP frame templating** — see above.
- **Enlarged socket buffers** (`net.core.rmem_max/wmem_max=16 MB`) — eliminates
  `RcvbufErrors`/loss under burst (was ~0.6% loss).

## Accuracy

- **Resolution:** bounded by `clock_gettime` on TX (tens of ns) and the kernel software
  RX timestamp (~µs, stamped in the NAPI path). No TSC or PHC is used in the RTT.
- **Unicast:** round-trip on a single host's clock → no sync error. RX stamped in the
  NAPI `netif_receive_skb` path (before the socket queue) removes queueing/scheduling
  jitter from the RX leg. Measured intra-cluster p50 ≈ 24–30 µs; loss driven to 0
  after the rmem fix.
- **Multicast:** one-way source→dest, so accuracy is bounded by **clock sync**.
  Nodes sync to the Nitro PHC (`/dev/ptp0`) via chrony refclock (±50–500 ns) plus
  a tight NTP fallback; sub-µs skew was verified before runs.
- **AF_XDP TX vs kernel** (A/B on 2× c7i.2xlarge): p50 −2 µs (~6%), p99 −3 µs,
  p99.9 −14.5 µs (~21%), max 488→82 µs.

## Linux specifics

- **AF_XDP / XDP_REDIRECT** — `bpf_redirect_map` into an `XSKMAP` delivers frames
  to userspace with zero copy at the earliest driver hook.
- **UMEM + 4 rings** (fill/completion/rx/tx) via libbpf/libxdp; `RLIMIT_MEMLOCK`
  raised to `RLIM_INFINITY` for the locked UMEM.
- **eBPF maps** — `XSKMAP`, `config_map`, `stats`; the config map is updated from
  userspace at runtime (`config_map_fd_`).
- **Timestamping** — `SO_TIMESTAMPING` (SW+HW), `SO_TIMESTAMP` fallback.
- **`SO_BUSY_POLL`**, **`SCHED_FIFO`**, **`mlockall`**, **`SO_RCVBUF`/`SO_RCVTIMEO`**.
- **Core isolation** — `isolcpus`/`nohz_full`/`rcu_nocbs`/`nosmt` grub cmdline.

## AWS / ENA specifics

- **Native XDP on ENA needs single-page frames** → **MTU 3498** (set by
  `ena-mtu.service`); larger MTUs disable native XDP.
- **RSS → queue 0** (`ethtool -X … equal 1`) so *all* RX lands on the single
  AF_XDP socket's queue (the replicator binds queue 0).
- **Interrupt coalescing off** (`adaptive-rx off`, `rx-usecs=0`, `tx-usecs=0`) and
  **ENA IRQs pinned to CPU 0** (`ena-irq-affinity.service`, irqbalance disabled)
  to keep the isolated measurement cores quiet.
- **Nitro PHC** (`/dev/ptp0`, enabled via the `ena` module + `modprobe.d`) feeds
  chrony as a refclock for the one-way multicast clock sync; also the source of
  RX hardware timestamps.
- **No TX hardware timestamp on ENA** → the TX clock is `CLOCK_REALTIME`
  (`clock_gettime` before the send); no TSC.
- **No native L2 multicast on ENA** → multicast is carried in a plain unicast UDP packet tagged
  with an 8-byte **m2u** header and intercepted by `mcast.o` (preserving zero-copy).
- **Zero-copy AF_XDP is supported on ENA** in DRV mode (Nitro v4/v5).
- **Placement groups** — cluster PGs (single-AZ) minimize intra-cluster latency;
  the benchmark tags/positions nodes by PG/AZ/VPC/Region/Account.

## Control protocol (default port 12345, configurable via `AFXDP_CONTROL_PORT`)

Binary UDP, identical in AF_XDP and echo-mode:

| Opcode | Payload | Action |
|--------|---------|--------|
| `0x01` | 4B IP + 2B port | ADD destination (unicast) |
| `0x02` | 4B IP + 2B port | REMOVE destination |
| `0x03` | (none) | LIST — replies `[1B count][per dest: 4B IP + 2B port]` |
| `0x04` | 4B group IP | MCAST_JOIN (mcast mode) — dst inferred from sender |
| `0x05` | 4B group IP | MCAST_LEAVE (mcast mode) |

`replicator_ctl` is the CLI client for this protocol.
