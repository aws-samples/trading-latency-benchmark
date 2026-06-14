#!/usr/bin/env bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

REGION=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --region) REGION="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--region REGION]"
            echo ""
            echo "Destroys the ClockBoundMeasureStack."
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

CDK_REGION_FLAG=""
if [[ -n "$REGION" ]]; then
    CDK_REGION_FLAG="-c region=$REGION"
fi

cd "$PROJECT_DIR"

if [[ ! -d .venv ]]; then
    python3 -m venv .venv
fi

source .venv/bin/activate
pip install -q -r requirements.txt

echo "Destroying ClockBoundMeasureStack..."
npx cdk destroy $CDK_REGION_FLAG --force
