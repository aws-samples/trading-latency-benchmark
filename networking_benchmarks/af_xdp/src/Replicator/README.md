# `src/Replicator/` — the AF_XDP packet replicator engine

The replicator is a multi-queue, zero-copy UDP **fan-out** engine built on
**AF_XDP**. It receives UDP packets at the earliest driver hook (XDP), replicates
each one to N registered destinations, and transmits every copy back out the NIC
— all without traversing the kernel network stack on the hot path, and without
copying the payload when it can be avoided.

Two datapaths share this engine:

- **Unicast (`ucast`)** — a peer sends UDP to the replicator's `listen_ip:port`;
  the replicator echoes/fans it out. Used by the `rtt` round-trip probe.
- **Multicast-over-unicast (`mcast` / “m2u”)** — a source sends a plain unicast
  UDP frame carrying an 8-byte `m2u{magic,group}` tag; the replicator fans the
  group out to every destination that joined it. Used for one-way fan-out latency.

---

## Data path (one packet, end to end)

The diagram is the source of truth for the numbered steps; every step is
explained in detail in [Step-by-step](#step-by-step-the-datapath-explained) below.
Left column = the pipeline stage; right column = the concrete tech / kernel
feature / data structure used at that stage.

```
                       ┌───────────────────────────────────────────────────────────────┐
  SOURCE HOST          │  STAGE                           TECH / FEATURE / DATA STRUCT │
  ───────────          └───────────────────────────────────────────────────────────────┘
  mcast_send / rtt ──▶ [0] build frame + AF_XDP TX        ucast: Eth/IP/UDP/payload
                            (off-box, see tools/)         mcast: Eth/IP/UDP/ m2u{magic,group} /payload
                                                          TX-only AF_XDP (rtt --xdp-tx) or kernel sendto
        
        │ wire (ENA, single-AZ cluster PG / cross-AZ / cross-region)
        ▼
╔════════════════════════════════ REPLICATOR HOST ════════════════════════════════╗
║                                                                                  ║
║  [1] NIC ingress                       ENA (Nitro), MTU 3498, RSS→queue 0        ║
║      frame DMA'd into RX descr ring    (ethtool -X equal 1), coalescing off,     ║
║                                        ENA hard IRQ pinned to isolated CPU       ║
║        │                                                                         ║
║        ▼                                                                         ║
║  [2] XDP hook  (DRV + ZEROCOPY)        eBPF prog: ucast.o | mcast.o at the       ║
║      parse Eth→IPv4→UDP (+m2u tag)     earliest driver hook (before skb alloc)   ║
║      lookup {ip|group, port}           config_map (BPF_MAP_TYPE_ARRAY, 16 slots) ║
║        │                                                                         ║
║        ├─ no match ───────────────▶ XDP_PASS → kernel UDP stack (SSH, etc.)      ║
║        │                                                                         ║
║        ├─ match, fwd_map[slot].enabled (mcast kernel mode only):                 ║
║        │      rewrite L2/L3/L4 in place, recompute IP csum ─▶ XDP_TX  ═════════╗ ║
║        │      (fwd_map, BPF_MAP_TYPE_ARRAY; NIC re-TX; userspace bypassed)     ║ ║
║        │                                                                       ║ ║
║        └─ match (default) ─▶ bpf_redirect_map(xsks_map, rx_queue_index)        ║ ║
║        ▼                                                                       ║ ║
║  [3] AF_XDP RX redirect (zero-copy)    xsks_map (BPF_MAP_TYPE_XSKMAP) →        ║ ║
║      frame already sits in UMEM        this queue's XSK. Descriptor {addr,len} ║ ║
║      RX region; only a descr is queued pushed on the RX ring. NO copy.         ║ ║
║        │                                UMEM: 4096×4096B frames; RX=2048..4095 ║ ║
║        ▼                                                                       ║ ║
║  [4] userspace RX drain                XdpSocket::receive() — NAPI busy-poll   ║  ║
║      (packet-processor thread,         (SO_BUSY_POLL + recvfrom MSG_DONTWAIT); ║  ║
║       pinned to isolated CPU+1)        reads RX ring → offsets[]/lengths[];    ║  ║
║        │                                _mm_pause() spin when empty (no sleep) ║  ║
║        ▼                                batch = 256 (mcast) | 64 (ucast)       ║  ║
║  [5] per-frame loop                    __builtin_prefetch next frame;          ║  ║
║      stats + replicatePacket()         per-queue+total atomics (memory_order   ║  ║
║        │                                _relaxed); recycleFrames() → FILL ring ║  ║
║        ▼                                                                       ║  ║
║  [6] RX stamp + parse                  clock_gettime(CLOCK_REALTIME)→          ║  ║
║      extractUdpPayload / …M2u          replicator_ns (mcast, bswap→BE) written ║  ║
║      → payload_data, len, group_nbo    into payload[m2u+16]; m2u magic="M2CU"  ║  ║
║        │                                                                       ║  ║
║        ▼                                                                       ║  ║
║  [7] per-group fan-out set             getCachedGroupDestinations(group_nbo):   ║  ║
║      → const vector<Destination>&      thread_local cache, 100ms TTL, lock-free ║  ║
║        │                                (rebuilt from group_destinations_ under ║  ║
║        │                                 destinations_mutex_ only on refresh)   ║  ║
║        ▼                                                                       ║  ║
║  [8] drain TX completions (1×/batch)   pollTxCompletions() → COMPLETION ring;   ║  ║
║        │                                frees TX frames for reuse               ║  ║
║        ▼                                                                       ║  ║
║  [9] for each of K destinations: build + queue one TX frame                     ║  ║
║      ┌─────────────────────────────────────────────────────────────────────┐  ║  ║
║      │ mac==broadcast? (ARP unresolved) ─▶ kernel sendto (output_socket_)   │  ║  ║
║      │ fwd_mode=inplace & last dest ─▶ patchHeadersInPlace + forwardFrame   │  ║  ║
║      │                                  InPlace() (re-TX the RX frame, 0-cp)│  ║  ║
║      │ else (copy, default): sendSinglePacketDirect()                        │  ║  ║
║      │    reserveTxRing(1,&idx) ─▶ tx_addr=(idx & 2047)*4096                 │  ║  ║
║      │    createUdpPacket(): Eth(cached MAC)/IP(cached saddr,RFC1071 csum)/  │  ║  ║
║      │      UDP + payload copy into TX frame (0..2047)                       │  ║  ║
║      │    stamp replicator_tx_ns @ off 74 (mcast) ─▶ setTxDescriptor ─▶      │  ║  ║
║      │    submitTxRing(1)                                                    │  ║  ║
║      └─────────────────────────────────────────────────────────────────────┘  ║  ║
║        │                                TX ring + UMEM TX region (frames 0..2047)║  ║
║        ▼                                                                       ║  ║
║  [10] driver kick (1×, after all K)    requestDriverPoll() → sendto(MSG_DONTWAIT)║  ║
║        │                                / needs_wakeup; NIC pulls TX ring       ║  ║
║        ▼                                                                       ║  ║
║  [11] NIC egress ── K unicast frames ──┴────────────────────────────◀═════════╝  ║
╚══════════════════════════════════════════════════════════════════════════════════╝
        │ wire ×K
        ▼
  DEST HOST(s)   [12] mcast_receive (AF_XDP RX, its own mcast.o + config_map[0]) OR
                      a kernel UDP socket (rtt echo). Stamp rx_ns (CLOCK_REALTIME);
                      compute hop1 = replicator_ns−ts_ns, hop2 = rx_ns−replicator_tx_ns,
                      total = rx_ns−ts_ns.
```

## Replication paths — kernel / copy / zero-copy (and the flags that select them)

"How a packet is replicated" is governed by **three independent flag axes**. Two
words are overloaded, so read carefully:

- **`--echo-mode` (CLI)** ≠ **`REPLICATOR_FWD_MODE=kernel` (env)**. The former
  turns AF_XDP *off entirely* (a plain-UDP echo backend); the latter is an
  *in-kernel XDP_TX forward* that still uses the XDP datapath.
- **socket zero-copy** (`zero_copy` CLI positional → `XDP_FLAGS_ZERO_COPY`) ≠
  **zero-copy forward** (`REPLICATOR_FWD_MODE=inplace`). The former is how the NIC
  moves bytes into/out of the UMEM; the latter is whether the fan-out reuses the
  RX frame. They are orthogonal and can be combined.

### Axis 1 — datapath backend (CLI, `Main.cpp`)

| Flag | Backend | XDP? | Notes |
|------|---------|:---:|-------|
| `--echo-mode <ip> <port>` | `KernelEcho.cpp` (`run_echo_mode`) | no | Plain kernel UDP `recvfrom`→`sendto` echo/fan-out. No UMEM/rings/BPF, no root. CI/containers/macOS. |
| *(default)* `<iface> <ip> <port> …` | the `Replicator` engine | yes | Loads `ucast.o`/`mcast.o`; steps [1]–[11] of the datapath diagram. |

### Axis 2 — AF_XDP **socket** zero-copy (CLI positional `zero_copy`, default `true`)

Resolved in `Init.cpp` into the XDP bind/attach flags — this controls the
**NIC ↔ UMEM** transfer, independent of the forward mode below:

| `zero_copy` | Flag | Effect |
|-------------|------|--------|
| `true` / `1` (default) | `XDP_FLAGS_ZERO_COPY` | NIC DMAs frames **directly into UMEM** (RX) and out of it (TX) — no driver copy. Requires ENA ZC (Nitro v4/v5). |
| `false` / `0` | `XDP_FLAGS_DRV_MODE` | Native driver mode **without** ZC: the driver copies between its own buffers and the UMEM. Fallback when ZC is unavailable. |

### Axis 3 — **replication / forward mode** (env `REPLICATOR_FWD_MODE`, `Init.cpp`)

This is the axis the question is really about: once a packet is **matched at the
XDP hook [2]**, how are the K copies produced? The frame forks three ways
(`kernel` is mcast-only — it needs `mcast.o`'s `fwd_map`):

```
  matched frame sitting in a UMEM RX frame  (from XDP hook, step [2])
                              │
   ┌──────────────────────────┼───────────────────────────────────────────────┐
   │ FWD_MODE=kernel          │ FWD_MODE=copy   (DEFAULT)     │ FWD_MODE=inplace │
   ▼  (mcast only)            ▼                              ▼
 ══ KERNEL (XDP_TX) ══      ══ PACKET COPY ══             ══ ZERO-COPY (inplace) ══
 stays in step [2]:         steps [3]→[8] (AF_XDP RX,     steps [3]→[8] (same as copy),
 mcast.o reads fwd_map      busy-poll, parse, fan-out),   then step [9] per dest:
 [slot], rewrites L2/L3/    then step [9] PER DEST:        • LAST dest: patchHeaders
 L4 in the RX frame,          • reserveTxRing → pick a       InPlace() rewrites the RX
 recomputes IP csum, and      TX frame (0..2047)             frame's Eth/IP/UDP in place
 XDP_TX's it back out         • createUdpPacket() writes     (recompute IP csum) and
 the NIC.                     Eth/IP/UDP + memcpy() the      forwardFrameInPlace() re-TXes
 USERSPACE NEVER RUNS.        payload into that TX frame     THAT SAME UMEM frame
 (steps [3]–[11] skipped)     • setTxDescriptor → submit     • other K−1 dests: packet copy
                                                             (the RX frame can TX only once)
 ─ payload copies: 0         ─ payload copies: K            ─ payload copies: K−1
 ─ userspace hops:  0        ─ works for ANY K              ─ works for ANY K
 ─ 1 dest / group            ─ most robust (default)        ─ falls back to copy on
 ─ no replicator_ns split      ─                              patch/submit failure
   (BPF has no CLOCK_REALTIME)
```

Regardless of mode, an **unresolved-ARP** destination (broadcast MAC) is always
sent via the **kernel `sendto` fallback** (`sendToDestinationFallback`, step [9]),
because ENA drops broadcast-dst frames; the 100 ms cache refresh re-resolves the
MAC and restores the fast path.

| `REPLICATOR_FWD_MODE` | Diagram steps | Payload copies (per RX pkt, K dests) | Where it forks | Constraints |
|------|-------------|:---:|------|------|
| `copy` *(default / unset)* | [1]→[11] full | **K** | step [9], per destination | any K; most robust |
| `inplace` (zero-copy) | [1]→[8], then [9] split | **K−1** | step [9]: last dest reuses RX frame | any K; RX frame spent on last dest |
| `kernel` (XDP_TX) | [1]→[2] only | **0** | step [2] in `mcast.o` | **mcast only**; 1 dest/group; no `replicator_ns` split |

### When each mode shines

- **`copy` (default) — best median at low fan-out; the safe default.** Builds a
  fresh TX frame per destination, so the RX frame is recycled immediately and TX
  comes from a hot, pre-warmed TX-pool frame. Lowest per-packet setup cost, works
  for **any K**, and needs no `mcast.o`/`fwd_map`. Its cost is **K payload copies**
  and userspace fan-out jitter (worst tail). *Use for:* small fan-out (K≈1–few),
  small payloads, or whenever robustness matters more than the last µs. In the
  matrix it had the lowest min/p50 (26/42 µs) but the widest tail (p99 69, max 229).
- **`inplace` (zero-copy) — throughput/copy-bound high fan-out.** Re-transmits the
  RX UMEM frame itself for the last destination (patch headers, no payload copy);
  the other K−1 still copy. Saves **exactly K−1 payload copies**, so the benefit
  scales with **K and payload size** — it is a *bandwidth/CPU* optimization, not a
  latency one. At **K=1 it is counter-productive**: it saves 0 copies but adds
  RX-frame-borrow bookkeeping (the frame is withheld from the fill ring until TX
  completes, returned via completion polling), which *raised* the floor in the
  matrix (min 40 vs 26, p50 55 vs 42). *Use for:* **large K and/or large payloads**
  where avoiding K−1 memcpys per packet relieves a CPU/memory-bandwidth bottleneck.
- **`kernel` (XDP_TX) — best tail latency; single dest per group.** `mcast.o`
  rewrites L2/L3/L4 from `fwd_map` and re-transmits in the kernel — steps [3]–[11]
  and all of userspace are skipped, so there is no RX-ring dequeue, fan-out, or
  TX-ring hop to jitter. **Limited to one destination per group** (one `fwd_target`
  slot) and it cannot stamp `replicator_ns` (BPF has no `CLOCK_REALTIME`), so the
  receiver reports total one-way only (no hop split). *Use for:* **1→1 relay / point
  forwarding where tail latency is paramount.** In the matrix it had the tightest
  tail (p99 55, p99.9 60, max 153) at a p50 (43) on par with copy.

Rule of thumb: **K=1 & tail-critical → `kernel`; K=1 & simplest → `copy`;
K≫1 (or large payloads) → `inplace`.**


### Combined example (systemd, `bake-ami.sh` → `start-replicator.sh`)

`/etc/default/replicator` sets `REPLICATOR_MODE` (kernel|ucast|mcast) and
`REPLICATOR_ZEROCOPY` (socket ZC, Axis 2). `REPLICATOR_FWD_MODE` (Axis 3) is an
extra env you export to pick the copy strategy, e.g.:

```
REPLICATOR_MODE=mcast REPLICATOR_ZEROCOPY=true REPLICATOR_FWD_MODE=inplace \
  replicator eth0 224.0.31.50 5000 true --mcast
#            └Axis1: AF_XDP┘ └group┘ └port┘ └Axis2 ZC┘ └mcast.o┘
#   → ZC AF_XDP sockets (NIC↔UMEM no-copy) + inplace fan-out (K−1 payload copies)
```

---

## Step-by-step: the datapath explained

### [0] Source builds and transmits (off-box, `tools/`)
- **ucast:** the peer (`rtt`) sends a normal UDP datagram to the replicator's
  `listen_ip:listen_port`. It may send via `sendto()` or, with `--xdp-tx`, via a
  TX-only AF_XDP sender (inlined in `tools/rtt.cpp`) that builds the whole
  `Eth|IPv4|UDP|payload` frame in userspace and stamps the TX time immediately
  before submitting — removing the kernel TX stack from the measured leg.
- **mcast:** `mcast_send` builds `Eth|IPv4(proto 17)|UDP|m2u{magic="M2CU"(0x4D324355), group}|payload`
  and AF_XDP-TXs it as a **plain unicast** frame to the replicator's private IP
  (`-D`). ENA has no L2 multicast; the group lives inside the m2u tag. The
  payload's first 24 bytes carry `{seq(8), ts_ns(8), replicator_ns(8=0)}`.

### [1] NIC ingress — ENA
The frame arrives on the ENA physical NIC and is DMA'd into the driver's RX
descriptor ring. The AMI bakes several latency-critical NIC settings (see
`deploy/cdk/scripts/bake-ami.sh`): **MTU 3498** (ENA native XDP requires
single-page frames), **RSS indirection collapsed to queue 0**
(`ethtool -X … equal 1`) so all matching traffic lands on the one queue the
replicator binds, **interrupt coalescing off** (`rx-usecs=0 tx-usecs=0`), and the
**ENA hard IRQ pinned to the first isolated CPU** while apps run on isolated+1 —
so the IRQ never preempts the busy-poll thread.


### [2] XDP hook — `ucast.o` / `mcast.o` (kernel, DRV + ZEROCOPY)
The eBPF program runs at the earliest driver hook, before an `skb` is allocated.
Loaded by `Init.cpp` via `XdpSocket::loadXdpProgram()` in native driver mode with
zero-copy. Both programs:
1. Bounds-check and parse **Ethernet → IPv4 → UDP** (require `ETH_P_IP`,
   `IPPROTO_UDP`, `ihl*4 ∈ [20,60]`).
2. **`mcast.o` only:** also parse the 8-byte `m2u` tag and require
   `magic == M2CU`; read the 4-byte `group`.
3. Scan **`config_map`** (`BPF_MAP_TYPE_ARRAY`, 16 slots; `target_ip==0` = unused,
   scanned with `continue` not `break` because slots are sparse):
   - `ucast`: match `iph->daddr == target_ip && udp->dest == target_port`.
   - `mcast`: match `group == target_ip && udp->dest == target_port`.

   These 16 slots are *filter entries*, not destinations. In **ucast** only slot 0
   is ever populated (the single `listen_ip:listen_port`), so unicast intercepts
   **one** address; in **mcast** each slot is a joined group, so up to **16 groups**
   concurrently. Either way the matched traffic fans out to an *unbounded* set of
   destinations (tracked separately, not in `config_map`).
4. **Decision:**
   - **No match →** `XDP_PASS` (packet continues to the normal kernel stack — this
     is how SSH keeps working). `ucast.o` has a side-path here: if the payload
     carries the `rtt --xdp-rx` magic it stamps `bpf_ktime_get_ns()` into the
     payload and zeroes the UDP checksum before passing up.
   - **Match + `fwd_map[slot].enabled` (mcast `kernel` mode) →** rewrite L2 (dst/src
     MAC), L3 (dst/src IP + recomputed 20-byte IPv4 checksum), L4 (ports, UDP
     csum=0) from the `fwd_target`, then **`XDP_TX`** — the NIC re-transmits the
     frame; userspace is never involved.
   - **Match (default) →** `bpf_redirect_map(&xsks_map, ctx->rx_queue_index, …)`.

### [3] AF_XDP redirect into the RX ring (zero-copy)
`bpf_redirect_map` into **`xsks_map`** (`BPF_MAP_TYPE_XSKMAP`, keyed by RX queue
index → the bound AF_XDP socket) delivers the frame to userspace **without a
copy**: the frame already lives in a UMEM RX frame, and only a descriptor
`{addr, len}` is pushed onto the socket's **RX ring**. The UMEM is one contiguous
mmap'd region of `DEFAULT_UMEM_FRAMES = 4096` frames × 4096 B; **TX uses frames
0…2047**, **RX uses frames 2048…4095** (`UMEM_RX_FIRST_FRAME_IX = 2048`). Each XSK
has four rings: **FILL** (userspace → kernel: empty RX frames), **RX**
(kernel → userspace: filled), **TX** (userspace → kernel: to send), **COMPLETION**
(kernel → userspace: sent, now free).

### [4] Userspace RX drain — `XdpSocket::receive()` (NAPI busy-poll)
Each RX queue has a dedicated **packet-processor thread** (`Core.cpp::start()`),
pinned to an isolated CPU (`setCpuAffinity`). It calls `XdpSocket::receive()`,
which drains the RX ring into pre-allocated `offsets[]`/`lengths[]` vectors
(cache-aligned; `rx_batch = 256` in mcast, `64` in ucast). On an empty ring it
issues `recvfrom(fd, …, MSG_DONTWAIT)`; with `SO_PREFER_BUSY_POLL` + `SO_BUSY_POLL`
set on the XSK fd this runs the **NAPI poll in this pinned thread**, so the
NIC→ring fill isn't waiting on the deferred hard IRQ. This is what makes hop-1
latency independent of `gro_flush_timeout` (the baked 10µs `gro_flush_timeout` +
`napi_defer_hard_irqs` is only the backstop for windows when the thread is busy
fanning-out rather than polling). When nothing is received the loop spins on
`__builtin_ia32_pause()` — **no `sleep`, no `poll()`** — for lowest latency.

### [5] Per-frame loop — `processPacketsForQueue()`
For each received descriptor it prefetches the next frame
(`__builtin_prefetch`), bumps per-queue and total counters
(`std::atomic<uint64_t>`, `memory_order_relaxed`, cache-line aligned to avoid
false sharing), and calls `replicatePacket()`. After the batch it calls
`recycleFrames()` to return the consumed RX frames to the **FILL ring** so the
kernel can refill them.

### [6] RX timestamp + payload parse — `replicatePacket()` / `extractUdpPayload*()`
- **RX stamp (mcast):** at entry — *before* parsing — capture
  `clock_gettime(CLOCK_REALTIME)`, byte-swap to big-endian → `replicator_ns`. Doing
  it first keeps the hop-1/hop-2 split from charging parse cost to hop-1.
- **Parse:** `extractUdpPayload()` dispatches to `extractUdpPayloadMulticast()` (mcast) or
  the plain path (ucast). It returns `payload_data`, `payload_len`, and `group_nbo`:
  - **ucast:** `group_nbo = iph->daddr` (the listen IP); defence-in-depth re-checks
    `daddr == listen_ip_nbo_`. `payload_data` points past `Eth/IP/UDP`.
  - **mcast:** validates the m2u magic, reads `group_nbo` from the tag, and sets
    `payload_data` to the **start of the m2u header** so the fan-out re-emits
    `[m2u | app-payload]` verbatim. `payload_len` includes the 8-byte tag.
- **Write RX stamp (mcast):** copy `replicator_ns` into the app-payload slot at
  `payload_data + M2U_HDR(8) + 16` (i.e. `replicator_ns` field of the 24-byte app
  header), for the receiver's hop split.

### [7] Per-group fan-out set — `getCachedGroupDestinations(group_nbo)`
Returns a `const std::vector<Destination>&` for the group — **no copy, no lock on
the hot path**. Backed by a `thread_local ThreadLocalDestCache` with a **100 ms
TTL**. On expiry, `updateDestinationCache()` snapshots `group_destinations_`
(mcast) / `all_destinations_` (ucast) under `destinations_mutex_` once, rebuilds
the per-group vectors, and re-resolves any still-broadcast MACs (ARP self-heal).
Empty set → the packet is dropped (return 0).

### [8] Drain TX completions — `pollTxCompletions()` (once per batch)
Before queuing any TX, reclaim the **COMPLETION ring** once for the whole fan-out
batch (not once per destination) — freeing TX frames/ring slots for all K copies.

### [9] Build + queue one TX frame per destination
Per `Destination` in the fan-out set:
- **Unresolved ARP guard:** if `dest.mac` is still all-`0xFF` (broadcast), ENA
  would drop the frame, so route via the **kernel fallback** `sendto()` on
  `output_socket_` (`sendToDestinationFallback`). The 100 ms cache refresh
  re-resolves the MAC and restores the fast path automatically.
- **`inplace` mode, last destination:** `patchHeadersInPlace()` rewrites the RX
  frame's Eth/IP/UDP in place (recomputing the IP checksum, stamping
  `replicator_tx_ns`) and `forwardFrameInPlace()` submits that same UMEM frame to
  the TX ring — **zero payload copy**. Falls back to copy on failure.
- **`copy` mode (default):** `sendSinglePacketDirect()`:
  1. `reserveTxRing(1, &tx_idx)` — reserve one TX ring slot (retry once after
     `requestDriverPoll()` + `pollTxCompletions()`; throw → kernel fallback if
     still full, so packets are never silently dropped).
  2. Derive the frame address from the ring index: `tx_addr = (tx_idx & 2047) * 4096`
     (power-of-2 mask `DEFAULT_TX_FRAMES-1`) — guarantees a frame not in flight.
  3. `createUdpPacket()` builds `Eth|IPv4|UDP|payload` into that TX frame: dst MAC
     from ARP, **src MAC/IP from the cached interface values** (`cached_iface_mac_`,
     `cached_iface_saddr_nbo_` — parsed once at `initialize()`, no per-packet
     `inet_aton`), IP checksum per RFC 1071, `IP_DF` set, `id=0` (RFC 6864). In
     mcast the `payload` already begins with the m2u tag, so the destination
     receives the same `Eth|IP|UDP|m2u|payload` framing.
  4. Stamp `replicator_tx_ns` (CLOCK_REALTIME) at frame offset **74**
     (`Eth14+IP20+UDP8+m2u8+24`) for the receiver's hop-2 split.
  5. `setTxDescriptor(tx_idx, tx_addr, len)` then `submitTxRing(1)`.

### [10] Driver kick — `requestDriverPoll()` (once, after all K)
A single wakeup for the whole batch (avoids K−1 redundant `needs_wakeup` checks /
`sendto` syscalls). The driver pulls the TX ring and DMAs the frames out.

### [11] NIC egress
K unicast frames leave the ENA NIC, one per destination.

### [12] Destination (off-box)
- **mcast:** `mcast_receive` runs its own `mcast.o` + `config_map[0]` seeded with
  the group, AF_XDP-RXs the frame, stamps `rx_ns` (CLOCK_REALTIME) at XSK dequeue,
  and reads `ts_ns`/`replicator_ns`/`replicator_tx_ns` back out of the payload to
  report **hop-1** (`replicator_ns−ts_ns`, source→replicator), **hop-2**
  (`rx_ns−replicator_tx_ns`, replicator→dest), and **total** (`rx_ns−ts_ns`).
  Needs cross-host clock sync (AWS Time Sync / PHC, ~µs).
- **ucast:** the `rtt` client receives the echo on a kernel UDP socket
  (`SO_TIMESTAMPING`, single-host `CLOCK_REALTIME`) and computes round-trip — no
  clock sync needed.

---

## Unicast workflow (end to end)

1. **Startup** (`Main.cpp` → `Init.cpp::initialize`): bind `listen_ip:listen_port`,
   load **`ucast.o`** (DRV+ZC), create one `XdpSocket` per queue
   (`setupUMem`/`bind`/`registerXskMap`), open the control socket (port 12345) and
   the kernel fallback `output_socket_`, and cache the interface IP/MAC.
2. **Seed the filter** (`configureXdpProgram`): write `{listen_ip_nbo_, listen_port}`
   into `config_map[0]`; slots 1–15 go to the free pool (unused in ucast).
3. **Register destinations:** a peer sends `CTRL_ADD_DESTINATION` (or `rtt`
   auto-registers) → `addDestination()` ARP-resolves the MAC (3 retries) and adds
   to `all_destinations_`.
4. **Datapath:** steps [1]–[11]. `group_nbo` is the listen IP; the fan-out set is
   `all_destinations_` keyed by `listen_ip_nbo_`. For the `rtt` echo case the set is
   just the probe client.

## Multicast (m2u) workflow (end to end)

1. **Startup:** `--mcast` → load **`mcast.o`**; `initialize()` seeds the inner group
   into `config_map` immediately (`addGroupDynamic(listen_ip_nbo_)`) so the BPF
   filter matches before any join. `REPLICATOR_FWD_MODE` selects copy/inplace/kernel.
2. **Join** (`Control.cpp`): each destination sends `CTRL_MCAST_JOIN [0x04][4B group]`;
   the replicator infers the destination IP from the UDP source address, ARP-resolves
   it, `addGroupDynamic(group)` (allocates a `config_map` slot, ref-counted), and adds
   it to `group_destinations_[group]`. In `kernel` mode it also writes the
   `fwd_target` into `fwd_map[slot]` (`updateKernelFwdTarget`).
3. **Source stream:** `mcast_send` emits `m2u`-tagged unicast to the replicator.
4. **Datapath:** steps [1]–[11] (`copy`/`inplace`) or [1]–[2] (`kernel`, `XDP_TX`).
   `group_nbo` comes from the m2u tag; fan-out is per-group from
   `group_destinations_`.
5. **Leave:** `CTRL_MCAST_LEAVE` decrements the ref-count; the last leave zeroes the
   `config_map` (and `fwd_map`) slot and returns it to the free pool.

## Upstream control forwarding (optional)

With `--ctrl <group:port> --producer <ip:port>`, `Control.cpp` joins a control
multicast group (`joinControlMulticastGroup`) and a background thread
(`handleUpstreamControl`) forwards every received control datagram to the upstream
producer — decoupling destination-side control from the producer's location.

---

## File reference

| File | Role |
|------|------|
| `Replicator.hpp` | The single `Replicator` class declaration + `Destination` struct, control opcodes, `MAX_QUEUES`, the `ThreadLocalDestCache`, and all member state. |
| `Internal.hpp` | Shared implementation preamble for every `Replicator/*.cpp` TU: the common system-header block (`<bpf/*>`, `<netinet/*>`, …) and the `DEBUG_TX/DEBUG_PACKET` print macros. |
| `Main.cpp` | Entry point. CLI parse (`interface listen_ip port [zero_copy] [--mcast] [--ctrl] [--producer]`), root check, mode dispatch, signal handling, the 10 s stats loop, and the `--echo-mode` branch (calls `run_echo_mode`). |
| `Core.cpp` | Object lifecycle (ctor/dtor/move), `start()`/`stop()` (spawns/joins the per-queue processor threads + control + upstream threads), `isRunning`, `getStatistics`/`printStatistics`, and CPU affinity (`setCpuAffinity`, `initializeCpuCores`). |
| `Init.cpp` | One-time setup: pick + `loadXdpProgram` (`ucast.o`/`mcast.o`), create/bind/register one `XdpSocket` per queue, open control + fallback sockets, cache interface IP/MAC, resolve `REPLICATOR_FWD_MODE`; `configureXdpProgram()` seeds `config_map` and the free-slot pool. |
| `Groups.cpp` | Dynamic multicast group lifecycle against `config_map` — `addGroupDynamic`/`removeGroupDynamic` (ref-counted slot alloc/free under `group_mutex_`) — and `updateKernelFwdTarget` (writes `fwd_map` for `kernel` mode). |
| `Control.cpp` | The binary UDP control protocol: `handleControlProtocol` (recv loop) + `processControlMessage` (ADD/REMOVE/LIST/MCAST_JOIN/MCAST_LEAVE), plus upstream forwarding (`setUpstreamControl`, `joinControlMulticastGroup`, `handleUpstreamControl`). |
| `Destinations.cpp` | The `Destination` type (ctor validates IP, defaults MAC to broadcast; `operator<`), the canonical registry (`addDestination`/`removeDestination`/`getDestinations`), and the thread-local fan-out cache (`dest_cache_` definition, `getCachedGroupDestinations`, `updateDestinationCache`). |
| `DataPath.cpp` | The hot path: `processPacketsForQueue` (RX busy-poll loop), `replicatePacket` (RX stamp → parse → fan-out → TX), `extractUdpPayload`/`extractUdpPayloadMulticast`, `sendToDestinationWithQueue`/`sendToDestinationFallback`, `sendSinglePacketDirect`, `createUdpPacket`, `patchHeadersInPlace`. |
| `Net.cpp` | Address/interface helpers: `parseIpAddress`/`formatIpAddress`, `getInterfaceIp`/`getInterfaceMac` (ioctl), and ARP/gateway resolution (`getDestinationMac` + file-local `lookupArpEntry` `/proc/net/arp`, `lookupGateway` `/proc/net/route`, `triggerArpResolution`). |
| `XdpSocket.hpp` / `XdpSocket.cpp` | AF_XDP socket wrapper: UMEM allocation, the four rings, `setupUMem`/`bind`/`registerXskMap`, `receive`/`recycleFrames`, `reserveTxRing`/`setTxDescriptor`/`submitTxRing`/`requestDriverPoll`/`pollTxCompletions`, `forwardFrameInPlace`, and static XDP-program load/unload/map-fd helpers. Frame constants live here. |
| `KernelEcho.cpp` | The `--echo-mode` backend (`run_echo_mode`): a plain-UDP echo server speaking the same control protocol, so the logic runs with no root/XDP (containers, CI, macOS, the `tests/` suite). |

Shared header used across the engine and `tools/`: `src/common/ControlPort.hpp`
(`AFXDP_CONTROL_PORT`, default 12345), included as `"common/ControlPort.hpp"`.

---

## Key data structures

- **UMEM + 4 rings** (`XdpSocket`): 4096×4096 B frames; TX=0…2047, RX=2048…4095;
  FILL/RX/TX/COMPLETION; `TX_BATCH_SIZE=64`. Zero-copy DRV mode.
- **`config_map`** (BPF `ARRAY`, 16 slots): the XDP **filter** table of
  `{target_ip, target_port}` tuples to redirect into the AF_XDP socket — **not** a
  destination list. `MAX_GROUPS = 16` bounds *concurrent filter entries* (the
  per-packet unrolled scan must stay verifier-cheap), not fan-out targets.
  **ucast:** only **slot 0** is populated (`{listen_ip, listen_port}`) — a
  replicator has a single listen address; slots 1–15 stay unused. **mcast:**
  dynamic + ref-counted, `target_ip` is the group → **up to 16 groups at once**.
  Fan-out destinations are unbounded (see `all_destinations_`/`group_destinations_`).
- **`xsks_map`** (BPF `XSKMAP`, 256): RX queue index → AF_XDP socket; target of
  `bpf_redirect_map`.
- **`fwd_map`** (BPF `ARRAY`, 16; mcast only): parallel to `config_map`; `fwd_target`
  = {dmac,smac,dip,sip,dport,sport,enabled} for kernel `XDP_TX` forwarding.
- **`group_slots_` / `group_ref_counts_` / `free_slots_`** (`group_mutex_`): group
  NBO → slot, join ref-count, and the free-slot pool.
- **`all_destinations_` / `group_destinations_`** (`destinations_mutex_`): the
  canonical registry (IP→`Destination`) and per-group membership.
- **`ThreadLocalDestCache`** (`dest_cache_`): per-thread, cache-aligned, 100 ms TTL;
  the lock-free fan-out set on the hot path.
- **`Destination`**: `{ip_address, port, sockaddr_in addr, uint8_t mac[6]}` — MAC
  ARP-resolved at registration (broadcast until resolved → kernel fallback).

## Latency instrumentation (mcast, 24-byte app header)

| Bytes | Field | Stamped by | Clock |
|-------|-------|-----------|-------|
| 0–7  | `seq` | source (`mcast_send`) | — |
| 8–15 | `ts_ns` | source, just before TX | `CLOCK_REALTIME` |
| 16–23 | `replicator_ns` | replicator at RX ([6], payload off `m2u+16`) | `CLOCK_REALTIME` |
| (frame off 74) | `replicator_tx_ns` | replicator just before submit ([9]) | `CLOCK_REALTIME` |

Receiver computes **hop-1** = `replicator_ns − ts_ns`, **hop-2** =
`rx_ns − replicator_tx_ns`, **total** = `rx_ns − ts_ns`. (In `kernel` forward mode
`replicator_ns` is left as the source's zero — BPF has only `CLOCK_MONOTONIC` — so
only the total is reported.)

## Threading & CPU model

- One **packet-processor thread per RX queue**, pinned to a **dedicated isolated
  CPU** (`initializeCpuCores` + `setCpuAffinity`). Busy-polls; never sleeps.
- One **control thread** (`handleControlProtocol`) on port 12345, unpinned.
- One optional **upstream-control thread** (`handleUpstreamControl`).
- Statistics are per-queue `std::atomic` counters (relaxed, cache-aligned).

**Core selection (dynamic, IRQ-aware, size/metal-agnostic).** `initializeCpuCores`
picks the poll-thread core(s) in priority order:
1. **`REPLICATOR_CPUS`** env — explicit list (`"2,3"` or `"2-5"`); full operator control.
2. **`/sys/devices/system/cpu/isolated` minus its first entry.** The AMI pins the
   ENA hard IRQ to the **first isolated CPU** (`bake-ami.sh` `ena-irq-affinity`), so
   the poll thread is placed on the **next** isolated core(s) — one dedicated core
   per queue, **never sharing a core with the IRQ**. This scales automatically from
   a `c7i.4xlarge` (min supported) up to bare metal without code changes.
3. **Fallback** (no isolation configured): cores `1..num_queues`.

Resulting layout on the replicator node (with the baked `isolcpus=1-4`):
core 0 = OS, **core 1 = ENA hard IRQ** (isolated[0]), **core 2 = replicator poll**
(isolated[1]); the `rtt`/mcast clients then take cores 3+ (see `run_ucast.yaml`
`send_cpu`/`recv_cpu`), so IRQ, poll, and the measurement threads are all on
**distinct** cores. (Earlier the poll thread and IRQ collided on core 1 — the source
of `--xdp-tx` tail jitter; the skip-first-isolated rule fixes that.) On bare metal
the same code path applies — no VM/metal fork; you only size `isolcpus` (and,
optionally, `REPLICATOR_CPUS`) for the instance. `nosmt` keeps siblings off the
isolated cores.
