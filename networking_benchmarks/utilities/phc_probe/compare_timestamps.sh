#!/usr/bin/env bash
#
# compare_timestamps.sh — Hardware vs Software timestamp comparison
#
# This is a two-host test. Packets must arrive from a REMOTE host to get
# hardware timestamps — loopback packets bypass the NIC and get ts[2]=0.
#
# On the RECEIVER (EC2 instance with ENA PHC):
#   sudo bash scripts/compare_timestamps.sh server [interface] [port] [count]
#
# On the SENDER (any host):
#   bash scripts/compare_timestamps.sh client <receiver-ip> [port] [count]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'

usage() {
    echo -e "${BOLD}Hardware vs Software Timestamp Comparison${NC}"
    echo ""
    echo "  Receiver (EC2 with HW timestamps):"
    echo "    sudo $0 server <interface> [--port PORT] [--count N]"
    echo "    sudo $0 server <interface> [--port PORT] --live"
    echo "    sudo $0 live <interface> [--port PORT]          # shortcut for --live"
    echo ""
    echo "  Sender (any host):"
    echo "    $0 client <receiver-ip> [--port PORT] [--count N] [--interval MS]"
    echo ""
    echo "  NOTE: Packets must come from a REMOTE host for HW timestamps."
    echo "        Loopback packets bypass the NIC and will only get SW timestamps."
    exit 1
}

MODE="${1:-}"
shift || true

case "$MODE" in
    server|receiver)
        echo -e "${BOLD}Starting timestamp receiver...${NC}"
        echo -e "${DIM}(Waiting for packets from a remote sender)${NC}"
        echo ""
        sudo python3 -u "$SCRIPT_DIR/ts_receiver.py" "$@"
        ;;
    live)
        echo -e "${BOLD}Starting live HW-SW delta graph...${NC}"
        echo -e "${DIM}(Waiting for packets from a remote sender)${NC}"
        echo ""
        sudo python3 -u "$SCRIPT_DIR/ts_receiver.py" "$@" --live
        ;;
    client|sender)
        echo -e "${BOLD}Starting timestamp sender...${NC}"
        echo ""
        python3 -u "$SCRIPT_DIR/ts_sender.py" "$@"
        ;;
    *)
        usage
        ;;
esac
