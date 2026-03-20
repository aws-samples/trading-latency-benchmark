#!/bin/bash
# setup_ena_bypass.sh — Set up secondary ENI with DPDK ENA PMD for
# kernel-bypass TX.  Creates a reproducible environment where:
#   - Port 0 (TAP): RX multicast from local apps
#   - Port 1 (ENA): TX unicast directly to wire (full kernel bypass)
#
# Prerequisites:
#   - Secondary ENI already attached to the instance (see create_eni.sh)
#   - DPDK built with ENA PMD at /usr/local/lib64
#   - igb_uio built (see build_igb_uio.sh)
#
# Usage:  sudo ./setup_ena_bypass.sh [secondary-pci-addr]
#   e.g.: sudo ./setup_ena_bypass.sh 28:00.0
#
# Why igb_uio? AWS Nitro has no hardware IOMMU, so:
#   - vfio-pci (no-IOMMU): disables write-combine → no LLQ → ENA SQ creation fails
#   - uio_pci_generic: no MSI-X interrupt support → ENA admin timeouts
#   - igb_uio: supports MSI-X + write-combine BAR mapping → both work

set -euo pipefail

PCI_ADDR="${1:-28:00.0}"
DPDK_DEVBIND="/usr/local/bin/dpdk-devbind.py"
KMODS_DIR="${DPDK_KMODS_DIR:-$HOME/dpdk-kmods}"

echo "=== Setting up ENA kernel bypass on PCI $PCI_ADDR ==="

# --- 1. Load igb_uio module with write-combine (required for ENA LLQ) ---
if lsmod | grep -q igb_uio; then
    rmmod igb_uio
fi
if [ -f "$KMODS_DIR/linux/igb_uio/igb_uio.ko" ]; then
    insmod "$KMODS_DIR/linux/igb_uio/igb_uio.ko" wc_activate=1
else
    modprobe igb_uio wc_activate=1
fi
echo "  [+] igb_uio loaded (wc_activate=1)"

# Verify write-combine is active (required for ENA LLQ)
if dmesg | tail -20 | grep -q "wc_activate is set"; then
    echo "  [+] Verified: write-combine active (dmesg: 'igb_uio: wc_activate is set')"
else
    echo "  [!] WARNING: 'igb_uio: wc_activate is set' not found in dmesg"
    echo "      ENA LLQ may not work. Check: dmesg | grep igb_uio"
fi

# --- 2. Ensure hugepages are allocated ---
CURRENT_HP=$(cat /proc/sys/vm/nr_hugepages)
if [ "$CURRENT_HP" -lt 256 ]; then
    echo 256 > /proc/sys/vm/nr_hugepages
    echo "  [+] Hugepages set to 256 (512MB)"
else
    echo "  [+] Hugepages already at $CURRENT_HP"
fi

# --- 3. Find the kernel interface name for this PCI address ---
IFACE=""
for dir in /sys/class/net/*/device; do
    if [ -L "$dir" ]; then
        pci=$(readlink -f "$dir" | grep -oP '[0-9a-f]+:[0-9a-f]+\.[0-9a-f]+$')
        if [ "$pci" = "0000:$PCI_ADDR" ] || [ "$pci" = "$PCI_ADDR" ]; then
            IFACE=$(basename "$(dirname "$dir")")
            break
        fi
    fi
done

# --- 4. Bring down the interface and bind to igb_uio ---
if [ -n "$IFACE" ]; then
    echo "  [+] Found interface $IFACE on PCI $PCI_ADDR"
    ip link set "$IFACE" down 2>/dev/null || true
else
    echo "  [*] Interface for PCI $PCI_ADDR not found (may already be unbound)"
fi

$DPDK_DEVBIND --bind=igb_uio "0000:$PCI_ADDR"
echo "  [+] PCI $PCI_ADDR bound to igb_uio"

# --- 5. Pin primary NIC IRQs away from DPDK lcores ---
# DPDK poll-mode busy-spins at 100% on lcores 0-2. If primary NIC IRQs
# land on those CPUs, kernel softirq processing is starved, inflating
# kernel UDP latency from ~40us to 250+us.
# Auto-detect primary NIC (first ENA device that is UP and not the one we just bound)
PRIMARY_NIC=$(ip -o link show up | awk -F': ' '{print $2}' | grep -v lo | grep -v 'mcast' | head -1)
if [ -n "$PRIMARY_NIC" ] && grep -q "$PRIMARY_NIC" /proc/interrupts; then
    echo "  [+] Pinning $PRIMARY_NIC IRQs to CPUs 4-7 (away from DPDK lcores 0-2)"
    for irq in $(grep "$PRIMARY_NIC" /proc/interrupts | awk '{print $1}' | tr -d ':'); do
        echo f0 > /proc/irq/$irq/smp_affinity 2>/dev/null || true
    done
else
    echo "  [*] Primary NIC not found in /proc/interrupts, skipping IRQ pinning"
fi

# --- 6. Verify ---
echo ""
echo "=== DPDK device status ==="
$DPDK_DEVBIND --status-dev net
echo ""
echo "=== Hugepages ==="
grep -i huge /proc/meminfo | grep -v "0 kB"
echo ""
echo "Done. Start mcast2ucast with:"
echo "  sudo LD_LIBRARY_PATH=/usr/local/lib64 ./build/mcast2ucast \\"
echo "    -l 0-2 --allow=0000:$PCI_ADDR,llq_policy=1 \\"
echo "    --log-level=user1,7 -- --rx-port 0 --tx-port 0 --tap mcast0 --config deploy.conf"
