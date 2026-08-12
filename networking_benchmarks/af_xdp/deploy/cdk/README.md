# AF_XDP Benchmark — CDK Deployment

Fleet-driven CDK infrastructure for the AF_XDP latency benchmark. Deploys EC2 instances with configurable placement, multi-AZ, and cross-region topologies from JSON scenario files. Includes an AMI builder for pre-baked instances with zero provisioning time.

## Structure

| Directory | Description | Details |
|-----------|-------------|---------|
| [`lib/`](lib/README.md) | CDK stack constructs (FleetStack, AmiBuilderStack) | Placement validation, cross-region peering |
| [`scenarios/`](scenarios/README.md) | Pre-built fleet topologies (ucast + mcast) | 6 scenarios, costs, custom format |
| [`scripts/`](scripts/README.md) | AMI bake script and configs applied | Binaries, sysctl, chrony, systemd units |
| `bin/` | CDK app entry point | Fleet resolution, deployment type routing |

## Prerequisites

- AWS CDK CLI (`npm install -g aws-cdk`)
- AWS credentials configured
- An EC2 key pair in the target region
- Node.js 18+

## Quick Start

```bash
npm install

# Deploy fleet (stock AL2023 — needs ansible provisioning)
cdk deploy --context keyPairName=virginia --context scenario=ucast/az-cpg-3

# Or build AMI first (~10 min), then deploy (instant readiness)
cdk deploy --context keyPairName=virginia --context deploymentType=ami-builder
cdk deploy --context keyPairName=virginia --context scenario=ucast/az-cpg-3 --context amiId=ami-xxx
```

## Deployment Types

| Type | Context | Description |
|------|---------|-------------|
| `fleet` (default) | `--context scenario=...` | Deploy EC2 fleet from scenario |
| `ami-builder` | `--context deploymentType=ami-builder` | Build pre-baked AMI (~10 min) |

## Fleet Spec

JSON array of node entries — all fields optional with defaults:

```json
[{"count": 3, "pgType": "cluster"}]
```

| Field | Default | Description |
|-------|---------|-------------|
| `type` | `c7i.2xlarge` | EC2 instance type |
| `count` | `1` | Number of instances |
| `role` | `replicator` | `source`, `replicator`, `destination` |
| `pgType` | none | `cluster`, `spread`, `partition` |
| `pgName` | auto | Group label. With `pgType`, same name = shared placement group; always recorded in `FleetManifest` for reporting/clustering/disambiguation (label every entry) |
| `az` | `a` | AZ suffix or full name |
| `region` | stack region | Triggers cross-region VPC peering |

### Loading

```bash
--context scenario=ucast/az-cpg-3              # from scenarios/
--context fleet=@path/to/file.json             # from file
--context fleet='[{"count":2}]'                # inline
```

## Multiple Stacks

```bash
cdk deploy --context scenario=ucast/az-cpg-3 --context stackName=cpg-bench
cdk deploy --context scenario=ucast/xaz-xcpg-10 --context stackName=xcpg-bench
cdk destroy --context stackName=cpg-bench
```

## Context Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `keyPairName` | (required) | SSH key pair name |
| `deploymentType` | `fleet` | `fleet` or `ami-builder` |
| `region` | `us-east-1` | Primary AWS region |
| `stackName` | `XdpStack` | CloudFormation stack name |
| `scenario` | — | Scenario path (e.g. `ucast/az-cpg-3`) |
| `fleet` | — | Inline JSON or `@file.json` |
| `amiId` | AL2023 latest | Custom/baked AMI |
| `secondaryAmiId` | AL2023 latest | AMI for secondary region |
| `secondaryKeyPairName` | `keyPairName` | Key pair in secondary region |
| `vpcCidr` | `10.61.0.0/16` | Primary VPC CIDR |
| `secondaryVpcCidr` | `10.62.0.0/16` | Secondary VPC CIDR |
| `dataPort` | `5000` | UDP data port for SG rules |
| `instanceType` | `c7i.xlarge` | AMI builder instance type |
| `gitRepo` | `aws-samples/...` | AMI builder source repo |
| `gitRef` | `main` | AMI builder git ref |

## Cleanup

```bash
cdk destroy --context keyPairName=virginia --context stackName=<name>
```

All resources use `RemovalPolicy.DESTROY`.
