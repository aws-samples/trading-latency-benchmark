#!/bin/bash
# setup_instance.sh — Per-host idempotent setup for the mcast2ucast bench.
#
# Runs on each EC2 instance (sender + receiver) via ssh.  Calls the
# existing AMI-baked scripts (setup_ena_bypass.sh, tune_os.sh,
# end_to_end_ptp.sh), generates deploy.conf, starts mcast2ucast, and
# configures the mcast0 TAP interface.
#
# Usage:
#   sudo ./setup_instance.sh \
#       --role sender|receiver \
#       --pci 28:00.0 \
#       --primary-nic ens5 \
#       --tap-ip 10.99.0.1 \
#       --peer-tap-ip 10.99.0.2 \
#       --peer-secondary-ip 10.200.0.55 \
#       --peer-secondary-mac 02:aa:bb:cc:dd:02
#
# Exit codes:
#   0  success
#   1  bad args / missing dependency / pre-flight failure
#   2  setup_ena_bypass.sh failed
#   3  tune_os.sh failed
#   4  end_to_end_ptp.sh failed
#   5  mcast2ucast did not open mcast0 within 30s

set -euo pipefail

ROLE=""
PCI=""
PRIMARY_NIC=""
TAP_IP=""
PEER_TAP_IP=""
PEER_SECONDARY_IP=""
PEER_SECONDARY_MAC=""
GATEWAY_MAC=""
SKIP_DEPLOY_CONF=0

while [ $# -gt 0 ]; do
    case "$1" in
        --role)               ROLE="$2"; shift 2 ;;
        --pci)                PCI="$2"; shift 2 ;;
        --primary-nic)        PRIMARY_NIC="$2"; shift 2 ;;
        --tap-ip)             TAP_IP="$2"; shift 2 ;;
        --peer-tap-ip)        PEER_TAP_IP="$2"; shift 2 ;;
        --peer-secondary-ip)  PEER_SECONDARY_IP="$2"; shift 2 ;;
        --peer-secondary-mac) PEER_SECONDARY_MAC="$2"; shift 2 ;;
        --gateway-mac)        GATEWAY_MAC="$2"; shift 2 ;;
        --skip-deploy-conf)   SKIP_DEPLOY_CONF=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

case "$ROLE" in sender|receiver) ;; *)
    echo "ERROR: --role must be sender or receiver, got '$ROLE'" >&2
    exit 1
    ;;
esac

if [ -z "$TAP_IP" ]; then
    echo "ERROR: --tap-ip is required" >&2
    exit 1
fi

# Auto-detect primary NIC and secondary PCI BDF when not explicitly given.
# Different EC2 generations name the primary differently (ens5 on m6i/m7i,
# enp39s0 on c7a, eth0 on metal). Rather than hardcode per-class, infer:
#   - primary NIC = the only ENA-driven interface that's currently UP
#     (the secondary ENI is bound to igb_uio and absent from `ip link`)
#   - secondary PCI = the ENA PCI BDF whose net symlink does NOT match
#     the primary NIC. We probe before setup_ena_bypass.sh rebinds it,
#     when both ENIs still appear under /sys/class/net.
if [ -z "$PRIMARY_NIC" ]; then
    # Don't pipe a generator into `head -1`: under `set -e`, the SIGPIPE
    # the generator gets when `head` closes the pipe propagates as 141.
    # Collect into an array and pick [0] instead.
    candidates=()
    for nic in /sys/class/net/*; do
        n=$(basename "$nic")
        [ "$n" = lo ] && continue
        [ -e "$nic/device/driver" ] || continue
        drv=$(basename "$(readlink -f "$nic/device/driver")" 2>/dev/null)
        [ "$drv" = ena ] || continue
        if ip -br a show "$n" 2>/dev/null | grep -q "UP "; then
            candidates+=("$n")
        fi
    done
    if [ "${#candidates[@]}" -eq 0 ]; then
        echo "ERROR: could not auto-detect primary NIC" >&2
        ip -br a >&2
        exit 1
    fi
    PRIMARY_NIC="${candidates[0]}"
    echo "[setup] auto-detected primary NIC: $PRIMARY_NIC"
fi

if [ -z "$PCI" ]; then
    primary_pci=$(basename "$(readlink -f /sys/class/net/$PRIMARY_NIC/device)" 2>/dev/null)
    # Pick first ENA-driven BDF that is NOT the primary's own.
    pci_candidates=()
    for dev in /sys/bus/pci/drivers/ena/0000:*; do
        [ -e "$dev" ] || continue
        bdf=$(basename "$dev")
        [ "$bdf" = "$primary_pci" ] && continue
        # Strip 0000: domain — orchestrate.sh format is bus:dev.func
        pci_candidates+=("${bdf#0000:}")
    done
    if [ "${#pci_candidates[@]}" -eq 0 ]; then
        echo "ERROR: could not auto-detect secondary ENA PCI BDF" >&2
        echo "primary_pci=$primary_pci" >&2
        ls -la /sys/bus/pci/drivers/ena/ >&2 || true
        exit 1
    fi
    PCI="${pci_candidates[0]}"
    echo "[setup] auto-detected secondary PCI: $PCI"
fi

if [ "$ROLE" = "sender" ]; then
    for var in PEER_SECONDARY_IP PEER_SECONDARY_MAC; do
        if [ -z "${!var}" ]; then
            # NOTE: $var is e.g. PEER_SECONDARY_IP — we want to render it
            # as --peer-secondary-ip. Compose lowercase + tr separately
            # because ${var,,//_/-} is NOT valid bash composition.
            flag="--$(echo "$var" | tr '[:upper:]_' '[:lower:]-')"
            echo "ERROR: sender requires $flag" >&2
            exit 1
        fi
    done
fi

MCAST_DIR="/home/ec2-user/mcast2ucast"
# PRIMARY_NIC is consumed via ${!var} indirect expansion in the required-flag
# loop above; PEER_TAP_IP is part of the public flag interface for future use
# (e.g. static ARP seeding) but no downstream command consumes it today.
# Reference both here so shellcheck does not flag them as unused.
: "${PRIMARY_NIC}" "${PEER_TAP_IP}"

# 1. Pre-flight: AMI must have everything baked
# DPDK is built with versioned soname (libdpdk.so.NN). Just check the
# canonical pkg-config file presence — that's what bootstrap.sh installs.
if [ ! -f /usr/local/lib64/pkgconfig/libdpdk.pc ]; then
    echo "ERROR: DPDK pkg-config not found at /usr/local/lib64/pkgconfig/libdpdk.pc — wrong AMI?" >&2
    exit 1
fi
if [ ! -x "$MCAST_DIR/build/mcast2ucast" ]; then
    echo "ERROR: $MCAST_DIR/build/mcast2ucast not found — wrong AMI?" >&2
    exit 1
fi

# 2. Clean restart of any prior daemon. DPDK rte_eal_cleanup() can take
# several seconds to release the PCI device + hugepages; if we let
# setup_ena_bypass.sh rmmod igb_uio while the daemon still holds the
# device, rmmod fails with EBUSY. Poll until the process is gone, then
# escalate to SIGKILL on timeout.
if pgrep -x mcast2ucast >/dev/null; then
    echo "[setup] killing existing mcast2ucast"
    pkill -TERM -x mcast2ucast || true
    for _ in $(seq 1 10); do
        pgrep -x mcast2ucast >/dev/null || break
        sleep 1
    done
    if pgrep -x mcast2ucast >/dev/null; then
        echo "[setup] mcast2ucast did not exit cleanly after 10s, sending SIGKILL"
        pkill -KILL -x mcast2ucast || true
        sleep 1
    fi
fi

# 3. Bind secondary ENI to igb_uio (also pins primary NIC IRQs)
echo "[setup] running setup_ena_bypass.sh $PCI"
"$MCAST_DIR/setup_ena_bypass.sh" "$PCI" || { echo "ERROR: setup_ena_bypass failed" >&2; exit 2; }

# 4. Runtime OS tuning (AMI was baked with --grub already)
echo "[setup] running tune_os.sh"
"$MCAST_DIR/tune_os.sh" || { echo "ERROR: tune_os failed" >&2; exit 3; }

# 5. chrony / PTP
echo "[setup] running end_to_end_ptp.sh"
"$MCAST_DIR/end_to_end_ptp.sh" || { echo "ERROR: end_to_end_ptp failed" >&2; exit 4; }

# 6. Generate deploy.conf (or skip if cmd_setup already SCPed one)
DEPLOY_CONF="$MCAST_DIR/deploy.conf"
if [ "$SKIP_DEPLOY_CONF" -eq 1 ]; then
    if [ ! -f "$DEPLOY_CONF" ]; then
        echo "ERROR: --skip-deploy-conf set but $DEPLOY_CONF does not exist" >&2
        exit 1
    fi
    echo "[setup] using pre-existing $DEPLOY_CONF (--skip-deploy-conf)"
else
    if [ "$ROLE" = "sender" ]; then
        cat > "$DEPLOY_CONF" <<EOF
# Auto-generated by setup_instance.sh — sender side.
subscriber 224.0.0.101 $PEER_SECONDARY_IP 5001 $PEER_SECONDARY_MAC
EOF
    else
        cat > "$DEPLOY_CONF" <<EOF
# Auto-generated by setup_instance.sh — receiver side (decapsulate-only).
EOF
    fi
    echo "[setup] wrote $DEPLOY_CONF"
fi

# 7. Start mcast2ucast in background
echo "[setup] starting mcast2ucast"
GATEWAY_MAC_FLAG=""
[ -n "$GATEWAY_MAC" ] && GATEWAY_MAC_FLAG="--gateway-mac $GATEWAY_MAC"
nohup env LD_LIBRARY_PATH=/usr/local/lib64 \
    "$MCAST_DIR/build/mcast2ucast" \
        -l 0-2 \
        --allow="0000:$PCI,llq_policy=1" \
        --log-level=user1,7 \
        -- \
        --rx-port 0 --tx-port 0 \
        --tap mcast0 \
        --config "$DEPLOY_CONF" \
        $GATEWAY_MAC_FLAG \
    > /tmp/mcast2ucast.log 2>&1 &

# 8. Wait up to 30s for mcast0 to appear
for _ in $(seq 1 30); do
    if ip link show mcast0 >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
if ! ip link show mcast0 >/dev/null 2>&1; then
    echo "ERROR: mcast0 did not appear within 30s" >&2
    echo "----- last 50 lines of /tmp/mcast2ucast.log -----" >&2
    tail -n 50 /tmp/mcast2ucast.log >&2 || true
    exit 5
fi

# 9. Configure TAP IP, route, sysctls
ip addr replace "${TAP_IP}/24" dev mcast0
if ! ip route show 224.0.0.0/4 dev mcast0 | grep -q 'mcast0'; then
    ip route add 224.0.0.0/4 dev mcast0
fi
sysctl -qw net.ipv4.conf.all.accept_local=1
sysctl -qw net.ipv4.conf.all.rp_filter=0
sysctl -qw net.ipv4.conf.mcast0.rp_filter=0

echo "[setup] OK role=$ROLE tap_ip=$TAP_IP"
