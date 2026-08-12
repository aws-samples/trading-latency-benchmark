# scenarios/

Pre-built fleet specifications for common benchmark topologies.

Load via: `--context scenario=<subdir>/<name>` (e.g. `--context scenario=ucast/az-cpg-3`)

## ucast/ — Unicast RTT benchmarks

All nodes act as peers (role defaults to `replicator`). Measures point-to-point latency.

Cost/hr figures are rough and assume the default instance type (`c7i.2xlarge`);
use the AWS pricing calculator for current rates.

| File | Topology | Instances | Cost/hr |
|------|----------|-----------|---------|
| `az-cpg-3` | Same AZ (a), single cluster PG | 3 | ~$1.07 |
| `xaz-xcpg-10` | Cross AZ (a+b): 2 CPGs + 2 SPGs + 2 unplaced | 10 | ~$3.57 |
| `xregion-4` | Cross region (us-east-1 ↔ eu-west-2), 1 cluster PG × 2 per region | 4 | ~$1.43 + transfer |

## mcast/ — Multicast fan-out benchmarks

Explicit roles: source → replicator → destination(s). Measures fan-out latency.

| File | Topology | Instances | Cost/hr |
|------|----------|-----------|---------|
| `xaz-cpg-3` | Cross AZ: source+replicator cluster PG (az a), destination in az b | 3 | ~$1.07 |
| `xregion-3` | Cross region: source+replicator CPG, destination in eu-west-2 | 3 | ~$1.07 + transfer |
| `xregion-8` | Cross region: source+replicator us CPG, 2 CPGs + 1 SPG in eu-west-2 | 8 | ~$2.85 + transfer |

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
