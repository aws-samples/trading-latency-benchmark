#!/usr/bin/env bash
# Setup Open Onload with ENA Linux Driver on Amazon Linux 2023 (x86, Nitro V5+)
# Usage: sudo bash setup_onload_ena.sh <onload-interface> [--from <step>] [--patches <tar>]
#
# Steps:
#   1  Install dependencies
#   2  Clone ENA driver
#   3  Extract patches
#   4  Apply ENA patches
#   5  Build and load ENA driver
#   6  Configure queues and MTU
#   7  Clone and patch Onload
#   8  Build and install Onload
#   9  Load and configure Onload
#
# Requirements:
#   - x86 instance with n-tuple flow steering (e.g. i7ie.*, m8i.*, r8i.*)
#   - Amazon Linux 2023 with 6.1 kernel
#   - 2 ENIs: one for SSH, one for onload (in different subnets)
#   - ena-onload-patches.tar available locally or path provided

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(pwd)"
ONLOAD_COMMIT="2d4ec08aa"

# --- Argument parsing ---
INTERFACE=""
FROM_STEP=1
PATCHES_TAR="$WORK_DIR/ena-onload-patches.tar"

usage() {
    echo "Usage: $0 <onload-interface> [--from <step>] [--patches <path/to/ena-onload-patches.tar>]"
    echo ""
    echo "Steps:"
    echo "  1  Install dependencies"
    echo "  2  Clone ENA driver"
    echo "  3  Extract patches"
    echo "  4  Apply ENA patches"
    echo "  5  Build and load ENA driver"
    echo "  6  Configure queues and MTU"
    echo "  7  Clone and patch Onload"
    echo "  8  Build and install Onload"
    echo "  9  Load and configure Onload"
    echo ""
    echo "Example: $0 enp40s0 --from 7"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --from)   FROM_STEP="$2"; shift 2 ;;
        --patches) PATCHES_TAR="$2"; shift 2 ;;
        --help|-h) usage ;;
        -*) echo "Unknown option: $1"; usage ;;
        *)  INTERFACE="$1"; shift ;;
    esac
done

PATCHES_DIR="$WORK_DIR/ena-onload-patches"

if [[ -z "$INTERFACE" ]]; then usage; fi

if [[ "$EUID" -ne 0 ]]; then
    echo "This script must be run as root (sudo)."
    exit 1
fi

if ! [[ "$FROM_STEP" =~ ^[1-9]$ ]]; then
    echo "ERROR: --from must be a step number between 1 and 9."
    exit 1
fi

step() {
    local n="$1"
    local label="$2"
    if (( n < FROM_STEP )); then
        echo "==> [${n}/9] Skipping: $label"
        return 1  # signal caller to skip
    fi
    echo "==> [${n}/9] $label..."
    return 0
}

# --- Steps ---

if step 1 "Installing dependencies"; then
    dnf install -y git libcap-devel kernel-devel-$(uname -r) gcc iperf

    # kernel-debuginfo provides vmlinux (needed for BTF generation during module build).
    # Enable the debuginfo repo explicitly — it's disabled by default on AL2023.
    dnf install -y --enablerepo=amazonlinux-debuginfo \
        kernel-debuginfo-$(uname -r) || {
        echo "WARNING: kernel-debuginfo not available via dnf."
        echo "Falling back to /sys/kernel/btf/vmlinux symlink (sufficient for most builds)."
        # Modern kernels (6.1+) expose BTF via sysfs; create a symlink where the build expects vmlinux
        KDIR="/lib/modules/$(uname -r)/build"
        if [[ -f /sys/kernel/btf/vmlinux && ! -e "$KDIR/vmlinux" ]]; then
            ln -sf /sys/kernel/btf/vmlinux "$KDIR/vmlinux"
            echo "    Symlinked /sys/kernel/btf/vmlinux -> $KDIR/vmlinux"
        fi
    }
fi

if step 2 "Cloning ENA driver"; then
    cd "$WORK_DIR"
    if [[ ! -d amzn-drivers ]]; then
        git clone https://github.com/amzn/amzn-drivers.git
    fi
fi

if step 3 "Extracting ENA+Onload patches from $PATCHES_TAR"; then
    if [[ ! -f "$PATCHES_TAR" ]]; then
        echo "ERROR: Patches tar not found at $PATCHES_TAR"
        echo "Copy ena-onload-patches.tar to the instance and re-run."
        exit 1
    fi
    tar -xvf "$PATCHES_TAR" -C "$WORK_DIR"
fi

if step 4 "Applying patches to ENA driver"; then
    cd "$WORK_DIR/amzn-drivers"
    git am "$PATCHES_DIR/ena-patches/"*.patch
fi

if step 5 "Building and loading ENA driver"; then
    cd "$WORK_DIR/amzn-drivers/kernel/linux/ena"
    make clean
    make

    # Validate the built module before touching the live driver
    if ! modinfo ./ena.ko &>/dev/null; then
        echo "ERROR: ena.ko failed modinfo check — aborting before unloading live driver."
        exit 1
    fi

    # Locate the distro fallback module in case insmod fails and we need to recover
    FALLBACK_KO="$(find /lib/modules/$(uname -r) -name 'ena.ko*' 2>/dev/null | head -1)"

    rmmod ena || true

    if ! insmod ena.ko; then
        echo "ERROR: insmod ena.ko failed."
        if [[ -n "$FALLBACK_KO" ]]; then
            echo "Attempting to restore distro ENA module: $FALLBACK_KO"
            modprobe ena && echo "Fallback ENA module loaded — network should recover." \
                         || echo "WARNING: fallback also failed; check network connectivity."
        else
            echo "WARNING: No fallback ENA module found. Network may be unavailable."
        fi
        exit 1
    fi
fi

if step 6 "Configuring ENA queues and MTU"; then
    MAX_QUEUES=$(ethtool -l "$INTERFACE" | awk '/Pre-set maximums:/,/Current/' | grep Combined | head -1 | awk '{print $2}')
    HALF_QUEUES=$(( MAX_QUEUES / 2 ))
    echo "    Max queues: $MAX_QUEUES -> setting to $HALF_QUEUES"
    ethtool -L "$INTERFACE" combined "$HALF_QUEUES"
    ifconfig "$INTERFACE" mtu 3200
fi

if step 7 "Cloning and patching Onload"; then
    cd "$WORK_DIR"
    if [[ ! -d onload ]]; then
        git clone https://github.com/Xilinx-CNS/onload.git
    fi
    cd "$WORK_DIR/onload"
    git checkout "$ONLOAD_COMMIT"
    git am "$PATCHES_DIR/onload-patches/"*.patch
fi

if step 8 "Building and installing Onload"; then
    cd "$WORK_DIR/onload"
    scripts/onload_install --debug
fi

if step 9 "Loading and configuring Onload"; then
    onload_tool reload --onload-only
    echo "/usr/src/onload/src/tools/bpf_link_helper/bpf-link-helper" \
        > /sys/module/sfc_resource/parameters/bpf_link_helper
    echo "$INTERFACE" > /sys/module/sfc_resource/afxdp/register

    echo ""
    echo "==> Sanity check (look for XDP program loaded):"
    dmesg | grep -i "$INTERFACE.*XDP" || echo "    (no XDP message found yet - check dmesg manually)"
fi

echo ""
echo "Setup complete. To test with iperf:"
echo "  1. On a non-onload instance: iperf -s -i1"
echo "  2. On this instance (as root):"
echo "     EF_USE_HUGE_PAGES=0 EF_AF_XDP_ZEROCOPY=1 onload iperf -c <server-ip> -M 3000 -t 5000 -i 1"
echo ""
echo "Verify AF_XDP is in use:"
echo "  ethtool -n $INTERFACE"
echo "  watch \"ethtool -S $INTERFACE | egrep 'xsk'\""
