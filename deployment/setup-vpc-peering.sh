#!/bin/bash

# Cross-Region VPC Peering Setup Script
# Sets up VPC peering between Latency Hunting Stack and a target VPC in another region
# Handles route tables and security group updates automatically

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
HUNTING_STACK_NAME="LatencyHuntingStack"
HUNTING_REGION=""
TARGET_VPC_ID=""
TARGET_REGION=""
TARGET_SG_ID=""

# Function to print colored output
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

# Function to show usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Set up cross-region VPC peering between Latency Hunting Stack and target VPC

REQUIRED OPTIONS:
    --target-vpc-id VPC_ID       Target VPC ID (e.g., vpc-02393b8e30c6e3e5d)
    --target-region REGION       Target VPC region (e.g., eu-central-1)

OPTIONAL:
    --hunting-region REGION      Hunting stack region (default: auto-detect from stack)
    --target-sg-id SG_ID        Target security group ID to update (default: all SGs in target VPC)
    -h, --help                   Show this help message

EXAMPLES:
    # Basic usage - auto-detect hunting region
    $0 --target-vpc-id vpc-02393b8e30c6e3e5d --target-region eu-central-1

    # Specify hunting region explicitly
    $0 --target-vpc-id vpc-02393b8e30c6e3e5d --target-region eu-central-1 --hunting-region ap-northeast-1

    # Update specific security group only
    $0 --target-vpc-id vpc-02393b8e30c6e3e5d --target-region eu-central-1 --target-sg-id sg-1234567890abcdef0

NOTES:
    - Both VPCs must be in the same AWS account
    - CIDR blocks must not overlap
    - Script requires jq to be installed

EOF
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --target-vpc-id)
            TARGET_VPC_ID="$2"
            shift 2
            ;;
        --target-region)
            TARGET_REGION="$2"
            shift 2
            ;;
        --hunting-region)
            HUNTING_REGION="$2"
            shift 2
            ;;
        --target-sg-id)
            TARGET_SG_ID="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            ;;
    esac
done

# Validate required parameters
if [ -z "$TARGET_VPC_ID" ]; then
    print_error "Target VPC ID is required"
    usage
fi

if [ -z "$TARGET_REGION" ]; then
    print_error "Target region is required"
    usage
fi

# Check if jq is installed
if ! command -v jq &> /dev/null; then
    print_error "jq is not installed. Please install it first:"
    echo "  macOS: brew install jq"
    echo "  Linux: sudo apt-get install jq or sudo yum install jq"
    exit 1
fi

print_info "Starting Cross-Region VPC Peering Setup"
print_info "========================================"
print_info "Target VPC: $TARGET_VPC_ID"
print_info "Target Region: $TARGET_REGION"
print_info ""

# Step 1: Get Hunting Stack details
print_step "1. Getting Latency Hunting Stack details..."

# Auto-detect hunting region if not provided
if [ -z "$HUNTING_REGION" ]; then
    print_info "Auto-detecting hunting stack region..."
    # Try common regions
    for region in ap-northeast-1 us-east-1 eu-central-1 us-west-2; do
        if aws cloudformation describe-stacks --stack-name "$HUNTING_STACK_NAME" --region "$region" &> /dev/null; then
            HUNTING_REGION="$region"
            print_info "Found hunting stack in region: $HUNTING_REGION"
            break
        fi
    done
    
    if [ -z "$HUNTING_REGION" ]; then
        print_error "Could not find LatencyHuntingStack in any region. Please specify --hunting-region"
        exit 1
    fi
fi

# Get hunting VPC details from CloudFormation
HUNTING_VPC_ID=$(aws cloudformation describe-stack-resources \
    --stack-name "$HUNTING_STACK_NAME" \
    --region "$HUNTING_REGION" \
    --query 'StackResources[?ResourceType==`AWS::EC2::VPC`].PhysicalResourceId' \
    --output text)

if [ -z "$HUNTING_VPC_ID" ]; then
    print_error "Could not find VPC in hunting stack"
    exit 1
fi

HUNTING_VPC_CIDR=$(aws ec2 describe-vpcs \
    --vpc-ids "$HUNTING_VPC_ID" \
    --region "$HUNTING_REGION" \
    --query 'Vpcs[0].CidrBlock' \
    --output text)

print_info "Hunting VPC ID: $HUNTING_VPC_ID"
print_info "Hunting VPC CIDR: $HUNTING_VPC_CIDR"
print_info "Hunting Region: $HUNTING_REGION"
print_info ""

# Step 2: Get Target VPC details
print_step "2. Getting Target VPC details..."

TARGET_VPC_CIDR=$(aws ec2 describe-vpcs \
    --vpc-ids "$TARGET_VPC_ID" \
    --region "$TARGET_REGION" \
    --query 'Vpcs[0].CidrBlock' \
    --output text)

if [ -z "$TARGET_VPC_CIDR" ]; then
    print_error "Could not find target VPC or access denied"
    exit 1
fi

print_info "Target VPC CIDR: $TARGET_VPC_CIDR"
print_info ""

# Check for CIDR overlap
if [ "$HUNTING_VPC_CIDR" == "$TARGET_VPC_CIDR" ]; then
    print_error "VPC CIDR blocks overlap! Cannot create peering."
    print_error "Hunting: $HUNTING_VPC_CIDR"
    print_error "Target: $TARGET_VPC_CIDR"
    exit 1
fi

# Step 3: Create VPC Peering Connection
print_step "3. Creating VPC Peering Connection..."

PEERING_CONNECTION_ID=$(aws ec2 create-vpc-peering-connection \
    --vpc-id "$HUNTING_VPC_ID" \
    --peer-vpc-id "$TARGET_VPC_ID" \
    --peer-region "$TARGET_REGION" \
    --region "$HUNTING_REGION" \
    --query 'VpcPeeringConnection.VpcPeeringConnectionId' \
    --output text)

if [ -z "$PEERING_CONNECTION_ID" ]; then
    print_error "Failed to create VPC peering connection"
    exit 1
fi

print_info "Peering Connection ID: $PEERING_CONNECTION_ID"

# Add Name tag to peering connection
aws ec2 create-tags \
    --resources "$PEERING_CONNECTION_ID" \
    --tags "Key=Name,Value=Hunting-to-Target-Peering" \
    --region "$HUNTING_REGION" || true

print_info "Waiting for peering connection to be available..."
sleep 3

# Step 4: Accept VPC Peering Connection (from target region)
print_step "4. Accepting VPC Peering Connection in target region..."

aws ec2 accept-vpc-peering-connection \
    --vpc-peering-connection-id "$PEERING_CONNECTION_ID" \
    --region "$TARGET_REGION" > /dev/null

print_info "Peering connection accepted"
print_info "Waiting for peering to become active..."
sleep 5

# Step 5: Update Route Tables in Hunting VPC
print_step "5. Updating route tables in Hunting VPC..."

HUNTING_ROUTE_TABLES=$(aws ec2 describe-route-tables \
    --filters "Name=vpc-id,Values=$HUNTING_VPC_ID" \
    --region "$HUNTING_REGION" \
    --query 'RouteTables[].RouteTableId' \
    --output text)

for rt_id in $HUNTING_ROUTE_TABLES; do
    print_info "Adding route to $rt_id for $TARGET_VPC_CIDR"
    aws ec2 create-route \
        --route-table-id "$rt_id" \
        --destination-cidr-block "$TARGET_VPC_CIDR" \
        --vpc-peering-connection-id "$PEERING_CONNECTION_ID" \
        --region "$HUNTING_REGION" 2>/dev/null || print_warning "Route may already exist in $rt_id"
done

print_info ""

# Step 6: Update Route Tables in Target VPC
print_step "6. Updating route tables in Target VPC..."

TARGET_ROUTE_TABLES=$(aws ec2 describe-route-tables \
    --filters "Name=vpc-id,Values=$TARGET_VPC_ID" \
    --region "$TARGET_REGION" \
    --query 'RouteTables[].RouteTableId' \
    --output text)

for rt_id in $TARGET_ROUTE_TABLES; do
    print_info "Adding route to $rt_id for $HUNTING_VPC_CIDR"
    aws ec2 create-route \
        --route-table-id "$rt_id" \
        --destination-cidr-block "$HUNTING_VPC_CIDR" \
        --vpc-peering-connection-id "$PEERING_CONNECTION_ID" \
        --region "$TARGET_REGION" 2>/dev/null || print_warning "Route may already exist in $rt_id"
done

print_info ""

# Step 7: Update Security Groups in Target VPC
print_step "7. Updating security groups in Target VPC..."

if [ -n "$TARGET_SG_ID" ]; then
    # Update specific security group
    SECURITY_GROUPS="$TARGET_SG_ID"
    print_info "Updating specified security group: $TARGET_SG_ID"
else
    # Get all security groups in target VPC
    SECURITY_GROUPS=$(aws ec2 describe-security-groups \
        --filters "Name=vpc-id,Values=$TARGET_VPC_ID" \
        --region "$TARGET_REGION" \
        --query 'SecurityGroups[].GroupId' \
        --output text)
    print_info "Found $(echo $SECURITY_GROUPS | wc -w) security groups to update"
fi

for sg_id in $SECURITY_GROUPS; do
    print_info "Adding ingress rule to $sg_id for $HUNTING_VPC_CIDR"
    
    # Add rule to allow all traffic from hunting VPC
    aws ec2 authorize-security-group-ingress \
        --group-id "$sg_id" \
        --ip-permissions "IpProtocol=-1,FromPort=-1,ToPort=-1,IpRanges=[{CidrIp=$HUNTING_VPC_CIDR,Description='Allow all from Latency Hunting Stack'}]" \
        --region "$TARGET_REGION" 2>/dev/null || print_warning "Rule may already exist in $sg_id"
done

print_info ""

# Summary
print_step "✅ VPC Peering Setup Complete!"
echo ""
print_info "Summary:"
print_info "========"
print_info "Peering Connection ID: $PEERING_CONNECTION_ID"
print_info "Hunting VPC: $HUNTING_VPC_ID ($HUNTING_VPC_CIDR) in $HUNTING_REGION"
print_info "Target VPC: $TARGET_VPC_ID ($TARGET_VPC_CIDR) in $TARGET_REGION"
print_info ""
print_info "Next Steps:"
print_info "1. Verify connectivity by pinging between instances"
print_info "2. Update your Ansible inventory with target instance IPs"
print_info "3. Run latency tests from hunting instances to target"
print_info ""
print_info "To test connectivity:"
print_info "  ssh into a hunting instance and ping a target instance private IP"
print_info ""
print_info "To remove peering later:"
print_info "  aws ec2 delete-vpc-peering-connection --vpc-peering-connection-id $PEERING_CONNECTION_ID --region $HUNTING_REGION"
