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
    -c, --vpc-cidr CIDR          VPC CIDR block (default: 10.100.0.0/16)
    -h, --help                   Show this help message

EXAMPLES:
    # Deploy in Tokyo region with default settings
    $0 --region ap-northeast-1

    # Deploy with custom CIDR to avoid overlap
    $0 --region ap-northeast-1 --vpc-cidr 10.200.0.0/16

    # Deploy with custom key pair and CIDR
    $0 --key-pair my-keypair --vpc-cidr 10.150.0.0/16

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
print_info "Region: $REGION"
print_info "Key Pair: $KEY_PAIR_NAME"
print_info "VPC CIDR: $VPC_CIDR"
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
CDK_CONTEXT="$CDK_CONTEXT --context vpcCidr=$VPC_CIDR"

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
