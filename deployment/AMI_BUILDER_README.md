# OS-Tuned AMI Builder

This directory contains tools to build pre-optimized Amazon Machine Images (AMIs) for low-latency trading applications. The AMI builder automates the process of deploying an EC2 instance, applying comprehensive OS-level performance tuning, and creating an immutable AMI that can be reused across all deployment types.

## Overview

The AMI builder consists of:
- **CDK Stack** (`cdk/lib/ami-builder-stack.ts`): Deploys a single EC2 instance for AMI creation
- **Build Script** (`build-tuned-ami.sh`): Orchestrates deployment, tuning, and AMI creation
- **Ansible Playbook** (`ansible/tune_os.yaml`): Applies comprehensive OS optimizations

## Prerequisites

- AWS CLI configured with appropriate credentials
- AWS CDK installed and bootstrapped
- Ansible 2.9+ installed
- SSH key pair registered in AWS (default: `virginia`)
- `jq` command-line JSON processor
- Node.js 18+ (for CDK)
- **Important**: The AMI builder instance tag must be included in `deployment/ansible/inventory/inventory.aws_ec2.yml`
  - The instance is tagged as `Name: Trading-Benchmark-AMI-Builder-Instance`
  - This tag must be in the `include_filters` section of the inventory file
  - Already configured in the repository's inventory file

### Installing jq

```bash
# Ubuntu/Debian
sudo apt-get install jq

# macOS
brew install jq

# Amazon Linux/RHEL
sudo yum install jq
```

## Quick Start

Build an OS-tuned AMI with default settings:

```bash
cd deployment
./build-tuned-ami.sh --key-file ~/.ssh/virginia.pem
```

This will:
1. Deploy a c7i.4xlarge instance in us-east-1
2. Verify instance is discoverable in Ansible dynamic inventory
3. Apply all OS tuning optimizations via Ansible playbook
4. Wait for automatic reboot
5. Verify OS tuning was applied successfully:
   - Check for tuning report file (`/root/trading_system_tuning_report.txt`)
   - Verify Transparent Huge Pages disabled
   - Verify CPU cores isolated
   - Verify hugepages configured
6. Create an AMI from the tuned instance
7. Clean up the temporary instance

Expected duration: **20-30 minutes**

## Usage

```bash
./build-tuned-ami.sh [options]
```

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `-i, --instance-type TYPE` | EC2 instance type to use | `c7i.4xlarge` |
| `-k, --key-file PATH` | Path to SSH private key | `~/.ssh/virginia.pem` |
| `-n, --ami-name NAME` | Custom AMI name | `trading-benchmark-tuned-TIMESTAMP` |
| `-r, --region REGION` | AWS region | `us-east-1` |
| `--no-cleanup` | Keep instance running after AMI creation | Cleanup enabled |
| `-h, --help` | Show help message | - |

### Examples

**Build AMI with custom instance type:**
```bash
./build-tuned-ami.sh --instance-type c6in.4xlarge --key-file ~/.ssh/my-key.pem
```

**Build AMI with custom name:**
```bash
./build-tuned-ami.sh --ami-name my-trading-ami-v1 --key-file ~/.ssh/virginia.pem
```

**Build AMI and keep instance for inspection:**
```bash
./build-tuned-ami.sh --no-cleanup --key-file ~/.ssh/virginia.pem
# Instance will remain running - remember to destroy the stack later:
# cd cdk && cdk destroy TradingBenchmarkAmiBuilderStack
```

**Build AMI in different region:**
```bash
./build-tuned-ami.sh --region us-west-2 --key-file ~/.ssh/my-west-key.pem
```

## What Gets Tuned?

The AMI builder applies the following optimizations from `tune_os.yaml`:

### CPU Optimizations
- Disables hyperthreading
- Sets CPU governor to `performance`
- Disables C-states and P-states
- **Dynamic CPU isolation** based on instance size:
  - Automatically detects vCPU count
  - Scales housekeeping cores: 1 core (≤4 vCPUs), 2 cores (8 vCPUs), 4 cores (16-32 vCPUs), 8 cores (48-96 vCPUs), 16 cores (128-192 vCPUs)
  - Isolates remaining cores for trading applications
  - Supports instances up to 48xlarge (192 vCPUs)
- Moves kernel workqueues to housekeeping cores
- Sets TSC as clocksource

### Memory Optimizations
- Disables Transparent Huge Pages (THP)
- Configures explicit hugepages
- Sets VM dirty ratios for optimal I/O
- Disables NUMA balancing
- Configures shared memory limits for IPC

### Network Optimizations
- Disables hardware offloading (TSO, GSO, GRO)
- Sets static interrupt moderation (1 µs)
- Configures busy polling for lower latency
- Optimizes TCP/IP stack parameters
- Configures RSS and XPS for optimal packet processing
- Increases network buffer sizes

### I/O Optimizations
- Sets mq-deadline I/O scheduler
- Optimizes NVMe settings
- Disables block layer statistics
- Disables entropy collection from block devices

### System Optimizations
- Disables unnecessary services (irqbalance, SSM agent)
- Disables iptables
- Disables syscall auditing
- Configures RT priorities for trading processes
- Configures Chrony for precise time synchronization with AWS Time Sync Service

### Persistence
All optimizations persist across reboots via:
- systemd services
- sysctl configuration files
- udev rules
- GRUB kernel command-line parameters
- rc.local boot script

## Using the Tuned AMI

After building, the script outputs an AMI ID. Use it in your deployments:

### Option 1: CDK Context
```bash
cd cdk
cdk deploy --context deploymentType=cluster --context baseAmi=ami-xxxxxxxxx
```

### Option 2: CDK Code
Update your CDK stack to use the tuned AMI:

```typescript
import { MachineImage } from 'aws-cdk-lib/aws-ec2';

const tunedAmi = MachineImage.genericLinux({
  'us-east-1': 'ami-xxxxxxxxx'
});

const instance = new Instance(this, 'TradingInstance', {
  // ... other props
  machineImage: tunedAmi,
});
```

### Option 3: Modify Existing Stacks
The AMI builder stack supports a `baseAmi` context parameter. Update `single-instance-stack.ts`, `cluster-placement-group-stack.ts`, or `multi-az-stack.ts` to accept and use this parameter.

## Benefits

### Faster Deployments
- **Without AMI**: Deploy instance → Wait 5 min → Run tune_os.yaml → Wait 10-15 min → Reboot → Wait 5 min = **~25 minutes**
- **With AMI**: Deploy instance from tuned AMI → Ready in **~2 minutes**

### Consistency
- All instances guaranteed to have identical OS-level optimizations
- No risk of missing tuning steps or misconfiguration
- Immutable infrastructure pattern

### Version Control
- Tag AMIs with different tuning configurations
- Roll back to previous tuning versions if needed
- Track which AMI version is deployed where

### Cost Optimization
- Reduce time-to-production for new instances
- Minimize time spent tuning instances during testing
- AMI storage cost is negligible (~$0.05/month per GB)

## Verification

After creating an AMI, you can verify the optimizations:

### 1. Deploy an instance from the AMI
```bash
cdk deploy --context deploymentType=single --context baseAmi=ami-xxxxxxxxx
```

### 2. SSH into the instance
```bash
ssh -i ~/.ssh/virginia.pem ec2-user@<instance-ip>
```

### 3. Run verification commands
```bash
# Check Transparent Huge Pages (should be "never")
cat /sys/kernel/mm/transparent_hugepage/enabled

# Check CPU isolation (should show isolated cores)
cat /sys/devices/system/cpu/isolated

# Check hugepages
grep Huge /proc/meminfo

# Check CPU governor (should be "performance")
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Check I/O scheduler (should be "mq-deadline" or "[mq-deadline]")
cat /sys/block/nvme0n1/queue/scheduler

# Check kernel command line (should include isolcpus, nohz_full, etc.)
cat /proc/cmdline

# View full tuning report
cat /root/trading_system_tuning_report.txt
```

## Troubleshooting

### Build fails at CDK deploy step
- Ensure AWS credentials are configured: `aws sts get-caller-identity`
- Ensure CDK is bootstrapped: `cd cdk && cdk bootstrap`
- Check that the key pair exists in AWS: `aws ec2 describe-key-pairs --key-names virginia`

### Build fails at Ansible step

**"Instance not found in dynamic inventory" error:**
- The instance must have the tag `Name: Trading-Benchmark-AMI-Builder-Instance`
- This tag must be in `deployment/ansible/inventory/inventory.aws_ec2.yml`
- Check the inventory file includes:
  ```yaml
  include_filters:
    - tag:Name:
      - 'Trading-Benchmark-AMI-Builder-Instance'
  ```

**Other Ansible issues:**
- Verify SSH key permissions: `chmod 400 ~/.ssh/virginia.pem`
- Ensure security group allows SSH from your IP
- Check Ansible is installed: `ansible --version`
- Verify the instance tag exists: `aws ec2 describe-instances --instance-ids <instance-id> --query 'Reservations[0].Instances[0].Tags'`

### Build fails at AMI creation
- Ensure instance is stopped before AMI creation
- Check AWS service limits for AMIs in the region
- Verify IAM permissions include `ec2:CreateImage`

### SSH timeout when waiting for instance
- Check AWS console to verify instance is running
- Verify instance has a public IP address
- Ensure security group allows SSH (port 22) from your IP
- Try increasing `MAX_WAIT_TIME` in the script

### jq command not found
```bash
# Install jq
sudo apt-get install jq  # Ubuntu/Debian
brew install jq          # macOS
sudo yum install jq      # Amazon Linux/RHEL
```

### OS tuning verification failures

If you see warnings like "⚠ Hugepages not detected" or "⚠ CPU isolation not detected":
- Some settings only take effect after a full reboot
- The script creates the AMI after stopping the instance, which ensures settings persist
- You can verify after deploying an instance from the AMI

**To manually verify tuning on a deployed instance:**
```bash
# SSH into instance deployed from the AMI
ssh -i ~/.ssh/your-key.pem ec2-user@<instance-ip>

# Check tuning report
sudo cat /root/trading_system_tuning_report.txt

# Check THP
cat /sys/kernel/mm/transparent_hugepage/enabled  # Should show [never]

# Check CPU isolation
cat /sys/devices/system/cpu/isolated  # Should show isolated cores

# Check hugepages
grep Huge /proc/meminfo  # Should show allocated pages

# Check kernel parameters
cat /proc/cmdline  # Should include isolcpus, nohz_full, etc.
```

## Advanced Usage

### Building AMIs for Multiple Instance Types

**Important**: The CPU isolation settings are **baked into the AMI at build time**. For optimal performance, build separate AMIs for different instance size classes:

```bash
# Build AMI for 16-32 vCPU instances
./build-tuned-ami.sh --instance-type c7i.4xlarge --ami-name trading-16vcpu-v1

# Build AMI for 48-96 vCPU instances
./build-tuned-ami.sh --instance-type c7i.24xlarge --ami-name trading-96vcpu-v1

# Build AMI for 128-192 vCPU instances
./build-tuned-ami.sh --instance-type c7i.48xlarge --ami-name trading-192vcpu-v1
```

**Dynamic CPU Allocation at Build Time:**
- The Ansible playbook detects the builder instance's vCPU count
- Calculates optimal core allocation (housekeeping vs isolated)
- Writes these settings to GRUB config (kernel boot parameters)
- Settings are **baked into the AMI** and persist to instances launched from it

**Recommended AMI Strategy:**

| Instance Size | Build AMI On | Housekeeping | Isolated | Use For |
|---------------|--------------|--------------|----------|---------|
| Small (4-8 vCPUs) | c7i.2xlarge | 2 cores | 6 cores | Development/testing |
| Medium (16-32 vCPUs) | c7i.4xlarge | 4 cores | 12-28 cores | Production (most common) |
| Large (48-96 vCPUs) | c7i.24xlarge | 8 cores | 40-88 cores | High-throughput workloads |
| X-Large (128-192 vCPUs) | c7i.48xlarge | 16 cores | 112-176 cores | Maximum performance |

**Cross-Instance Compatibility:**
- AMIs work across instance types but with suboptimal core allocation
- Example: AMI built on c7i.4xlarge (16 vCPU) used on c7i.48xlarge (192 vCPU) will only isolate 12 cores instead of 176
- For best performance: **Match AMI size class to deployment instance size**

### Customizing Tuning Parameters

To create AMIs with different tuning profiles:

1. Modify `ansible/tune_os.yaml` to adjust parameters:
   - `isolated_cores`: Change core isolation range
   - `housekeeping_cores`: Adjust system task cores
   - `busy_poll_value`: Tune busy polling behavior

2. Build AMI with a descriptive name:
   ```bash
   ./build-tuned-ami.sh --ami-name trading-16core-v1
   ```

3. Document the configuration in AMI tags or description

### Automating AMI Builds

Integrate AMI building into CI/CD:

```bash
#!/bin/bash
# ci-build-ami.sh

# Exit on error
set -e

# Build AMI
AMI_ID=$(./build-tuned-ami.sh \
  --ami-name "trading-ami-$(git rev-parse --short HEAD)" \
  --key-file "$SSH_KEY_PATH" | \
  grep "AMI ID:" | awk '{print $3}')

# Share AMI with other accounts
aws ec2 modify-image-attribute \
  --image-id "$AMI_ID" \
  --launch-permission "Add=[{UserId=123456789012}]"

# Export for subsequent steps
echo "TUNED_AMI_ID=$AMI_ID" >> $GITHUB_OUTPUT
```

## Cleanup

### Remove Old AMIs

AMIs incur storage costs. Clean up old AMIs periodically:

```bash
# List your trading AMIs
aws ec2 describe-images \
  --owners self \
  --filters "Name=name,Values=trading-benchmark-tuned-*" \
  --query 'Images[*].[ImageId,Name,CreationDate]' \
  --output table

# Deregister an old AMI
aws ec2 deregister-image --image-id ami-xxxxxxxxx

# Delete associated snapshots
aws ec2 describe-snapshots \
  --owner-ids self \
  --filters "Name=description,Values=*ami-xxxxxxxxx*" \
  --query 'Snapshots[*].SnapshotId' \
  --output text | xargs -n1 aws ec2 delete-snapshot --snapshot-id
```

### Clean Up Failed Builds

If a build fails and leaves resources:

```bash
cd cdk
cdk destroy TradingBenchmarkAmiBuilderStack --force

# Or manually via AWS CLI
aws ec2 describe-instances \
  --filters "Name=tag:Purpose,Values=os-tuned-ami" "Name=instance-state-name,Values=running" \
  --query 'Reservations[*].Instances[*].InstanceId' \
  --output text | xargs -n1 aws ec2 terminate-instances --instance-ids
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     build-tuned-ami.sh                          │
│                    (Orchestration Script)                       │
└────────┬────────────────────────────────────────────────────────┘
         │
         ├─► Step 1: Deploy CDK Stack (ami-builder-stack.ts)
         │   └─► Creates: VPC, Security Group, Single EC2 Instance
         │
         ├─► Step 2: Wait for Instance Ready
         │   └─► SSH connectivity check
         │
         ├─► Step 3: Create Ansible Inventory
         │   └─► Temporary inventory file with instance IP
         │
         ├─► Step 4: Run tune_os.yaml Playbook
         │   └─► Applies all OS optimizations + automatic reboot
         │
         ├─► Step 5: Wait for Reboot Complete
         │   └─► SSH connectivity check after reboot
         │
         ├─► Step 6: Create AMI
         │   ├─► Stop instance
         │   ├─► Create AMI from stopped instance
         │   └─► Tag AMI with metadata
         │
         └─► Step 7: Cleanup (optional)
             └─► Destroy CDK stack

Output: AMI ID saved to ami-builder-latest.txt
```

## Support

For issues or questions:
1. Check the [main README](../README.md)
2. Review the [CLAUDE.md](../CLAUDE.md) documentation
3. Examine logs in the CDK outputs: `cdk/ami-builder-outputs.json`
4. Check Ansible output for tuning errors
5. Open an issue on GitHub

## License

This library is licensed under the MIT-0 License. See the LICENSE file.
