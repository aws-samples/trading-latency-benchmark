#!/bin/bash
# ptp.sh — Configure PTP-grade time synchronization on AWS EC2 instances.
#
# AWS Nitro provides a hardware PHC device (/dev/ptp0) via the ENA driver when
# phc_enable=1.  Reading the PHC directly via chrony's refclock directive
# bypasses NTP-UDP entirely and achieves ±50–500 ns accuracy between instances
# in the same AZ — far better than software-timestamp NTP (±1–5 µs).
#
# ENA driver does NOT support per-packet hardware timestamping (SIOCSHWTSTAMP).
# Do NOT use "hwtimestamp <iface>" in chrony — it will fatal-error on ENA.
#
# This script:
#   1. Ensures /etc/chrony.conf includes /etc/chrony.d/*.conf (AL2023 default omits it)
#   2. Tightens NTP polling for the AWS time source (fallback if PHC unavailable)
#   3. Detects driver: ENA vs non-ENA
#      ENA:  checks phc_enable; if 0, writes modprobe option (reboot required);
#            if 1, writes refclock PHC /dev/ptp0 config for chrony
#      Other: enables ethtool hardware RX timestamping if supported
#   4. Restarts chrony and verifies synchronisation
#
# Usage:  sudo ./ptp.sh
#         Run on BOTH instances before latency testing.
#         If phc_enable was 0, the script will exit with code 2 — reboot the
#         instance and re-run to complete chrony PHC configuration.

set -euo pipefail

echo "=== Configuring PTP-grade time sync ==="

CHRONY_CONF="/etc/chrony.conf"
CHRONY_AWS_NTP="/etc/chrony.d/aws-ptp.conf"
CHRONY_AWS_PHC="/etc/chrony.d/aws-phc.conf"

# --- 1. Ensure chrony.conf includes the drop-in directory ---
# Amazon Linux 2023 ships without an include directive; without it every
# file placed in chrony.d is silently ignored.
if ! grep -qE '^include /etc/chrony\.d/\*\.conf' "$CHRONY_CONF" 2>/dev/null; then
    echo "include /etc/chrony.d/*.conf" >> "$CHRONY_CONF"
    echo "  [+] Added include /etc/chrony.d/*.conf to $CHRONY_CONF"
fi

# --- 2. Tighten NTP polling for the AWS time source ---
# This serves as the NTP fallback path when refclock PHC is not yet active.
# minpoll 2 / maxpoll 2 = poll every 4 s (tightest reasonable without PHC).
# xleave enables interleaved mode for better software-timestamp accuracy.
cat > "$CHRONY_AWS_NTP" << 'EOF'
# AWS Nitro time sync — aggressive polling for sub-10us NTP accuracy.
# minpoll 2 = every 4s, maxpoll 2 = every 4s (tightest reasonable)
# xleave enables interleaved mode for better accuracy
# polltarget 16 keeps the filter window tight
server 169.254.169.123 prefer iburst minpoll 2 maxpoll 2 xleave polltarget 16

# Reduce maxslewrate for tighter tracking
maxslewrate 500

# Enable RTC kernel sync
rtcsync
EOF
echo "  [+] Created $CHRONY_AWS_NTP"

# Comment out any existing 169.254.169.123 server line so aws-ptp.conf owns it.
# If the default entry is left in place it takes precedence and the tighter
# minpoll/xleave settings are silently ignored.
if grep -qE '^server 169\.254\.169\.123' "$CHRONY_CONF" 2>/dev/null; then
    sed -i 's/^server 169\.254\.169\.123/# server 169.254.169.123/' "$CHRONY_CONF"
    echo "  [+] Commented out duplicate 169.254.169.123 in $CHRONY_CONF"
fi

# --- 3. Driver-specific PHC / hardware timestamping setup ---
PRIMARY_NIC=$(ip -o link show up | awk -F': ' '{print $2}' | grep -v lo | grep -v 'mcast' | head -1)
PRIMARY_NIC="${PRIMARY_NIC:-eth0}"
DRIVER=$(ethtool -i "$PRIMARY_NIC" 2>/dev/null | awk '/^driver:/{print $2}' || echo "unknown")

echo "  [*] Primary NIC: $PRIMARY_NIC  driver: $DRIVER"

if [[ "$DRIVER" == "ena" ]]; then
    # ── ENA: PHC device (/dev/ptp0) via phc_enable=1 ──────────────────────
    # ENA does NOT support SIOCSHWTSTAMP (per-packet HW timestamping).
    # "hwtimestamp <iface>" in chrony will fatal-error on ENA.
    # Instead, use "refclock PHC /dev/ptp0" which reads the Nitro hypervisor
    # clock directly — achieves ±50–500 ns vs ±1–5 µs with software NTP.
    #
    # phc_enable=1 is a module load-time parameter; it cannot be changed live
    # (unloading ENA kills the NIC on EC2).  Write the modprobe option now;
    # activation requires a reboot.

    PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo "0")

    if [[ "$PHC_ENABLE" == "1" ]]; then
        echo "  [+] ENA phc_enable=1 — PHC device active"

        # Verify /dev/ptp0 exists
        if [[ -c /dev/ptp0 ]]; then
            echo "  [+] /dev/ptp0 confirmed"
        else
            echo "  [!] phc_enable=1 but /dev/ptp0 not found — driver may need reload"
        fi

        # Write refclock PHC config for chrony
        cat > "$CHRONY_AWS_PHC" << 'EOF'
# ENA PHC (Nitro hypervisor clock) as chrony reference — ±50–500 ns accuracy.
# Bypasses NTP-UDP; reads the PHC device directly via ioctl(PHC_TIME_GETTIME).
# Requires phc_enable=1 in /etc/modprobe.d/ena-phc.conf.
# NOTE: do NOT use hwtimestamp <iface> on ENA — SIOCSHWTSTAMP is not supported;
# chrony will fatal-error if hwtimestamp is set for an ENA interface.
refclock PHC /dev/ptp0 poll 0 dpoll -2 trust prefer
EOF
        echo "  [+] Created $CHRONY_AWS_PHC (refclock PHC /dev/ptp0)"

    else
        echo "  [!] ENA phc_enable=$PHC_ENABLE — writing modprobe option"
        cat > /etc/modprobe.d/ena-phc.conf << 'EOF'
options ena phc_enable=1
EOF
        echo "  [+] Wrote /etc/modprobe.d/ena-phc.conf"
        echo "  [!] phc_enable activates on next boot — reboot required."
        echo "      NTP-only mode active for now (±1–5 µs accuracy)."
        echo "      After reboot, re-run this script to activate refclock PHC."
    fi

else
    # ── Non-ENA: hardware RX timestamping via ethtool ─────────────────────
    if ethtool -T "$PRIMARY_NIC" 2>/dev/null | grep -q "hardware-receive"; then
        ethtool --set-hwtstamp "$PRIMARY_NIC" rx-filter all 2>/dev/null || true
        echo "  [+] $PRIMARY_NIC hardware RX timestamping enabled"

        PHC_INDEX=$(ethtool -T "$PRIMARY_NIC" 2>/dev/null | awk '/PTP Hardware Clock:/{print $NF}')
        if [[ -n "$PHC_INDEX" && "$PHC_INDEX" != "none" ]] && [[ -c "/dev/ptp${PHC_INDEX}" ]]; then
            echo "  [+] PHC device: /dev/ptp${PHC_INDEX}"

            cat > "$CHRONY_AWS_PHC" << EOF
# Hardware PHC (PTP clock index ${PHC_INDEX}) as chrony reference.
refclock PHC /dev/ptp${PHC_INDEX} poll 0 dpoll -2 trust prefer
EOF
            echo "  [+] Created $CHRONY_AWS_PHC (refclock PHC /dev/ptp${PHC_INDEX})"
        fi
    else
        echo "  [-] No hardware timestamping support on $PRIMARY_NIC — NTP-only mode"
    fi
fi

# --- 4. Restart chrony ---
systemctl restart chronyd
sleep 2
echo "  [+] chronyd restarted"

# --- 5. Force a burst sync to converge quickly ---
chronyc burst 4/4 169.254.169.123 2>/dev/null || true
sleep 5

# --- 6. Verify synchronisation ---
echo ""
echo "=== Time sync status ==="
TRACKING_OUT=$(chronyc tracking)
echo "$TRACKING_OUT"
echo ""
echo "=== Source stats ==="
chronyc sourcestats
echo ""
echo "=== Sources ==="
chronyc sources -v

# --- 7. Report offset ---
OFFSET=$(echo "$TRACKING_OUT" | grep "Last offset" | awk '{print $4}')
echo ""
echo "=== Current offset: ${OFFSET} seconds ==="
if [[ "$DRIVER" == "ena" ]]; then
    PHC_ENABLE_NOW=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo "0")
    if [[ "$PHC_ENABLE_NOW" == "1" ]]; then
        echo "    ENA refclock PHC active — target: < 500 ns"
    else
        echo "    ENA NTP-only (phc_enable=0) — target: < 10 µs"
        echo "    Reboot this instance then re-run ptp.sh to activate refclock PHC."
        exit 2
    fi
else
    echo "    target: < 10 µs for accurate latency measurement"
fi
echo ""
echo "Done. Wait 30–60 seconds for optimal convergence, then run latency tests."
