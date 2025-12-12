#!/bin/bash

# Latency Hunting Deployment Script
# This script deploys diverse EC2 instance types with placement groups
# to find optimal network placement for latency-sensitive applications

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
REGION="ap-northeast-1"  # Tokyo region
KEY_PAIR_NAME="virginia"
VPC_CIDR="10.100.0.0/16"  # Default non-overlapping CIDR
VPC_ID=""  # Optional: use existing VPC
SUBNET_ID=""  # Required for BYOVPC mode
SECURITY_GROUP_ID=""  # Optional for BYOVPC mode
USE_EXISTING_VPC="false"  # Set to true to use BYOVPC stack
ELASTIC_IPS=""  # Optional: comma-separated list of Elastic IP allocation IDs
STACK_NAME="LatencyHuntingStack"

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

# Function to show usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Deploy Latency Hunting stack to find optimal placement for low latency

OPTIONS:
    -r, --region REGION          AWS region (default: ap-northeast-1 / Tokyo)
    -k, --key-pair KEY_NAME      EC2 key pair name (default: virginia)
    -c, --vpc-cidr CIDR          VPC CIDR block for new VPC (default: 10.100.0.0/16)
    -v, --vpc-id VPC_ID          VPC ID for BYOVPC mode (requires --subnet-id)
    -s, --subnet-id SUBNET_ID    Subnet ID for BYOVPC mode (required with --vpc-id)
    -g, --security-group-id SG_ID Security group ID (optional, creates one if not provided)
    -e, --elastic-ips EIP_LIST   Comma-separated Elastic IP allocation IDs (optional)
    --use-existing-vpc           Use BYOVPC stack (never creates/manages VPC)
    -h, --help                   Show this help message

EXAMPLES:
    # Deploy with CDK-managed VPC (creates new VPC)
    $0 --region ap-northeast-1 --key-pair my-keypair

    # Use existing VPC (RECOMMENDED - never manages VPC)
    $0 --use-existing-vpc \
      --region ap-northeast-1 \
      --vpc-id vpc-02393b8e30c6e3e5d \
      --subnet-id subnet-xxxxx \
      --key-pair tokyo_keypair

    # With existing security group
    $0 --use-existing-vpc \
      --region ap-northeast-1 \
      --vpc-id vpc-02393b8e30c6e3e5d \
      --subnet-id subnet-xxxxx \
      --security-group-id sg-xxxxx \
      --key-pair my-keypair

    # With Elastic IPs (first N instances get EIPs, rest get regular public IPs)
    $0 --region eu-central-1 \
      --key-pair frankfurt \
      --elastic-ips eipalloc-12345678,eipalloc-87654321,eipalloc-abcdef01

EOF
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--region)
            REGION="$2"
            shift 2
            ;;
        -k|--key-pair)
            KEY_PAIR_NAME="$2"
            shift 2
            ;;
        -c|--vpc-cidr)
            VPC_CIDR="$2"
            shift 2
            ;;
        -v|--vpc-id)
            VPC_ID="$2"
            shift 2
            ;;
        -s|--subnet-id)
            SUBNET_ID="$2"
            shift 2
            ;;
        -g|--security-group-id)
            SECURITY_GROUP_ID="$2"
            shift 2
            ;;
        -e|--elastic-ips)
            ELASTIC_IPS="$2"
            shift 2
            ;;
        --use-existing-vpc)
            USE_EXISTING_VPC="true"
            shift
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

print_info "Starting Latency Hunting Deployment"
print_info "================================"

if [ "$USE_EXISTING_VPC" = "true" ]; then
    print_info "Mode: BYOVPC (Bring Your Own VPC)"
    print_info "Region: $REGION"
    print_info "Key Pair: $KEY_PAIR_NAME"
    print_info "VPC ID: $VPC_ID"
    print_info "Subnet ID: $SUBNET_ID"
    if [ -n "$SECURITY_GROUP_ID" ]; then
        print_info "Security Group: $SECURITY_GROUP_ID"
    else
        print_info "Security Group: Will create minimal one"
    fi
    if [ -n "$ELASTIC_IPS" ]; then
        print_info "Elastic IPs: $ELASTIC_IPS"
    fi
    STACK_NAME="LatencyHuntingBYOVPCStack"
    
    # Validate required parameters
    if [ -z "$VPC_ID" ]; then
        print_error "VPC ID is required when using --use-existing-vpc"
        exit 1
    fi
    if [ -z "$SUBNET_ID" ]; then
        print_error "Subnet ID is required when using --use-existing-vpc"
        exit 1
    fi
else
    print_info "Mode: Standard (CDK-managed VPC)"
    print_info "Region: $REGION"
    print_info "Key Pair: $KEY_PAIR_NAME"
    if [ -n "$VPC_ID" ]; then
        print_info "Using existing VPC: $VPC_ID"
    else
        print_info "Creating new VPC with CIDR: $VPC_CIDR"
    fi
    if [ -n "$ELASTIC_IPS" ]; then
        print_info "Elastic IPs: $ELASTIC_IPS"
    fi
fi
print_info ""

# Check if CDK is installed
if ! command -v cdk &> /dev/null; then
    print_error "AWS CDK is not installed. Please install it first:"
    echo "  npm install -g aws-cdk"
    exit 1
fi

# Check if AWS CLI is configured
if ! aws sts get-caller-identity &> /dev/null; then
    print_error "AWS CLI is not configured. Please run 'aws configure' first."
    exit 1
fi

# Navigate to CDK directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CDK_DIR="$SCRIPT_DIR/cdk"

cd "$CDK_DIR"

# Install dependencies if needed
if [ ! -d "node_modules" ]; then
    print_info "Installing CDK dependencies..."
    npm install
fi

# Bootstrap CDK if needed (only first time per account/region)
print_info "Checking CDK bootstrap status..."
if ! aws cloudformation describe-stacks --stack-name CDKToolkit --region "$REGION" &> /dev/null; then
    print_warning "CDK not bootstrapped in $REGION. Bootstrapping now..."
    cdk bootstrap aws://$(aws sts get-caller-identity --query Account --output text)/$REGION
fi

# Build CDK context parameters
CDK_CONTEXT="--context deploymentType=latency-hunting"
CDK_CONTEXT="$CDK_CONTEXT --context region=$REGION"
CDK_CONTEXT="$CDK_CONTEXT --context keyPairName=$KEY_PAIR_NAME"

# Add Elastic IPs if provided
if [ -n "$ELASTIC_IPS" ]; then
    CDK_CONTEXT="$CDK_CONTEXT --context elasticIps=$ELASTIC_IPS"
fi

# Add mode-specific parameters
if [ "$USE_EXISTING_VPC" = "true" ]; then
    CDK_CONTEXT="$CDK_CONTEXT --context useExistingVpc=true"
    CDK_CONTEXT="$CDK_CONTEXT --context vpcId=$VPC_ID"
    CDK_CONTEXT="$CDK_CONTEXT --context subnetId=$SUBNET_ID"
    if [ -n "$SECURITY_GROUP_ID" ]; then
        CDK_CONTEXT="$CDK_CONTEXT --context securityGroupId=$SECURITY_GROUP_ID"
    fi
else
    # Standard mode - use original VPC logic
    if [ -n "$VPC_ID" ]; then
        CDK_CONTEXT="$CDK_CONTEXT --context vpcId=$VPC_ID"
    else
        CDK_CONTEXT="$CDK_CONTEXT --context vpcCidr=$VPC_CIDR"
    fi
fi

# Deploy the stack
print_info "Deploying Latency Hunting Stack..."
print_info "This will launch multiple instance types with placement groups"
print_info "Some instance types may fail due to capacity constraints - this is expected"
print_info ""

if cdk deploy $STACK_NAME $CDK_CONTEXT --require-approval never; then
    print_info ""
    print_info "✅ Deployment successful!"
    print_info ""
    
    # Get stack outputs
    print_info "Fetching stack outputs..."
    OUTPUTS=$(aws cloudformation describe-stacks \
        --stack-name "$STACK_NAME" \
        --region "$REGION" \
        --query 'Stacks[0].Outputs' \
        --output json)
    
    # Create output directory and save outputs to file
    mkdir -p "$SCRIPT_DIR/latency-hunting"
    OUTPUT_FILE="$SCRIPT_DIR/latency-hunting/deployment-outputs.json"
    echo "$OUTPUTS" > "$OUTPUT_FILE"
    print_info "Stack outputs saved to: $OUTPUT_FILE"
    
    # Get total instance types attempted
    TOTAL_TYPES=$(echo "$OUTPUTS" | jq -r '.[] | select(.OutputKey=="TotalInstanceTypes") | .OutputValue')
    
    # Query actual instances via EC2 API using tags
    print_info ""
    print_info "Querying deployed instances..."
    INSTANCES=$(aws ec2 describe-instances \
        --region "$REGION" \
        --filters "Name=tag:Architecture,Values=latency-hunting" "Name=instance-state-name,Values=pending,running" \
        --query 'Reservations[].Instances[].[InstanceId,InstanceType,State.Name,PublicIpAddress]' \
        --output json)
    
    SUCCESS_COUNT=$(echo "$INSTANCES" | jq '. | length')
    
    print_info ""
    print_info "Summary:"
    print_info "  Total instance types attempted: $TOTAL_TYPES"
    print_info "  Successfully launched: $SUCCESS_COUNT"
    print_info "  Failed (capacity/compatibility issues): $((TOTAL_TYPES - SUCCESS_COUNT))"
    print_info ""
    
    # Show successful instances
    print_info "Successfully launched instances:"
    echo "$INSTANCES" | jq -r '.[] | "\(.[1]) (\(.[2])): \(.[0]) - \(.[3] // "pending")"' | while read line; do
        print_info "  $line"
    done
    
    print_info ""
    print_info "Next steps:"
    print_info "  1. Wait for instances to fully initialize (2-3 minutes)"
    print_info "  2. Run latency tests: ./latency-hunting/run-hunting-tests.sh"
    print_info "  3. Analyze results: ./latency-hunting/analyze-hunting-results.sh"
    
else
    print_error "Deployment failed. Check the error messages above."
    exit 1
fi
