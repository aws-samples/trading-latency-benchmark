#!/usr/bin/env bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults ─────────────────────────────────────────────────────────────────
INSTANCE_TYPE_1="m7i.4xlarge"
INSTANCE_TYPE_2="m5.4xlarge"
CLEANUP=false
REGION=""
VERBOSE=false

# ── Parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --instance-type-1) INSTANCE_TYPE_1="$2"; shift 2 ;;
        --instance-type-2) INSTANCE_TYPE_2="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --cleanup) CLEANUP=true; shift ;;
        --verbose) VERBOSE=true; shift ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --instance-type-1 TYPE  Instance type for first EC2 (default: c6in.4xlarge)"
            echo "  --instance-type-2 TYPE  Instance type for second EC2 (default: c6in.4xlarge)"
            echo "  --region REGION         AWS region to deploy to"
            echo "  --cleanup               Destroy stack after test"
            echo "  --verbose               Pass --verbose to run_clock_bound.sh"
            echo "  -h, --help              Show this help"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

CDK_REGION_FLAG=""
RUN_REGION_FLAG=""
if [[ -n "$REGION" ]]; then
    CDK_REGION_FLAG="-c region=$REGION"
    RUN_REGION_FLAG="--region $REGION"
fi

# ── Setup ────────────────────────────────────────────────────────────────────
cd "$PROJECT_DIR"

if [[ ! -d .venv ]]; then
    echo "Creating virtual environment..."
    python3 -m venv .venv
fi

source .venv/bin/activate
pip install -q -r requirements.txt

echo "═══════════════════════════════════════════════════════════════"
echo "  ClockBound Measure — End-to-End Test"
echo "═══════════════════════════════════════════════════════════════"
echo "  Instance 1: $INSTANCE_TYPE_1"
echo "  Instance 2: $INSTANCE_TYPE_2"
echo "  Region:     ${REGION:-$(aws configure get region 2>/dev/null || echo 'default')}"
echo "  Cleanup:    $CLEANUP"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# ── Deploy ───────────────────────────────────────────────────────────────────
echo "[1/3] Deploying stack..."
npx cdk deploy \
    $CDK_REGION_FLAG \
    --parameters InstanceType1="$INSTANCE_TYPE_1" \
    --parameters InstanceType2="$INSTANCE_TYPE_2" \
    --require-approval never \
    2>&1 | tail -15

echo ""
echo "[2/3] Running clock-bound queries..."
echo ""

VERBOSE_FLAG=""
if [[ "$VERBOSE" == "true" ]]; then
    VERBOSE_FLAG="--verbose"
fi

"$SCRIPT_DIR/run_clock_bound.sh" $RUN_REGION_FLAG $VERBOSE_FLAG

# ── Cleanup ──────────────────────────────────────────────────────────────────
echo ""
if [[ "$CLEANUP" == "true" ]]; then
    echo "[3/3] Destroying stack..."
    "$SCRIPT_DIR/cleanup.sh" $RUN_REGION_FLAG
else
    echo "[3/3] Skipping cleanup (--no-cleanup). Stack is still running."
    echo "  To destroy later: ./scripts/cleanup.sh ${RUN_REGION_FLAG}"
fi

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  E2E test complete."
echo "═══════════════════════════════════════════════════════════════"
