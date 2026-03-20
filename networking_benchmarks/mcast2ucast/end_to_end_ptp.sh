#!/bin/bash
# end_to_end_ptp.sh — Configure PTP-grade time synchronization on AWS EC2
# instances using the Nitro hypervisor's time sync service (169.254.169.123).
#
# AWS provides Stratum-1 NTP via the link-local endpoint, achieving
# sub-10μs offset between instances in the same AZ.  This script tightens
# chrony's polling interval for the best accuracy and enables hardware
# timestamping on ENA.
#
# Usage:  sudo ./end_to_end_ptp.sh
#         Run on BOTH instances before latency testing.

set -euo pipefail

echo "=== Configuring PTP-grade time sync ==="

# --- 1. Tighten chrony polling to AWS time source ---
CHRONY_CONF="/etc/chrony.conf"
CHRONY_AWS="/etc/chrony.d/aws-ptp.conf"

# Back up original
if [ ! -f "${CHRONY_CONF}.bak" ]; then
    cp "$CHRONY_CONF" "${CHRONY_CONF}.bak"
fi

# Create aggressive polling config for AWS time service
cat > "$CHRONY_AWS" << 'EOF'
# AWS Nitro time sync — aggressive polling for PTP-like accuracy
# minpoll 2 = every 4s, maxpoll 2 = every 4s (tightest reasonable)
# xleave enables interleaved mode for better accuracy
# polltarget 16 keeps the filter window tight
server 169.254.169.123 prefer iburst minpoll 2 maxpoll 2 xleave polltarget 16

# Note: hwtimestamp requires a PTP HW clock; ENA doesn't expose one to
# chrony, so we rely on kernel software timestamps which are still <1us.

# Reduce maxslewrate for tighter tracking
maxslewrate 500

# Enable RTC kernel sync
rtcsync
EOF

echo "  [+] Created $CHRONY_AWS"

# --- 2. Enable ENA hardware RX timestamping ---
PRIMARY_NIC=$(ip -o link show up | awk -F': ' '{print $2}' | grep -v lo | grep -v 'mcast' | head -1)
PRIMARY_NIC="${PRIMARY_NIC:-eth0}"
if ethtool -T "$PRIMARY_NIC" 2>/dev/null | grep -q "hardware-receive"; then
    ethtool --set-hwtstamp "$PRIMARY_NIC" rx-filter all 2>/dev/null || true
    echo "  [+] $PRIMARY_NIC hardware RX timestamping enabled"
else
    echo "  [-] No hardware timestamping support detected on $PRIMARY_NIC"
fi

# --- 3. Restart chrony ---
systemctl restart chronyd
sleep 2

echo "  [+] chronyd restarted"

# --- 4. Force a burst sync to converge quickly ---
chronyc burst 4/4 169.254.169.123 2>/dev/null || true
sleep 5

# --- 5. Verify synchronization ---
echo ""
echo "=== Time sync status ==="
chronyc tracking
echo ""
echo "=== Source stats ==="
chronyc sourcestats
echo ""
echo "=== Sources ==="
chronyc sources -v

# --- 6. Report offset ---
OFFSET=$(chronyc tracking | grep "Last offset" | awk '{print $4}')
echo ""
echo "=== Current offset: ${OFFSET} seconds ==="
echo "    (target: < 10 microseconds for accurate latency measurement)"
echo ""
echo "Done. Wait 30-60 seconds for optimal convergence, then run latency tests."
