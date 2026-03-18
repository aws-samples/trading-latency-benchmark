#!/bin/bash
# bootstrap.sh — Install DPDK, mcast2ucast, igb_uio, and all dependencies.
# Run on a fresh Amazon Linux 2023 instance.
#
# Usage: sudo ./bootstrap.sh
#
# What it does:
#   1. Installs build dependencies (meson, ninja, gcc, kernel-devel, etc.)
#   2. Downloads and builds DPDK 25.11 from source
#   3. Clones trading-latency-benchmark repo and builds mcast2ucast
#   4. Builds igb_uio kernel module (for ENA kernel bypass)
#   5. Allocates hugepages
#   6. Applies OS tuning (runtime only, use --grub for boot params)
#
# After running, mcast2ucast is ready at ~/mcast2ucast/build/mcast2ucast

set -euo pipefail

DPDK_VERSION="25.11"
DPDK_URL="https://fast.dpdk.org/rel/dpdk-${DPDK_VERSION}.tar.xz"
REPO_URL="https://github.com/aws-samples/trading-latency-benchmark.git"
HOME_DIR="/home/ec2-user"
DPDK_DIR="${HOME_DIR}/dpdk-${DPDK_VERSION}"
MCAST_DIR="${HOME_DIR}/mcast2ucast"
KMODS_DIR="${HOME_DIR}/dpdk-kmods"
NPROC=$(nproc)

# Run as root but build as ec2-user
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Run with sudo"
    exit 1
fi

echo "=== mcast2ucast Bootstrap ==="
echo "  DPDK version: ${DPDK_VERSION}"
echo "  CPUs: ${NPROC}"
echo ""

# --- 1. Install build dependencies ---
echo "=== [1/6] Installing build dependencies ==="
dnf install -y \
    gcc gcc-c++ make \
    meson ninja-build \
    numactl-devel elfutils-libelf-devel libpcap-devel libatomic \
    kernel-devel-$(uname -r) \
    python3 python3-pip \
    git tar xz \
    ethtool iproute iptables \
    2>&1 | tail -5

pip3 install pyelftools 2>&1 | tail -2
echo "  [+] Dependencies installed"

# --- 2. Build DPDK ---
echo ""
echo "=== [2/6] Building DPDK ${DPDK_VERSION} ==="
if [ -f "/usr/local/lib64/pkgconfig/libdpdk.pc" ]; then
    echo "  [+] DPDK already installed at /usr/local/lib64, skipping"
else
    cd "${HOME_DIR}"
    if [ ! -f "dpdk-${DPDK_VERSION}.tar.xz" ]; then
        echo "  [+] Downloading DPDK ${DPDK_VERSION}..."
        sudo -u ec2-user curl -sLO "${DPDK_URL}"
    fi
    if [ ! -d "${DPDK_DIR}" ]; then
        sudo -u ec2-user tar xf "dpdk-${DPDK_VERSION}.tar.xz"
    fi
    cd "${DPDK_DIR}"
    if [ ! -d "build" ]; then
        sudo -u ec2-user meson setup build
    fi
    sudo -u ec2-user ninja -C build -j${NPROC}
    ninja -C build install
    ldconfig
    echo "  [+] DPDK ${DPDK_VERSION} installed to /usr/local"
fi

# --- 3. Clone repo and build mcast2ucast ---
echo ""
echo "=== [3/6] Building mcast2ucast ==="
if [ ! -d "${HOME_DIR}/trading-latency-benchmark" ]; then
    echo "  [+] Cloning trading-latency-benchmark..."
    sudo -u ec2-user git clone "${REPO_URL}" "${HOME_DIR}/trading-latency-benchmark"
fi

# Symlink for convenience
if [ ! -e "${MCAST_DIR}" ]; then
    sudo -u ec2-user ln -sf "${HOME_DIR}/trading-latency-benchmark/mcast2ucast" "${MCAST_DIR}"
fi

cd "${MCAST_DIR}"
if [ ! -d "build" ]; then
    sudo -u ec2-user PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig meson setup build
fi
sudo -u ec2-user meson compile -C build
echo "  [+] mcast2ucast built at ${MCAST_DIR}/build/mcast2ucast"

# --- 4. Build benchmark tools ---
echo ""
echo "=== [4/6] Building benchmark tools ==="
cd "${MCAST_DIR}/benchmarks"
sudo -u ec2-user make
echo "  [+] Benchmarks built"

# --- 5. Build igb_uio ---
echo ""
echo "=== [5/6] Building igb_uio kernel module ==="
if [ ! -d "${KMODS_DIR}" ]; then
    echo "  [+] Cloning dpdk-kmods..."
    sudo -u ec2-user git clone https://dpdk.org/git/dpdk-kmods "${KMODS_DIR}"
fi
cd "${KMODS_DIR}/linux/igb_uio"
sudo -u ec2-user make clean 2>/dev/null || true
sudo -u ec2-user make
if [ -f igb_uio.ko ]; then
    cp igb_uio.ko "/lib/modules/$(uname -r)/extra/" 2>/dev/null || \
        install -D igb_uio.ko "/lib/modules/$(uname -r)/extra/igb_uio.ko"
    depmod -a
    echo "  [+] igb_uio.ko built and installed"
else
    echo "  [!] igb_uio build failed"
fi

# --- 6. Allocate hugepages ---
echo ""
echo "=== [6/6] Allocating hugepages ==="
CURRENT_HP=$(cat /proc/sys/vm/nr_hugepages)
if [ "$CURRENT_HP" -lt 1024 ]; then
    sysctl -w vm.nr_hugepages=1024
    echo "  [+] Hugepages set to 1024"
else
    echo "  [+] Hugepages already at ${CURRENT_HP}"
fi

# --- Summary ---
echo ""
echo "========================================="
echo "  Bootstrap complete!"
echo "========================================="
echo ""
echo "  DPDK:        /usr/local/lib64"
echo "  mcast2ucast: ${MCAST_DIR}/build/mcast2ucast"
echo "  igb_uio:     ${KMODS_DIR}/linux/igb_uio/igb_uio.ko"
echo "  Benchmarks:  ${MCAST_DIR}/benchmarks/latency_test"
echo "  Hugepages:   $(cat /proc/sys/vm/nr_hugepages)"
echo ""
echo "  Next steps:"
echo "    sudo ./setup_ena_bypass.sh          # Bind secondary ENI to DPDK"
echo "    sudo ./tune_os.sh --grub --reboot   # Apply OS tuning (requires reboot)"
echo ""
echo "  LD_LIBRARY_PATH=/usr/local/lib64 needed at runtime."
echo ""
