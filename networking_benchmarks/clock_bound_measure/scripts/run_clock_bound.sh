#!/usr/bin/env bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
set -euo pipefail

# ── Defaults ─────────────────────────────────────────────────────────────────
STACK_NAME="ClockBoundMeasureStack"
REGION=""
VERBOSE=false
TIMEOUT=900  # seconds to wait for bootstrap (Rust compilation takes ~7 min)

# ── Parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --stack-name) STACK_NAME="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --verbose) VERBOSE=true; shift ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--stack-name NAME] [--region REGION] [--verbose] [--timeout SECS]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

REGION_FLAG=""
if [[ -n "$REGION" ]]; then
    REGION_FLAG="--region $REGION"
fi

# ── Discover instances from CloudFormation outputs ───────────────────────────
echo "Querying stack outputs for: $STACK_NAME"

get_output() {
    aws cloudformation describe-stacks \
        --stack-name "$STACK_NAME" $REGION_FLAG \
        --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" \
        --output text
}

INSTANCE1_ID=$(get_output "Instance1Id")
INSTANCE2_ID=$(get_output "Instance2Id")
INSTANCE1_TYPE=$(get_output "Instance1Type")
INSTANCE2_TYPE=$(get_output "Instance2Type")

if [[ -z "$INSTANCE1_ID" || -z "$INSTANCE2_ID" ]]; then
    echo "ERROR: Could not retrieve instance IDs from stack $STACK_NAME"
    exit 1
fi

echo "Instance 1: $INSTANCE1_ID ($INSTANCE1_TYPE)"
echo "Instance 2: $INSTANCE2_ID ($INSTANCE2_TYPE)"

# ── Wait for instances to be ready ───────────────────────────────────────────
wait_for_ready() {
    local instance_id="$1"
    local elapsed=0
    local interval=15

    echo "Waiting for $instance_id to complete bootstrap..."

    while [[ $elapsed -lt $TIMEOUT ]]; do
        # Check SSM connectivity
        local status
        status=$(aws ssm describe-instance-information \
            $REGION_FLAG \
            --filters "Key=InstanceIds,Values=$instance_id" \
            --query "InstanceInformationList[0].PingStatus" \
            --output text 2>/dev/null || echo "None")

        if [[ "$status" == "Online" ]]; then
            # Check if bootstrap completed
            local cmd_id
            cmd_id=$(aws ssm send-command \
                $REGION_FLAG \
                --instance-ids "$instance_id" \
                --document-name "AWS-RunShellScript" \
                --parameters 'commands=["test -f /opt/clock-bound/status.json && echo READY || echo WAITING"]' \
                --query "Command.CommandId" \
                --output text 2>/dev/null)

            sleep 3

            local result
            result=$(aws ssm get-command-invocation \
                $REGION_FLAG \
                --command-id "$cmd_id" \
                --instance-id "$instance_id" \
                --query "StandardOutputContent" \
                --output text 2>/dev/null || echo "WAITING")

            if [[ "$result" == *"READY"* ]]; then
                echo "  $instance_id: READY"
                return 0
            fi
        fi

        sleep "$interval"
        elapsed=$((elapsed + interval))
        echo "  $instance_id: waiting... (${elapsed}s / ${TIMEOUT}s)"
    done

    echo "ERROR: Timeout waiting for $instance_id"
    return 1
}

wait_for_ready "$INSTANCE1_ID"
wait_for_ready "$INSTANCE2_ID"

# ── Run clock-bound query on each instance ───────────────────────────────────
run_query() {
    local instance_id="$1"
    local cmd_id
    cmd_id=$(aws ssm send-command \
        $REGION_FLAG \
        --instance-ids "$instance_id" \
        --document-name "AWS-RunShellScript" \
        --parameters 'commands=["cat /opt/clock-bound/status.json","echo ---SEPARATOR---","systemctl status clockbound --no-pager 2>&1 || true","echo ---SEPARATOR---","/usr/local/bin/clock-bound-client 2>&1 || echo CLIENT_NOT_AVAILABLE"]' \
        --query "Command.CommandId" \
        --output text)

    # Wait for command to complete (client binary runs 100M iterations)
    local cmd_status="InProgress"
    local wait_elapsed=0
    while [[ "$cmd_status" == "InProgress" || "$cmd_status" == "Pending" ]] && [[ $wait_elapsed -lt 90 ]]; do
        sleep 5
        wait_elapsed=$((wait_elapsed + 5))
        cmd_status=$(aws ssm get-command-invocation \
            $REGION_FLAG \
            --command-id "$cmd_id" \
            --instance-id "$instance_id" \
            --query "Status" \
            --output text 2>/dev/null || echo "InProgress")
    done

    local output
    output=$(aws ssm get-command-invocation \
        $REGION_FLAG \
        --command-id "$cmd_id" \
        --instance-id "$instance_id" \
        --query "StandardOutputContent" \
        --output text)

    echo "$output"
}

echo ""
echo "Running clock-bound queries..."
echo ""

OUTPUT1=$(run_query "$INSTANCE1_ID")
OUTPUT2=$(run_query "$INSTANCE2_ID")

# ── Parse results ────────────────────────────────────────────────────────────
parse_sync() {
    echo "$1" | python3 -c "
import sys, json
try:
    data = json.loads(sys.stdin.read().split('---SEPARATOR---')[0].strip())
    print(data.get('sync_mechanism', 'UNKNOWN'))
except:
    print('UNKNOWN')
"
}

parse_client_output() {
    echo "$1" | python3 -c "
import sys, re
parts = sys.stdin.read().split('---SEPARATOR---')
if len(parts) >= 3:
    client = parts[2].strip()
    if 'CLIENT_NOT_AVAILABLE' in client or not client:
        print('N/A (client not built)')
    else:
        m = re.search(r'within (\d+\.\d+) and (\d+\.\d+)', client)
        if m:
            earliest = float(m.group(1))
            latest = float(m.group(2))
            # [earliest, latest] = now ± bound, so the bound is half the width
            bound_ns = int((latest - earliest) / 2 * 1e9)
            bound_us = bound_ns / 1000.0
            status = 'Sync' if 'Synchronized' in client else 'Unknown'
            print(f'±{bound_us:.1f}µs ({status})')
        else:
            print(client.splitlines()[0][:35] if client.splitlines() else 'N/A')
else:
    print('N/A (parse error)')
"
}

SYNC1=$(parse_sync "$OUTPUT1")
SYNC2=$(parse_sync "$OUTPUT2")
CLIENT1=$(parse_client_output "$OUTPUT1")
CLIENT2=$(parse_client_output "$OUTPUT2")

# ── Display summary ──────────────────────────────────────────────────────────
DISPLAY_REGION="${REGION:-$(aws configure get region 2>/dev/null || echo 'unknown')}"

printf "\n"
printf "Region: %s\n\n" "$DISPLAY_REGION"
printf "╔══════════════════════╦══════════════════╦═══════════╦═════════════════════╗\n"
printf "║ %-20s ║ %-16s ║ %-9s ║ %-19s ║\n" "Instance ID" "Instance Type" "Sync" "ClockBound"
printf "╠══════════════════════╬══════════════════╬═══════════╬═════════════════════╣\n"
printf "║ %-20s ║ %-16s ║ %-9s ║ %-19s ║\n" "$INSTANCE1_ID" "$INSTANCE1_TYPE" "$SYNC1" "$CLIENT1"
printf "║ %-20s ║ %-16s ║ %-9s ║ %-19s ║\n" "$INSTANCE2_ID" "$INSTANCE2_TYPE" "$SYNC2" "$CLIENT2"
printf "╚══════════════════════╩══════════════════╩═══════════╩═════════════════════╝\n"

DASHBOARD_URL=$(get_output "DashboardUrl")
if [[ -n "$DASHBOARD_URL" && "$DASHBOARD_URL" != "None" ]]; then
    printf "\nContinuous metrics: %s\n" "$DASHBOARD_URL"
fi

# ── Verbose output ───────────────────────────────────────────────────────────
if [[ "$VERBOSE" == "true" ]]; then
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "  Instance 1: $INSTANCE1_ID ($INSTANCE1_TYPE)"
    echo "════════════════════════════════════════════════════════════════"
    echo "$OUTPUT1"
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "  Instance 2: $INSTANCE2_ID ($INSTANCE2_TYPE)"
    echo "════════════════════════════════════════════════════════════════"
    echo "$OUTPUT2"
fi
