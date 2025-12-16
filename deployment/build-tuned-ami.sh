#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# Script to build an OS-tuned AMI for trading applications
# This script deploys a single EC2 instance, applies OS tuning, and creates an AMI

set -e  # Exit on error

# Configuration variables with defaults
STACK_NAME="TradingBenchmarkAmiBuilderStack"
INSTANCE_TYPE="${INSTANCE_TYPE:-c7i.4xlarge}"
SSH_KEY_FILE="${SSH_KEY_FILE:-~/.ssh/tlb-demo.pem}"
SSH_KEY_NAME=""  # Will be derived from SSH_KEY_FILE
REGION="${AWS_REGION:-us-east-1}"
AMI_NAME_PREFIX="trading-benchmark-tuned"
CLEANUP="${CLEANUP:-true}"
MAX_WAIT_TIME=600  # 10 minutes
ANSIBLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/ansible" && pwd)"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to display usage information
function show_usage() {
    cat << EOF
Usage: $0 [options]

Build an OS-tuned AMI for trading applications by deploying an EC2 instance,
running OS tuning optimizations, and creating an AMI from the tuned instance.

Options:
  -i, --instance-type TYPE    EC2 instance type to use (default: c7i.4xlarge)
  -k, --key-file PATH         Path to SSH private key (default: ~/.ssh/virginia.pem)
  -n, --ami-name NAME         Custom AMI name (default: trading-benchmark-tuned-TIMESTAMP)
  -r, --region REGION         AWS region (default: us-east-1)
  --no-cleanup                Keep the instance running after AMI creation
  -h, --help                  Show this help message

Environment Variables:
  INSTANCE_TYPE               EC2 instance type (same as --instance-type)
  SSH_KEY_FILE                Path to SSH key (same as --key-file)
  AWS_REGION                  AWS region (same as --region)
  CLEANUP                     Set to 'false' to keep instance (same as --no-cleanup)

Example:
  $0 --instance-type c7i.4xlarge --key-file ~/.ssh/my-key.pem

EOF
}

# Parse command line arguments
CUSTOM_AMI_NAME=""
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -i|--instance-type)
            INSTANCE_TYPE="$2"
            shift 2
            ;;
        -k|--key-file)
            SSH_KEY_FILE="$2"
            shift 2
            ;;
        -n|--ami-name)
            CUSTOM_AMI_NAME="$2"
            shift 2
            ;;
        -r|--region)
            REGION="$2"
            shift 2
            ;;
        --no-cleanup)
            CLEANUP="false"
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            show_usage
            exit 1
            ;;
    esac
done

# Validate SSH key file
SSH_KEY_FILE=$(eval echo "$SSH_KEY_FILE")
if [ ! -f "$SSH_KEY_FILE" ]; then
    echo -e "${RED}Error: SSH key file not found: $SSH_KEY_FILE${NC}"
    exit 1
fi

# Derive SSH_KEY_NAME from SSH_KEY_FILE if not set
# Extract filename without path and .pem extension
if [ -z "$SSH_KEY_NAME" ]; then
    SSH_KEY_NAME=$(basename "$SSH_KEY_FILE" .pem)
fi

# Set AMI name
if [ -z "$CUSTOM_AMI_NAME" ]; then
    TIMESTAMP=$(date +%Y%m%d-%H%M%S)
    AMI_NAME="${AMI_NAME_PREFIX}-${TIMESTAMP}"
else
    AMI_NAME="$CUSTOM_AMI_NAME"
fi

echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN}Trading Benchmark AMI Builder${NC}"
echo -e "${GREEN}======================================================${NC}"
echo "Instance Type: $INSTANCE_TYPE"
echo "SSH Key File: $SSH_KEY_FILE"
echo "SSH Key Name: $SSH_KEY_NAME"
echo "Region: $REGION"
echo "AMI Name: $AMI_NAME"
echo "Cleanup After: $CLEANUP"
echo -e "${GREEN}======================================================${NC}"

# Step 1: Deploy CDK stack
echo -e "\n${YELLOW}[Step 1/7] Deploying CDK stack...${NC}"
cd "$(dirname "${BASH_SOURCE[0]}")/cdk"

# Check if node_modules exists, if not run npm install
if [ ! -d "node_modules" ]; then
    echo "Installing CDK dependencies..."
    npm install
fi

# Set AWS region environment variables for CDK
export AWS_DEFAULT_REGION="$REGION"
export CDK_DEFAULT_REGION="$REGION"

# Deploy the AMI builder stack
echo "Deploying AMI builder stack to region: $REGION"
cdk deploy $STACK_NAME \
    --context deploymentType=ami-builder \
    --context instanceType="$INSTANCE_TYPE" \
    --context keyPairName="$SSH_KEY_NAME" \
    --context region="$REGION" \
    --require-approval never \
    --outputs-file ./ami-builder-outputs.json

# Extract outputs
INSTANCE_ID=$(jq -r ".${STACK_NAME}.InstanceId" ./ami-builder-outputs.json)
INSTANCE_IP=$(jq -r ".${STACK_NAME}.InstancePublicIp" ./ami-builder-outputs.json)

if [ -z "$INSTANCE_ID" ] || [ "$INSTANCE_ID" == "null" ]; then
    echo -e "${RED}Error: Failed to get instance ID from CDK outputs${NC}"
    exit 1
fi

echo -e "${GREEN}Instance deployed: $INSTANCE_ID ($INSTANCE_IP)${NC}"

# Step 2: Wait for instance to be running and ready
echo -e "\n${YELLOW}[Step 2/7] Waiting for instance to be ready...${NC}"
echo "Waiting for instance status checks..."
aws ec2 wait instance-status-ok --instance-ids "$INSTANCE_ID" --region "$REGION" || {
    echo -e "${RED}Warning: Instance status check wait timed out, continuing anyway...${NC}"
}

# Wait for SSH to be available
echo "Waiting for SSH to be available..."
ELAPSED=0
while ! ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=5 \
    -q ec2-user@"$INSTANCE_IP" exit 2>/dev/null; do

    if [ $ELAPSED -ge $MAX_WAIT_TIME ]; then
        echo -e "${RED}Error: Timeout waiting for SSH access${NC}"
        exit 1
    fi

    echo "Waiting for SSH... ($ELAPSED seconds elapsed)"
    sleep 10
    ELAPSED=$((ELAPSED + 10))
done

echo -e "${GREEN}Instance is ready and SSH is accessible${NC}"

# Step 3: Run OS tuning playbook using dynamic inventory
echo -e "\n${YELLOW}[Step 3/7] Running OS tuning playbook...${NC}"
echo "This will take several minutes and will reboot the instance..."
cd "$ANSIBLE_DIR"

# Set AWS region for dynamic inventory lookup
export AWS_REGION="$REGION"

# Verify instance is discoverable in dynamic inventory
INVENTORY_HOSTNAME="Trading-Benchmark-AMI-Builder-Instance"
echo "Verifying instance is discoverable in dynamic inventory (region: $REGION)..."
echo "Looking for host: $INVENTORY_HOSTNAME"
ANSIBLE_INVENTORY_CHECK=$(ansible-inventory -i ./inventory/ami-builder-inventory.aws_ec2.yml --list 2>&1 | grep -c "$INVENTORY_HOSTNAME" || true)
if [ "$ANSIBLE_INVENTORY_CHECK" -eq 0 ]; then
    echo -e "${RED}Error: Host '$INVENTORY_HOSTNAME' not found in dynamic inventory${NC}"
    echo "Checking what's in the inventory..."
    ansible-inventory -i ./inventory/ami-builder-inventory.aws_ec2.yml --graph
    echo ""
    echo -e "${YELLOW}Troubleshooting: Ensure instance has these tags:${NC}"
    echo "  - Role: ami-builder"
    echo "  - Purpose: os-tuned-ami"
    echo "  - Name: Trading-Benchmark-AMI-Builder-Instance"
    exit 1
fi
echo -e "${GREEN}Host '$INVENTORY_HOSTNAME' found in inventory${NC}"

# Run the playbook using the AMI builder dynamic inventory
echo "Running OS tuning playbook on host: $INVENTORY_HOSTNAME"
ANSIBLE_OUTPUT=$(mktemp)
ansible-playbook tune_os.yaml \
    -i ./inventory/ami-builder-inventory.aws_ec2.yml \
    --key-file "$SSH_KEY_FILE" \
    -e 'ansible_host_key_checking=False' \
    --limit "$INVENTORY_HOSTNAME" 2>&1 | tee "$ANSIBLE_OUTPUT"

ANSIBLE_EXIT_CODE=${PIPESTATUS[0]}
if [ $ANSIBLE_EXIT_CODE -ne 0 ]; then
    echo -e "${RED}Error: Ansible playbook failed with exit code $ANSIBLE_EXIT_CODE${NC}"
    echo "Check output above for details"
    rm -f "$ANSIBLE_OUTPUT"
    exit 1
fi

# Verify playbook ran on at least one host
HOSTS_CHANGED=$(grep -c "ok=" "$ANSIBLE_OUTPUT" || true)
if [ "$HOSTS_CHANGED" -eq 0 ]; then
    echo -e "${RED}Error: Ansible playbook did not run on any hosts${NC}"
    echo "Playbook output:"
    cat "$ANSIBLE_OUTPUT"
    rm -f "$ANSIBLE_OUTPUT"
    exit 1
fi

rm -f "$ANSIBLE_OUTPUT"
echo -e "${GREEN}OS tuning completed successfully${NC}"

# Step 4: Wait for instance to be back online after reboot
echo -e "\n${YELLOW}[Step 4/7] Waiting for instance to come back online after reboot...${NC}"
sleep 30  # Give it some time to start rebooting

ELAPSED=0
while ! ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=5 \
    -q ec2-user@"$INSTANCE_IP" exit 2>/dev/null; do

    if [ $ELAPSED -ge $MAX_WAIT_TIME ]; then
        echo -e "${RED}Error: Timeout waiting for instance to come back online${NC}"
        exit 1
    fi

    echo "Waiting for instance to come back online... ($ELAPSED seconds elapsed)"
    sleep 10
    ELAPSED=$((ELAPSED + 10))
done

echo -e "${GREEN}Instance is back online${NC}"

# Give the system a bit more time to fully stabilize
echo "Waiting for system to stabilize..."
sleep 30

# Verify tuning was applied
echo "Verifying OS tuning..."

# Check if tuning report exists
TUNING_REPORT_EXISTS=$(ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    ec2-user@"$INSTANCE_IP" \
    "sudo test -f /root/trading_system_tuning_report.txt && echo 'exists' || echo 'missing'" 2>/dev/null)

if [ "$TUNING_REPORT_EXISTS" != "exists" ]; then
    echo -e "${RED}Error: OS tuning report not found on instance${NC}"
    echo -e "${RED}This indicates the tune_os.yaml playbook did not complete successfully${NC}"
    rm -f "$TEMP_INVENTORY"
    exit 1
fi
echo -e "${GREEN}✓ Tuning report found${NC}"

# Verify specific tuning markers
echo "Checking tuning markers..."
VERIFICATION_FAILED=0

# Detect architecture on remote instance
REMOTE_ARCH=$(ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    ec2-user@"$INSTANCE_IP" \
    "uname -m 2>/dev/null || echo 'unknown'")

IS_ARM64=false
if [ "$REMOTE_ARCH" = "aarch64" ]; then
    IS_ARM64=true
fi

# Check THP configuration (architecture-aware)
THP_STATUS=$(ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    ec2-user@"$INSTANCE_IP" \
    "cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo 'error'")

if [ "$IS_ARM64" = true ]; then
    # Graviton: expect madvise (AWS best practice for better TLB performance)
    if echo "$THP_STATUS" | grep -q "madvise"; then
        echo -e "${GREEN}✓ THP configured as 'madvise' (Graviton/ARM64 best practice for TLB performance)${NC}"
    else
        echo -e "${YELLOW}⚠ THP status: $THP_STATUS (expected 'madvise' for Graviton)${NC}"
        VERIFICATION_FAILED=1
    fi
else
    # x86: expect never (traditional HFT/trading best practice)
    if echo "$THP_STATUS" | grep -q "never"; then
        echo -e "${GREEN}✓ THP disabled (x86 best practice for predictable latency)${NC}"
    else
        echo -e "${YELLOW}⚠ THP status: $THP_STATUS (expected 'never' for x86)${NC}"
        VERIFICATION_FAILED=1
    fi
fi

# Check CPU isolation
CPU_ISOLATED=$(ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    ec2-user@"$INSTANCE_IP" \
    "cat /sys/devices/system/cpu/isolated 2>/dev/null || echo 'error'")

if [ "$CPU_ISOLATED" != "error" ] && [ -n "$CPU_ISOLATED" ]; then
    echo -e "${GREEN}✓ CPU cores isolated: $CPU_ISOLATED${NC}"
else
    echo -e "${YELLOW}⚠ CPU isolation not detected (may require reboot to take effect)${NC}"
fi

# Check hugepages
HUGEPAGES=$(ssh -i "$SSH_KEY_FILE" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    ec2-user@"$INSTANCE_IP" \
    "grep HugePages_Total /proc/meminfo | awk '{print \$2}' 2>/dev/null || echo '0'")

if [ "$HUGEPAGES" -gt 0 ]; then
    echo -e "${GREEN}✓ Hugepages configured: $HUGEPAGES pages${NC}"
else
    echo -e "${YELLOW}⚠ Hugepages not detected${NC}"
    VERIFICATION_FAILED=1
fi

if [ $VERIFICATION_FAILED -eq 1 ]; then
    echo -e "${YELLOW}Warning: Some tuning verifications failed, but continuing...${NC}"
    echo -e "${YELLOW}The AMI may need a reboot for all settings to take effect${NC}"
fi

echo -e "${GREEN}OS tuning verification completed${NC}"

# Step 5: Stop instance and create AMI
echo -e "\n${YELLOW}[Step 5/7] Stopping instance and creating AMI...${NC}"
echo "Stopping instance..."
aws ec2 stop-instances --instance-ids "$INSTANCE_ID" --region "$REGION" > /dev/null
aws ec2 wait instance-stopped --instance-ids "$INSTANCE_ID" --region "$REGION"
echo -e "${GREEN}Instance stopped${NC}"

echo "Creating AMI: $AMI_NAME"
AMI_ID=$(aws ec2 create-image \
    --instance-id "$INSTANCE_ID" \
    --name "$AMI_NAME" \
    --description "OS-tuned AMI for trading applications (built from $INSTANCE_TYPE)" \
    --tag-specifications "ResourceType=image,Tags=[{Key=Name,Value=$AMI_NAME},{Key=Purpose,Value=trading-benchmark},{Key=Tuned,Value=true}]" \
    --region "$REGION" \
    --output text \
    --query 'ImageId')

if [ -z "$AMI_ID" ] || [ "$AMI_ID" == "None" ]; then
    echo -e "${RED}Error: Failed to create AMI${NC}"
    exit 1
fi

echo -e "${GREEN}AMI creation initiated: $AMI_ID${NC}"
echo "Waiting for AMI to be available (this may take several minutes)..."
aws ec2 wait image-available --image-ids "$AMI_ID" --region "$REGION"
echo -e "${GREEN}AMI is now available!${NC}"

# Step 6: Clean up
if [ "$CLEANUP" == "true" ]; then
    echo -e "\n${YELLOW}[Step 6/7] Cleaning up resources...${NC}"

    # Use CloudFormation to delete stack (more reliable than CDK with version mismatches)
    echo "Destroying CDK stack via CloudFormation..."
    aws cloudformation delete-stack --stack-name "$STACK_NAME" --region "$REGION"

    echo "Waiting for stack deletion to complete..."
    aws cloudformation wait stack-delete-complete --stack-name "$STACK_NAME" --region "$REGION" 2>/dev/null || {
        echo -e "${YELLOW}Note: Stack deletion initiated but may still be in progress${NC}"
    }

    # Clean up output file
    CDK_DIR="$(dirname "${BASH_SOURCE[0]}")/cdk"
    rm -f "$CDK_DIR/ami-builder-outputs.json"

    echo -e "${GREEN}Cleanup completed${NC}"
else
    echo -e "\n${YELLOW}[Step 6/7] Skipping cleanup (instance will remain running)${NC}"
    echo "Instance ID: $INSTANCE_ID"
    echo "Instance IP: $INSTANCE_IP"
    echo ""
    echo "To destroy the stack later, run:"
    echo "  cd deployment/cdk && cdk destroy $STACK_NAME"
fi

# Final output
echo ""
echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN}AMI BUILD COMPLETE!${NC}"
echo -e "${GREEN}======================================================${NC}"
echo "AMI ID: $AMI_ID"
echo "AMI Name: $AMI_NAME"
echo "Region: $REGION"
echo ""
echo "You can now use this AMI in your deployments:"
echo "  cdk deploy --context baseAmi=$AMI_ID"
echo ""
echo "Or reference it in your CDK code:"
echo "  MachineImage.genericLinux({ '$REGION': '$AMI_ID' })"
echo -e "${GREEN}======================================================${NC}"

# Save AMI ID to a file for easy reference
AMI_INFO_FILE="$(dirname "${BASH_SOURCE[0]}")/ami-builder-latest.txt"
cat > "$AMI_INFO_FILE" << EOF
AMI_ID=$AMI_ID
AMI_NAME=$AMI_NAME
REGION=$REGION
INSTANCE_TYPE=$INSTANCE_TYPE
BUILD_DATE=$(date)
EOF

echo "AMI details saved to: $AMI_INFO_FILE"
