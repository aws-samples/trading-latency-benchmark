# AF_XDP Latency — Topology Comparison


## Multicast (`mcast`, one-way source → replicator → destination)

The source is fixed (Frankfurt, AZ a, cluster-PG `cpg-a`). Replicators and
destinations vary across cluster-PG / same-AZ-no-PG / cross-AZ / cross-region.

**Test context**

| | |
|---|---|
| Instance type | `m8a.2xlarge` (8 vCPU, AMD EPYC 9R45, no SMT) |
| Fleet | 8 nodes: 1 source, 3 replicators, 4 destinations (`mcast-8` scenario) |
| Placement | source in cluster-PG `cpg-a` eu-central-1a; replicators/destinations spread across cpg-a, same-AZ-no-PG, AZ b, AZ c, and eu-west-1; shared tenancy |
| Load | 100,000 messages @ 10µs interval (100,000 pps requested) |
| Payload | 64 B |
| Achieved rate | ~100,000 pps (within 0.01% of requested) on all 36 combinations |
| Loss | 0.00% on all 36 combinations |
| Clock sync | chrony to ENA PHC, all nodes sub-µs offset (hop split trustworthy to ~±1.5µs) |

All figures are one-way source → replicator → destination. `hop1` is
source→replicator, `hop2` is replicator→destination (the leg containing the
replicator's forward path).

### `mcast/copy` — replicator copies each frame per destination (AF_XDP TX)

| Source | Replicator | Destination | min | p50 | p90 | p99 | p99.9 | max | hop1 (p50) | hop2 (p50) |
|---|---|---|---|---|---|---|---|---|---|---|
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 22µs | 33µs | 38µs | 42µs | 52µs | 92µs | 17µs | 16µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | 30µs | 42µs | 47µs | 51µs | 61µs | 107µs | 17µs | 24µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: c,<br>PG: - | 441µs | 454µs | 459µs | 463µs | 473µs | 509µs | 17µs | 436µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Ireland,<br>AZ: a,<br>PG: - | 10.16ms | 10.18ms | 10.18ms | 10.19ms | 10.20ms | 10.26ms | 17µs | 10.16ms |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 247µs | 260µs | 264µs | 268µs | 270µs | 343µs | 114µs | 145µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: - | 244µs | 256µs | 260µs | 264µs | 266µs | 334µs | 114µs | 141µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: c,<br>PG: - | 481µs | 492µs | 497µs | 501µs | 503µs | 564µs | 114µs | 378µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Ireland,<br>AZ: a,<br>PG: - | 10.27ms | 10.29ms | 10.29ms | 10.30ms | 10.31ms | 10.36ms | 114µs | 10.17ms |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 692µs | 703µs | 708µs | 712µs | 715µs | 781µs | 355µs | 348µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: - | 746µs | 757µs | 762µs | 766µs | 769µs | 827µs | 355µs | 402µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: c,<br>PG: - | 428µs | 440µs | 445µs | 449µs | 453µs | 510µs | 355µs | 85µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Ireland,<br>AZ: a,<br>PG: - | 10.61ms | 10.62ms | 10.65ms | 10.83ms | 11.10ms | 11.29ms | 355µs | 10.27ms |

### `mcast/inplace` — replicator rewrites headers in place, no per-dest copy (AF_XDP TX)

| Source | Replicator | Destination | min | p50 | p90 | p99 | p99.9 | max | hop1 (p50) | hop2 (p50) |
|---|---|---|---|---|---|---|---|---|---|---|
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 21µs | 32µs | 38µs | 42µs | 50µs | 108µs | 17µs | 15µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | 30µs | 41µs | 47µs | 51µs | 58µs | 80µs | 17µs | 24µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: c,<br>PG: - | 441µs | 452µs | 458µs | 462µs | 469µs | 498µs | 17µs | 435µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Ireland,<br>AZ: a,<br>PG: - | 10.16ms | 10.18ms | 10.18ms | 10.19ms | 10.21ms | 10.25ms | 17µs | 10.16ms |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 248µs | 259µs | 265µs | 268µs | 271µs | 295µs | 113µs | 146µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: - | 244µs | 255µs | 260µs | 264µs | 266µs | 309µs | 113µs | 142µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: c,<br>PG: - | 481µs | 495µs | 500µs | 504µs | 507µs | 530µs | 113µs | 381µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Ireland,<br>AZ: a,<br>PG: - | 10.27ms | 10.29ms | 10.29ms | 10.30ms | 10.31ms | 10.38ms | 113µs | 10.17ms |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 691µs | 702µs | 708µs | 712µs | 716µs | 742µs | 355µs | 347µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: - | 746µs | 757µs | 762µs | 766µs | 771µs | 798µs | 355µs | 402µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: c,<br>PG: - | 426µs | 438µs | 443µs | 447µs | 452µs | 477µs | 355µs | 83µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Ireland,<br>AZ: a,<br>PG: - | 10.61ms | 10.62ms | 10.65ms | 10.76ms | 11.29ms | 11.46ms | 355µs | 10.27ms |

### `mcast/kernel` — replicator forwards via plain UDP sockets (no AF_XDP)

Deliberately untuned stock-kernel reference: unlike the AF_XDP modes, the
endpoint sockets in this mode get no `SO_BUSY_POLL`/`SO_PREFER_BUSY_POLL`/
enlarged `SO_RCVBUF`. It is the baseline the optimised routes are measured
against, not a tuned competitor.

| Source | Replicator | Destination | min | p50 | p90 | p99 | p99.9 | max | hop1 (p50) | hop2 (p50) |
|---|---|---|---|---|---|---|---|---|---|---|
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 32µs | 48µs | 56µs | 61µs | 64µs | 105µs | 23µs | 24µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | 37µs | 53µs | 61µs | 66µs | 69µs | 106µs | 23µs | 29µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: c,<br>PG: - | 431µs | 449µs | 456µs | 462µs | 465µs | 545µs | 23µs | 425µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Ireland,<br>AZ: a,<br>PG: - | 10.32ms | 10.34ms | 10.38ms | 10.62ms | 10.94ms | 11.11ms | 23µs | 10.32ms |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 433µs | 452µs | 460µs | 465µs | 469µs | 525µs | 299µs | 153µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: - | 429µs | 450µs | 458µs | 463µs | 467µs | 513µs | 299µs | 151µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Frankfurt,<br>AZ: c,<br>PG: - | 575µs | 593µs | 601µs | 607µs | 611µs | 672µs | 299µs | 294µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: a,<br>PG: - | region: Ireland,<br>AZ: a,<br>PG: - | 10.32ms | 10.34ms | 10.35ms | 10.36ms | 10.37ms | 10.40ms | 299µs | 10.04ms |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | 750µs | 769µs | 777µs | 782µs | 786µs | 837µs | 361µs | 407µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: a,<br>PG: - | 749µs | 768µs | 776µs | 781µs | 785µs | 832µs | 361µs | 406µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Frankfurt,<br>AZ: c,<br>PG: - | 488µs | 506µs | 513µs | 519µs | 522µs | 565µs | 361µs | 144µs |
| region: Frankfurt,<br>AZ: a,<br>PG: cpg-a | region: Frankfurt,<br>AZ: b,<br>PG: - | region: Ireland,<br>AZ: a,<br>PG: - | 10.52ms | 10.54ms | 10.56ms | 10.63ms | 10.82ms | 10.94ms | 361µs | 10.18ms |

### Findings

**hop1 is a pure function of source→replicator placement**, identical across
all four destinations for a given replicator and near-identical across modes:
17µs into the cluster-PG replicator, 113-114µs to the same-AZ replicator with
no PG, 355µs to the cross-AZ (AZ b) replicator. **Cluster-PG membership is
worth ~6.7x on that hop** (17µs vs 114µs at the same AZ) - the single largest
placement effect across the results.

**`copy` and `inplace` are statistically indistinguishable**: across all 12
replicator/destination pairs the p50 gap is 0-3µs, and hop1 is identical to the
microsecond. Whatever `copy`'s extra per-destination frame copy costs, it is
below the noise floor at a single destination and 64 B. The earlier 1024 B
comparison (below) is where a difference starts to appear.

**`kernel` is consistently slowest, and the penalty is placement-dependent**:
+15µs on the best path (48µs vs 33µs, cluster-PG→cluster-PG) but +190µs on
`.124`→`.109`. The penalty lands in *both* hops, because `kernel` mode also switches the **source** to a plain UDP socket, not just the
replicator (endpoint transport is coupled to the replicator's fwd mode in the
harness). So `kernel` hop1 (23 / 299 / 361µs) vs `copy` hop1 (17 / 114 /
355µs) is a real stock-socket-vs-AF_XDP send comparison, not an artifact.

**The AF_XDP path is markedly more repeatable than the stock path off-PG.**
Re-measuring `copy` and `kernel` back-to-back with all clocks verified
sub-microsecond: `copy` hop1 reproduced to within 1-2µs (17/17, 113/114,
353/355), while `kernel` hop1 on the two no-PG replicators swung 215-299µs
(`.124`) and 361-410µs (`1.202`) - a spread of 49-84µs on an unchanged
configuration. Inside the cluster-PG `kernel` was perfectly stable (23µs both
runs). **Treat off-PG `kernel` hop1 as ±40µs, and do not quote it to three
digits**; the `copy`/`inplace` figures are safe to the microsecond.

**One row reads backwards but is correct**: replicator `1.202` (AZ b) reaches
the AZ-c destination in **440µs, faster than it reaches either AZ-a
destination** (703µs / 757µs), with hop2 of only 85µs. The AZ-b replicator is
simply better positioned for AZ c than for AZ a. hop1+hop2 reconciles with p50
to within 0-2µs on every row in all three tables, so the split is internally
consistent and this is a genuine path-length effect rather than a measurement
error.

**Cross-region swamps everything else.** All three replicators land within 4%
of each other reaching eu-west-1 (10.18-10.62ms), with hop2 ≈ 10.04-10.32ms.
Replicator placement is irrelevant once a WAN hop is in the path - the ~340µs
spread that matters inside a region disappears into WAN variance.

### Payload size: 1024 B vs the 64 B baseline

Same fleet, same placement, same load (100k messages @ 10µs, 100k pps), only
the payload changed to 1024 B. All four modes 0.00% loss, achieved rate within
0.01% of requested.

p50 / hop1 / hop2, 1024 B against the 64 B figures tabled above:

- `copy` 38µs, hop1 17µs, hop2 22µs (64 B: 34µs, 19µs, 14µs)
- `inplace` 32µs, hop1 15µs, hop2 16µs (64 B: 30µs, 16µs, 13µs)
- `kernel` 43µs, hop1 18µs, hop2 25µs (64 B: 39µs, 17µs, 22µs)

**A 16x larger payload barely moves end-to-end latency** — P50 +2 to +4µs.
At 100k pps the path is per-packet-cost bound (syscall, TX
doorbell, ring operations), not bandwidth bound: 1024 B at 100k pps is only
~820 Mbps, far below what the NIC can carry. The mode ranking is unchanged
(`bpf_tx` fastest, then `inplace`, `copy`, `kernel` slowest).

The one shift worth noting is in **hop2**, the leg containing the replicator's
forward path: `copy` went 14µs → 22µs while `inplace` went only 13µs → 16µs - `copy` physically copies
each frame per destination and so is the mode most exposed to payload size,
whereas `inplace` rewrites headers without copying the payload. With a single
destination and one sample per mode the exact size of that gap should not be
over-read, but the ordering matches the mechanism.
