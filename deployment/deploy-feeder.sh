#!/bin/bash

# HFT Feed Handler Deployment Script
#
# Deploys FeederStack (MockExchange + Feeder) to the feeder region and
# SubscriberStack to the subscriber region via CDK, then provisions all
# instances via Ansible:
#   1. cdk deploy FeederStack     (feeder region — exchange + feeder)
#   2. cdk deploy SubscriberStack (subscriber region — subscriber instances)
#   3. provision_feeder_nodes.yaml  (build xdp-tools + benchmark on feeder + subscribers)
#   4. provision_exchange.yaml      (build benchmark + configure GRE tunnel on exchange)
#   5. tune_os.yaml + tune_feeder.yaml (OS + BPF/XDP tuning on feeder)
#
# Cross-region fan-out:
#   Feeder (London) fans unicast UDP to Subscriber public IPs (Frankfurt).
#   Register after deploy: ./control_client <FeederPrivateIp> add <SubscriberPublicIp> <data-port>

set -e

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()    { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; }
step()    { echo -e "${CYAN}[STEP]${NC} $1"; }

# ── Defaults ──────────────────────────────────────────────────────────────────
FEEDER_REGION="eu-west-2"        # London   — exchange + feeder
SUBSCRIBER_REGION="eu-central-1" # Frankfurt — subscribers
FEEDER_KEY_PAIR="london"
SUBSCRIBER_KEY_PAIR=""           # defaults to FEEDER_KEY_PAIR
FEEDER_SSH_KEY=""                # defaults to ~/.ssh/<FEEDER_KEY_PAIR>.pem
SUBSCRIBER_SSH_KEY=""            # defaults to ~/.ssh/<SUBSCRIBER_KEY_PAIR>.pem
FEEDER_INSTANCE_TYPE=""
EXCHANGE_INSTANCE_TYPE=""
SUBSCRIBER_INSTANCE_TYPE=""
SUBSCRIBER_COUNT=""
FEEDER_VPC_CIDR=""
SUBSCRIBER_VPC_CIDR=""
FEEDER_AZ=""
SUBSCRIBER_AZ=""
MULTICAST_GROUP=""
DATA_PORT=""
CTRL_PORT=""
SOURCE_FEEDER_CIDR=""
FEEDER_CIDR=""
FEEDER_STACK_NAME="FeederStack"
SUBSCRIBER_STACK_NAME="SubscriberStack"
SKIP_TUNING="false"

# ── Usage ─────────────────────────────────────────────────────────────────────
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Deploy FeederStack (MockExchange + Feeder) to the feeder region and
SubscriberStack to the subscriber region, then provision via Ansible.

REGIONS:
    --feeder-region REGION            Feeder/exchange AWS region (default: eu-west-2)
    --subscriber-region REGION        Subscriber AWS region (default: eu-central-1)
    -r, --region REGION               Alias for --feeder-region

KEY PAIRS / SSH:
    --feeder-key-pair KEY_NAME        EC2 key pair name in feeder region (default: london)
    --subscriber-key-pair KEY_NAME    EC2 key pair name in subscriber region (default: same as feeder)
    --feeder-ssh-key PATH             Path to feeder SSH private key (default: ~/.ssh/<FEEDER_KEY_PAIR>.pem)
    --subscriber-ssh-key PATH         Path to subscriber SSH private key (default: ~/.ssh/<SUBSCRIBER_KEY_PAIR>.pem)
    -k, --key-pair KEY_NAME           Alias for --feeder-key-pair
    -i, --ssh-key PATH                Alias for --feeder-ssh-key

INSTANCE TYPES:
    -f, --feeder-type INSTANCE_TYPE   Feeder instance type (default: c7i.4xlarge)
    -e, --exchange-type INSTANCE_TYPE Mock exchange instance type (default: c7i.4xlarge)
    -s, --subscriber-type INST_TYPE   Subscriber instance type (default: c7i.4xlarge)
    -n, --subscriber-count N          Number of subscriber instances (default: 2)

NETWORK:
    -c, --feeder-vpc-cidr CIDR        Feeder VPC CIDR (default: 10.61.0.0/16)
        --subscriber-vpc-cidr CIDR    Subscriber VPC CIDR (default: 10.62.0.0/16)
    -z, --feeder-az AZ                Feeder availability zone
        --subscriber-az AZ            Subscriber availability zone
    -m, --multicast-group ADDR        Inner multicast group address (default: 224.0.31.50)
    -p, --data-port PORT              UDP data port (default: 5000)
        --ctrl-port PORT              UDP upstream control port (default: 5001)
    -g, --source-feeder-cidr CIDR     Upstream source feeder public IP /32 for feeder SG rule
        --feeder-cidr CIDR            Feeder public IP /32 for subscriber SG rule

OTHER:
        --skip-tuning                 Skip tune_os.yaml + tune_feeder.yaml
    -h, --help                        Show this help

EXAMPLES:
    # Minimal - feeder in London, subscribers in Frankfurt (script defaults)
    $0

    # Custom regions / key pairs
    $0 --feeder-region eu-west-2 --subscriber-region eu-central-1 \\
       --feeder-key-pair london --subscriber-key-pair frankfurt

    # Lock down SG rules once IPs are known
    $0 --source-feeder-cidr 1.2.3.4/32 --feeder-cidr 5.6.7.8/32

EOF
    exit 1
}

# ── Arg parsing ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --feeder-region)          FEEDER_REGION="$2";            shift 2 ;;
        --subscriber-region)      SUBSCRIBER_REGION="$2";        shift 2 ;;
        -r|--region)              FEEDER_REGION="$2";            shift 2 ;;
        --feeder-key-pair)        FEEDER_KEY_PAIR="$2";          shift 2 ;;
        --subscriber-key-pair)    SUBSCRIBER_KEY_PAIR="$2";      shift 2 ;;
        -k|--key-pair)            FEEDER_KEY_PAIR="$2";          shift 2 ;;
        --feeder-ssh-key)         FEEDER_SSH_KEY="$2";           shift 2 ;;
        --subscriber-ssh-key)     SUBSCRIBER_SSH_KEY="$2";       shift 2 ;;
        -i|--ssh-key)             FEEDER_SSH_KEY="$2";           shift 2 ;;
        -f|--feeder-type)         FEEDER_INSTANCE_TYPE="$2";     shift 2 ;;
        -e|--exchange-type)       EXCHANGE_INSTANCE_TYPE="$2";   shift 2 ;;
        -s|--subscriber-type)     SUBSCRIBER_INSTANCE_TYPE="$2"; shift 2 ;;
        -n|--subscriber-count)    SUBSCRIBER_COUNT="$2";         shift 2 ;;
        -c|--feeder-vpc-cidr)     FEEDER_VPC_CIDR="$2";          shift 2 ;;
        --subscriber-vpc-cidr)    SUBSCRIBER_VPC_CIDR="$2";      shift 2 ;;
        -z|--feeder-az)           FEEDER_AZ="$2";                shift 2 ;;
        --subscriber-az)          SUBSCRIBER_AZ="$2";            shift 2 ;;
        -m|--multicast-group)     MULTICAST_GROUP="$2";          shift 2 ;;
        -p|--data-port)           DATA_PORT="$2";                shift 2 ;;
        --ctrl-port)              CTRL_PORT="$2";                shift 2 ;;
        -g|--source-feeder-cidr)  SOURCE_FEEDER_CIDR="$2";       shift 2 ;;
        --feeder-cidr)            FEEDER_CIDR="$2";              shift 2 ;;
        --skip-tuning)            SKIP_TUNING="true";            shift ;;
        -h|--help)                usage ;;
        *) error "Unknown option: $1"; usage ;;
    esac
done

FEEDER_KEY_PAIR="${FEEDER_KEY_PAIR:-"london"}"
SUBSCRIBER_KEY_PAIR="${SUBSCRIBER_KEY_PAIR:-"frankfurt"}"
FEEDER_SSH_KEY="${FEEDER_SSH_KEY:-$HOME/.ssh/${FEEDER_KEY_PAIR}.pem}"
SUBSCRIBER_SSH_KEY="${SUBSCRIBER_SSH_KEY:-$HOME/.ssh/${SUBSCRIBER_KEY_PAIR}.pem}"

# ── Prerequisites ─────────────────────────────────────────────────────────────
for cmd in cdk aws ansible-playbook jq; do
    if ! command -v "$cmd" &>/dev/null; then
        error "'$cmd' is not installed or not on PATH."
        exit 1
    fi
done

if ! aws sts get-caller-identity &>/dev/null; then
    error "AWS CLI not configured. Run 'aws configure' first."
    exit 1
fi

if [ ! -f "$FEEDER_SSH_KEY" ]; then
    error "Feeder SSH key not found: $FEEDER_SSH_KEY"
    error "Provide the correct path with --feeder-ssh-key or ensure ~/.ssh/${FEEDER_KEY_PAIR}.pem exists."
    exit 1
fi
chmod 600 "$FEEDER_SSH_KEY"

if [ ! -f "$SUBSCRIBER_SSH_KEY" ]; then
    error "Subscriber SSH key not found: $SUBSCRIBER_SSH_KEY"
    error "Provide the correct path with --subscriber-ssh-key or ensure ~/.ssh/${SUBSCRIBER_KEY_PAIR}.pem exists."
    exit 1
fi
chmod 600 "$SUBSCRIBER_SSH_KEY"

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CDK_DIR="$SCRIPT_DIR/cdk"
ANSIBLE_DIR="$SCRIPT_DIR/ansible"

# ── Banner ────────────────────────────────────────────────────────────────────
info "Feeder Deployment"
info "============================="
info "Feeder region:     $FEEDER_REGION  (exchange + feeder)"
info "Subscriber region: $SUBSCRIBER_REGION  (subscribers)"
info "Feeder key pair:   $FEEDER_KEY_PAIR"
info "Subscriber key:    $SUBSCRIBER_KEY_PAIR"
info "Feeder SSH key:    $FEEDER_SSH_KEY"
info "Subscriber SSH:    $SUBSCRIBER_SSH_KEY"
[ -n "$EXCHANGE_INSTANCE_TYPE"   ] && info "Exchange type:     $EXCHANGE_INSTANCE_TYPE"
[ -n "$FEEDER_INSTANCE_TYPE"     ] && info "Feeder type:       $FEEDER_INSTANCE_TYPE"
[ -n "$SUBSCRIBER_INSTANCE_TYPE" ] && info "Subscriber type:   $SUBSCRIBER_INSTANCE_TYPE"
[ -n "$SUBSCRIBER_COUNT"         ] && info "Subscriber count:  $SUBSCRIBER_COUNT"
[ -n "$MULTICAST_GROUP"          ] && info "Multicast group:   $MULTICAST_GROUP"
[ -n "$DATA_PORT"                ] && info "Data port:         $DATA_PORT"
[ -n "$CTRL_PORT"                ] && info "Ctrl port:         $CTRL_PORT"
[ -n "$SOURCE_FEEDER_CIDR"       ] && info "Source CIDR:       $SOURCE_FEEDER_CIDR"
[ -n "$FEEDER_CIDR"              ] && info "Feeder CIDR:       $FEEDER_CIDR"
info ""

# ── CDK bootstrap ─────────────────────────────────────────────────────────────
cd "$CDK_DIR"
if [ ! -d "node_modules" ]; then
    step "Installing CDK dependencies..."
    npm install
fi

ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
for REGION in "$FEEDER_REGION" "$SUBSCRIBER_REGION"; do
    if ! aws cloudformation describe-stacks --stack-name CDKToolkit --region "$REGION" &>/dev/null; then
        step "Bootstrapping CDK in $REGION..."
        cdk bootstrap "aws://${ACCOUNT}/${REGION}"
    fi
done

# ── Build CDK context ─────────────────────────────────────────────────────────
CTX="--context deploymentType=feeder"
CTX="$CTX --context feederRegion=$FEEDER_REGION"
CTX="$CTX --context subscriberRegion=$SUBSCRIBER_REGION"
CTX="$CTX --context keyPairName=$FEEDER_KEY_PAIR"
CTX="$CTX --context subscriberKeyPairName=$SUBSCRIBER_KEY_PAIR"
[ -n "$FEEDER_INSTANCE_TYPE"     ] && CTX="$CTX --context feederInstanceType=$FEEDER_INSTANCE_TYPE"
[ -n "$EXCHANGE_INSTANCE_TYPE"   ] && CTX="$CTX --context exchangeInstanceType=$EXCHANGE_INSTANCE_TYPE"
[ -n "$SUBSCRIBER_INSTANCE_TYPE" ] && CTX="$CTX --context subscriberInstanceType=$SUBSCRIBER_INSTANCE_TYPE"
[ -n "$SUBSCRIBER_COUNT"         ] && CTX="$CTX --context subscriberCount=$SUBSCRIBER_COUNT"
[ -n "$FEEDER_VPC_CIDR"          ] && CTX="$CTX --context feederVpcCidr=$FEEDER_VPC_CIDR"
[ -n "$SUBSCRIBER_VPC_CIDR"      ] && CTX="$CTX --context subscriberVpcCidr=$SUBSCRIBER_VPC_CIDR"
[ -n "$FEEDER_AZ"                ] && CTX="$CTX --context feederAz=$FEEDER_AZ"
[ -n "$SUBSCRIBER_AZ"            ] && CTX="$CTX --context subscriberAz=$SUBSCRIBER_AZ"
[ -n "$MULTICAST_GROUP"          ] && CTX="$CTX --context multicastGroup=$MULTICAST_GROUP"
[ -n "$DATA_PORT"                ] && CTX="$CTX --context dataPort=$DATA_PORT"
[ -n "$CTRL_PORT"                ] && CTX="$CTX --context ctrlPort=$CTRL_PORT"
[ -n "$SOURCE_FEEDER_CIDR"       ] && CTX="$CTX --context sourceFeederCidr=$SOURCE_FEEDER_CIDR"
[ -n "$FEEDER_CIDR"              ] && CTX="$CTX --context feederCidr=$FEEDER_CIDR"

# ── CDK deploy ────────────────────────────────────────────────────────────────
# step "Deploying $FEEDER_STACK_NAME to $FEEDER_REGION..."
# cdk deploy "$FEEDER_STACK_NAME" $CTX --require-approval never

step "Deploying $SUBSCRIBER_STACK_NAME to $SUBSCRIBER_REGION..."
cdk deploy "$SUBSCRIBER_STACK_NAME" $CTX --require-approval never

# ── Fetch FeederStack outputs ─────────────────────────────────────────────────
step "Fetching $FEEDER_STACK_NAME outputs from $FEEDER_REGION..."
FEEDER_OUTPUTS=$(aws cloudformation describe-stacks \
    --stack-name "$FEEDER_STACK_NAME" \
    --region "$FEEDER_REGION" \
    --query 'Stacks[0].Outputs' \
    --output json)

get_feeder_output() {
    echo "$FEEDER_OUTPUTS" | jq -r --arg key "$1" '.[] | select(.OutputKey==$key) | .OutputValue'
}

FEEDER_PUBLIC_IP=$(get_feeder_output 'FeederPublicIp')
FEEDER_PRIVATE_IP=$(get_feeder_output 'FeederPrivateIp')
EXCHANGE_PUBLIC_IP=$(get_feeder_output 'ExchangePublicIp')
EXCHANGE_PRIVATE_IP=$(get_feeder_output 'ExchangePrivateIp')

if [ -z "$EXCHANGE_PUBLIC_IP" ] || [ -z "$FEEDER_PUBLIC_IP" ]; then
    error "One or more feeder/exchange IPs are empty - CloudFormation outputs lookup failed."
    error "Stack: $FEEDER_STACK_NAME  Region: $FEEDER_REGION"
    error "Run: aws cloudformation describe-stacks --stack-name $FEEDER_STACK_NAME --region $FEEDER_REGION"
    exit 1
fi

# ── Fetch SubscriberStack outputs ─────────────────────────────────────────────
step "Fetching $SUBSCRIBER_STACK_NAME outputs from $SUBSCRIBER_REGION..."
SUBSCRIBER_OUTPUTS=$(aws cloudformation describe-stacks \
    --stack-name "$SUBSCRIBER_STACK_NAME" \
    --region "$SUBSCRIBER_REGION" \
    --query 'Stacks[0].Outputs' \
    --output json)

SUBSCRIBER_PUBLIC_IPS=()
while IFS= read -r ip; do
    [ -n "$ip" ] && SUBSCRIBER_PUBLIC_IPS+=("$ip")
done < <(echo "$SUBSCRIBER_OUTPUTS" | jq -r '.[] | select(.OutputKey | test("^Subscriber[0-9]+PublicIp$")) | .OutputValue' | sort)

if [ ${#SUBSCRIBER_PUBLIC_IPS[@]} -eq 0 ]; then
    error "No subscriber IPs found in $SUBSCRIBER_STACK_NAME ($SUBSCRIBER_REGION)."
    error "Run: aws cloudformation describe-stacks --stack-name $SUBSCRIBER_STACK_NAME --region $SUBSCRIBER_REGION"
    exit 1
fi

info ""
info "Exchange  ($FEEDER_REGION):      $EXCHANGE_PUBLIC_IP (public) / $EXCHANGE_PRIVATE_IP (private)"
info "Feeder    ($FEEDER_REGION):      $FEEDER_PUBLIC_IP (public) / $FEEDER_PRIVATE_IP (private)"
info "Subscribers ($SUBSCRIBER_REGION): ${SUBSCRIBER_PUBLIC_IPS[*]}"

# Save outputs for reference
echo "$FEEDER_OUTPUTS"     > "$SCRIPT_DIR/feeder-outputs.json"
echo "$SUBSCRIBER_OUTPUTS" > "$SCRIPT_DIR/subscriber-outputs.json"
info "CFn outputs:  feeder-outputs.json  subscriber-outputs.json"
info ""

# ── Wait for SSH ──────────────────────────────────────────────────────────────
wait_for_ssh() {
    local host="$1"
    local label="$2"
    local key="$3"
    local attempts=0
    local max=30
    step "Waiting for SSH on $label ($host)..."
    until ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes \
              -i "$key" "ec2-user@$host" exit 0 2>/dev/null; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge "$max" ]; then
            error "SSH to $host timed out after $((max * 10))s"
            exit 1
        fi
        sleep 10
    done
    info "  $label is reachable."
}

wait_for_ssh "$EXCHANGE_PUBLIC_IP" "Exchange" "$FEEDER_SSH_KEY"
wait_for_ssh "$FEEDER_PUBLIC_IP"   "Feeder"   "$FEEDER_SSH_KEY"
for ip in "${SUBSCRIBER_PUBLIC_IPS[@]}"; do
    wait_for_ssh "$ip" "Subscriber ($ip)" "$SUBSCRIBER_SSH_KEY"
done

# ── Ansible inventory ─────────────────────────────────────────────────────────
# Dynamic EC2 inventory — discovers instances in both regions by Role tag.
# SSH keys default to ~/.ssh/london.pem (eu-west-2) and ~/.ssh/frankfurt.pem (eu-central-1).
# Override in feeder.aws_ec2.yml or via ansible_ssh_private_key_file group_vars.
INVENTORY="$ANSIBLE_DIR/inventory/feeder.aws_ec2.yml"
info ""

# Use an array — avoids word-splitting of the --ssh-extra-args value
ANSIBLE_OPTS=(-i "$INVENTORY" --ssh-extra-args '-o StrictHostKeyChecking=no')

# ── Provision feeder + subscribers ────────────────────────────────────────────
step "Provisioning feeder nodes (build xdp-tools + benchmark)..."
cd "$ANSIBLE_DIR"
ansible-playbook "${ANSIBLE_OPTS[@]}" provision_feeder_nodes.yaml

# ── Provision exchange (build + GRE tunnel) ───────────────────────────────────
step "Provisioning mock exchange (build benchmark + configure GRE tunnel to feeder)..."
ansible-playbook "${ANSIBLE_OPTS[@]}" provision_exchange.yaml \
    --extra-vars "feeder_private_ip=$FEEDER_PRIVATE_IP"

# ── OS + feeder tuning ────────────────────────────────────────────────────────
if [ "$SKIP_TUNING" = "false" ]; then
    step "Applying feeder-specific tuning on feeder..."
    ansible-playbook "${ANSIBLE_OPTS[@]}" -l feeder tune_feeder.yaml
else
    warn "Skipping tuning (--skip-tuning passed)."
fi

# ── Done ──────────────────────────────────────────────────────────────────────
info ""
info "======================================================"
info "  Feed Handler deployment complete"
info "======================================================"
info ""
info "Exchange  ($FEEDER_REGION):      $EXCHANGE_PUBLIC_IP (public)  /  $EXCHANGE_PRIVATE_IP (private)"
info "Feeder    ($FEEDER_REGION):      $FEEDER_PUBLIC_IP (public)    /  $FEEDER_PRIVATE_IP (private)"
info "Subscribers ($SUBSCRIBER_REGION):"
for ip in "${SUBSCRIBER_PUBLIC_IPS[@]}"; do
    info "  $ip"
done
info ""
info "Next steps (run manually after deploy):"
info ""
info "  1. Start feeder (GRE mode) in $FEEDER_REGION:"
info "     ssh ec2-user@$FEEDER_PUBLIC_IP"
info "     sudo ./packet_replicator eth0 ${MULTICAST_GROUP:-224.0.31.50} ${DATA_PORT:-5000} true --gre"
info ""
info "  2. Register subscribers on feeder (cross-region, public IPs):"
for ip in "${SUBSCRIBER_PUBLIC_IPS[@]}"; do
    info "     ./control_client $FEEDER_PRIVATE_IP add $ip ${DATA_PORT:-5000}"
done
info ""
info "  Inventory: ansible/inventory/feeder.aws_ec2.yml (dynamic EC2, reuse for ad-hoc playbook runs)"
info ""
info "  3. Send test traffic from exchange (GRE tunnel already configured by Ansible):"
info "     ssh ec2-user@$EXCHANGE_PUBLIC_IP"
info "     ./test_client ${MULTICAST_GROUP:-224.0.31.50} ${DATA_PORT:-5000} 10 'trade' --iface eth0"
info ""
info "  Cross-region: to fan-out to this feeder from another region's source feeder:"
info "     ./control_client <source-feeder-private-ip> add $FEEDER_PUBLIC_IP ${DATA_PORT:-5000}"
[ -z "$SOURCE_FEEDER_CIDR" ] && \
    info "     Then redeploy with --source-feeder-cidr <source-feeder-public-ip>/32 to restrict SG."
