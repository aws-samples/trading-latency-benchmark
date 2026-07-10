#!/bin/bash
# verify_ptp_hwtstamp.sh — Read-only PTP / chrony / HW-timestamp gate.
#
# Runs on the receiver host before each benchmark run.  Prints a single
# OK line on success containing the offset; exits non-zero with a clear
# message on failure.  The orchestrator parses stdout for offset_us.
#
# Usage:  ./verify_ptp_hwtstamp.sh [--primary-nic <iface>] [--max-offset-us <n>]
#
# Exit codes:
#   0  pass
#   1  ethtool -T missing 'hardware-receive'
#   2  chronyd not active OR usage error (bad arg / non-numeric threshold)
#   3  chrony Leap status not Normal
#   4  chrony offset above threshold
#   5  could not parse chronyc output (format change / chrony down)

set -euo pipefail

PRIMARY_NIC=""
MAX_OFFSET_US=100

while [ $# -gt 0 ]; do
    case "$1" in
        --primary-nic) PRIMARY_NIC="$2"; shift 2 ;;
        --max-offset-us) MAX_OFFSET_US="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,16p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if ! [[ "$MAX_OFFSET_US" =~ ^[0-9]+$ ]]; then
    echo "ERROR: --max-offset-us must be a non-negative integer, got '$MAX_OFFSET_US'" >&2
    exit 2
fi

if [ -z "$PRIMARY_NIC" ]; then
    PRIMARY_NIC=$(ip -o link show up | awk -F': ' '{print $2}' | grep -v lo | grep -v 'mcast' | head -1)
    PRIMARY_NIC="${PRIMARY_NIC:-eth0}"
fi

# 1. NIC capability
if ! ethtool -T "$PRIMARY_NIC" 2>/dev/null | grep -q 'hardware-receive'; then
    echo "FAIL: ethtool -T $PRIMARY_NIC lacks 'hardware-receive'" >&2
    exit 1
fi

# 2. chronyd running
if ! systemctl is-active --quiet chronyd; then
    echo "FAIL: chronyd is not active" >&2
    exit 2
fi

# Snapshot chronyd state once — the Leap-status and offset checks below both
# read it, and a second `chronyc tracking` would fork chronyd's socket RPC again.
TRACKING=$(chronyc tracking)

# 3. Leap status
if ! echo "$TRACKING" | grep -q '^Leap status.*Normal'; then
    echo "FAIL: chronyc Leap status not Normal" >&2
    echo "$TRACKING" >&2
    exit 3
fi

# 4. Offset (absolute value, in microseconds)
OFFSET_S=$(echo "$TRACKING" | awk -F': *' '/^System time/ {print $2}' | awk '{print $1}')
if [ -z "$OFFSET_S" ]; then
    echo "FAIL: could not parse 'System time' from chronyc tracking" >&2
    exit 5
fi
OFFSET_US=$(awk -v s="$OFFSET_S" 'BEGIN{ v = s+0; if (v<0) v = -v; printf("%.0f", v*1e6) }')

# Integer compare; OFFSET_US is rounded already.
if [ "$OFFSET_US" -gt "$MAX_OFFSET_US" ]; then
    echo "FAIL: chrony offset ${OFFSET_US}us exceeds max ${MAX_OFFSET_US}us" >&2
    exit 4
fi

echo "OK offset_us=$OFFSET_US max=$MAX_OFFSET_US"
