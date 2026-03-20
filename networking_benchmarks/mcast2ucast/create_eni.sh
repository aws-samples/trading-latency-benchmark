#!/bin/bash
# create_eni.sh — Create and attach a secondary ENI for DPDK kernel bypass.
# Run this from a machine with AWS CLI access (not from the instance itself).
#
# Usage: ./create_eni.sh <instance-id> <subnet-id> <security-group-id> [region]
# Example:
#   ./create_eni.sh i-0123456789abcdef0 subnet-0123456789abcdef0 sg-0123456789abcdef0 eu-central-1

set -euo pipefail

INSTANCE_ID="${1:?Usage: $0 <instance-id> <subnet-id> <sg-id> [region]}"
SUBNET_ID="${2:?Missing subnet-id}"
SG_ID="${3:?Missing security-group-id}"
REGION="${4:-eu-central-1}"

echo "=== Creating secondary ENI for DPDK ==="
echo "  Instance: $INSTANCE_ID"
echo "  Subnet:   $SUBNET_ID"
echo "  SG:       $SG_ID"
echo "  Region:   $REGION"
echo ""

# 1. Create the ENI
ENI_ID=$(aws ec2 create-network-interface \
    --subnet-id "$SUBNET_ID" \
    --groups "$SG_ID" \
    --description "mcast2ucast DPDK secondary ENI" \
    --region "$REGION" \
    --query 'NetworkInterface.NetworkInterfaceId' \
    --output text)
echo "  [+] Created ENI: $ENI_ID"

# 2. Get the private IP and MAC
ENI_IP=$(aws ec2 describe-network-interfaces \
    --network-interface-ids "$ENI_ID" \
    --region "$REGION" \
    --query 'NetworkInterfaces[0].PrivateIpAddress' \
    --output text)
ENI_MAC=$(aws ec2 describe-network-interfaces \
    --network-interface-ids "$ENI_ID" \
    --region "$REGION" \
    --query 'NetworkInterfaces[0].MacAddress' \
    --output text)
echo "  [+] IP: $ENI_IP  MAC: $ENI_MAC"

# 3. Disable source/dest check (required for forwarding rewritten packets)
aws ec2 modify-network-interface-attribute \
    --network-interface-id "$ENI_ID" \
    --no-source-dest-check \
    --region "$REGION"
echo "  [+] Source/dest check disabled"

# 4. Attach to instance at device index 1
ATTACH_ID=$(aws ec2 attach-network-interface \
    --network-interface-id "$ENI_ID" \
    --instance-id "$INSTANCE_ID" \
    --device-index 1 \
    --region "$REGION" \
    --query 'AttachmentId' \
    --output text)
echo "  [+] Attached: $ATTACH_ID"

# 5. Enable delete-on-termination so cleanup is automatic
aws ec2 modify-network-interface-attribute \
    --network-interface-id "$ENI_ID" \
    --attachment "AttachmentId=$ATTACH_ID,DeleteOnTermination=true" \
    --region "$REGION"
echo "  [+] Delete-on-termination enabled"

echo ""
echo "=== Summary ==="
echo "  ENI ID:     $ENI_ID"
echo "  Private IP: $ENI_IP"
echo "  MAC:        $ENI_MAC"
echo "  Attachment: $ATTACH_ID"
echo ""
echo "SSH into the instance and run:"
echo "  sudo ./setup_ena_bypass.sh"
echo "to bind the new NIC to DPDK."
