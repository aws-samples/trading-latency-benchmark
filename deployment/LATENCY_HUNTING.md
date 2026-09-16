# Latency Hunting Deployment Strategy

## Overview

The Latency Hunting deployment strategy is designed to find optimal network placement for latency-sensitive trading applications by deploying diverse instance types across different Cluster Placement Groups (CPGs). This approach helps identify which network spines provide the lowest latency to your target instance.

## Why EC2 Hunting?

In AWS EC2, instances in different Cluster Placement Groups may be placed on different network spines. When your target instance is not in the same CPG and there's no mechanism to share CPG information, you need to "hunt" for the optimal placement by:

1. **Launching diverse instance types** - Each with its own CPG
2. **Covering different network spines** - Maximize network topology coverage
3. **Measuring latency** - Test against your target instance
4. **Keeping the best** - Retain instances with lowest latency
5. **Scaling winners** - Launch more instances from top-performing CPGs

## Key Features

- **Resilient Deployment**: Handles capacity errors gracefully - deployment doesn't fail if specific instance types are unavailable
- **Diverse Instance Types**: Deploys 36+ different low-cost instance types (excluding GPUs)
- **Individual CPGs**: Each instance type gets its own Cluster Placement Group
- **Automated Testing**: Scripts to run latency benchmarks across all instances
- **Results Analysis**: Identifies best performers and provides recommendations
- **Selective Cleanup**: Keep top performers, clean up poor performers

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Target Instance                      │
│              (Your Trading Server/Exchange)             │
└────────────────────────┬────────────────────────────────┘
                         │
                         │ Latency Tests
                         │
         ┌───────────────┴───────────────┐
         │                               │
    ┌────▼────┐                    ┌────▼────┐
    │  CPG 1  │                    │  CPG N  │
    │         │                    │         │
    │ c7i.lg  │  ...  ...  ...     │ m5.2xl  │
    └─────────┘                    └─────────┘
    
    Each instance type in its own CPG
    to maximize network spine coverage
```

## Instance Types Deployed

The deployment includes **50+ diverse instance types** across current and previous generations to maximize network spine coverage. This comprehensive list covers Intel, AMD, and ARM (Graviton) architectures spanning from 1st generation (2006) to 8th generation (2024).

> **Key Features**:
> - **50+ Total Instance Types**: Maximum diversity across all generations (CPG-compatible only)
> - **Multi-Architecture**: Intel Xeon, AMD EPYC, AWS Graviton (ARM Neoverse)
> - **Generations 1-8**: From 2006 (M1) to 2024 (C8g/M8g/R8g Graviton4)
> - **Network Performance**: Up to 200 Gbps (c7gn, c8g families)
> - **Cost Range**: From ultra-cheap (t1.micro, m1, c1) to modern efficient (Graviton4)
> - **Previous Gen Included**: M1, M2, M3, C1, C3, C4, etc. for cost-effective probes
> - **Excludes Expensive**: No GPU (P/G), HPC, Mac, U/X/z1d high-memory, or metal instances

## Quick Start

### Prerequisites

- AWS CLI configured with appropriate credentials
- AWS CDK installed (`npm install -g aws-cdk`)
- SSH key pair created in Tokyo region (or your target region)
- `jq` installed for JSON parsing
- Target instance already deployed (optional, for latency testing)

### Step 1: Deploy Hunting Instances

#### BYOVPC Mode (Recommended)**

This mode **never creates or manages VPC infrastructure** - perfect when you want to use your existing VPC without CDK touching it.

```bash
cd deployment

# Deploy into existing VPC/subnet
./deploy-latency-hunting.sh \
  --use-existing-vpc \
  --region ap-northeast-1 \
  --vpc-id $VPC_ID \
  --subnet-id $SUBNET_ID \
  --key-pair $SSH_KEYPAIR

# With existing security group
./deploy-latency-hunting.sh \
  --use-existing-vpc \
  --region ap-northeast-1 \
  --vpc-id $VPC_ID \
  --subnet-id $SUBNET_ID \
  --security-group-id $SECURITY_GROUP_ID \
  --key-pair $SSH_KEYPAIR
```

**What BYOVPC Mode Does:**
- **Never creates/destroys VPC** - uses your existing VPC, subnet ..etc.
- Only creates: Instances, Placement Groups, Lambda (optional: Security Group)
- Respects your network configuration completely
- Clean stack deletion - only removes instances/placement groups

#### CDK-Managed VPC Mode**

Use this if you want CDK to manage the VPC (creates new VPC):

```bash
cd deployment

# CDK creates new VPC
./deploy-latency-hunting.sh --region ap-northeast-1 --key-pair your-keypair

# CDK creates VPC with custom CIDR
./deploy-latency-hunting.sh --region ap-northeast-1 --vpc-cidr 10.200.0.0/16 --key-pair your-keypair
```

⚠️ **Warning**: Switching between modes (CDK-managed → BYOVPC) requires stack deletion and recreation.


**Expected Results**: 
- Total attempted: 50+ instance types (all support Cluster Placement Groups)
- Typically successful: 30-50 instances (varies by region capacity)
- Failed: 25-45 instances (capacity constraints for old/new/specialized types)
- Note: Excluded some instance types like T1, T3a due to no CPG support


### Step 2: Set Up Cross-Region VPC Peering (if target is in another region)

If your target instance is in a different region (e.g., target in eu-central-1, hunting stack in ap-northeast-1), use private line providers or set up VPC peering:

```bash
cd deployment
./setup-vpc-peering.sh \
  --target-vpc-id $VPC_ID \
  --target-region eu-central-1 \
  --hunting-region ap-northeast-1
```

This script will:
- Create cross-region VPC peering connection
- Accept the peering (both sides)
- Update all route tables in both VPCs
- Add security group rules to allow traffic from hunting VPC CIDR (10.100.0.0/16)

**Requirements:**
- Both VPCs must be in the same AWS account
- CIDR blocks must not overlap
- `jq` must be installed (`brew install jq` on macOS)

### Step 3: Run Latency Tests

Wait 5-10 minutes for instances to initialize and complete automatic setup, then run tests. Runs tests on all instances in parallel using Ansible which provides better error handling and progress tracking. Use existing Ansible playbooks and inventory. Once done fetch histogram logs and generate report to find out optimum CPG. Then scale winners by launching more instances in winning placement groups manually or via cdk.


## Usage Examples

### Initial Discovery
Run latency benchmarks against the target host
```bash
ansible-playbook run_hunting_benchmark.yaml --key-file ~/.ssh/tokyo_keypair.pem -i ./inventory/hunting_inventory.aws_ec2.yml
```

Analyze latency percentiles p50, p90, p99 ..etc.
```bash
ansible-playbook fetch_hunting_histogram_logs.yaml --key-file ~/.ssh/tokyo_keypair.pem -i ./inventory/hunting_inventory.aws_ec2.yml
```

### Example 2: Iterative Refinement
Remove low performing instances and CPGs and launch new ones.


### Cost Advantages
- **Previous Generation**: 40-60% cheaper than current gen, perfect for network probes
- **Graviton ARM**: 20-40% better price-performance than equivalent x86
- **M1/C1 instances**: Ultra cheap probes from 2006-2008 era, still available in some regions

## Advanced Usage

### Custom Instance Type List

Edit `deployment/cdk/lib/latency-hunting-stack.ts`:

```typescript
const instanceTypes = [
  'c7i.large',
  'c7i.xlarge',
  // Add your custom types here
  'your.type',
];
```


## References

- [AWS Cluster Placement Groups](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/placement-groups.html)
- [EC2 Instance Types](https://aws.amazon.com/ec2/instance-types/)
- [Network Performance](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/network-performance.html)
- [Trading Latency Benchmark](../../README.md)
