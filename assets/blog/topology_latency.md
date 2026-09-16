# AF_XDP Latency — Topology Comparison

Raw data and explanations for AF_XDP unicast RTT and multicast one-way
benchmarks on `c7i.4xlarge` fleets.
100k messages @ 10k rate.

The "Destination" column is always relative to the source's cluster-PG/AZ/Region matching.

## Unicast (`ucast`, round-trip)

Pairs are bucketed by topology tier; for each tier, the row shown is the
single forwarding mode (`ucast/kernel` or `ucast/xdp`) with the better p50
at that tier — every column in the row comes from that same mode's
measurement, so values are internally consistent (no mixing kernel's p50
with xdp's max, etc.).

**Note on "min":** for the tiers of same-AZ and below, the
source sheet records p50/p90/p99/p99.9/max per pair, not a per-sample
minimum — "min" there is the lowest p50 observed across all pairs in that
tier, i.e. the single best-performing pair's median, a "best pair" indicator. The dedicated-host row IS a per-sample minimum (single-pair run).

| Topology tier | min | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| dedicated host (same physical server) | 17µs | 22µs | 25µs | 28µs | 29µs | 30µs |
| same-AZ + cluster-PG | 22µs | 40µs | 43µs | 47µs | 56µs | 105µs |
| same-AZ, different PG | 23µs | 44µs | 47µs | 51µs | 61µs | 127µs |
| cross-AZ (same region) | 157µs | 173µs | 176µs | 180µs | 203µs | 343µs |
| cross-region | 11.36ms | 11.78ms | 11.79ms | 11.80ms | 11.88ms | 11.96ms |


## Multicast (`mcast`, one-way source → replicator → destination)

**Live 8-node fleet** (`mcast-8` scenario: 1 source, 3 replicators at
different placements, 4 destinations at different placements/regions),
100k messages @ 10k rate (100µs interval), all pairs 0% loss. Figures below
are the lowest p50 of the two fan-out fwd modes (`copy`/`inplace`) per
replicator/destination pair, all columns from that same mode's row (no
cross-mode mixing); `kernel` mode is excluded from this selection since it
has no hop1/hop2 split.

| Replicator | Destination | min | p50 | p90 | p99 | p99.9 | max | hop1 (p50) | hop2 (p50) |
|---|---|---|---|---|---|---|---|---|---|
| same-AZ + cluster-PG | same-AZ, different PG | 181µs | 197µs | 205µs | 210µs | 215µs | 305µs | 24µs | 172µs |
| same-AZ + cluster-PG | same-AZ + cluster-PG* | 33µs | 50µs | 57µs | 62µs | 68µs | 171µs | 24µs | 25µs |
| same-AZ + cluster-PG | cross-AZ (same region) | 384µs | 401µs | 409µs | 415µs | 422µs | 514µs | 24µs | 377µs |
| same-AZ + cluster-PG | cross-region | 6.60ms | 6.61ms | 6.63ms | 6.68ms | 6.97ms | 7.57ms | 24µs | 6.59ms |
| same-AZ, different PG | same-AZ, different PG | 184µs | 201µs | 209µs | 214µs | 221µs | 310µs | 172µs | 29µs |
| same-AZ, different PG | same-AZ + cluster-PG* | 243µs | 261µs | 268µs | 274µs | 280µs | 374µs | 172µs | 88µs |
| same-AZ, different PG | cross-AZ (same region) | 386µs | 405µs | 413µs | 419µs | 427µs | 516µs | 172µs | 233µs |
| same-AZ, different PG | cross-region | 5.92ms | 5.94ms | 5.95ms | 5.96ms | 5.98ms | 6.18ms | 172µs | 5.77ms |
| cross-AZ, different PG | same-AZ, different PG | 602µs | 618µs | 626µs | 631µs | 642µs | 1.54ms | 312µs | 306µs |
| cross-AZ, different PG | same-AZ + cluster-PG* | 632µs | 646µs | 652µs | 655µs | 663µs | 731µs | 316µs | 330µs |
| cross-AZ, different PG | cross-AZ (same region) | 395µs | 411µs | 419µs | 425µs | 437µs | 1.34ms | 312µs | 99µs |
| cross-AZ, different PG | cross-region | 6.09ms | 6.11ms | 6.12ms | 6.14ms | 6.28ms | 7.04ms | 312µs | 5.80ms |

\* "same-AZ + cluster-PG" always refers to the same destination
(cluster-PG `cpg-a`, eu-central-1a) — its own placement never changes; what
changes per row is which replicator is relaying to it.