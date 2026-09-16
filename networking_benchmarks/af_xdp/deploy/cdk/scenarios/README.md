# scenarios/

Pre-built fleet specifications for common benchmark topologies.

Load via: `--context scenario=<subdir>/<name>` (e.g. `--context scenario=ucast/az-cpg-3`)

## ucast/ — Unicast RTT benchmarks

All nodes act as peers (role defaults to `replicator`). Measures point-to-point latency.

**Instance type: `c7i.4xlarge`** (set per entry). Unicast co-locates the replicator
poll thread + rtt sender + rtt receiver on the *same* node, needing 5 dedicated cores
(OS, ENA IRQ, replicator poll, rtt send, rtt recv) — more than a `c7i.2xlarge` has
after `nosmt` (4 online). Core pinning is derived **dynamically at runtime** from the
isolated set (`run_ucast.yaml` `auto_pin` + the replicator's `initializeCpuCores`), so
the same AMI/code adapts to any size. Cost/hr assumes `c7i.4xlarge` (~2× the 2xlarge
rate); use the AWS pricing calculator for current rates.

| File | Topology | Instances | Cost/hr |
|------|----------|-----------|---------|
| `az-cpg-2` | Same AZ (a), single cluster PG — minimal ucast pair | 2 | ~$1.43 |
| `az-cpg-3` | Same AZ (a), single cluster PG | 3 | ~$2.14 |
| `xaz-xcpg-10` | Cross AZ (a+b): 2 CPGs + 2 SPGs + 2 unplaced | 10 | ~$7.14 |
| `xregion-4` | Cross region (us-east-1 ↔ eu-west-2), 1 cluster PG × 2 per region | 4 | ~$2.85 + transfer |

## mcast/ — Multicast fan-out benchmarks

Explicit roles: source → replicator → destination(s). Measures fan-out latency.

**Instance type: `c7i.2xlarge`** (set per entry). Each node runs a single busy-poll app
(`mcast_send` | replicator poll | `mcast_receive`), so 3 dedicated cores (OS, ENA IRQ,
app) fit a 2xlarge — the cost-efficient choice. Pinning is derived dynamically, same as
ucast.

| File | Topology | Instances | Cost/hr |
|------|----------|-----------|---------|
| `az-cpg-3` | Same AZ (a): source + replicator + destination in one cluster PG | 3 | ~$1.07 |
| `xregion-3` | Cross region: source+replicator CPG, destination in eu-west-2 | 3 | ~$1.07 + transfer |
| `xregion-8` | Cross region: source+replicator us CPG, 2 CPGs + 1 SPG in eu-west-2 | 8 | ~$2.85 + transfer |
| `xregion-xaz-xpg-6` | Source+replicator CPG (az a) → destinations across az a/b + eu-west-2 | 6 | ~$2.14 + transfer |

## Custom scenarios

Create any JSON file with the `FleetEntry` schema:

```json
[
  {"count": 2, "pgType": "cluster", "pgName": "group-a"},
  {"count": 2, "pgType": "cluster", "pgName": "group-b", "az": "b"},
  {"count": 3, "pgType": "spread", "region": "eu-west-2"}
]
```

### FleetEntry fields

| Field | Default | Description |
|-------|---------|-------------|
| `type` | `c7i.2xlarge` | EC2 instance type |
| `count` | `1` | Number of instances |
| `role` | `replicator` | `source`, `replicator`, `destination` |
| `az` | `a` | AZ suffix or full name |
| `pgType` | none | `cluster`, `spread`, `partition` |
| `pgName` | auto | Group label. With `pgType`, entries sharing a name share one placement group. Recorded per-node in the `FleetManifest` (even without `pgType`), so it doubles as a reporting/clustering/disambiguation label. Optional — entries with no placement group may omit it (they report as "no PG"). |
| `region` | stack region | Triggers cross-region VPC peering |

> **Naming:** placement-group members share a `pgName` — e.g. `cpg-a`, `spg-b-1`,
> `us-a-cpg-1`, `eu-a-cpg-dst`. It is recorded per-node in the `FleetManifest`, so
> reports can group/disambiguate by placement group without parsing instance names.
> Entries with no `pgType` may omit `pgName` and appear as "no PG".

Load via `--context fleet=@path/to/custom.json`
