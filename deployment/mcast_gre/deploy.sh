#!/bin/bash
#
# Single-region deployment: CDK (SingleRegionStack) + Ansible provisioning.
# If the stack already exists, CDK deploy is skipped and the script jumps
# straight to fetching outputs and configuring nodes.
#
set -euo pipefail

# ── Colour helpers ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
step()  { echo -e "${CYAN}[STEP]${NC}  $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CDK_DIR="$SCRIPT_DIR/cdk"

# ── Defaults ───────────────────────────────────────────────────────────────────
REGION="eu-central-1"
KEY_PAIR=""
SSH_KEY_FILE=""
AMI_ID=""
INSTANCE_TYPE=""
SUBSCRIBER_COUNT=""
STACK_NAME="SingleRegionStack"
INVENTORY=""          # defaults to inventory.yml after arg parsing

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Deploy SingleRegionStack (exchange + feeder + subscribers in a CPG) then
provision all nodes via Ansible.  If the stack already exists, CDK deploy
is skipped and the script proceeds directly to fetching outputs.

CDK:
    -r, --region REGION           AWS region (default: eu-central-1)
    -k, --key-pair KEY_NAME       EC2 key pair name in the region (required for new deployments)
        --ami AMI_ID              Custom AMI ID (default: latest Amazon Linux 2023)
        --instance-type TYPE      Instance type for all nodes (default: c7i.4xlarge)
    -n, --subscriber-count N      Number of subscriber instances (default: 1)

SSH / Ansible:
    -i, --ssh-key PATH            Path to SSH private key (required)
        --inventory PATH          Override Ansible inventory (default: inventory.aws_ec2.yml)

OTHER:
    -h, --help                    Show this help

EXAMPLES:
    $0 --region eu-west-2 --key-pair my-key --ssh-key ~/.ssh/my-key.pem
    $0 --region eu-west-2 --ssh-key ~/.ssh/my-key.pem   # stack already exists
EOF
    exit 1
}

# ── Arg parsing ────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--region)          REGION="$2";            shift 2 ;;
        -k|--key-pair)        KEY_PAIR="$2";          shift 2 ;;
        --ami)                AMI_ID="$2";            shift 2 ;;
        --instance-type)      INSTANCE_TYPE="$2";     shift 2 ;;
        -n|--subscriber-count) SUBSCRIBER_COUNT="$2"; shift 2 ;;
        -i|--ssh-key)         SSH_KEY_FILE="$2";      shift 2 ;;
        --inventory)          INVENTORY="$2";         shift 2 ;;
        -h|--help)            usage ;;
        *) error "Unknown option: $1"; usage ;;
    esac
done

INVENTORY="${INVENTORY:-$SCRIPT_DIR/inventory.aws_ec2.yml}"

# ── Prerequisites ──────────────────────────────────────────────────────────────
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

if [[ -z "$SSH_KEY_FILE" ]]; then
    error "--ssh-key is required."
    exit 1
fi
if [[ ! -f "$SSH_KEY_FILE" ]]; then
    error "SSH key not found: $SSH_KEY_FILE"
    exit 1
fi
chmod 600 "$SSH_KEY_FILE"

# ── CDK bootstrap ──────────────────────────────────────────────────────────────
cd "$CDK_DIR"
if [ ! -d "node_modules" ]; then
    step "Installing CDK dependencies..."
    npm install
fi

ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
if ! aws cloudformation describe-stacks --stack-name CDKToolkit --region "$REGION" &>/dev/null; then
    step "Bootstrapping CDK in $REGION..."
    cdk bootstrap "aws://${ACCOUNT}/${REGION}"
fi

# ── CDK deploy (skip if stack already exists) ──────────────────────────────────
CTX="--context deploymentType=single-region"
CTX="$CTX --context region=$REGION"
[ -n "$KEY_PAIR"          ] && CTX="$CTX --context keyPairName=$KEY_PAIR"
[ -n "$AMI_ID"            ] && CTX="$CTX --context amiId=$AMI_ID"
[ -n "$INSTANCE_TYPE"     ] && CTX="$CTX --context instanceType=$INSTANCE_TYPE"
[ -n "$SUBSCRIBER_COUNT"  ] && CTX="$CTX --context subscriberCount=$SUBSCRIBER_COUNT"

if aws cloudformation describe-stacks --stack-name "$STACK_NAME" --region "$REGION" &>/dev/null; then
    warn "Stack $STACK_NAME already exists in $REGION — skipping CDK deploy."
else
    if [[ -z "$KEY_PAIR" ]]; then
        error "--key-pair is required for new deployments."
        exit 1
    fi
    step "Deploying $STACK_NAME to $REGION..."
    cdk deploy --all $CTX --require-approval never
fi

# ── Fetch stack outputs ────────────────────────────────────────────────────────
step "Fetching $STACK_NAME outputs from $REGION..."
STACK_OUTPUTS=$(aws cloudformation describe-stacks \
    --stack-name "$STACK_NAME" \
    --region "$REGION" \
    --query 'Stacks[0].Outputs' \
    --output json)

get_output() {
    echo "$STACK_OUTPUTS" | jq -r --arg key "$1" '.[] | select(.OutputKey==$key) | .OutputValue'
}

EXCHANGE_PUBLIC_IP=$(get_output 'ExchangePublicIp')
EXCHANGE_PRIVATE_IP=$(get_output 'ExchangePrivateIp')
FEEDER_PUBLIC_IP=$(get_output 'FeederPublicIp')
FEEDER_PRIVATE_IP=$(get_output 'FeederPrivateIp')

EXCHANGE_PUBLIC_IP=$(get_output 'ExchangePublicIp')
EXCHANGE_PRIVATE_IP=$(get_output 'ExchangePrivateIp')
FEEDER_PUBLIC_IP=$(get_output 'FeederPublicIp')
FEEDER_PRIVATE_IP=$(get_output 'FeederPrivateIp')

SUBSCRIBER_PUBLIC_IPS=()
while IFS= read -r ip; do
    [ -n "$ip" ] && SUBSCRIBER_PUBLIC_IPS+=("$ip")
done < <(echo "$STACK_OUTPUTS" | jq -r '.[] | select(.OutputKey | test("^Subscriber[0-9]+PublicIp$")) | .OutputValue' | sort)

SUBSCRIBER_PRIVATE_IPS=()
while IFS= read -r ip; do
    [ -n "$ip" ] && SUBSCRIBER_PRIVATE_IPS+=("$ip")
done < <(echo "$STACK_OUTPUTS" | jq -r '.[] | select(.OutputKey | test("^Subscriber[0-9]+PrivateIp$")) | .OutputValue' | sort)

if [ -z "$EXCHANGE_PUBLIC_IP" ] || [ -z "$FEEDER_PUBLIC_IP" ]; then
    error "One or more IPs are empty — CloudFormation outputs lookup failed."
    error "Run: aws cloudformation describe-stacks --stack-name $STACK_NAME --region $REGION"
    exit 1
fi

info ""
info "Exchange    ($REGION): $EXCHANGE_PUBLIC_IP (public) / $EXCHANGE_PRIVATE_IP (private)"
info "Feeder      ($REGION): $FEEDER_PUBLIC_IP (public) / $FEEDER_PRIVATE_IP (private)"
info "Subscribers ($REGION):"
for i in "${!SUBSCRIBER_PUBLIC_IPS[@]}"; do
    info "  ${SUBSCRIBER_PUBLIC_IPS[$i]} (public) / ${SUBSCRIBER_PRIVATE_IPS[$i]:-unknown} (private)"
done

echo "$STACK_OUTPUTS" > "$SCRIPT_DIR/stack-outputs.json"
info "CFn outputs: stack-outputs.json"
info ""

# ── Wait for SSH ───────────────────────────────────────────────────────────────
wait_for_ssh() {
    local host="$1" label="$2" attempts=0 max=30
    step "Waiting for SSH on $label ($host)..."
    until ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes \
              -i "$SSH_KEY_FILE" "ec2-user@$host" exit 0 2>/dev/null; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge "$max" ]; then
            error "SSH to $host timed out after $((max * 10))s"
            exit 1
        fi
        sleep 10
    done
    info "  $label is reachable."
}

wait_for_ssh "$EXCHANGE_PUBLIC_IP" "Exchange"
wait_for_ssh "$FEEDER_PUBLIC_IP"   "Feeder"
for ip in "${SUBSCRIBER_PUBLIC_IPS[@]}"; do
    wait_for_ssh "$ip" "Subscriber ($ip)"
done

# ── Ansible ────────────────────────────────────────────────────────────────────
export SSH_KEY_FILE
export AWS_DEFAULT_REGION="$REGION"

ANSIBLE_OPTS=(-i "$INVENTORY" --ssh-extra-args '-o StrictHostKeyChecking=no')

step "Feeder tuning (BPF JIT + ENA XDP queue headroom)..."
ansible-playbook "${ANSIBLE_OPTS[@]}" -l feeder "$SCRIPT_DIR/tune_feeder.yaml"

step "Provisioning nodes (xdp-tools + binaries + GRE + services)..."
ansible-playbook "${ANSIBLE_OPTS[@]}" "$SCRIPT_DIR/configure.yaml" \
    --extra-vars "feeder_private_ip=$FEEDER_PRIVATE_IP"


# ── Done ───────────────────────────────────────────────────────────────────────
info ""
info "=================================================="
info "  Deployment complete"
info "=================================================="
info ""
info "Exchange    ($REGION): $EXCHANGE_PUBLIC_IP (public)  /  $EXCHANGE_PRIVATE_IP (private)"
info "Feeder      ($REGION): $FEEDER_PUBLIC_IP (public)    /  $FEEDER_PRIVATE_IP (private)"
info "Subscribers ($REGION):"
for i in "${!SUBSCRIBER_PUBLIC_IPS[@]}"; do
    info "  ${SUBSCRIBER_PUBLIC_IPS[$i]} (public) / ${SUBSCRIBER_PRIVATE_IPS[$i]:-unknown} (private)"
done
info ""
BENCH="~/gre-benchmark/deployment/mcast_gre/benchmark"

info "Next steps:"
info ""
info "  1. Check feeder service:"
info "       ssh -i $SSH_KEY_FILE ec2-user@$FEEDER_PUBLIC_IP"
info "       sudo systemctl status packet-replicator"
info ""
info "  2. On each subscriber — start receiver (run BEFORE sender):"
for i in "${!SUBSCRIBER_PUBLIC_IPS[@]}"; do
    info "       ssh -i $SSH_KEY_FILE ec2-user@${SUBSCRIBER_PUBLIC_IPS[$i]}"
    info "       sudo $BENCH/receiver -i \$IFACE -B $BENCH/subscriber_filter.o -c 10000"
done
info ""
info "  3. On exchange — start sender (after all receivers are waiting):"
info "       ssh -i $SSH_KEY_FILE ec2-user@$EXCHANGE_PUBLIC_IP"
info "       sudo $BENCH/sender -I \$IFACE -D $FEEDER_PRIVATE_IP -g 224.0.31.50 -p 5000 -c 10000"
