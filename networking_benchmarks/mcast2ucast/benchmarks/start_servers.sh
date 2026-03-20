#!/bin/bash
# start_servers.sh — Start latency benchmark servers on the subscriber host.
#
# Starts both the direct UDP reflector and the multicast reflector.
# Pins both to CPU 6 (away from DPDK lcores 0-2).
#
# Usage: ./start_servers.sh <mcast0-ip>
# Example:
#   ./start_servers.sh 10.0.1.10

set -euo pipefail

MCAST0_IP="${1:?Usage: $0 <mcast0-ip>}"
CPU=6
MCAST_GROUP="224.0.0.101"
MCAST_PORT="5001"
DIRECT_PORT="9999"

DIR="$(cd "$(dirname "$0")" && pwd)"

# Kill any existing servers
pkill -f "latency_test server" 2>/dev/null || true
pkill -f "latency_test mcast-server" 2>/dev/null || true
sleep 1

echo "Starting benchmark servers (pinned to CPU $CPU)..."

taskset -c "$CPU" "$DIR/latency_test" server "$DIRECT_PORT" &
echo "  [+] Direct UDP server on port $DIRECT_PORT (pid $!)"

taskset -c "$CPU" "$DIR/latency_test" mcast-server "$MCAST_GROUP" "$MCAST_PORT" "$MCAST0_IP" &
echo "  [+] Multicast server on $MCAST_GROUP:$MCAST_PORT via $MCAST0_IP (pid $!)"

echo ""
echo "Servers running. Stop with: pkill -f latency_test"
