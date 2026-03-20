#!/bin/bash
# run_benchmarks.sh — Run latency benchmarks from the client (producer) host.
#
# Prerequisites:
#   - mcast2ucast running on both instances with ENA bypass
#   - mcast0 configured with IP, multicast route, sysctl on both instances
#   - latency_test built on both instances (cd benchmarks && make)
#   - latency_test server + mcast-server running on the remote instance
#   - NIC IRQs pinned away from DPDK lcores on both instances
#
# Usage: ./run_benchmarks.sh <remote-ip> <mcast0-local-ip> [iterations] [payload-size]
# Example:
#   ./run_benchmarks.sh 10.0.1.10 10.0.1.5 1000 64

set -euo pipefail

REMOTE_IP="${1:?Usage: $0 <remote-ip> <mcast0-local-ip> [iterations] [payload-size]}"
MCAST0_IP="${2:?Missing mcast0-local-ip}"
ITERATIONS="${3:-1000}"
PAYLOAD="${4:-64}"

# Pin benchmark processes to CPU 6 (away from DPDK lcores 0-2)
CPU=6
MCAST_GROUP="224.0.0.101"
MCAST_PORT="5001"
DIRECT_PORT="9999"
RETURN_PORT="7777"

DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Latency Benchmarks ==="
echo "  Remote:     $REMOTE_IP"
echo "  mcast0 IP:  $MCAST0_IP"
echo "  Iterations: $ITERATIONS"
echo "  Payload:    $PAYLOAD bytes"
echo "  Pinned to:  CPU $CPU"
echo ""

echo "--- Warmup: Direct UDP (100 iterations) ---"
taskset -c "$CPU" "$DIR/latency_test" direct "$REMOTE_IP" "$DIRECT_PORT" 100 "$PAYLOAD" > /dev/null 2>&1 || true
sleep 1

echo "--- Direct UDP (kernel sockets) ---"
taskset -c "$CPU" "$DIR/latency_test" direct "$REMOTE_IP" "$DIRECT_PORT" "$ITERATIONS" "$PAYLOAD"
echo ""

echo "--- Warmup: mcast2ucast e2e (100 iterations) ---"
taskset -c "$CPU" "$DIR/latency_test" mcast "$MCAST_GROUP" "$MCAST_PORT" "$MCAST0_IP" "$RETURN_PORT" 100 "$PAYLOAD" > /dev/null 2>&1 || true
sleep 1

echo "--- mcast2ucast e2e ---"
taskset -c "$CPU" "$DIR/latency_test" mcast "$MCAST_GROUP" "$MCAST_PORT" "$MCAST0_IP" "$RETURN_PORT" "$ITERATIONS" "$PAYLOAD"
echo ""

echo "=== Done ==="
