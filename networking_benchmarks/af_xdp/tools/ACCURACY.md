# Measurement accuracy

How one-way and round-trip latency numbers from this suite are kept honest:
what could bias them, how each candidate error term was sized, and what is
reported as a result.

Unicast RTT (`rtt`) is a **round-trip** probe on a single host's clock, so
offset cancels by construction - clock accuracy plays no part in it. Everything
below concerns the **multicast one-way** path (`mcast_send` → replicator →
`mcast_receive`), where the two timestamps come from different hosts and a
clock disagreement is indistinguishable from real transit time unless it is
measured and reported.

## 1. Method: why differencing changes what a term costs you

The offset between two hosts is recovered by **reciprocal probing** - measure
A→C and C→A, then take half the difference:

```
A,C - src/dst nodes via R - replicator node
d: wire delay
o: time offset from UTC
b_tx,A b_rx,C: bias TX,RX 

owd_AC = d + o + b_tx,A + b_rx,C
owd_CA = d − o + b_tx,C + b_rx,A

(owd_AC − owd_CA)/2 = o      when the hosts are identical, bias cancels
(owd_AC + owd_CA)/2 = d + b_tx + b_rx    bias survives in the sum
```

A constant, symmetric bias (stamp placement, kernel path length) cancels in
the *difference* and barely affects a recovered offset, but survives in the
*sum* and does affect absolute delay. Time-varying noise cancels in neither -
it is what the difference actually recovers. This is why several terms below
turned out far less important than their raw magnitude suggests, and why one
modest-looking term dominates.

## 2. What was measured, and what it came back as

Reference signal: a one-way hop of roughly 16us in a same-AZ cluster placement
group. Shares below are against that.

| Term | Figure | Share of signal | Verdict |
|---|---|---|---|
| Absolute PHC error bound vs UTC | 22.4-22.7us, common-mode across nodes to 0.3us | 0% | Not a relative error - a magnitude, not a signed offset, so it cannot be differenced out. Kept only as a tripwire: a frozen/absent reading means a node fell off the PHC onto NTP fallback |
| Stamp-placement effect on offset | 53ns | ~0.3% | Cancels in differencing |
| Receive wake latency (NAPI vs userspace stamp) | 344ns net | ~2% | Cancels in differencing |
| Apparatus soundness (`CLOCK_MONOTONIC` cross-check) | 80-120ns | ~0.7% | Verified sound - the four timestamps involved are not lying to each other |
| Path asymmetry + accumulated stamp error (triangle closure) | 536ns p99 | ~3.4% | Bounded, same-AZ CPG only |
| Sampler measurement floor | ~1.0us | ~6% | Hard limit - no reported band can be tighter than this |
| **Clock wander (chrony's discipline loop)** | **1908ns MAD, ±3-7us on a seconds timescale** | **20-45%** | **Dominant, by more than an order of magnitude. This is what gets reported** |
| Hardware-vs-wire RX stamp bias (`b_rx`) | unresolved | unknown | Genuinely open - see §5 |
| TX submit-to-wire cost (`b_tx`) | unmeasured, est. 1-3us | 6-19% | Cancels in the offset (differencing), survives only in absolute delay; no hardware TX timestamp on this fleet to measure it directly |

Everything measurable except wander came back at or below the ~1us sampler
floor. That is the whole reason the design is one continuous clock-wander
sampler and one reported band, rather than a multi-transport, multi-stamp
architecture.

### Why wander dominates and cannot be removed

Not the PHC itself: chronyd tracks the PHC refclock to single-digit-to-low
hundreds of nanoseconds, and the PHC error bound is common-mode across nodes
(above). What wanders is chronyd's **discipline of the system clock against
that PHC** - a control loop with servo overshoot and jitter in when it samples
and corrects, running independently per host on its own oscillator.

Two hosts' wander is well-modeled as independent (separate oscillators,
separate discipline loops, correlated only through a shared reference sitting
above this noise): predicted pairwise MAD via `sqrt(2) x` per-node MAD lands
at 84-90% of what's actually observed, which is a reasonable fit for an upper
bound. Wander is **zero-mean**, so:

- **A median over many samples is sound.** Wander averages out; comparative
  claims between configurations survive.
- **A single sample, or any tail statistic, does not.** At p99 the wander is a
  material fraction of the reported interval.

This is why batch size changes the confidence in a reported **median**, not
the physical magnitude of wander itself - a larger sample count makes the
median converge faster to the true value, but does not shrink the underlying
clock noise, and tail statistics (p99, max) remain exactly as wander-limited
at any batch size.

## 3. What is reported

Every wander-sampled campaign gets a measured band, not an assumed one:

```
one-way total: 16.2us (median, n=3000)   clock-wander band: +/-2.2us (upper bound)
                                          measurement floor: 1.0us
```

Per participating node, continuously for the campaign's duration,
`phcsample` (`tools/phcsample.c`) samples that node's PHC against its own
`CLOCK_REALTIME`, emitting per-sample offset, the sampler's own read bracket,
and the driver's PHC error bound. From that, per node:

- `wander_med_ns`, `wander_mad_scaled_ns` (raw MAD x 1.4826, the
  normal-consistency-scaled robust sigma estimator - **always use the scaled
  form**, never raw MAD or mean/sd; these distributions are heavy-tailed
  enough that mean/sd misread this fleet's own reference data by conflating a
  one-sided-tail mean with a -15ns median)
- `wander_p1_ns`/`wander_p99_ns`, `wander_excursion_pct` (share beyond 3us)
- `bracket_med_ns` - the sampler's own measurement floor

The pairwise band for a measurement between nodes A and C:

```
band_ns = sqrt(wander_mad_scaled_A^2 + wander_mad_scaled_C^2)
```

When a campaign is bidirectional, the reciprocal offset's spread across
rounds is preferred over this quadrature estimate - it is the pairwise wander
measured directly, on the same timescale as the reported value, with no
independence assumption.

## 4. Gates - reject-only, never correct

The band describes clock behavior; it never adjusts a sample. A run fails one
of these gates outright rather than being published with a misleading number.

| Gate | Checks | Rejects when |
|---|---|---|
| G1 - sample coverage | Sampler ran for (most of) the whole window | `sample_count` < 90% of expected |
| G2 - sentinel rate | ioctl failures are not dominant | `sentinel_count` >= 5% |
| G3 - no step | No clock step mid-campaign | any step detected - a step invalidates every sample straddling it |
| G4 - PHC counters clean | Driver error/skip counters unchanged across the window | any counter moved, or ioctl error ratio too high |
| G5 - clock source unambiguous | Exactly one source selected (PHC or NTP fallback), no duplicate refclock lines | source ambiguous or absent |
| G6 - excursion norm | Excursion rate is near the established fleet baseline (~21%) | far above baseline (flagged, not rejected - a chronyd-health signal) |

The multicast one-way orchestration adds two more, both hard rejects rather
than gates on the reported band itself:

- **Clock-sync gate** - a run whose replicator/destination sampled any
  negative-hop clock-skew reading is rejected outright: even one such sample
  means the hop split is not trustworthy for that run.
- **Loss gate** (`max_loss_pct`, default 2%) - `rtt`/`mcast_receive` compute
  percentiles only from datagrams that returned, so a lossy run reports the
  distribution of its surviving, non-random subset. A rejected pair is
  recorded as a failure, not published as a plausible-looking wrong number.

## 5. What remains open

- **`b_rx`, the true wire-to-NAPI-stamp interval, is unmeasured.** A
  packet-size sweep (serialisation time should grow with frame size if a
  stamp is wire-referenced) found the gap between ENA's "hardware" RX
  timestamp and the software NAPI stamp is flat across sizes - the hardware
  stamp is completion-referenced, not wire-referenced. No method currently
  available on this hardware isolates true wire time.
- **`b_tx` is unmeasured** - no hardware TX timestamp exists on this fleet's
  instance family. It cancels in the recovered offset (differencing) and
  affects only the absolute one-way delay, not comparative claims.
- **PHC-to-PHC offset, isolated from system-clock discipline noise, is not
  separated from wander.** Both PHCs on this fleet trace to the same regional
  time-sync reference, so any residual PHC-to-PHC divergence is expected to be
  small and GNSS/infrastructure-related (receiver noise, antenna delay
  variance) - nanosecond-scale, well below the ~1.9us wander and the ~1.0us
  sampler floor. Not worth further dedicated measurement: resolving it
  precisely would not move any figure in §2 above the floor.
- **Everything above was measured at one point in configuration space**:
  same-AZ, identical instance family on both ends, one region, one tuning
  profile. The cancellations this document relies on (`b_tx,A = b_tx,B`, etc.)
  depend on both ends being identical - a heterogeneous instance pair loses
  that guarantee, and the uncancelled difference lands directly in the
  offset. Path asymmetry (closure residual) and common-mode PHC cancellation
  are both expected to grow outside a single region/AZ, and neither has been
  measured there yet.
- **Whether running a probe client perturbs a node's own clock discipline.**
  An unresolved anomaly: a serve-only node showed a markedly lower excursion
  rate than nodes running a client, in a way that did not look load-
  proportional. Not yet instrumented further.
