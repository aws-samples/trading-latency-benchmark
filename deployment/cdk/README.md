# Trading Latency Benchmark CDK

This directory contains the AWS CDK code for deploying the infrastructure required for the trading latency benchmark.

## Available Deployment Options

The CDK code supports multiple deployment architectures:

1. **Single Instance Stack** (default): Deploys two EC2 instances for benchmarking different instance types
2. **Cluster Placement Group Stack**: Deploys client and server instances within a Cluster Placement Group for optimal network performance
3. **Multi-AZ Stack**: Deploys instances across multiple availability zones to measure cross-AZ latency

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

## Context Parameters

| Parameter | Description | Default Value | Example Values |
|-----------|-------------|---------------|---------------|
| `deploymentType` | Type of deployment architecture | `single` | `single`, `cluster`, `multi-az` |
| `instanceType1` | EC2 instance type for first instance in single mode | `c7i.4xlarge` | `c7i.4xlarge`, `c6i.8xlarge` |
| `instanceType2` | EC2 instance type for second instance in single mode | `c6in.4xlarge` | `c6in.4xlarge`, `r6i.4xlarge` |
| `clientInstanceType` | EC2 instance type for client in cluster mode | `c7i.4xlarge` | `c7i.4xlarge`, `c6i.8xlarge` |
| `serverInstanceType` | EC2 instance type for server in cluster mode | `c6in.4xlarge` | `c6in.4xlarge`, `r6i.4xlarge` |
| `instanceType` | EC2 instance type for all instances in multi-az mode | `r4.xlarge` | `r4.xlarge`, `c5.4xlarge` |
| `keyPairName` | EC2 key pair name for SSH access (single instance stack) | `frankfurt` | `my-keypair`, `virginia` |
| `availabilityZone` | Specific availability zone for deployment (single instance stack) | First AZ in region | `eu-central-1a`, `us-east-1a` |
| `vpcCidr` | CIDR block for VPC | `10.50.0.0/16` | `10.50.0.0/16`, `10.100.0.0/16` |
| `region` | AWS region for deployment | `us-east-1` | `eu-central-1`, `us-west-2` |

## Useful Commands

* `npm run build`   compile typescript to js
* `npm run watch`   watch for changes and compile
* `npm run test`    perform the jest unit tests
* `cdk deploy`      deploy this stack to your default AWS account/region
* `cdk diff`        compare deployed stack with current state
* `cdk synth`       emits the synthesized CloudFormation template
