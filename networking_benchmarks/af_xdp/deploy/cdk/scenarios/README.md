# scenarios/

Pre-built fleet specifications for common benchmark topologies.

Load via: `--context scenario=<name>` (e.g. `--context scenario=mcast-8`), or via
`afxdpctl up --scenario <name>`.

Files are named `<datapath>-<size>`: `ucast-*` drive the `rtt` round-trip probe,
`mcast-*` drive the `mcast_send` -> replicator -> `mcast_receive` fan-out, and
`all-*` carries both role sets in one fleet. The trailing number is the instance
count, so the file name alone tells you what a deploy will cost.

## ucast/ - Unicast RTT benchmarks

All nodes act as peers (role is irrelevant for ucast - the orchestrator uses all online nodes). Measures point-to-point latency.

**Default instance type: `m8a.2xlarge`** (`DEFAULT_INSTANCE_TYPE` in `lib/fleet.ts`; overridable per entry).
Unicast co-locates the replicator poll thread + rtt sender + rtt receiver on the *same* node,
needing 5 dedicated cores (OS, ENA IRQ, replicator poll, rtt send, rtt recv). Core pinning is
derived **dynamically at runtime** from the isolated set (`run_ucast.yaml` `auto_pin` + the
replicator's `initializeCpuCores`), so the same AMI/code adapts to any size or vendor
(Intel `c7i`/`c6in`/... or AMD `m8a`).

| File | Topology | Instances | Instance type | Notes |
|------|----------|:---------:|---------------|-------|
| `ucast-3.json` | Same AZ (a), single cluster PG `cpg-a` | 3 | `c7i.4xlarge` | Standard ucast triangle. Intel, SMT on - 16 vCPUs across 8 physical cores |
| `ucast-metal-5.json` | 2 in AZ a cluster PG `cpg-a` + 1 in AZ a unplaced + 1 in AZ b + 1 in eu-west-1 | 5 | `m8azn.metal-12xl` | Bare metal placement matrix. The cross-region entry requires the AMI baked in eu-west-1. Comparable to the `mcast2ucast` DPDK benchmark's metal runs |

## mcast/ - Multicast fan-out benchmarks

Explicit roles: source -> replicator -> destination(s). Measures fan-out latency.

Each node runs a single busy-poll app (`mcast_send` | replicator poll | `mcast_receive`), so
3 dedicated cores (OS, ENA IRQ, app) fit a 2xlarge - the cost-efficient choice, on either
Intel (`c7i.2xlarge`) or AMD (`m8a.2xlarge`, no SMT - 8 vCPUs = 8 online cores, more headroom
than the Intel equivalent's 4).

| File | Topology | Instances | Instance type | Notes |
|------|----------|:---------:|---------------|-------|
| `mcast-3.json` | Same AZ (a), single cluster PG `cpg-a`: source + replicator + destination | 3 | `m8a.2xlarge` | Minimal mcast test - the mode-comparison baseline, since placement is held constant |
| `mcast-8.json` | Source in AZ a cluster PG `cpg-a`; replicators in AZ a cluster PG `cpg-a` / AZ a unplaced / AZ b; destinations in AZ a cluster PG `cpg-a` / AZ a unplaced / AZ c / eu-west-1 AZ a | 8 | default (`m8a.2xlarge`) | 3-replicator x 4-destination matrix - see below |

### `mcast-8.json` - the current mcast results scenario

This is the scenario behind the current mcast numbers. It sets no `type` on any entry, so all
8 nodes inherit the `m8a.2xlarge` default.

It deliberately varies replicator and destination placement against a **fixed source**:

| Role | Placements |
|------|------------|
| source | 1: AZ a, cluster PG `cpg-a` |
| replicator | 3: cluster PG `cpg-a` / same-AZ-unplaced / cross-AZ (b) |
| destination | 4: cluster PG `cpg-a` / same-AZ-unplaced / cross-AZ (c) / cross-region (eu-west-1 AZ a) |

One sweep therefore covers 3 x 4 = 12 source/replicator/destination combinations per
forwarding mode, without redeploying the fleet between placement variants.

## all - Combined placement comparison

A large fleet for comparing placement strategies in one deployment. Uses the mcast datapath
with multiple source, replicator, and destination placement strategies.

| File | Topology | Instances | Instance type | Notes |
|------|----------|:---------:|---------------|-------|
| `all-11.json` | 2 source (`tenancy: host`) in AZ a cluster PG `cpg-a`; replicators in AZ a cluster PG `cpg-a` / AZ a unplaced / AZ b; 3 destinations in AZ a cluster PG `cpg-a` + 1 AZ a unplaced + 1 AZ b + 1 in eu-west-2 AZ a cluster PG | 11 | default (`m8a.2xlarge`) | Placement comparison. See the region warning and orchestrator note below |

> **Warning - `all-11.json` targets a different secondary region.** Its cross-region
> destination is in **eu-west-2**, while both `mcast-8.json` and `ucast-metal-5.json`
> target **eu-west-1**. Cross-region deployment requires the AMI to have been baked in the
> secondary region, so `all-11.json` will fail unless an AMI exists in eu-west-2. This looks
> like a leftover that probably wants changing to eu-west-1. The JSON has **not** been
> changed - only this note was added.

**Note on orchestrator behavior:** The control-plane `RunMcastMatrix` picks **one source**
and **one replicator** (first online match) and fans out to **all destinations**. The extra
sources and replicators are present to enable **targeted runs** via ansible (where you
explicitly select the replicator IP) or future orchestrator extensions that iterate over
replicators. With the control-plane as-is, only 1 source - 1 replicator - all destinations
will be exercised per campaign.

## Custom Scenarios

Create any JSON file with the `FleetEntry` schema:

```json
[
  {"count": 2, "type": "m8a.2xlarge", "pgType": "cluster", "pgName": "group-a"},
  {"count": 2, "type": "m8a.2xlarge", "pgType": "cluster", "pgName": "group-b", "az": "b"},
  {"count": 3, "type": "m8a.2xlarge", "pgType": "spread", "region": "eu-west-1"}
]
```

### FleetEntry Fields (complete)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `type` | string | `m8a.2xlarge` | EC2 instance type |
| `count` | number | `1` | Number of instances |
| `role` | string | `destination` | `source`, `replicator`, or `destination` |
| `az` | string | first AZ of region (`a`) | AZ suffix (e.g. `"b"`) or full name (e.g. `"eu-west-1a"`) |
| `pgType` | string | none | `cluster`, `spread`, or `partition` |
| `pgName` | string | auto | Group label. Entries sharing a name share one placement group. Also recorded per-node in the `FleetManifest` as a reporting/clustering label. |
| `region` | string | stack region | Triggers cross-region deployment (max one secondary region) |
| `tenancy` | string | `shared` | `shared` (multi-tenant host), `instance` (Dedicated Instance - single-tenant hardware, no placement control), or `host` (Dedicated Host - one host allocated per `(type, AZ)` group and shared by every entry in that group). Not supported with `pgType: "spread"` |
| `hostId` | string | none | Requires `tenancy: "host"`. A real host ID (`h-` + 17 hex chars) targets that already-allocated host; any other string is a logical alias, and all entries sharing the alias share one newly-allocated host |

### Loading

```bash
# Named scenario (from this directory):
--context scenario=mcast-8

# From any file on disk:
--context fleet=@/path/to/my-custom.json

# Inline JSON:
--context fleet='[{"count":2,"type":"m8a.2xlarge","pgType":"cluster"}]'
```

### Validation

CDK validates at synth time:
- **Cluster placement** entries across multiple AZs -> error
- **Spread placement** >7 instances per AZ -> error
- **Non-shared tenancy** combined with a spread placement group -> error
- **`hostId` without `tenancy: "host"`** -> error
- **Comment-only entries** (objects with only `_comment` key) are silently filtered out
- **Empty fleet** -> error listing all available scenarios

### Example: placement matrix (illustrative only - no file in this directory)

```json
[
  {"count": 1, "type": "m8a.2xlarge", "az": "a"},
  {"count": 1, "type": "m8a.2xlarge", "az": "b"},
  {"count": 2, "type": "m8a.2xlarge", "az": "a", "pgType": "cluster", "pgName": "cpg-a-2"},
  {"count": 2, "type": "m8a.2xlarge", "az": "b", "pgType": "cluster", "pgName": "cpg-b-2"},
  {"count": 2, "type": "m8a.2xlarge", "az": "a", "pgType": "spread", "pgName": "spg-a-1"},
  {"count": 2, "type": "m8a.2xlarge", "az": "b", "pgType": "spread", "pgName": "spg-b-1"}
]
```

This would create 10 instances across 2 AZs with a mix of cluster PGs, spread PGs, and
unplaced nodes - testing placement impact on latency. Save it to a file and load it with
`--context fleet=@<path>`.
