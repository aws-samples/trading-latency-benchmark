#!/usr/bin/env bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
set -euo pipefail
exec > /var/log/clock-bound-bootstrap.log 2>&1

export HOME=/root

echo "=== ClockBound Bootstrap Starting ==="
date -u

# Prevent dnf cache corruption from concurrent cloud-init/makecache operations
systemctl stop dnf-makecache.timer 2>/dev/null || true
systemctl stop dnf-makecache.service 2>/dev/null || true
systemctl disable dnf-makecache.timer 2>/dev/null || true
while pgrep -x dnf >/dev/null 2>&1 || pgrep -x rpm >/dev/null 2>&1; do
    echo "Waiting for existing dnf/rpm process to finish..."
    sleep 5
done

# ── Step 1: Detect NIC and driver ────────────────────────────────────────────
IFACE=""
for dev in /sys/class/net/*; do
    name=$(basename "$dev")
    [[ "$name" == "lo" || "$name" == docker* || "$name" == veth* ]] && continue
    state=$(cat "$dev/operstate" 2>/dev/null || echo "down")
    [[ "$state" != "up" ]] && continue
    IFACE="$name"
    break
done

if [[ -z "$IFACE" ]]; then
    echo "WARN: No UP interface found, defaulting to eth0"
    IFACE="eth0"
fi

DRIVER=$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/{print $2}' || echo "unknown")
echo "Interface: $IFACE, Driver: $DRIVER"

# ── Step 2: PTP Detection and Setup ─────────────────────────────────────────
SYNC_MECHANISM="NTP"
PHC_INDEX=""

if [[ "$DRIVER" == "ena" ]]; then
    PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo "0")
    echo "ENA phc_enable=$PHC_ENABLE"

    if [[ "$PHC_ENABLE" == "0" ]]; then
        echo "Configuring ENA with phc_enable=1..."
        # Persist the module parameter for current and future loads
        echo "options ena phc_enable=1" > /etc/modprobe.d/ena.conf

        # Reload ENA module with PHC enabled
        modprobe -r ena
        modprobe ena phc_enable=1

        # Wait for interface to come back
        for i in $(seq 1 30); do
            sleep 1
            # Interface name may change after reload
            for dev in /sys/class/net/*; do
                devname=$(basename "$dev")
                [[ "$devname" == "lo" || "$devname" == docker* || "$devname" == veth* ]] && continue
                devstate=$(cat "$dev/operstate" 2>/dev/null || echo "down")
                if [[ "$devstate" == "up" ]]; then
                    IFACE="$devname"
                    break 2
                fi
            done
        done
        echo "Interface after reload: $IFACE (state: $(cat /sys/class/net/$IFACE/operstate 2>/dev/null))"

        PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo "0")
        echo "ENA phc_enable after reload: $PHC_ENABLE"
    fi
fi

# Check for PTP hardware clock
PHC_INDEX=$(ethtool -T "$IFACE" 2>/dev/null | awk '/PTP Hardware Clock:/{print $NF}' || echo "")
if [[ -n "$PHC_INDEX" && "$PHC_INDEX" != "none" ]] && [[ "$PHC_INDEX" -ge 0 ]] 2>/dev/null; then
    if [[ -c "/dev/ptp${PHC_INDEX}" ]]; then
        SYNC_MECHANISM="PTP"
        echo "PTP Hardware Clock detected: /dev/ptp${PHC_INDEX}"

        # Configure chrony to use PHC as preferred reference clock
        echo "refclock PHC /dev/ptp${PHC_INDEX} poll 0 delay 0.000010 prefer" >> /etc/chrony.conf
        systemctl restart chronyd
        echo "Configured chrony with PHC /dev/ptp${PHC_INDEX} as preferred refclock"
    fi
else
    echo "No PTP Hardware Clock available, using NTP"
fi

# ── Step 3: Install ClockBound daemon ────────────────────────────────────────
# NOTE: When stable RPMs are published to GitHub releases, install via:
#   curl -fSL -o /tmp/clockbound.rpm "https://github.com/aws/clock-bound/releases/download/<VERSION>/clock-bound-<VERSION>-1.$(uname -m).rpm"
#   rpm -i /tmp/clockbound.rpm
mkdir -p /opt/clock-bound

echo "Building ClockBound from source..."
# Retry dnf install — NAT gateway or DNS may not be ready immediately after boot
DNF_OK=false
for attempt in $(seq 1 12); do
    if dnf install -y --setopt=cachedir=/tmp/dnf-cb gcc git rust cargo; then
        DNF_OK=true
        break
    fi
    echo "dnf install failed (attempt $attempt/12), retrying in 15s..."
    rm -rf /tmp/dnf-cb
    sleep 15
done
[[ "$DNF_OK" == "true" ]] || { echo "ERROR: dnf install failed after 12 attempts"; exit 1; }

cd /opt/clock-bound
git clone https://github.com/aws/clock-bound.git repo
cd repo
cargo build --release -p clock-bound --features daemon
cp target/release/clockbound /usr/local/bin/clockbound

cat > /etc/systemd/system/clockbound.service <<'UNIT'
[Unit]
Description=ClockBound Daemon
After=network.target

[Service]
ExecStart=/usr/local/bin/clockbound
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload

systemctl enable clockbound
systemctl start clockbound
echo "ClockBound daemon started"

# ── Step 4: Ensure Rust toolchain is available ───────────────────────────────
if ! command -v cargo &>/dev/null; then
    dnf install -y --setopt=cachedir=/tmp/dnf-cb gcc git rust cargo
fi

# ── Step 5: Build example client ────────────────────────────────────────────
cd /opt/clock-bound
if [[ ! -d repo ]]; then
    git clone https://github.com/aws/clock-bound.git repo
fi
cd repo/examples/client/rust
cargo build --release 2>&1 | tail -5

# Find the built binary — name varies by version
CLIENT_BIN=$(find /opt/clock-bound/repo/target/release -maxdepth 1 -type f -executable \
    ! -name "*.d" ! -name "*.so" ! -name "clockbound" \
    | head -1)
if [[ -n "$CLIENT_BIN" ]]; then
    cp "$CLIENT_BIN" /usr/local/bin/clock-bound-client
    echo "Client binary installed: $(basename $CLIENT_BIN)"
else
    echo "WARNING: No client binary found in target/release"
fi

# ── Step 6: Write status marker ─────────────────────────────────────────────
INSTANCE_TYPE=$(TOKEN=$(curl -s -X PUT "http://169.254.169.254/latest/api/token" \
    -H "X-aws-ec2-metadata-token-ttl-seconds: 21600") && \
    curl -s -H "X-aws-ec2-metadata-token: $TOKEN" \
    http://169.254.169.254/latest/meta-data/instance-type)

cat > /opt/clock-bound/status.json <<EOF
{
    "instance_type": "$INSTANCE_TYPE",
    "sync_mechanism": "$SYNC_MECHANISM",
    "phc_device": "/dev/ptp${PHC_INDEX:-none}",
    "driver": "$DRIVER",
    "interface": "$IFACE",
    "bootstrap_completed": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

echo "=== ClockBound Bootstrap Complete ==="
echo "Status: $(cat /opt/clock-bound/status.json)"
