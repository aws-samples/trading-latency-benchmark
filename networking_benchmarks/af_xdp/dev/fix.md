# mcast benchmark: why AF_XDP measured no faster than plain kernel sockets

Investigation log from validating `REPLICATOR_FWD_MODE=kernel` live on a 4-node
`m8a.2xlarge` fleet (source + replicator + 2 destinations, one cluster PG,
eu-central-1a). The kernel baseline initially measured *faster* than the AF_XDP
`copy`/`inplace` modes, which contradicts the architecture. Four separate errors
were responsible. Three are in the benchmark harness, one is a fleet tuning
interaction that penalises only the AF_XDP path.

## Final verified result

Fix deployed, **deferral at production values** (`napi_defer_hard_irqs=2`,
`gro_flush_timeout=10µs`), load-matched, zero loss, single destination, run via
`afxdpctl run mcast copy,kernel`:

| Rate | copy p50 | kernel p50 | copy hop1 / hop2 | kernel hop1 / hop2 |
|---|---|---|---|---|
| 50k pps | **38 µs** | 44 µs | 21 / **17** | 21 / **25** |
| 100k pps | **41 µs** | 47 µs | 23 / **17** | 25 / **22** |

AF_XDP is ~6 µs (~14 %) faster end-to-end at both rates, and the gap sits in
**hop2** - the leg containing the replicator's forward path - exactly where it
should. Every earlier "kernel wins" / "dead heat" number was an artifact.

### How the destination fix progressed (same `copy` config throughout)

| `mcast_receive` RX driver | deferral | copy p50 | copy hop2 |
|---|---|---|---|
| original (`poll()` gated on `needs_wakeup`) | ON | 45 µs | 24 µs |
| attempt 1 - `recvfrom(MSG_DONTWAIT)` | ON | 45 µs | 22 µs |
| **attempt 2 - `poll(fd,1,0)` unconditional** | **ON** | **37 µs** | **16 µs** |
| reference: original, deferral off | OFF | 36 µs | 11 µs |

The working fix reaches with deferral *enabled* what previously required
disabling deferral fleet-wide. Attempt 1 is recorded because it disproves the
intuitive remedy: `recvfrom` does not drive AF_XDP busy-poll.

---

## Error 1 — deferred-IRQ config taxes ONLY the AF_XDP RX path (~10 µs/hop)

The dominant effect, and the reason the two paths appeared equal.

The fleet sets `napi_defer_hard_irqs=2` and `gro_flush_timeout=10µs`
(`bake-ami.sh`). The intent is a low-latency deferred-NAPI regime where the
application drives NAPI itself via busy-poll instead of waiting for hard IRQs.

`mcast_receive`'s AF_XDP loop does **not** drive NAPI:

```cpp
uint32_t rcvd = xsk_ring_cons__peek(&rx, BATCH, &idx_rx);
if (rcvd == 0) {
        if (xsk_ring_prod__needs_wakeup(&fq))
                poll(&pfd, 1, timeout * 1000);
        continue;
}
```

`xsk_ring_cons__peek` is a pure userspace ring read - no syscall. The `poll()`
is reached only when the *fill* ring needs a wakeup, which is not the common
case on an empty RX ring. `SO_BUSY_POLL` only takes effect inside a socket
syscall, so on this path the socket option is set but never exercised: the
frame's arrival in the RX ring still waits on the 10 µs `gro_flush_timeout`
deferral timer, once per packet.

The kernel path does not have this problem. `recvfrom()` is a socket syscall, so
`SO_BUSY_POLL` engages, NAPI is polled inline, and the deferral is bypassed.

Net effect: the AF_XDP receive path pays a ~10 µs per-hop penalty that the plain
socket path does not - almost exactly cancelling AF_XDP's bypass advantage.

### Evidence

Identical test (kernel replicator, AF_XDP endpoints), only `napi_defer_hard_irqs`
and `gro_flush_timeout` changed to 0 on the replicator and destination:

| | p50 | hop1 (kernel `recvfrom` RX on replicator) | hop2 (AF_XDP RX on destination) |
|---|---|---|---|
| deferral ON | 48 µs | 24 µs | **23 µs** |
| deferral OFF | 39 µs | 25 µs | **13 µs** |

`hop2` improved by exactly the 10 µs deferral value; `hop1` did not move. The
asymmetry is the proof: the hop whose RX is AF_XDP was paying the deferral, the
hop whose RX is a busy-polled kernel socket was not.

## Error 2 — endpoint transport is coupled to the replicator's fwd mode

`control_plane/agent/runner.go`, in **both** `RunMcastSend` and
`RunMcastReceive`:

```go
if p.Variation == "kernel" {
        flags += " -k"
}
```

So `modes=["kernel"]` switches *all three* nodes to plain sockets, and
`modes=["copy"]` switches all three to AF_XDP. The campaign therefore compares
whole-stack AF_XDP against whole-stack kernel across three hops, not the
replicator's forward mode. The replicator is one hop of three, so the variable
under test is diluted roughly threefold and both endpoint legs move at the same
time as the replicator.

This is a legitimate experiment ("end-to-end plain-socket baseline", which is
what the roadmap plan specified for `-k`), but it is *not* the experiment that
answers "how does the replicator's fwd mode affect latency", and the reports
were being read as if it were.

## Error 3 — runs were at/above the sender's saturation point, at unequal loads

`mcast_send` cannot reach the requested rate at small intervals, and the two
transports saturate at *different* rates, so nominally-equal runs offered
different load:

| Requested | copy achieved | kernel achieved |
|---|---|---|
| 200k pps (`-i 5`) | ~91k pps (100k pkts in 1.1 s) | ~143k pps (100k pkts in 0.7 s) |
| 50k pps (`-i 20`) | ~42k pps (2.4 s) | ~42k pps (2.4 s) |

At 200k requested the two modes ran 57 % apart in offered load - those latency
numbers are not comparable at all. Even the 50k request fell ~17 % short.

Latency also rose monotonically with offered load while the modes converged
(5k pps: kernel 36 / copy 45 -> 100k pps: kernel 46 / copy 45), which is the
signature of a queueing-dominated regime rather than a per-packet-cost regime.
The 50k-pps runs *were* load-matched, so the tie there was real - it was caused
by Error 1, not by load.

## Error 4 — `Variation` never reached `CmdMcastSend` (fixed)

`orchestrator.go`'s `CmdMcastSend` dispatch omitted `Variation`, while
`CmdMcastReceive` set it correctly. Since `RunMcastSend` gates `-k` on
`p.Variation`, the source always sent via AF_XDP TX regardless of the mode under
test, so kernel mode's hop1 measured the AF_XDP send path. Fixed in commit
`89cb6a0` by adding `Variation: mode` to that dispatch.

---

## Ruled out with evidence (not assumption)

- **ARP-unresolved fallback.** Hypothesis: `copy` was silently routing through
  `sendToDestinationFallback()` (a plain kernel `sendto`) because destinations
  still had the broadcast MAC, making `copy` and `kernel` the same code path.
  **Disproven:** ARP is resolved during runs (the empty table seen afterwards was
  just ~60 s idle expiry), and `UdpOutDatagrams` on the replicator moved
  695096 -> 695120 (**+24**) across a 100k-packet / 200k-fan-out `copy` run, not
  +200000. AF_XDP TX genuinely bypassed the UDP stack.
- **Mode switching not taking effect.** The replicator journal shows real
  alternation between `Forward mode: kernel (plain UDP sockets)` /
  `Replicator initialized successfully (kernel fwd mode, no AF_XDP)` and
  `Forward mode: copy` / `Loading XDP program: ./xdp/mcast.o`.
- **NIC tuning wrong.** Verified on replicator and destination: `rx-usecs=0`,
  `tx-usecs=0`, adaptive RX off, RSS indirection table all zeros (single ring 0),
  ENA IRQs 67-70 pinned to CPU 0, replicator poll thread pinned to isolated
  CPU 1, `isolcpus=1-4`, `nohz_full=1-4`, `net.core.rmem_max=16777216`.

---

## Fix: make `mcast_receive`'s AF_XDP path actually drive NAPI

The goal is for the AF_XDP RX loop to poll NAPI inline on an empty ring, so the
`gro_flush_timeout` deferral is never the delivery mechanism - matching what the
replicator's own RX path already does and what `bake-ami.sh`'s deferred-NAPI
tuning assumes. Do **not** fix this by disabling deferral fleet-wide: `README.md`
warns `gro_flush_timeout=0` strands packets in busy-poll gaps and produces
multi-second bursts, and it would also regress the replicator/`rtt` paths that
currently depend on the deferred regime.

### 1. Drive the busy-poll with `poll()`, not `recvfrom()` (implemented)

**`poll()` is the AF_XDP busy-poll entry point** - `xsk_poll()` calls
`sk_busy_loop()` when `SO_BUSY_POLL` is active on the fd. `recvfrom()` does
**not** reliably enter that path on an XSK fd. This was established
experimentally, not from documentation: a first attempt using
`recvfrom(MSG_DONTWAIT)` on every empty peek was measured and **did not remove
the penalty** (see the attempt table below).

Applied in `tools/mcast_receive.cpp`, on an empty `xsk_ring_cons__peek`:

```c
if (rcvd == 0) {
        poll(&pfd, 1, 0);                  /* one non-blocking busy-poll pass */
        if (xsk_ring_prod__needs_wakeup(&fq))
                recvfrom(xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
        ...
        continue;
}
```

Key points:
- `poll()` must be called **unconditionally** on an empty peek. The original code
  did call `poll()`, but gated behind `if (xsk_ring_prod__needs_wakeup(&fq))` -
  a *fill-ring* condition that is usually false on an empty RX ring, so the poll
  almost never ran. That gate was the whole bug.
- Zero timeout keeps it a single non-blocking pass so the loop keeps spinning.
- `recvfrom` is retained strictly for the fill-ring wakeup it is meant for.
- The original `poll(&pfd, 1, timeout * 1000)` (up to 30 s) was also the loop's
  implicit give-up mechanism. Removing it required adding an explicit idle
  deadline, or a stalled run spins an isolated core at 100% until the external
  `timeout` fires.

### 2. Same fix applied to the replicator - correctness only, no measured gain

`XdpSocket::receive()` had the identical ineffective pattern (`recvfrom` on empty
peek, with a comment claiming it drove NAPI). Changed to the same `poll()`-based
form, and confirmed busy-poll is genuinely configured there:

```
XSK busy-poll (fd=9): SO_PREFER_BUSY_POLL=ok SO_BUSY_POLL(50us)=ok
                      SO_BUSY_POLL_BUDGET(64)=ok readback=50us
```

**It produced no measurable improvement** (copy p50 37 -> 38 us, hop1 21 -> 21).
That is expected in hindsight: **hop1 was never deferral-bound**. At 10k pps with
deferral fully *disabled*, hop1 was still 25 us - it is dominated by the source's
TX doorbell plus wire (see the hop1 section below), which a replicator-side RX
change cannot affect. hop2 is where the AF_XDP RX deferral actually lived, which
is why fixing the *destination* mattered and fixing the replicator did not.

The change is kept for correctness, not speed: it removes a latent dependency on
`gro_flush_timeout` keeping its current value, and makes the replicator's RX
genuinely busy-polled as its comment always claimed.

### 3. `mcast_send` needs no fix - verified

`mcast_send` is TX-only: `rx_size = 0` and the RX ring is `nullptr` in
`xsk_socket__create`. There is no RX ring, so no busy-poll RX path exists and no
deferral penalty can apply. Its per-packet `needs_wakeup` -> `sendto()` doorbell
is the *required* TX kick under `XDP_USE_NEED_WAKEUP`, not a defect.

### 4. Deferral settings restored

`napi_defer_hard_irqs` / `gro_flush_timeout` were returned to the baked values
(`2` / `10000`) on the replicator and destination before the acceptance test, so
the results below hold under production tuning. Disabling deferral fleet-wide was
only ever a diagnostic: `README.md` warns `gro_flush_timeout=0` strands packets
in busy-poll gaps.

### 5. Harness changes so these errors cannot recur silently

- **Decouple endpoint transport from replicator fwd mode.** Add an explicit
  knob (e.g. `endpoint_transport: afxdp|kernel` on `McastMatrixParams`) instead
  of deriving `-k` from `Variation`. Keep the current all-kernel behaviour
  available as an explicit choice, since that end-to-end baseline is a valid
  experiment - just no longer the *only* thing `kernel` can mean.
- **Report achieved vs requested rate per run.** `mcast_receive` already prints
  elapsed time and packet count; surface achieved pps in the JSON/telemetry and
  flag any run that lands below ~90 % of requested. Every saturated run above
  would then have been self-evident rather than needing manual log archaeology.
- **Record the NIC tuning state in run metadata.** `napi_defer_hard_irqs`,
  `gro_flush_timeout`, `rx-usecs`, RSS ring count. A run whose tuning differs
  from the baked baseline is not comparable to one that matches it, and there is
  currently no way to tell from the report.

---

## Follow-up investigation: why hop1 looked disproportionately large

Observed in the isolation runs (copy replicator, AF_XDP endpoints, deferral off):
hop1 = 25 µs against hop2 = 11 µs, despite both being one wire hop plus one RX.
hop2 additionally contains the replicator's processing and fan-out, so hop1
being more than double hop2 was backwards.

### Finding: hop1's excess is a per-packet TX doorbell on an idle ring, and it
### disappears with offered rate

Rate sweep, identical setup, only `mcast_send -i` varied:

| Requested rate | p50 | **hop1** | hop2 |
|---|---|---|---|
| 10k pps (`-i 100`) | 36 µs | **25 µs** | 11 µs |
| 50k pps (`-i 20`) | 35 µs | **18 µs** | 17 µs |
| 100k pps (`-i 10`) | 36 µs | **17 µs** | 18 µs |

hop1 *decreases* as rate increases - the opposite of queueing - so it carries a
fixed per-packet cost that amortises away under load. At 50-100k pps hop1 and
hop2 converge to ~17-18 µs each, which is a sensible symmetric one-way cost for
a same-cluster-PG hop. **There is no hop1/hop2 asymmetry at adequate rate; the
anomaly is specific to the low-rate regime.**

Mechanism, from `tools/mcast_send.cpp`'s hot path:

```cpp
uint64_t ts_be = htobe64_(now_ns());              // <-- hop1 starts here
__builtin_memcpy(frame + TS_OFF, &ts_be, 8);
...
xsk_ring_prod__submit(&tx_ring, 1);
if (xsk_ring_prod__needs_wakeup(&tx_ring))
        sendto(xsk_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);   // doorbell syscall
```

The socket is bound with `XDP_USE_NEED_WAKEUP`, so the kernel raises the
needs-wakeup flag whenever the driver has no TX work in flight. At 100 µs
inter-packet gaps the TX ring is idle on every packet, the flag is essentially
always set, and each packet pays a full `sendto()` doorbell (plus the driver
picking the descriptor up cold). All of that happens *after* `ts_ns` is stamped,
so it is charged to hop1. At 10-20 µs gaps the driver is still draining, the flag
is frequently clear, the kick amortises, and hop1 drops by ~8 µs.

This is a measurement-placement artifact, not a datapath fault: the stamp sits
before the submit (deliberately, to mark "handed to the transport"), so any
transport-side wakeup cost lands inside hop1 rather than being attributed to the
sender.

### Clock skew: real but minor, and in the opposite direction

Initially suspected as the cause. Measured instead:

| Node | chrony `System time` | RMS offset |
|---|---|---|
| replicator | 1.183 µs **slow** of NTP | 1.558 µs |
| destination | 0.166 µs fast of NTP | 0.121 µs |

The replicator being *slow* understates `replicator_ns`, which makes hop1 appear
**smaller** and hop2 larger - it works against the observed asymmetry rather
than explaining it, and ~1.2 µs cannot account for a 7-14 µs gap. (The agent's
reported `clock_offset_us` of 3.4 µs for this node is a different, noisier
measurement - `chronyc makestep`+burst achieved offset - not the steady-state
tracking offset.)

What the skew *does* mean: the replicator's timestamps carry roughly an order of
magnitude more jitter than the destination's (RMS 1.56 µs vs 0.12 µs, and it
syncs to PHC0 while running 7.3 ppm frequency-slow). **Any hop1/hop2 split on
this fleet is therefore only trustworthy to about ±1.5 µs.** Differences smaller
than that between modes should not be reported as real. End-to-end totals are
unaffected, since source and destination clocks agree to ~0.2 µs and the
replicator's offset cancels out of `rx_ns - ts_ns`.

### Consequences for the benchmark

- **Do not compare hop1 across runs at different offered rates.** hop1 is
  rate-sensitive by ~8 µs across 10k-100k pps purely from TX doorbell behaviour.
  This also explains the earlier orchestrator runs showing hop1 = 26-27 µs at
  200k-requested: that is the saturated regime, not a slower path.
- **Prefer >= 50k pps for any hop-split comparison**, where the TX ring stays hot
  and hop1/hop2 are symmetric and stable.
- **Re-sync or exclude the replicator's clock** before treating a hop split as
  precise; `chronyc makestep` on that node, or report hop values with a stated
  ±1.5 µs uncertainty.
- Consider stamping `ts_ns` *after* the doorbell rather than before, or recording
  both, so the wakeup cost can be attributed explicitly instead of silently
  inflating hop1 at low rates. This is a measurement-design choice and should be
  decided deliberately - moving the stamp changes the meaning of hop1 and breaks
  comparability with all historical runs.
