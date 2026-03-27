# Trading Latency Benchmark CDK

This directory contains the AWS CDK code for deploying the infrastructure required for the trading latency benchmark.

## Available Deployment Options

The CDK code supports multiple deployment architectures:

1. **Single Instance Stack** (default): Deploys two EC2 instances in a single availability zone (AZ) but **not within a Cluster Placement Group** for benchmarking different instance types
2. **Cluster Placement Group Stack**: Deploys two EC2 instances (client and server) in a single AZ **within** a Cluster Placement Group (CPG) for optimal network performance
3. **Multi-AZ Stack**: Deploys instances across multiple AZs to measure cross-AZ latency
4. **AMI Builder Stack**: Builds a pre-tuned Amazon Machine Image with OS-level optimisations baked in
5. **Latency Hunting Stack**: Deploys instances for network placement optimisation and latency hunting

> **GRE feed-handler topology** (mock exchange + feeder + subscribers) is deployed by a separate CDK app in [`deployment/mcast_gre/cdk/`](../mcast_gre/README.md) using the `deploy.sh` script.

## Prerequisites

- AWS CDK v2 installed
- Node.js 14.x or later
- AWS CLI configured with appropriate credentials

## Installation

```bash
npm install
```

## Deployment

### Default Deployment (Single Instance Stack)

```bash
cdk deploy
```

You can specify instance types, keypair, and availability zone:

```bash
cdk deploy --context instanceType1=c7i.4xlarge --context instanceType2=c6in.4xlarge --context keyPairName=my-keypair --context availabilityZone=us-east-1a
```

To deploy to Frankfurt (eu-central-1a):

```bash
cdk deploy --context region=eu-central-1 --context availabilityZone=eu-central-1a --context keyPairName=frankfurt
```

### Cluster Placement Group Deployment

Deploy client and server instances within a Cluster Placement Group for minimal network latency:

```bash
cdk deploy --context deploymentType=cluster
```

You can also specify instance types for client and server:

```bash
cdk deploy --context deploymentType=cluster --context clientInstanceType=c7i.4xlarge --context serverInstanceType=c6in.4xlarge
```

### Multi-AZ Deployment

Deploy instances across multiple availability zones to measure cross-AZ latency:

```bash
cdk deploy --context deploymentType=multi-az
```

You can specify the instance type for all AZ instances:

```bash
cdk deploy --context deploymentType=multi-az --context instanceType=r4.xlarge
```

### Latency Hunting Deployment

Deploy instances for network placement optimisation:

```bash
# New VPC
cdk deploy --context deploymentType=latency-hunting --context keyPairName=my-keypair

# Existing VPC (BYOVPC)
cdk deploy --context deploymentType=latency-hunting \
  --context useExistingVpc=true \
  --context vpcId=vpc-xxxxxxxx \
  --context subnetId=subnet-xxxxxxxx \
  --context keyPairName=my-keypair
```

## Context Parameters

### Common

| Parameter | Description | Default |
|-----------|-------------|---------|
| `deploymentType` | Deployment architecture | `single` — `single`, `cluster`, `multi-az`, `ami-builder`, `latency-hunting` |
| `keyPairName` | EC2 key pair name for SSH | `virginia` |
| `availabilityZone` | Specific AZ | First AZ in region |
| `vpcCidr` | VPC CIDR block | `10.50.0.0/16` |
| `region` | AWS region | `us-east-1` |

### Single / Cluster / Multi-AZ stacks

| Parameter | Description | Default |
|-----------|-------------|---------|
| `instanceType1` | First instance type (single mode) | `c7i.4xlarge` |
| `instanceType2` | Second instance type (single mode) | `c6in.4xlarge` |
| `clientInstanceType` | Client instance type (cluster mode) | `c7i.4xlarge` |
| `serverInstanceType` | Server instance type (cluster mode) | `c6in.4xlarge` |
| `instanceType` | Instance type for all instances (multi-az mode) | `r4.xlarge` |

### Latency Hunting stack (`deploymentType=latency-hunting`)

| Parameter | Description | Default |
|-----------|-------------|---------|
| `useExistingVpc` | Import an existing VPC instead of creating one | `false` |
| `vpcId` | Existing VPC ID (required when `useExistingVpc=true`) | — |
| `subnetId` | Existing subnet ID (required when `useExistingVpc=true`) | — |
| `securityGroupId` | Existing security group ID (optional) | — |
| `elasticIps` | Comma-separated list of Elastic IPs to associate | — |

## Useful Commands

* `npm run build`   compile typescript to js
* `npm run watch`   watch for changes and compile
* `npm run test`    perform the jest unit tests
* `cdk deploy`      deploy this stack to your default AWS account/region
* `cdk diff`        compare deployed stack with current state
* `cdk synth`       emits the synthesized CloudFormation template
