#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
#
# End-to-End Integration Test for Trading Latency Benchmark
#
# This script performs a full lifecycle test:
#   1. Build an OS-tuned AMI
#   2. Deploy single-instance stack (client+server on one EC2)
#   3. Provision instances (install deps, build client/server)
#   4. Apply OS tuning
#   5. Start mock trading server
#   6. Start latency test client
#   7. Wait for client to finish (polls every 15s)
#   8. Collect results
#   9. Validate and tear down
#
# Usage:
#   ./e2e_integration_test.sh [options]
#
# Options:
#   -k, --key-file PATH          Path to SSH private key (required)
#   -r, --region REGION           AWS region (default: us-east-1)
#   --client-instance-type TYPE   EC2 instance type (default: c6in.2xlarge)
#   --test-size COUNT             Number of trading rounds per client (default: 10)
#   --base-ami AMI_ID             Use existing tuned AMI (skips AMI build and OS tuning)
#   --no-cleanup                  Keep all resources on success (always kept on failure)
#   --start-from-step N           Resume from step N (1-9), skipping earlier steps
#   -h, --help                    Show this help message

set -euo pipefail

# ============================================================
# Configuration
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEPLOYMENT_DIR="${REPO_ROOT}/deployment"
CDK_DIR="${DEPLOYMENT_DIR}/cdk"
ANSIBLE_DIR="${DEPLOYMENT_DIR}/ansible"
LOG_DIR="${SCRIPT_DIR}/logs/$(date +%Y%m%d-%H%M%S)"

REGION="${AWS_REGION:-us-east-1}"
SSH_KEY_FILE="${SSH_KEY_FILE:-}"
# Instance type: c6in.2xlarge chosen as default to avoid capacity issues seen with
# c7i.4xlarge in us-east-1. c6in provides 50 Gbps network bandwidth which is
# sufficient for latency benchmarking. Single EC2 instance runs both client and server.
# Override with --client-instance-type if needed.
CLIENT_INSTANCE_TYPE="c6in.2xlarge"
TEST_SIZE=10
BASE_AMI=""
CLEANUP_ON_SUCCESS=true
START_FROM_STEP=1

CLUSTER_STACK_NAME="TradingBenchmarkSingleInstanceStack"
INVENTORY="${ANSIBLE_DIR}/inventory/inventory.aws_ec2.yml"

# Track what was created for cleanup
AMI_ID=""
AMI_BUILT=false
CLUSTER_DEPLOYED=false
TEST_PASSED=false
SERVER_PRIVATE_IP=""

# ============================================================
# Colors and logging
# ============================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()      { echo -e "${BLUE}[$(date +%H:%M:%S)]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[$(date +%H:%M:%S)] ✓${NC} $*"; }
log_warn() { echo -e "${YELLOW}[$(date +%H:%M:%S)] ⚠${NC} $*"; }
log_err()  { echo -e "${RED}[$(date +%H:%M:%S)] ✗${NC} $*"; }
log_step() { echo -e "\n${BLUE}══════════════════════════════════════════════════${NC}"; echo -e "${BLUE}  $*${NC}"; echo -e "${BLUE}══════════════════════════════════════════════════${NC}"; }

# ============================================================
# Usage
# ============================================================
show_usage() {
    sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# //'
}

# ============================================================
# Argument parsing
# ============================================================
while [[ $# -gt 0 ]]; do
    case $1 in
        -k|--key-file)        SSH_KEY_FILE="$2"; shift 2 ;;
        -r|--region)          REGION="$2"; shift 2 ;;
        --client-instance-type) CLIENT_INSTANCE_TYPE="$2"; shift 2 ;;
        --server-instance-type) log_warn "--server-instance-type ignored (single EC2 mode)"; shift 2 ;;
        --test-size)          TEST_SIZE="$2"; shift 2 ;;
        --base-ami)           BASE_AMI="$2"; shift 2 ;;
        --no-cleanup)         CLEANUP_ON_SUCCESS=false; shift ;;
        --start-from-step)    START_FROM_STEP="$2"; shift 2 ;;
        -h|--help)            show_usage; exit 0 ;;
        *) log_err "Unknown option: $1"; show_usage; exit 1 ;;
    esac
done

# Validate required args
if [[ -z "$SSH_KEY_FILE" ]]; then
    log_err "SSH key file is required (--key-file)"
    show_usage
    exit 1
fi

SSH_KEY_FILE=$(eval echo "$SSH_KEY_FILE")
if [[ ! -f "$SSH_KEY_FILE" ]]; then
    log_err "SSH key file not found: $SSH_KEY_FILE"
    exit 1
fi

SSH_KEY_NAME=$(basename "$SSH_KEY_FILE" .pem)

# Validate start-from-step
if [[ "$START_FROM_STEP" -lt 1 || "$START_FROM_STEP" -gt 9 ]]; then
    log_err "--start-from-step must be between 1 and 9"
    exit 1
fi

# When resuming from a later step, assume prior infrastructure exists
if [[ "$START_FROM_STEP" -ge 2 ]]; then
    if [[ -z "$BASE_AMI" ]]; then
        # Try to load from last build
        AMI_ID=$(grep "^AMI_ID=" "${DEPLOYMENT_DIR}/ami-builder-latest.txt" 2>/dev/null | cut -d= -f2 || echo "")
        if [[ -z "$AMI_ID" ]]; then
            log_err "Resuming from step $START_FROM_STEP requires --base-ami or a previous ami-builder-latest.txt"
            exit 1
        fi
        log "Loaded AMI from previous build: $AMI_ID"
    else
        AMI_ID="$BASE_AMI"
    fi
fi

if [[ "$START_FROM_STEP" -ge 3 ]]; then
    CLUSTER_DEPLOYED=true
fi

# ============================================================
# Setup log directory
# ============================================================
mkdir -p "$LOG_DIR"
MAIN_LOG="${LOG_DIR}/e2e_test.log"
exec > >(tee -a "$MAIN_LOG") 2>&1

log_step "E2E Integration Test Starting"
log "Repo Root:           $REPO_ROOT"
log "Region:              $REGION"
log "SSH Key:             $SSH_KEY_FILE ($SSH_KEY_NAME)"
log "Client Instance:     $CLIENT_INSTANCE_TYPE"
log "Mode:                Single EC2 (client+server on localhost)"
log "Test Size:           $TEST_SIZE"
log "Base AMI:            ${BASE_AMI:-none (will build)}"
log "Cleanup on Success:  $CLEANUP_ON_SUCCESS"
log "Start from Step:     $START_FROM_STEP"
log "Log Directory:       $LOG_DIR"

# ============================================================
# Cleanup handler
# ============================================================
cleanup() {
    local exit_code=$?

    log_step "Cleanup"

    if [[ "$TEST_PASSED" == true && "$CLEANUP_ON_SUCCESS" == true ]]; then
        log "Tests passed - tearing down all resources"

        if [[ "$CLUSTER_DEPLOYED" == true ]]; then
            log "Destroying cluster stack..."
            aws cloudformation delete-stack \
                --stack-name "$CLUSTER_STACK_NAME" \
                --region "$REGION" 2>/dev/null || true
            aws cloudformation wait stack-delete-complete \
                --stack-name "$CLUSTER_STACK_NAME" \
                --region "$REGION" 2>/dev/null || log_warn "Cluster stack deletion may still be in progress"
            log_ok "Cluster stack destroyed"
        fi

        if [[ "$AMI_BUILT" == true && -n "$AMI_ID" ]]; then
            log "Deregistering AMI $AMI_ID..."
            aws ec2 deregister-image --image-id "$AMI_ID" --region "$REGION" 2>/dev/null || true

            # Clean up associated snapshots
            SNAPSHOTS=$(aws ec2 describe-snapshots \
                --owner-ids self \
                --filters "Name=description,Values=*${AMI_ID}*" \
                --query 'Snapshots[*].SnapshotId' \
                --output text \
                --region "$REGION" 2>/dev/null || echo "")
            for snap in $SNAPSHOTS; do
                aws ec2 delete-snapshot --snapshot-id "$snap" --region "$REGION" 2>/dev/null || true
            done
            log_ok "AMI and snapshots cleaned up"
        fi

        log_ok "All resources cleaned up"
    elif [[ "$TEST_PASSED" == true ]]; then
        log "Tests passed - keeping resources (--no-cleanup)"
        [[ -n "$AMI_ID" ]] && log "AMI ID: $AMI_ID"
    else
        log_warn "Tests FAILED or interrupted - keeping resources for investigation"
        [[ -n "$AMI_ID" ]] && log "AMI ID: $AMI_ID"
        [[ "$CLUSTER_DEPLOYED" == true ]] && log "Cluster stack: $CLUSTER_STACK_NAME"
        log "Logs: $LOG_DIR"
        log ""
        log "To clean up manually:"
        [[ "$CLUSTER_DEPLOYED" == true ]] && log "  aws cloudformation delete-stack --stack-name $CLUSTER_STACK_NAME --region $REGION"
        [[ -n "$AMI_ID" ]] && log "  aws ec2 deregister-image --image-id $AMI_ID --region $REGION"
    fi

    if [[ "$TEST_PASSED" == true ]]; then
        log_step "E2E INTEGRATION TEST: PASSED"
    else
        log_step "E2E INTEGRATION TEST: FAILED (exit code: $exit_code)"
    fi

    exit $exit_code
}

trap cleanup EXIT

# ============================================================
# Helper: run ansible playbook with logging
# ============================================================
run_playbook() {
    local name="$1"
    local playbook="$2"
    shift 2
    local log_file="${LOG_DIR}/ansible_${name}.log"

    log "Running playbook: $playbook"
    ansible-playbook "$playbook" \
        --key-file "$SSH_KEY_FILE" \
        -e 'ansible_host_key_checking=False' \
        "$@" 2>&1 | tee "$log_file"

    local rc=${PIPESTATUS[0]}
    if [[ $rc -ne 0 ]]; then
        log_err "Playbook $playbook failed (exit code: $rc)"
        log_err "See log: $log_file"
        return 1
    fi
    log_ok "Playbook $playbook completed"
    return 0
}

# ============================================================
# Helper: wait for SSH on instances
# ============================================================
wait_for_ssh() {
    local inventory="$1"
    local max_wait=300
    local elapsed=0

    log "Waiting for SSH access on all instances via inventory..."

    while true; do
        PING_OUTPUT=$(ansible all -i "$inventory" \
            --key-file "$SSH_KEY_FILE" \
            -e 'ansible_host_key_checking=False' \
            -m ping 2>&1 || true)

        TOTAL_HOSTS=$(echo "$PING_OUTPUT" | grep -cE "SUCCESS|UNREACHABLE|FAILED" || echo "0")
        SUCCESS_HOSTS=$(echo "$PING_OUTPUT" | grep -c "SUCCESS" || echo "0")

        if [[ "$TOTAL_HOSTS" -gt 0 && "$SUCCESS_HOSTS" -eq "$TOTAL_HOSTS" ]]; then
            log_ok "SSH access confirmed on all $SUCCESS_HOSTS instance(s)"
            return 0
        fi

        if [[ $elapsed -ge $max_wait ]]; then
            log_err "Timeout waiting for SSH access (${max_wait}s)"
            log_err "  Reachable: $SUCCESS_HOSTS / $TOTAL_HOSTS"
            echo "$PING_OUTPUT" | grep -E "UNREACHABLE|FAILED" || true
            return 1
        fi

        log "  Waiting... ${elapsed}s ($SUCCESS_HOSTS/$TOTAL_HOSTS reachable)"
        sleep 15
        elapsed=$((elapsed + 15))
    done
}

# ============================================================
# Step 1: Build OS-Tuned AMI
# ============================================================
if [[ "$START_FROM_STEP" -le 1 ]]; then
if [[ -n "$BASE_AMI" ]]; then
    log_step "Step 1: Using provided AMI (skipping build)"
    AMI_ID="$BASE_AMI"
    log "AMI ID: $AMI_ID"
else
    log_step "Step 1: Building OS-Tuned AMI"

    AMI_BUILD_LOG="${LOG_DIR}/ami_build.log"

    "${DEPLOYMENT_DIR}/build-tuned-ami.sh" \
        --instance-type "$CLIENT_INSTANCE_TYPE" \
        --key-file "$SSH_KEY_FILE" \
        --region "$REGION" \
        --ami-name "e2e-test-$(date +%Y%m%d-%H%M%S)" \
        2>&1 | tee "$AMI_BUILD_LOG"

    if [[ ${PIPESTATUS[0]} -ne 0 ]]; then
        log_err "AMI build failed"
        exit 1
    fi

    # Extract AMI ID from the build output
    AMI_ID=$(grep "^AMI_ID=" "${DEPLOYMENT_DIR}/ami-builder-latest.txt" 2>/dev/null | cut -d= -f2)

    if [[ -z "$AMI_ID" || "$AMI_ID" == "None" ]]; then
        log_err "Failed to extract AMI ID from build output"
        exit 1
    fi

    AMI_BUILT=true
    log_ok "AMI built: $AMI_ID"
fi
else
    log_step "Step 1: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 2: Deploy Cluster Placement Group Stack
# ============================================================
if [[ "$START_FROM_STEP" -le 2 ]]; then
log_step "Step 2: Deploying Single Instance Stack"

cd "$CDK_DIR"

if [[ ! -d "node_modules" ]]; then
    log "Installing CDK dependencies..."
    npm install
fi

export AWS_DEFAULT_REGION="$REGION"
export CDK_DEFAULT_REGION="$REGION"

cdk deploy "$CLUSTER_STACK_NAME" \
	--context deploymentType=single \
	--context singleEc2Instance=true \
	--context instanceType1="$CLIENT_INSTANCE_TYPE" \
	--context baseAmi="$AMI_ID" \
	--context keyPairName="$SSH_KEY_NAME" \
	--context region="$REGION" \
	--require-approval never \
	--outputs-file "${LOG_DIR}/cluster-outputs.json" \
	2>&1 | tee "${LOG_DIR}/cdk_deploy.log"

if [[ ${PIPESTATUS[0]} -ne 0 ]]; then
    log_err "CDK deploy failed"
    exit 1
fi

CLUSTER_DEPLOYED=true
log_ok "Single instance stack deployed"

# Single EC2 mode: server runs on localhost, no separate server IP needed
SERVER_PRIVATE_IP="localhost"
CLIENT_IP=$(jq -r ".${CLUSTER_STACK_NAME}.Instance1PublicIp" "${LOG_DIR}/cluster-outputs.json" 2>/dev/null || echo "")
log "Instance IP: $CLIENT_IP"
log "Server endpoint: $SERVER_PRIVATE_IP (same instance)"
else
    log_step "Step 2: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 3: Wait for instances and provision
# ============================================================
if [[ "$START_FROM_STEP" -le 3 ]]; then
log_step "Step 3: Provisioning Instances"

cd "$ANSIBLE_DIR"
export AWS_REGION="$REGION"

wait_for_ssh "$INVENTORY"

# Build extra vars for config customization
PROVISION_EXTRA_VARS=()
if [[ -n "$SERVER_PRIVATE_IP" ]]; then
    PROVISION_EXTRA_VARS+=(-e "trading_server_host=$SERVER_PRIVATE_IP")
fi
PROVISION_EXTRA_VARS+=(-e "trading_test_size=$TEST_SIZE")

sleep 5
run_playbook "provision" provision_ec2.yaml -i "$INVENTORY" "${PROVISION_EXTRA_VARS[@]}"
else
    log_step "Step 3: Skipped (resuming from step $START_FROM_STEP)"
    cd "$ANSIBLE_DIR"
    export AWS_REGION="$REGION"

    # Single EC2 mode: server always runs on localhost
    SERVER_PRIVATE_IP="localhost"
fi

# ============================================================
# Step 4: Apply OS Tuning (skipped when using a pre-built tuned AMI)
# ============================================================
if [[ -n "$AMI_ID" ]]; then
    log_step "Step 4: Skipped (AMI already includes OS tuning)"
elif [[ "$START_FROM_STEP" -le 4 ]]; then
log_step "Step 4: Applying OS Tuning"

sleep 5
run_playbook "tune_os" tune_os.yaml -i "$INVENTORY"

# Wait for reboot after tuning
log "Waiting for instances to reboot after OS tuning..."
sleep 60
wait_for_ssh "$INVENTORY"
else
    log_step "Step 4: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 5: Start Server
# ============================================================
if [[ "$START_FROM_STEP" -le 5 ]]; then
log_step "Step 5: Starting Mock Trading Server"

sleep 5
run_playbook "start_server" restart_mock_trading_server.yaml \
    -i "$INVENTORY"

# Give server time to start
sleep 10
else
    log_step "Step 5: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 6: Start Latency Test
# ============================================================
if [[ "$START_FROM_STEP" -le 6 ]]; then
log_step "Step 6: Starting Latency Test"

sleep 5
run_playbook "start_client" start_latency_test.yaml \
    -i "$INVENTORY"
else
    log_step "Step 6: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 7: Wait for Java client to finish
# ============================================================
if [[ "$START_FROM_STEP" -le 7 ]]; then
log_step "Step 7: Waiting for ExchangeFlow client to complete"

ELAPSED=0
POLL_INTERVAL=15

while true; do
    CHECK_OUTPUT=$(ansible-playbook check_client_running.yaml \
        --key-file "$SSH_KEY_FILE" \
        -e 'ansible_host_key_checking=False' \
        -i "$INVENTORY" 2>&1 || true)

    # Count RUNNING vs STOPPED across all hosts
    RUNNING_COUNT=$(echo "$CHECK_OUTPUT" | grep -c "RUNNING" || true)
    STOPPED_COUNT=$(echo "$CHECK_OUTPUT" | grep -c "STOPPED" || true)

    if [[ "$STOPPED_COUNT" -gt 0 && "$RUNNING_COUNT" -eq 0 ]]; then
        log_ok "ExchangeFlow client finished on all hosts after ${ELAPSED}s"
        break
    fi

    log "  Client still running... ${ELAPSED}s elapsed (checking every ${POLL_INTERVAL}s)"
    sleep $POLL_INTERVAL
    ELAPSED=$((ELAPSED + POLL_INTERVAL))
done
else
    log_step "Step 7: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 8: Collect results
# ============================================================
if [[ "$START_FROM_STEP" -le 8 ]]; then
log_step "Step 8: Collecting Results"

sleep 5
run_playbook "fetch_logs" fetch_histogram_logs.yaml -i "$INVENTORY"
else
    log_step "Step 8: Skipped (resuming from step $START_FROM_STEP)"
fi

# ============================================================
# Step 9: Validate results
# ============================================================
log_step "Step 9: Validating Results"

HISTOGRAM_DIR="${REPO_ROOT}/histogram_logs"
HLOG_FILES=$(find "$HISTOGRAM_DIR" -maxdepth 2 -name "*.hlog" -type f 2>/dev/null || echo "")

if [[ -z "$HLOG_FILES" ]]; then
    log_err "No histogram log files found - test may not have produced results"
    exit 1
fi

log_ok "Histogram log files found:"
for f in $HLOG_FILES; do
    FILE_SIZE=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f" 2>/dev/null || echo "0")
    log "  $(basename "$f") (${FILE_SIZE} bytes)"

    if [[ "$FILE_SIZE" -lt 100 ]]; then
        log_err "Histogram file too small - test likely failed to record data"
        exit 1
    fi
done

# Copy results to test log directory
cp $HLOG_FILES "$LOG_DIR/" 2>/dev/null || true

# Try to generate a latency report if the JAR is available
JAVA_CLIENT_JAR="${REPO_ROOT}/hft_client/java_client/target/ExchangeFlow-1.0-SNAPSHOT.jar"
RELEASE_JAR_URL="https://github.com/aws-samples/trading-latency-benchmark/releases/download/v1.0.0/ExchangeFlow-v1.0.0.jar"
DOWNLOADED_JAR="${REPO_ROOT}/hft_client/java_client/target/ExchangeFlow-v1.0.0.jar"

if [[ ! -f "$JAVA_CLIENT_JAR" && -f "$DOWNLOADED_JAR" ]]; then
    JAVA_CLIENT_JAR="$DOWNLOADED_JAR"
fi

if [[ ! -f "$JAVA_CLIENT_JAR" ]]; then
    log_warn "Java client JAR not found locally"
    log "  Checked: $JAVA_CLIENT_JAR"

    # Prompt user to download from GitHub release
    read -r -p "$(echo -e "${YELLOW}Download JAR from GitHub release? [Y/n]:${NC} ")" DOWNLOAD_CHOICE
    DOWNLOAD_CHOICE="${DOWNLOAD_CHOICE:-Y}"

    if [[ "$DOWNLOAD_CHOICE" =~ ^[Yy]$ ]]; then
        log "Downloading JAR from $RELEASE_JAR_URL ..."
        mkdir -p "$(dirname "$DOWNLOADED_JAR")"
        if curl -fSL -o "$DOWNLOADED_JAR" "$RELEASE_JAR_URL"; then
            log_ok "Downloaded to $DOWNLOADED_JAR"
            JAVA_CLIENT_JAR="$DOWNLOADED_JAR"
        else
            log_err "Failed to download JAR"
        fi
    else
        log "Skipping JAR download"
    fi
fi

if [[ -f "$JAVA_CLIENT_JAR" ]]; then
    log "Generating latency report using $(basename "$JAVA_CLIENT_JAR")..."
    for f in $HLOG_FILES; do
        REPORT=$(java -jar "$JAVA_CLIENT_JAR" latency-report "$f" 2>&1 || echo "REPORT_FAILED")
        if [[ "$REPORT" == *"REPORT_FAILED"* ]]; then
            log_warn "Could not generate report for $(basename "$f")"
        else
            echo "$REPORT" | tee "${LOG_DIR}/$(basename "$f" .hlog)_report.txt"
        fi
    done
else
    log_warn "No JAR available - skipping report generation"
    log "  (Reports can be generated on the EC2 instance)"
fi

# ============================================================
# All checks passed
# ============================================================
TEST_PASSED=true
log_ok "All validation checks passed"
log "Results saved to: $LOG_DIR"
