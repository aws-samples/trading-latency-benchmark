#!/bin/bash
# check_ptp.sh — Verify PTP clock sync status on benchmark instances via SSM.
#
# Default mode prints a one-line-per-instance summary table at the end.
# --verbose additionally prints the full per-instance diagnostic block.
#
# Usage:
#   scripts/check_ptp.sh --stack-name TgwMulticastBenchmark [--verbose]

set -euo pipefail

STACK_NAME=""
VERBOSE="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stack-name) STACK_NAME="$2"; shift 2 ;;
    --verbose|-v) VERBOSE="true"; shift ;;
    -h|--help)
      echo "Usage: $0 --stack-name <name> [--verbose]"
      exit 0
      ;;
    *) echo "Usage: $0 --stack-name <name> [--verbose]" >&2; exit 1 ;;
  esac
done

[[ -z "$STACK_NAME" ]] && { echo "ERROR: --stack-name is required" >&2; exit 1; }

get_output() {
  aws cloudformation describe-stacks --stack-name "$STACK_NAME" \
    --query "Stacks[0].Outputs[?OutputKey=='${1}'].OutputValue" --output text
}

PUB=$(get_output "PublisherInstanceId")
SUBS=$(get_output "SubscriberInstanceIds")
IFS=',' read -ra SUB_IDS <<< "$SUBS"
ALL=("$PUB" "${SUB_IDS[@]}")

# ---------------------------------------------------------------------------
# Remote diagnostic command
#
# Emits a human-readable block followed by KV:<key>=<value> lines that the
# orchestrator parses for the summary table. Keep the KV keys stable.
# ---------------------------------------------------------------------------
REMOTE_CMD='echo === PTP Hardware Clock ===; \
PHC_DEV=""; PHC_NAME="none"; PTP_FOUND=false; \
for f in /sys/class/ptp/*/clock_name; do \
  if [ -f "$f" ]; then \
    NAME=$(cat "$f"); \
    DEV=/dev/$(basename $(dirname $f)); \
    echo "  Device: $DEV  Clock: $NAME"; \
    if echo "$NAME" | grep -q ena-ptp; then PTP_FOUND=true; PHC_DEV="$DEV"; PHC_NAME="$NAME"; fi; \
  fi; \
done; \
$PTP_FOUND || echo "  No ENA PTP device found"; \
echo; \
echo === Symlink ===; \
if [ -e /dev/ptp_ena ]; then ls -la /dev/ptp_ena; SYMLINK=yes; else echo "  /dev/ptp_ena not found"; SYMLINK=no; fi; \
echo; \
echo === phc_enable parameter ===; \
PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo 0); \
echo "  phc_enable=$PHC_ENABLE"; \
echo; \
echo === HW packet timestamping state ===; \
HW_TS_STATE="?"; HW_TS_BDF="?"; \
for d in /sys/bus/pci/devices/*/hw_packet_timestamping_state; do \
  if [ -f "$d" ]; then \
    HW_TS_STATE=$(cat "$d" 2>/dev/null || echo "?"); \
    HW_TS_BDF=$(basename "$(dirname "$d")"); \
    echo "  $HW_TS_BDF: hw_packet_timestamping_state=$HW_TS_STATE"; \
    break; \
  fi; \
done; \
[ "$HW_TS_STATE" = "?" ] && echo "  (sysfs entry not present — kernel may predate the ENA HW-timestamp feature)"; \
echo; \
echo === ethtool -T ===; \
IFACE=$(ip -o -4 route show to default | awk "{print \$5}" | head -1); \
ETHTOOL_T=$(ethtool -T "$IFACE" 2>&1 || true); \
echo "  Interface: $IFACE"; \
echo "$ETHTOOL_T" | grep -E "hardware-(receive|raw-clock|transmit)" | sed "s/^/  /" || true; \
HW_RX="no"; echo "$ETHTOOL_T" | grep -q hardware-receive && HW_RX="yes"; \
HW_RAW="no"; echo "$ETHTOOL_T" | grep -q hardware-raw-clock && HW_RAW="yes"; \
echo; \
echo === Chrony Sources ===; \
chronyc sources 2>&1; \
echo; \
echo === Chrony Tracking ===; \
TRACKING=$(chronyc tracking 2>&1); \
echo "$TRACKING" | grep -E "(Reference|System time|Last offset|RMS offset|Leap)"; \
echo; \
echo === Verdict ===; \
SRC=$(chronyc sources 2>/dev/null | grep "^\^\*\|^#\*" || true); \
ACTIVE=$(echo "$SRC" | awk "{print \$2}"); \
[ -z "$ACTIVE" ] && ACTIVE="none"; \
if echo "$SRC" | grep -q PHC; then SYNC=PTP; \
elif echo "$SRC" | grep -q 169.254; then SYNC=NTP; \
else SYNC=UNKNOWN; fi; \
echo "  SYNC: $SYNC (active=$ACTIVE)"; \
LAST_OFFSET=$(echo "$TRACKING" | awk "/Last offset/ {print \$4}"); \
RMS_OFFSET=$(echo "$TRACKING" | awk "/RMS offset/ {print \$4}"); \
[ -z "$LAST_OFFSET" ] && LAST_OFFSET="?"; \
[ -z "$RMS_OFFSET" ] && RMS_OFFSET="?"; \
echo "  Last offset: $LAST_OFFSET s   RMS offset: $RMS_OFFSET s"; \
echo; \
echo "KV:phc_dev=$PHC_DEV"; \
echo "KV:phc_name=$PHC_NAME"; \
echo "KV:phc_enable=$PHC_ENABLE"; \
echo "KV:ptp_ena_symlink=$SYMLINK"; \
echo "KV:hw_ts_state=$HW_TS_STATE"; \
echo "KV:hw_rx=$HW_RX"; \
echo "KV:hw_raw_clock=$HW_RAW"; \
echo "KV:active_source=$ACTIVE"; \
echo "KV:sync=$SYNC"; \
echo "KV:last_offset=$LAST_OFFSET"; \
echo "KV:rms_offset=$RMS_OFFSET"'

# Use python3 to safely encode the command into the SSM JSON payload.
PARAMETERS_JSON=$(python3 -c "import json,sys; print(json.dumps({'commands':[sys.argv[1]],'executionTimeout':['15']}))" "$REMOTE_CMD")

# ---------------------------------------------------------------------------
# Per-instance arrays for the summary table
# ---------------------------------------------------------------------------
ROW_INST=()
ROW_ROLE=()
ROW_PHC_DEV=()
ROW_PHC_ENABLE=()
ROW_SYMLINK=()
ROW_HW_TS=()
ROW_HW_RX=()
ROW_ACTIVE=()
ROW_SYNC=()
ROW_LAST=()
ROW_RMS=()

for inst in "${ALL[@]}"; do
  ROLE="subscriber"
  [[ "$inst" == "$PUB" ]] && ROLE="publisher"

  if [[ "$VERBOSE" == "true" ]]; then
    echo "============================================"
    echo "Instance: $inst ($ROLE)"
    echo "============================================"
  else
    echo "Probing $inst ($ROLE)..."
  fi

  CMD_ID=$(aws ssm send-command \
    --instance-ids "$inst" \
    --document-name "AWS-RunShellScript" \
    --parameters "$PARAMETERS_JSON" \
    --query "Command.CommandId" --output text)

  # Poll up to ~30 s for the command to finish.
  OUTPUT=""
  for _ in $(seq 1 15); do
    sleep 2
    STATUS=$(aws ssm get-command-invocation \
      --command-id "$CMD_ID" --instance-id "$inst" \
      --query "Status" --output text 2>/dev/null || echo "Pending")
    case "$STATUS" in
      Success|Failed|Cancelled|TimedOut)
        OUTPUT=$(aws ssm get-command-invocation \
          --command-id "$CMD_ID" --instance-id "$inst" \
          --query "StandardOutputContent" --output text 2>/dev/null || echo "")
        break
        ;;
    esac
  done

  if [[ -z "$OUTPUT" ]]; then
    OUTPUT="(no output — command still pending or failed)"
  fi

  if [[ "$VERBOSE" == "true" ]]; then
    # Strip KV: lines from the verbose dump so they don't clutter the output.
    echo "$OUTPUT" | grep -v '^KV:' || true
    echo ""
  fi

  # Parse KV: lines into a temp associative-style lookup.
  get_kv() {
    local key="$1"
    echo "$OUTPUT" | awk -F= -v k="KV:$1" '$0 ~ "^"k"=" {sub("^"k"=",""); print; exit}'
  }

  ROW_INST+=("$inst")
  ROW_ROLE+=("$ROLE")
  ROW_PHC_DEV+=("$(get_kv phc_dev)")
  ROW_PHC_ENABLE+=("$(get_kv phc_enable)")
  ROW_SYMLINK+=("$(get_kv ptp_ena_symlink)")
  ROW_HW_TS+=("$(get_kv hw_ts_state)")
  ROW_HW_RX+=("$(get_kv hw_rx)")
  ROW_ACTIVE+=("$(get_kv active_source)")
  ROW_SYNC+=("$(get_kv sync)")
  ROW_LAST+=("$(get_kv last_offset)")
  ROW_RMS+=("$(get_kv rms_offset)")
done

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
echo ""
echo "============================================================ PTP SUMMARY ============================================================"
printf "%-21s %-11s %-10s %-7s %-9s %-9s %-7s %-16s %-8s %-13s %-13s\n" \
  "Instance" "Role" "PHC dev" "phc_en" "/dev/ena" "hw_ts" "hw_rx" "Active source" "Sync" "Last offset" "RMS offset"
printf "%-21s %-11s %-10s %-7s %-9s %-9s %-7s %-16s %-8s %-13s %-13s\n" \
  "--------------------" "----------" "---------" "------" "--------" "--------" "------" "---------------" "-------" "------------" "------------"

for i in "${!ROW_INST[@]}"; do
  printf "%-21s %-11s %-10s %-7s %-9s %-9s %-7s %-16s %-8s %-13s %-13s\n" \
    "${ROW_INST[$i]:-?}" \
    "${ROW_ROLE[$i]:-?}" \
    "${ROW_PHC_DEV[$i]:-?}" \
    "${ROW_PHC_ENABLE[$i]:-?}" \
    "${ROW_SYMLINK[$i]:-?}" \
    "${ROW_HW_TS[$i]:-?}" \
    "${ROW_HW_RX[$i]:-?}" \
    "${ROW_ACTIVE[$i]:-?}" \
    "${ROW_SYNC[$i]:-?}" \
    "${ROW_LAST[$i]:-?}" \
    "${ROW_RMS[$i]:-?}"
done

echo ""
echo "Legend:"
echo "  hw_ts   — /sys/bus/pci/devices/*/hw_packet_timestamping_state (ENA driver)"
echo "  hw_rx   — ethtool -T advertises 'hardware-receive' on the primary NIC"
echo "  Sync=PTP means chronyc shows a PHC reference as the active source."
echo ""

# Non-zero exit if any instance is not synced to PHC.
# We don't fail on hw_ts being '0' or '?', because run_benchmark.sh's mcast
# block enables HW packet timestamping at receiver start; this script is a
# standby diagnostic that runs between benchmark invocations.
EXIT_CODE=0
for s in "${ROW_SYNC[@]}"; do
  [[ "$s" != "PTP" ]] && EXIT_CODE=1
done
exit $EXIT_CODE
