#!/bin/bash
# build_igb_uio.sh — Build igb_uio kernel module from dpdk-kmods.
# igb_uio supports MSI-X interrupts and write-combine BAR mapping,
# both of which are required for ENA on AWS Nitro (no hardware IOMMU).
#
# Usage: sudo ./build_igb_uio.sh

set -euo pipefail

KMODS_DIR="${DPDK_KMODS_DIR:-$HOME/dpdk-kmods}"

echo "=== Building igb_uio kernel module ==="

# --- 1. Install kernel headers if needed ---
if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
    echo "  [+] Installing kernel-devel..."
    yum install -y kernel-devel-$(uname -r) 2>/dev/null || \
    amazon-linux-extras install -y kernel-devel 2>/dev/null || \
    dnf install -y kernel-devel-$(uname -r) 2>/dev/null || true
fi

# --- 2. Clone dpdk-kmods if not present ---
if [ ! -d "$KMODS_DIR" ]; then
    echo "  [+] Cloning dpdk-kmods..."
    cd "$(dirname "$KMODS_DIR")"
    git clone https://dpdk.org/git/dpdk-kmods
else
    echo "  [+] dpdk-kmods already at $KMODS_DIR"
fi

# --- 3. Build igb_uio ---
cd "$KMODS_DIR/linux/igb_uio"
echo "  [+] Building igb_uio.ko..."
make clean 2>/dev/null || true
make

if [ ! -f igb_uio.ko ]; then
    echo "  [!] Build failed — igb_uio.ko not found"
    exit 1
fi

# --- 4. Install the module ---
echo "  [+] Installing igb_uio.ko..."
cp igb_uio.ko /lib/modules/$(uname -r)/extra/ 2>/dev/null || \
    install -D igb_uio.ko /lib/modules/$(uname -r)/extra/igb_uio.ko
depmod -a

echo ""
echo "=== igb_uio built successfully ==="
echo "  Module: $KMODS_DIR/linux/igb_uio/igb_uio.ko"
echo "  Load with: modprobe igb_uio  (or insmod igb_uio.ko)"
