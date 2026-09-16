# lib/

CDK stack constructs.

## fleet.ts — `FleetStack`

Unified deployment stack for all benchmark topologies. Handles single-region (same/cross-AZ) and cross-region via a single fleet spec.

**Features:**
- Per-entry: type, count, role, az, pgType, pgName, region
- Placement groups: cluster (single-AZ enforced), spread (≤7/AZ), partition
- Cross-region: secondary VPC + VPC peering + bidirectional SG rules
- Dynamic instance creation with tags (Role, AZ, InstanceType; PlacementStrategy + PlacementGroup when in a PG)
- FleetManifest JSON output for script consumption — records `pgName` per node (including standalone/non-PG nodes) so reporting can cluster/disambiguate by group label

**Validation (synth-time):**
- Cluster placement across multiple AZs → error
- Spread >7 per AZ → error
- Empty fleet → error with available scenarios listed

## ami-builder.ts — `AmiBuilderStack`

Single-stack AMI builder. Launches a temporary instance, runs `bake-ami.sh` via UserData, waits for completion, creates AMI via Lambda, writes AMI ID to SSM, terminates instance.

**Resources created:**
- VPC (minimal, public subnet)
- Security group (SSH debug only)
- IAM role (SSM, CreateImage, StopInstances, CloudWatch Logs, SSM PutParameter)
- EC2 instance (UserData = gzip+base64 bake script with env vars; gzip keeps it under EC2's 16KB UserData limit)
- WaitConditionHandle + WaitCondition (20 min timeout)
- Lambda (CreateImage, wait for availability, write SSM, terminate)
- Custom Resource (triggers Lambda after WaitCondition passes)

**Output:** `AmiId` (also written to SSM `/af-xdp/ami/<region>`)
