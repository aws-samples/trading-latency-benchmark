#!/usr/bin/env bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
set -euo pipefail

# ── Defaults ─────────────────────────────────────────────────────────────────
STACK_NAME="ClockBoundMeasureStack"
REGION=""

# ── Parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --stack-name) STACK_NAME="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--stack-name NAME] [--region REGION]"
            echo ""
            echo "Demonstrates ClockBound event ordering by triggering events on"
            echo "two instances and showing whether bounded timestamps allow"
            echo "deterministic ordering."
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

REGION_FLAG=""
if [[ -n "$REGION" ]]; then
    REGION_FLAG="--region $REGION"
fi

# ── Discover instances ───────────────────────────────────────────────────────
get_output() {
    aws cloudformation describe-stacks \
        --stack-name "$STACK_NAME" $REGION_FLAG \
        --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" \
        --output text
}

INSTANCE_A_ID=$(get_output "Instance1Id")
INSTANCE_B_ID=$(get_output "Instance2Id")
INSTANCE_A_TYPE=$(get_output "Instance1Type")
INSTANCE_B_TYPE=$(get_output "Instance2Type")

if [[ -z "$INSTANCE_A_ID" || -z "$INSTANCE_B_ID" ]]; then
    echo "ERROR: Could not retrieve instance IDs from stack $STACK_NAME"
    echo "Deploy first: ./scripts/e2e_test.sh --region <region> --no-cleanup"
    exit 1
fi

# Get sync mechanism for each instance
get_sync() {
    local instance_id="$1"
    local cmd_id
    cmd_id=$(aws ssm send-command $REGION_FLAG \
        --instance-ids "$instance_id" \
        --document-name "AWS-RunShellScript" \
        --parameters 'commands=["cat /opt/clock-bound/status.json"]' \
        --query "Command.CommandId" --output text)
    sleep 3
    aws ssm get-command-invocation $REGION_FLAG \
        --command-id "$cmd_id" --instance-id "$instance_id" \
        --query "StandardOutputContent" --output text 2>/dev/null | \
        python3 -c "import sys,json; print(json.loads(sys.stdin.read()).get('sync_mechanism','?'))" 2>/dev/null || echo "?"
}

SYNC_A=$(get_sync "$INSTANCE_A_ID")
SYNC_B=$(get_sync "$INSTANCE_B_ID")

echo "═══════════════════════════════════════════════════════════════"
echo "  ClockBound Event Ordering Demo"
echo "═══════════════════════════════════════════════════════════════"
echo "  Instance A: $INSTANCE_A_ID ($INSTANCE_A_TYPE, $SYNC_A)"
echo "  Instance B: $INSTANCE_B_ID ($INSTANCE_B_TYPE, $SYNC_B)"
echo "  Region:     ${REGION:-$(aws configure get region 2>/dev/null || echo 'default')}"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# ── Measure actual clock error bounds from each instance ─────────────────────
get_bound() {
    local instance_id="$1"
    local cmd_id
    cmd_id=$(aws ssm send-command $REGION_FLAG \
        --instance-ids "$instance_id" \
        --document-name "AWS-RunShellScript" \
        --parameters 'commands=["/usr/local/bin/clock-bound-client 2>&1 | head -1"]' \
        --query "Command.CommandId" --output text)

    local status="InProgress"
    local elapsed=0
    while [[ "$status" == "InProgress" || "$status" == "Pending" ]] && [[ $elapsed -lt 90 ]]; do
        sleep 3
        elapsed=$((elapsed + 3))
        status=$(aws ssm get-command-invocation $REGION_FLAG \
            --command-id "$cmd_id" --instance-id "$instance_id" \
            --query "Status" --output text 2>/dev/null || echo "InProgress")
    done

    local output
    output=$(aws ssm get-command-invocation $REGION_FLAG \
        --command-id "$cmd_id" --instance-id "$instance_id" \
        --query "StandardOutputContent" --output text 2>/dev/null || echo "")

    # Return bound width in microseconds
    echo "$output" | python3 -c "
import sys, re
line = sys.stdin.read()
m = re.search(r'within ([\d.]+) and ([\d.]+)', line)
if m:
    print(f'{(float(m.group(2)) - float(m.group(1))) * 1e6:.1f}')
else:
    print('0')
"
}


# ── Measure actual bounds ────────────────────────────────────────────────────
echo "Measuring clock error bounds..."
BOUND_A_US=$(get_bound "$INSTANCE_A_ID")
echo "  Instance A: ±${BOUND_A_US}µs ($SYNC_A)"
BOUND_B_US=$(get_bound "$INSTANCE_B_ID")
echo "  Instance B: ±${BOUND_B_US}µs ($SYNC_B)"
echo ""

# ── Simulate event ordering at various delays ────────────────────────────────
# Uses measured bounds to show ordering logic. Given two events separated by
# delay D, event A has interval [T, T + bound_A] and event B has interval
# [T + D, T + D + bound_B]. Ordering is possible when A.latest < B.earliest,
# i.e., T + bound_A < T + D, i.e., D > bound_A.
# But since both have uncertainty: D > bound_A + bound_B for guaranteed ordering
# across instances (worst case: A is as late as possible, B is as early as possible).

DELAYS_US=(0 50 100 500 1000 2000 5000 10000)
ORDERED_AT=""

python3 -c "
bound_a = float('$BOUND_A_US')
bound_b = float('$BOUND_B_US')
sync_a = '$SYNC_A'
sync_b = '$SYNC_B'
delays_us = [0, 50, 100, 500, 1000, 2000, 5000, 10000]

# Minimum separation for ordering: A.latest < B.earliest
# A occupies [T_a - bound_a/2, T_a + bound_a/2] (centered on true time)
# B occupies [T_b - bound_b/2, T_b + bound_b/2]
# With delay D between true times: T_b = T_a + D
# ORDERED when: T_a + bound_a/2 < T_b - bound_b/2
#            => T_a + bound_a/2 < T_a + D - bound_b/2
#            => D > (bound_a + bound_b) / 2
# But ClockBound reports [earliest, latest] where width = bound,
# so the full width matters: ORDERED when delay > bound_a (width of A's interval)
# because A.latest = A.earliest + bound_a, and B.earliest must be after A.latest.
# Cross-instance: delay must exceed bound_a + bound_b to guarantee ordering
# regardless of where true time falls within each interval.
min_ordering_us = bound_a + bound_b

ordered_at = None
for delay_us in delays_us:
    # Simulate: A at time 1000000.0, B at time 1000000.0 + delay
    base = 1000000.0
    a_earliest = base
    a_latest = base + bound_a / 1e6
    b_earliest = base + delay_us / 1e6
    b_latest = base + delay_us / 1e6 + bound_b / 1e6

    can_order = a_latest < b_earliest

    # Format delay label
    if delay_us == 0:
        label = '0µs (simultaneous)'
    elif delay_us >= 1000:
        label = f'{delay_us/1000:.0f}ms'
    else:
        label = f'{delay_us}µs'

    # ASCII timeline (scale to 60 cols)
    all_min = min(a_earliest, b_earliest)
    all_max = max(a_latest, b_latest)
    span = all_max - all_min if all_max > all_min else 1e-6
    COLS = 60

    def to_col(t):
        return int(((t - all_min) / span) * COLS)

    a_s, a_e = to_col(a_earliest), max(to_col(a_latest), to_col(a_earliest) + 1)
    b_s, b_e = to_col(b_earliest), max(to_col(b_latest), to_col(b_earliest) + 1)

    a_line = ' ' * a_s + '[' + '=' * (a_e - a_s) + ']'
    b_line = ' ' * b_s + '[' + '=' * (b_e - b_s) + ']'

    print(f'── Delay: {label} ' + '─' * (50 - len(label)))
    print()
    print(f'  A: {a_line}  ±{bound_a:.0f}µs')
    print(f'  B: {b_line}  ±{bound_b:.0f}µs')
    print()

    if can_order:
        gap_s = min(a_e, b_e)
        gap_e = max(a_s, b_s)
        print('  ' + ' ' * (a_e + 3) + '↑ GAP ↑')
        print()
        print(f'  Verdict: ORDERED — A happened before B')
        if ordered_at is None:
            ordered_at = delay_us
    else:
        ol_s = max(a_s, b_s)
        print('  ' + ' ' * (ol_s + 3) + '↑ OVERLAP ↑')
        print()
        print(f'  Verdict: AMBIGUOUS — cannot determine order')
    print()

    if ordered_at is not None and delay_us > 0:
        break

# Summary
print('═' * 63)
print('  Summary')
print('═' * 63)
if ordered_at is not None:
    if ordered_at >= 1000:
        print(f'  Minimum delay for deterministic ordering: {ordered_at/1000:.1f}ms')
    else:
        print(f'  Minimum delay for deterministic ordering: {ordered_at}µs')
else:
    print('  Events could not be ordered at tested delays.')
print()
print(f'  Instance A bound: ±{bound_a:.0f}µs ({sync_a})')
print(f'  Instance B bound: ±{bound_b:.0f}µs ({sync_b})')
print()
print(f'  Theoretical minimum separation for ordering: {min_ordering_us:.0f}µs')
print(f'    = bound_A ({bound_a:.0f}µs) + bound_B ({bound_b:.0f}µs)')
print()
if bound_a < 100:
    print(f'  → PTP enables ordering events just {2*bound_a:.0f}µs apart (same-type)')
if bound_b > 1000:
    print(f'  → NTP requires events {2*bound_b:.0f}µs apart (same-type)')
print('═' * 63)
"

