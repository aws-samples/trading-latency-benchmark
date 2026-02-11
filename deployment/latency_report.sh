#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0

# Find and report on all histogram.hlog files under /home/ec2-user
HLOG_FILES=$(find /home/ec2-user -maxdepth 3 -name "histogram.hlog" -type f 2>/dev/null)

if [ -z "$HLOG_FILES" ]; then
    echo "No histogram.hlog files found under /home/ec2-user"
    exit 1
fi

for hlog in $HLOG_FILES; do
    echo "========================================================"
    echo "Report for: $hlog"
    echo "========================================================"
    java -jar ExchangeFlow-1.0-SNAPSHOT.jar latency-report "$hlog"
done
