# scenarios/

Pre-built fleet specifications for common benchmark topologies.

Load via: `--context scenario=<name>` (e.g. `--context scenario=ucast-az-cpg-3`)

## ucast/ — Unicast RTT benchmarks

All nodes act as peers (role is irrelevant for ucast — the orchestrator uses all online nodes). Measures point-to-point latency.

**Instance type: `c7i.4xlarge`** (set per entry). Unicast co-locates the replicator
poll thread + rtt sender + rtt receiver on the *same* node, needing 5 dedicated cores
(OS, ENA IRQ, replicator poll, rtt send, rtt recv). Core pinning is derived **dynamically at runtime** from the
isolated set (`run_ucast.yaml` `auto_pin` + the replicator's `initializeCpuCores`), so
the same AMI/code adapts to any size.

| File | Topology | Instances | Notes |
|------|----------|:---------:|-------|
| `ucast-az-cpg-3.json` | Same AZ (a), single cluster PG | 3 | Standard ucast triangle |
| `ucast-xaz-xcpg-10.json` | Cross AZ (a+b): 2 CPGs + 2 SPGs + 2 unplaced | 10 | Full placement matrix |

## mcast/ — Multicast fan-out benchmarks

Explicit roles: source → replicator → destination(s). Measures fan-out latency.

**Instance type: `c7i.2xlarge`** (set per entry). Each node runs a single busy-poll app
(`mcast_send` | replicator poll | `mcast_receive`), so 3 dedicated cores (OS, ENA IRQ,
app) fit a 2xlarge — the cost-efficient choice.

| File | Topology | Instances | Notes |
|------|----------|:---------:|-------|
| `mcast-az-cpg-3.json` | Same AZ (a): source + replicator + destination in one cluster PG | 3 | Minimal mcast test |
| `mcast-xregion-3.json` | Cross region: source+replicator CPG (us), destination (eu) | 3 | Cross-region fan-out |
| `mcast-xregion-xaz-xpg-6.json` | Source+replicator CPG (us-east-1a) → destinations across az-a/b + eu-west-2 | 6 | Mixed topology |

## all — Combined placement comparison

A large fleet for comparing placement strategies in one deployment. Uses the mcast
datapath with multiple replicator and destination placement strategies.

| File | Topology | Instances | Notes |
|------|----------|:---------:|-------|
| `all.json` | 2 sources + 14 replicators (7 CPG + 7 SPG) + 8 destinations (7 CPG + 1 cross-region) | 24 | Placement comparison — see note below |

**Note on orchestrator behavior:** The control-plane `RunMcastMatrix` picks **one source**
and **one replicator** (first online match) and fans out to **all destinations**. The extra
sources and replicators are present to enable **targeted runs** via ansible (where you
explicitly select the replicator IP) or future orchestrator extensions that iterate over
replicators. With the control-plane as-is, only 1 source → 1 replicator → 8 destinations
will be exercised per campaign.

## Custom Scenarios

Create any JSON file with the `FleetEntry` schema:

```json
[
  {"count": 2, "type": "c7i.4xlarge", "pgType": "cluster", "pgName": "group-a"},
  {"count": 2, "type": "c7i.4xlarge", "pgType": "cluster", "pgName": "group-b", "az": "b"},
  {"count": 3, "type": "c7i.2xlarge", "pgType": "spread", "region": "eu-west-2"}
]
```

### FleetEntry Fields (complete)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `type` | string | `c7i.4xlarge` | EC2 instance type |
| `count` | number | `1` | Number of instances |
| `role` | string | `destination` | `source`, `replicator`, or `destination` |
| `az` | string | `a` | AZ suffix (e.g. `"b"`) or full name (e.g. `"eu-west-2a"`) |
| `pgType` | string | none | `cluster`, `spread`, or `partition` |
| `pgName` | string | auto | Group label. Entries sharing a name share one placement group. Also recorded per-node in the `FleetManifest` as a reporting/clustering label. |
| `region` | string | stack region | Triggers cross-region deployment (max one secondary region) |

### Loading

```bash
# Named scenario (from this directory):
--context scenario=ucast-az-cpg-3

# From any file on disk:
--context fleet=@/path/to/my-custom.json

# Inline JSON:
--context fleet='[{"count":2,"type":"c7i.4xlarge","pgType":"cluster"}]'
```

### Validation

CDK validates at synth time:
- **Cluster placement** entries across multiple AZs → error
- **Spread placement** >7 instances per AZ → error
- **Comment-only entries** (objects with only `_comment` key) are silently filtered out
- **Empty fleet** → error listing all available scenarios

### Example: `ucast-xaz-xcpg-10.json`

```json
[
  {"count": 1, "type": "c7i.4xlarge", "az": "a"},
  {"count": 1, "type": "c7i.4xlarge", "az": "b"},
  {"count": 2, "type": "c7i.4xlarge", "az": "a", "pgType": "cluster", "pgName": "cpg-a-2"},
  {"count": 2, "type": "c7i.4xlarge", "az": "b", "pgType": "cluster", "pgName": "cpg-b-2"},
  {"count": 2, "type": "c7i.4xlarge", "az": "a", "pgType": "spread", "pgName": "spg-a-1"},
  {"count": 2, "type": "c7i.4xlarge", "az": "b", "pgType": "spread", "pgName": "spg-b-1"}
]
```

This creates 10 instances across 2 AZs with a mix of cluster PGs, spread PGs, and unplaced nodes — testing placement impact on latency.
