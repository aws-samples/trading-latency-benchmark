#!/bin/bash
# tune_os.sh — Ultra-low latency OS tuning for mcast2ucast on AWS EC2.
# Inspired by aws-samples/trading-latency-benchmark.
#
# Targets: Amazon Linux 2023 on m8a.2xlarge (AMD EPYC, 8 vCPUs, no SMT)
# DPDK lcores: 0-2 (isolated), Housekeeping: 3-7
#
# Usage: sudo ./tune_os.sh [--grub] [--reboot]
#   --grub    Update GRUB config (requires reboot to take effect)
#   --reboot  Reboot after applying GRUB changes
#
# Without --grub, only applies runtime tuning (no reboot needed).

set -euo pipefail

DPDK_CORES="0-2"
HOUSEKEEPING_CORES="3-7"
HOUSEKEEPING_MASK="f8"   # CPUs 3-7 = 0b11111000 = 0xf8
# Auto-detect primary NIC (first non-loopback, non-TAP interface that is UP)
PRIMARY_NIC=$(ip -o link show up | awk -F': ' '{print $2}' | grep -v lo | grep -v 'mcast' | head -1)
PRIMARY_NIC="${PRIMARY_NIC:-eth0}"
DO_GRUB=0
DO_REBOOT=0

for arg in "$@"; do
    case "$arg" in
        --grub)   DO_GRUB=1 ;;
        --reboot) DO_REBOOT=1 ;;
    esac
done

echo "=== mcast2ucast OS Tuning ==="
echo "  DPDK cores:         $DPDK_CORES"
echo "  Housekeeping cores: $HOUSEKEEPING_CORES"
echo "  Primary NIC:        $PRIMARY_NIC"
echo ""

# ============================================================
# 1. GRUB / Kernel Boot Parameters (persistent, needs reboot)
# ============================================================
if [ "$DO_GRUB" -eq 1 ]; then
    echo "--- Kernel boot parameters ---"

    # AL2023 uses BLS (Boot Loader Specification) — use grubby
    # All args must be passed in a single --args string
    BOOT_PARAMS="isolcpus=$DPDK_CORES"
    BOOT_PARAMS+=" nohz_full=$DPDK_CORES"
    BOOT_PARAMS+=" rcu_nocbs=$DPDK_CORES"
    BOOT_PARAMS+=" nohz=on"
    BOOT_PARAMS+=" nosoftlockup"
    BOOT_PARAMS+=" nmi_watchdog=0"
    BOOT_PARAMS+=" audit=0"
    BOOT_PARAMS+=" mce=ignore_ce"
    BOOT_PARAMS+=" cpuidle.off=1"
    BOOT_PARAMS+=" processor.max_cstate=0"
    BOOT_PARAMS+=" idle=poll"
    BOOT_PARAMS+=" skew_tick=1"
    BOOT_PARAMS+=" clocksource=tsc"
    BOOT_PARAMS+=" tsc=reliable"
    BOOT_PARAMS+=" transparent_hugepage=never"
    BOOT_PARAMS+=" rcupdate.rcu_cpu_stall_suppress=1"
    BOOT_PARAMS+=" rcu_nocb_poll"
    BOOT_PARAMS+=" kthread_cpus=$HOUSEKEEPING_CORES"

    # Metal instances have real AMD-Vi IOMMU which causes IO_PAGE_FAULT with igb_uio
    if grep -q "metal" /sys/devices/virtual/dmi/id/chassis_asset_tag 2>/dev/null || \
       [ "$(nproc)" -ge 48 ]; then
        BOOT_PARAMS+=" iommu=off"
        echo "  [+] Metal instance detected: adding iommu=off for igb_uio compatibility"
    fi

    # Remove first (idempotent), then add all at once
    for key in isolcpus nohz_full rcu_nocbs nohz nosoftlockup nmi_watchdog \
               audit mce cpuidle.off processor.max_cstate idle skew_tick \
               clocksource tsc transparent_hugepage rcupdate.rcu_cpu_stall_suppress \
               rcu_nocb_poll kthread_cpus iommu; do
        grubby --update-kernel=ALL --remove-args="$key" 2>/dev/null || true
    done
    grubby --update-kernel=ALL --args="$BOOT_PARAMS"
    echo "  [+] Applied kernel boot parameters via grubby"

    # Show what's configured
    echo "  [+] Current kernel args:"
    grubby --info=ALL 2>/dev/null | grep ^args | head -1 | sed 's/^/      /'

    echo ""
    echo "  *** REBOOT REQUIRED for kernel params to take effect ***"
    echo ""
fi

# ============================================================
# 2. Sysctl — Network stack tuning
# ============================================================
echo "--- Network sysctl tuning ---"

cat > /etc/sysctl.d/99-mcast2ucast.conf << 'SYSCTL'
# mcast2ucast low-latency network tuning

# Socket buffer sizes (16MB max)
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 2097152
net.core.wmem_default = 2097152
net.core.optmem_max = 16777216

# Backlog and queue sizing
net.core.netdev_max_backlog = 1000000
net.core.somaxconn = 65536
net.core.netdev_budget = 600

# Busy polling — spin-poll NIC queue instead of interrupt-driven wakeup
net.core.busy_poll = 50
net.core.busy_read = 50

# Eliminate TX queueing delay
net.core.default_qdisc = noqueue

# UDP buffer tuning
net.ipv4.udp_rmem_min = 8192
net.ipv4.udp_wmem_min = 8192
net.ipv4.udp_mem = 65536 131072 262144

# TCP low-latency (for any TCP control paths)
net.ipv4.tcp_low_latency = 1
net.ipv4.tcp_timestamps = 0
net.ipv4.tcp_sack = 0
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 5

# Multicast
net.ipv4.igmp_max_memberships = 5000

# Forwarding (needed for mcast0 <-> NIC)
net.ipv4.ip_forward = 1
net.ipv4.conf.all.accept_local = 1
net.ipv4.conf.all.rp_filter = 0
net.ipv4.conf.default.rp_filter = 0
SYSCTL

sysctl --system > /dev/null 2>&1
echo "  [+] Applied network sysctl tuning"

# ============================================================
# 3. Sysctl — Kernel/VM tuning
# ============================================================
echo "--- Kernel/VM sysctl tuning ---"

cat > /etc/sysctl.d/98-mcast2ucast-kernel.conf << 'SYSCTL'
# Kernel scheduling
kernel.sched_min_granularity_ns = 10000000
kernel.sched_migration_cost_ns = 5000000
kernel.sched_wakeup_granularity_ns = 15000000
kernel.numa_balancing = 0
kernel.timer_migration = 0
kernel.watchdog = 0
kernel.nmi_watchdog = 0
kernel.softlockup_panic = 0

# Memory
vm.swappiness = 0
vm.zone_reclaim_mode = 0
vm.max_map_count = 262144
vm.min_free_kbytes = 1000000
vm.dirty_ratio = 80
vm.dirty_background_ratio = 5
vm.vfs_cache_pressure = 50
vm.compaction_proactiveness = 0
vm.compact_unevictable_allowed = 0
vm.stat_interval = 10
SYSCTL

sysctl --system > /dev/null 2>&1
echo "  [+] Applied kernel/VM sysctl tuning"

# ============================================================
# 4. Transparent Huge Pages — disable (causes latency spikes)
# ============================================================
echo "--- Disable Transparent Huge Pages ---"

if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled
    echo "  [+] THP disabled"
fi
if [ -f /sys/kernel/mm/transparent_hugepage/khugepaged/defrag ]; then
    echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
    echo "  [+] THP defrag disabled"
fi

# ============================================================
# 5. Hugepages for DPDK
# ============================================================
echo "--- Hugepages ---"

CURRENT_HP=$(cat /proc/sys/vm/nr_hugepages)
if [ "$CURRENT_HP" -lt 1024 ]; then
    echo 1024 > /proc/sys/vm/nr_hugepages
    echo "  [+] Hugepages set to 1024 (2GB)"
else
    echo "  [+] Hugepages already at $CURRENT_HP"
fi

# ============================================================
# 6. NIC tuning — interrupt coalescing, ring buffers, offloads
# ============================================================
echo "--- NIC tuning ($PRIMARY_NIC) ---"

if ip link show "$PRIMARY_NIC" &>/dev/null; then
    # Disable adaptive coalescing, set to minimum (0 = interrupt per packet)
    ethtool -C "$PRIMARY_NIC" adaptive-rx off rx-usecs 0 tx-usecs 0 2>/dev/null && \
        echo "  [+] Interrupt coalescing: adaptive off, rx-usecs=0, tx-usecs=0" || \
        echo "  [*] Could not set interrupt coalescing (ENA may not support all params)"

    # Maximize ring buffer sizes
    ethtool -G "$PRIMARY_NIC" rx 4096 tx 4096 2>/dev/null && \
        echo "  [+] Ring buffers: rx=4096, tx=4096" || \
        echo "  [*] Could not set ring buffers (using driver defaults)"

    # Disable offloads that add latency
    ethtool -K "$PRIMARY_NIC" gro off gso off tso off 2>/dev/null && \
        echo "  [+] Disabled GRO/GSO/TSO offloads" || \
        echo "  [*] Could not disable some offloads"
else
    echo "  [*] $PRIMARY_NIC not found, skipping NIC tuning"
fi

# ============================================================
# 7. IRQ affinity — move NIC IRQs to housekeeping cores
# ============================================================
echo "--- IRQ affinity ---"

if grep -q "$PRIMARY_NIC" /proc/interrupts; then
    for irq in $(grep "$PRIMARY_NIC" /proc/interrupts | awk '{print $1}' | tr -d ':'); do
        echo "$HOUSEKEEPING_MASK" > /proc/irq/$irq/smp_affinity 2>/dev/null || true
    done
    echo "  [+] $PRIMARY_NIC IRQs pinned to CPUs $HOUSEKEEPING_CORES"
fi

# ============================================================
# 8. Workqueue affinity — keep kernel work off DPDK cores
# ============================================================
echo "--- Workqueue affinity ---"

find /sys/devices/virtual/workqueue -name cpumask -exec sh -c \
    "echo $HOUSEKEEPING_MASK > {} 2>/dev/null" \; 2>/dev/null
echo "  [+] Kernel workqueues pinned to CPUs $HOUSEKEEPING_CORES"

# Move RCU threads to housekeeping cores (if not done via boot params)
for pid in $(pgrep -f 'rcu[_]' 2>/dev/null || true); do
    taskset -pc "$HOUSEKEEPING_CORES" "$pid" > /dev/null 2>&1 || true
done
echo "  [+] RCU threads moved to CPUs $HOUSEKEEPING_CORES"

# ============================================================
# 9. Disable services that cause jitter
# ============================================================
echo "--- Disable jitter sources ---"

# irqbalance fights our IRQ pinning
systemctl stop irqbalance 2>/dev/null && systemctl disable irqbalance 2>/dev/null && \
    echo "  [+] Disabled irqbalance" || echo "  [*] irqbalance not present"

# SSM agent causes periodic CPU spikes
systemctl stop amazon-ssm-agent 2>/dev/null && \
    echo "  [+] Stopped amazon-ssm-agent" || echo "  [*] amazon-ssm-agent not present"

# Firewall adds per-packet overhead
systemctl stop firewalld 2>/dev/null && systemctl disable firewalld 2>/dev/null && \
    echo "  [+] Disabled firewalld" || echo "  [*] firewalld not present"
iptables -P FORWARD ACCEPT 2>/dev/null
iptables -F 2>/dev/null && echo "  [+] Flushed iptables rules" || true

# ============================================================
# 10. Qdisc — remove TX queueing on primary NIC
# ============================================================
echo "--- TX queueing ---"

if ip link show "$PRIMARY_NIC" &>/dev/null; then
    tc qdisc replace dev "$PRIMARY_NIC" root noqueue 2>/dev/null && \
        echo "  [+] Set noqueue qdisc on $PRIMARY_NIC" || \
        echo "  [*] Could not set noqueue (may need pfifo_fast)"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "=== Tuning Complete ==="
echo ""
echo "Runtime optimizations applied immediately."
if [ "$DO_GRUB" -eq 1 ]; then
    echo "GRUB updated — reboot required for: isolcpus, nohz_full, rcu_nocbs,"
    echo "  cpuidle.off, idle=poll, processor.max_cstate=0"
    echo ""
    if [ "$DO_REBOOT" -eq 1 ]; then
        echo "Rebooting in 3 seconds..."
        sleep 3
        reboot
    else
        echo "Run 'sudo reboot' to activate kernel parameters."
    fi
else
    echo "Run with --grub --reboot to also apply kernel boot parameters"
    echo "(isolcpus, nohz_full, C-state disable — biggest latency wins)."
fi
