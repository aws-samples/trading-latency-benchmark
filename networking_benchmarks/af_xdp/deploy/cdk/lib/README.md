# lib/

CDK stack constructs for the AF_XDP benchmark deployment.

## fleet.ts - `FleetStack`

Region-scoped deployment stack. Builds a VPC, security group, placement groups, and instances for the fleet entries that belong to its region.

**Features:**
- Per-entry fields: `type`, `count`, `role`, `az`, `pgType`, `pgName`, `region`
- Placement groups: cluster (single-AZ enforced), spread (≤7/AZ), partition
- Cross-region: secondary VPC + VPC peering + `Port.allTraffic()` SG rules via `connectRegions()`
- Dynamic instance creation with tags: `Name`, `Role`, `AZ`, `InstanceType`, `Region`, `PlacementStrategy`, `PlacementGroup`
- `FleetManifest` JSON output - records all node metadata for script/report consumption
- Per-node IAM: `ssm:GetParameter` on `af-xdp/*` (agent preflight SSM discovery); secondary nodes also get cross-region SSM read for the control-plane region
- UserData stamps `AGENT_ROLE=<role>` → `/etc/default/afxdp-agent`, restarts the agent
- Secondary region user-data replicates NATS URL/token from the control-plane region into `/etc/default/afxdp-agent`
- Source/dest check disabled on all instances (replication rewrites dst IP/MAC)

**AMI resolution:** Both primary and secondary stacks resolve the baked AMI from SSM `/af-xdp/ami/<region>` in their own region (written by `AmiBuilderStack` during each region's bake). Override with `--context amiId=<id>` (primary) or `--context secondaryAmiId=<id>` (secondary). If no AMI is resolvable (no SSM parameter and no explicit override), synth fails with an error directing the operator to bake that region or pass the override - there is no silent AL2023 fallback.

**Cross-region NATS discovery:** When `controlPlaneRegion` is set (secondary stacks), instance user-data reads `/af-xdp/nats-url` and `/af-xdp/nats-token` from the control-plane's region and writes `AGENT_NATS_URL` / `AGENT_NATS_TOKEN` into `/etc/default/afxdp-agent`. The agent preflight sources that file before falling back to local SSM. Fleet node IAM stays read-only (`ssm:GetParameter`) - no node can write a shared parameter.

**Cross-region security group:** When a peer VPC CIDR is configured, the SG grants `Port.allTraffic()` from the peer CIDR (mirroring the intra-group self-reference rule). An SG self-reference does not span cross-region peering, so enumerating individual ports left the rtt echo port (19020) closed.

**Validation (synth-time):**
- Cluster placement entries span multiple AZs → error
- Spread >7 instances per AZ → error
- Empty primary fleet → error (lists available scenarios)

**Exported types:**
- `FleetEntry` - the JSON schema for a single fleet node
- `ResolvedEntry` - FleetEntry with `_resolvedAz` (full AZ name)
- `PlacementStrategy` - `'cluster' | 'spread' | 'partition'`

**Exported functions:**
- `resolveAz(azSpec, region)` - resolves `"a"` → `"us-east-1a"`
- `partitionFleet(fleet, primaryRegion)` - splits entries into primary/secondary region sets
- `connectRegions(primary, secondary, opts)` - wires VPC peering + bidirectional routes

## ami-builder.ts - `AmiBuilderStack`

Single-stack AMI builder. Launches a temporary instance, runs `bake-ami.sh` via UserData, waits for completion, creates AMI via Lambda, writes AMI ID to SSM, terminates instance.

**Resources created:**
- VPC (minimal, single public subnet)
- Security group (SSH debug only)
- IAM role (SSM core, CreateImage, StopInstances, CloudWatch Logs, SSM PutParameter)
- EC2 instance (UserData = gzip+base64 bake script with env vars; gzip keeps it under EC2's 16KB UserData limit)
- WaitConditionHandle + WaitCondition (20 min timeout)
- Lambda (CreateImage → wait for availability → write SSM → terminate builder)
- Custom Resource (triggers Lambda after WaitCondition passes)

**Key behavior:**
- Agent build is **fatal** - a baked AMI without a working agent silently breaks the control plane, so the bake fails (FAILURE signal to CFN) if the Go agent doesn't compile
- Preflight retries SSM for up to 60s (control plane may still be booting when agents start)

**Output:** `AmiId` (also written to SSM `/af-xdp/ami/<region>`)

**Defaults:**
| Parameter | Default |
|-----------|---------|
| `instanceType` | `c7i.xlarge` |
| `gitRepo` | `https://github.com/aws-samples/trading-latency-benchmark.git` |
| `gitRef` | `main` |

## control-plane.ts - `ControlPlaneStack`

Central control plane - a single EC2 running nats-server + the Go backend (which serves the web dashboard + HTTP/SSE API). Fleet agents connect **outbound** to its Elastic IP on port 4222, so no VPC peering is needed across per-scenario fleet VPCs.

**Resources created:**
- VPC (single public subnet; host needs a public EIP)
- Security group (SSH + NATS 4222 + web 8080 - restricted to `clientCidr`)
- IAM role (SSM core + `ssm:PutParameter` on `af-xdp/*`)
- Elastic IP (stable endpoint for agents + optional DNS)
- EC2 instance with UserData that: installs Go, clones repo, builds the backend + web, installs nats-server v2.10.22, writes systemd units, starts services, publishes NATS URL/token to SSM
- Optional Route53 A record (if `hostedZoneId`, `zoneName`, `recordName` provided)

**SSM Parameters published (by UserData at boot):**
- `/af-xdp/nats-url` - e.g. `nats://1.2.3.4:4222` (or `tls://...` with `natsTls=true`)
- `/af-xdp/nats-token` - auth token (auto-generated if not provided)

**NATS auth:** token-based (`authorization { token: "..." }` in nats.conf). Optional TLS (self-signed cert on the host; agents connect with `insecure_skip_verify`).

**Defaults:**
| Parameter | Default |
|-----------|---------|
| `instanceType` | `t3.small` |
| `gitRepo` | `https://github.com/aws-samples/trading-latency-benchmark.git` |
| `gitRef` | `main` |
| `clientCidr` | `0.0.0.0/0` |
| `natsTls` | `false` |

**Outputs:** `PublicIp`, `WebUrl`, `NatsUrl`, `SsmNatsParam`, and optionally `WebUrlDns`/`NatsUrlDns` (when DNS configured).
