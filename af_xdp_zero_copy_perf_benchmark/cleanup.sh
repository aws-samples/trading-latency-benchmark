#!/bin/bash
# improved_run_v4.sh - Enhanced version with better XDP filtering that preserves SSH connections

# Check if running as root
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit 1
fi

# Define interface
INTERFACE="enp39s0"
IFINDEX=$(ip link show dev $INTERFACE | grep -o "^[0-9]*" | head -n1)
echo "Interface $INTERFACE has ifindex $IFINDEX"

# Thoroughly clean up any existing XDP programs
echo "Thoroughly cleaning up..."

# Try multiple methods to ensure XDP program removal
echo "Removing XDP programs using ip command..."
ip link set dev $INTERFACE xdp off 2>/dev/null || true
ip link set dev $INTERFACE xdpgeneric off 2>/dev/null || true
ip link set dev $INTERFACE xdpdrv off 2>/dev/null || true

echo "Removing XDP programs using xdp-loader..."
xdp-loader unload $INTERFACE --all 2>/dev/null || true

# Remove any BPF maps that might be lingering
echo "Cleaning up BPF maps..."
rm -rf /sys/fs/bpf/* 2>/dev/null || true

# Reset the interface
echo "Resetting network interface..."
ip link set dev $INTERFACE down
sleep 1
ip link set dev $INTERFACE up
sleep 2

# Verify no XDP programs are attached
echo "Verifying XDP cleanup..."
ip link show dev $INTERFACE | grep -i xdp || echo "No XDP programs attached - clean!"
