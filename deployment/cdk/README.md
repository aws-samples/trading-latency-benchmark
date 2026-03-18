# Trading Latency Benchmark CDK

This directory contains the AWS CDK code for deploying the infrastructure required for the trading latency benchmark.

## Available Deployment Options

The CDK code supports multiple deployment architectures:

1. **Single Instance Stack** (default): Deploys two EC2 instances in a single availability zone (AZ) but **not within a Cluster Placement Group** for benchmarking different instance types
2. **Cluster Placement Group Stack**: Deploys two EC2 instances (client and server) in a single AZ **within** a Cluster Placement Group (CPG) for optimal network performance
3. **Multi-AZ Stack**: Deploys instances across multiple AZs to measure cross-AZ latency
4. **HFT Feeder Stack**: Deploys a full feed-handler POC topology — mock exchange, feeder, and N subscribers — using AF_XDP zero-copy with GRE tunnel to simulate exchange multicast within AWS VPC

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

### Feed Handler POC Deployment

Deploys the full GRE-tunnel feed handler topology: mock exchange + feeder + N subscribers.

```bash
# Minimal — defaults: c7i.xlarge feeder, c7i.xlarge exchange, 2× c6in.xlarge subscribers
cdk deploy --context deploymentType=feeder --context keyPairName=my-keypair

# Custom instance types and subscriber count
cdk deploy --context deploymentType=feeder \
  --context keyPairName=my-keypair \
  --context feederInstanceType=c7i.4xlarge \
  --context exchangeInstanceType=c7i.4xlarge \
  --context subscriberInstanceType=c6in.xlarge \
  --context subscriberCount=2 \
  --context availabilityZone=us-east-1a
```

After `cdk deploy` completes, the stack outputs include:
- Public/private IPs for every instance
- Ready-to-run commands for each operational step (start feeder, register subscribers, send traffic)

**What happens automatically on first boot:**
- Build dependencies and xdp-tools are installed
- The benchmark binaries and eBPF programs are compiled (`make all`)
- On the exchange instance: the GRE tunnel to the feeder is configured (`ip tunnel add gre_feed`, `ip route add 224.0.0.0/4 dev gre_feed`)

**What requires manual steps after deploy:**
1. SSH to feeder, run `packet_replicator` (command in `Step1_StartFeeder` output)
2. Register each subscriber via `control_client` (command in `Step2_RegisterSubscribers` output)
3. SSH to exchange, run `test_client` or `market_data_provider_client` (commands in `Step3_*` outputs)
4. Optionally run Ansible tuning playbooks for production-grade performance (command in `OptionalTuning` output)

## Context Parameters

### Common

| Parameter | Description | Default |
|-----------|-------------|---------|
| `deploymentType` | Deployment architecture | `single` — `single`, `cluster`, `multi-az`, `feeder` |
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

### Feeder stack (`deploymentType=feeder`)

| Parameter | Description | Default |
|-----------|-------------|---------|
| `feederInstanceType` | Feeder EC2 instance type | `c7i.4xlarge` |
| `exchangeInstanceType` | Mock exchange instance type | `c7i.4xlarge` |
| `subscriberInstanceType` | Subscriber instance type | `c6in.4xlarge` |
| `subscriberCount` | Number of subscriber instances | `2` |
| `multicastGroup` | Inner multicast group address (listen_ip on feeder) | `224.0.31.50` |
| `dataPort` | UDP data port | `5000` |
| `ctrlPort` | UDP upstream control port | `5001` |

## Useful Commands

* `npm run build`   compile typescript to js
* `npm run watch`   watch for changes and compile
* `npm run test`    perform the jest unit tests
* `cdk deploy`      deploy this stack to your default AWS account/region
* `cdk diff`        compare deployed stack with current state
* `cdk synth`       emits the synthesized CloudFormation template
