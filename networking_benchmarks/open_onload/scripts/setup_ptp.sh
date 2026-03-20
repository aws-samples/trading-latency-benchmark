#!/usr/bin/env bash
#
# setup_ptp_hwtstamp.sh — End-to-end PTP hardware timestamping setup & validation
#
# Usage:
#   sudo ./setup_ptp_hwtstamp.sh [interface]   # explicit interface
#   sudo ./setup_ptp_hwtstamp.sh               # auto-detect first UP NIC
#
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
fail() { echo -e "${RED}[FAIL]${NC}  $*"; }
info() { echo -e "        $*"; }

die() { fail "$*"; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LINUXPTP_DIR="${SCRIPT_DIR}/../third_party/linuxptp"
HWSTAMP_CTL_BIN="${LINUXPTP_DIR}/hwstamp_ctl"

# ── Root check ───────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] || die "Must run as root (sudo)"

# ── Detect or accept interface ───────────────────────────────────────
detect_nic() {
    for dev in /sys/class/net/*; do
        local name=$(basename "$dev")
        [[ "$name" == "lo" || "$name" == docker* || "$name" == veth* || "$name" == virbr* ]] && continue
        local state=$(cat "$dev/operstate" 2>/dev/null || echo "down")
        [[ "$state" != "up" ]] && continue
        local ip=$(ip -4 addr show "$name" 2>/dev/null | grep -oP 'inet \K[0-9.]+' | head -1)
        [[ -n "$ip" ]] && echo "$name" && return
    done
    die "No non-loopback UP interface with IPv4 found"
}

IFACE="${1:-$(detect_nic)}"
PCI_ADDR=$(basename "$(readlink /sys/class/net/$IFACE/device 2>/dev/null)" 2>/dev/null || true)
DRIVER=$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/{print $2}' || echo "unknown")
NIC_IP=$(ip -4 addr show "$IFACE" 2>/dev/null | grep -oP 'inet \K[0-9.]+' | head -1)

echo "============================================"
echo "  PTP Hardware Timestamping Setup"
echo "============================================"
echo ""
info "Interface:  $IFACE ($NIC_IP)"
info "Driver:     $DRIVER"
info "PCI:        ${PCI_ADDR:-n/a}"
echo ""

# ── Step 1: ENA driver module — ensure PHC is enabled ────────────────
echo "── Step 1: ENA driver PHC module parameter ──"

if [[ "$DRIVER" == "ena" ]]; then
    PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo "n/a")
    if [[ "$PHC_ENABLE" == "1" ]]; then
        ok "phc_enable=1 (already set)"
    elif [[ "$PHC_ENABLE" == "0" ]]; then
        warn "phc_enable=0 — reloading ENA with phc_enable=1"
        warn "This will cause a brief network disruption"
        read -p "        Proceed? [y/N] " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            modprobe -r ena
            modprobe ena phc_enable=1
            # Wait for interface to come back
            for i in $(seq 1 30); do
                sleep 1
                state=$(cat /sys/class/net/$IFACE/operstate 2>/dev/null || echo "down")
                [[ "$state" == "up" ]] && break
            done
            PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo "0")
            [[ "$PHC_ENABLE" == "1" ]] && ok "ENA reloaded with phc_enable=1" || die "Failed to reload ENA"
        else
            warn "Skipped — HW timestamps may not be available"
        fi
    else
        warn "Not an ENA module or phc_enable not exposed ($PHC_ENABLE)"
    fi
else
    ok "Non-ENA driver ($DRIVER) — PHC typically built-in, no modprobe needed"
fi
echo ""

# ── Step 2: Verify PTP clock device exists ───────────────────────────
echo "── Step 2: PTP Hardware Clock device ──"

PHC_INDEX=$(ethtool -T "$IFACE" 2>/dev/null | awk '/PTP Hardware Clock:/{print $NF}')
if [[ -n "$PHC_INDEX" && "$PHC_INDEX" != "none" && "$PHC_INDEX" -ge 0 ]] 2>/dev/null; then
    if [[ -c "/dev/ptp${PHC_INDEX}" ]]; then
        ok "PHC device: /dev/ptp${PHC_INDEX}"
    else
        warn "PHC index $PHC_INDEX reported but /dev/ptp${PHC_INDEX} not found"
    fi
    # Check for ENA symlink
    [[ -e /dev/ptp_ena ]] && info "(also available as /dev/ptp_ena)"
else
    warn "No PTP Hardware Clock reported by ethtool -T"
fi
echo ""

# ── Step 3: Enable HW timestamping on the NIC via hwstamp_ctl ───────
echo "── Step 3: Enable HW RX timestamping (hwstamp_ctl / SIOCSHWTSTAMP) ──"

# Resolve hwstamp_ctl: system install > local build > build from source
HWSTAMP_CTL=""
if command -v hwstamp_ctl &>/dev/null; then
    HWSTAMP_CTL="hwstamp_ctl"
    ok "Using system hwstamp_ctl ($(which hwstamp_ctl))"
elif [[ -x "$HWSTAMP_CTL_BIN" ]]; then
    HWSTAMP_CTL="$HWSTAMP_CTL_BIN"
    ok "Using local build ($HWSTAMP_CTL_BIN)"
else
    info "hwstamp_ctl not found — building from linuxptp source..."
    if [[ ! -d "$LINUXPTP_DIR" ]]; then
        git clone --depth 1 https://github.com/richardcochran/linuxptp.git "$LINUXPTP_DIR" 2>&1 | tail -1
    fi
    # Build only hwstamp_ctl (needs hwstamp_ctl.o + version.o)
    make -C "$LINUXPTP_DIR" hwstamp_ctl 2>&1 | tail -3
    if [[ -x "$HWSTAMP_CTL_BIN" ]]; then
        HWSTAMP_CTL="$HWSTAMP_CTL_BIN"
        ok "Built hwstamp_ctl from linuxptp source"
    else
        warn "Failed to build hwstamp_ctl"
    fi
fi

if [[ -n "$HWSTAMP_CTL" ]]; then
    # hwstamp_ctl -i <iface> -r 1  sets rx_filter=HWTSTAMP_FILTER_ALL
    OUTPUT=$("$HWSTAMP_CTL" -i "$IFACE" -r 1 2>&1) && {
        ok "hwstamp_ctl -i $IFACE -r 1 succeeded"
        echo "$OUTPUT" | while IFS= read -r line; do info "$line"; done
    } || {
        warn "hwstamp_ctl failed: $OUTPUT"
        info "The sequencer binary will attempt SIOCSHWTSTAMP ioctl at runtime"
    }
else
    warn "hwstamp_ctl unavailable — sequencer will attempt SIOCSHWTSTAMP ioctl at runtime"
fi
echo ""

# ── Step 4: Verify via sysfs ─────────────────────────────────────────
echo "── Step 4: Verify HW timestamping state (sysfs) ──"

if [[ -n "$PCI_ADDR" && -f "/sys/bus/pci/devices/$PCI_ADDR/hw_packet_timestamping_state" ]]; then
    STATE=$(cat "/sys/bus/pci/devices/$PCI_ADDR/hw_packet_timestamping_state")
    echo "$STATE" | while IFS= read -r line; do info "$line"; done

    RX_CFG=$(echo "$STATE" | awk '/RX configuration:/{print $NF}')
    if [[ "$RX_CFG" == "1" ]]; then
        ok "RX configuration: 1 (HW RX timestamping ACTIVE)"
    else
        warn "RX configuration: $RX_CFG (HW RX timestamping NOT active)"
        info "It will be activated when the sequencer calls SIOCSHWTSTAMP"
    fi
else
    warn "sysfs hw_packet_timestamping_state not available for $PCI_ADDR"
fi
echo ""

# ── Step 5: ethtool -T summary ───────────────────────────────────────
echo "── Step 5: ethtool -T $IFACE ──"

ETHTOOL_OUT=$(ethtool -T "$IFACE" 2>&1)
echo "$ETHTOOL_OUT" | while IFS= read -r line; do info "$line"; done
echo ""

HAS_HW_RX=$(echo "$ETHTOOL_OUT" | grep -c "hardware-receive" || true)
HAS_FILTER_ALL=$(echo "$ETHTOOL_OUT" | grep -cw "all" || true)

if [[ "$HAS_HW_RX" -gt 0 && "$HAS_FILTER_ALL" -gt 0 ]]; then
    ok "NIC supports hardware-receive with filter=all"
else
    if [[ "$HAS_HW_RX" -eq 0 ]]; then
        warn "NIC does not advertise hardware-receive capability"
        info "Only software timestamps (ts[0]) will be available"
    fi
fi
echo ""

# ── Summary ──────────────────────────────────────────────────────────
echo "============================================"
echo "  Summary"
echo "============================================"
info "Interface:    $IFACE ($NIC_IP)"
info "Driver:       $DRIVER"
info "PCI:          ${PCI_ADDR:-n/a}"
[[ -n "$PHC_INDEX" && "$PHC_INDEX" -ge 0 ]] 2>/dev/null && info "PHC:          /dev/ptp${PHC_INDEX}"
info ""
info "To run the sequencer:"
info "  sudo hw_sequencer --interface $IFACE --port <PORT>"
info "  sudo hw_sequencer --interface $IFACE --port <PORT> --debug   # show ts[0]/ts[2]"
echo "============================================"