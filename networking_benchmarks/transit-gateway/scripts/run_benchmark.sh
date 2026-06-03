#!/bin/bash
set -euo pipefail

###############################################################################
# run_benchmark.sh — Orchestrate a TGW multicast benchmark via SSM Run Command
#
# Usage:
#   scripts/run_benchmark.sh --stack-name <name> [OPTIONS]
#
# Options:
#   --stack-name  NAME      CloudFormation stack name (required)
#   --group       ADDR      Multicast group address   (default: 239.1.1.1)
#   --port        PORT      UDP port                   (default: 5001)
#   --rate        RATE      Packets/sec (sockperf mps) or kbps (iperf3 -b)
#                                                       (default: 1000)
#   --duration    SECS      Benchmark duration seconds  (default: 60)
#   --tool        TOOL[,..] Benchmark tool(s); repeatable or comma-separated
#                           Choices: mcast, sockperf, iperf3
#                                                       (default: mcast,sockperf)
#
# The script queries CloudFormation stack outputs, waits for instance
# readiness, runs the benchmark via SSM, and prints per-subscriber results
# to stdout (pipe to collect_results.py for aggregation).
#
# Tools always run in canonical order: mcast → sockperf → iperf3, regardless
# of the order they are passed. The PHC pre-flight always runs (sockperf and
# iperf3 do not need it, but PTP correctness is checked unconditionally).
###############################################################################

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
STACK_NAME=""
GROUP="239.1.1.1"
PORT=5001
RATE=1000
DURATION=60
TOOLS=()
INSTANCE_TYPE=""
PLACEMENT="single-az-cpg"
NOTES=""
SAVE_REPORT=false

VALID_TOOLS=(mcast sockperf iperf3)
# Canonical execution order — runs always proceed in this order regardless of CLI input.
TOOL_ORDER=(mcast sockperf iperf3)

SSM_POLL_INTERVAL=10
SSM_POLL_MAX_ATTEMPTS=60
READY_POLL_INTERVAL=10
READY_POLL_MAX_ATTEMPTS=60
RECEIVER_SETTLE_SECS=5

# ---------------------------------------------------------------------------
# Usage / help
# ---------------------------------------------------------------------------
usage() {
  cat <<EOF
Usage: $(basename "$0") --stack-name <name> [OPTIONS]

Orchestrate a TGW multicast benchmark via SSM Run Command.

Required:
  --stack-name NAME    CloudFormation stack name

Options:
  --group      ADDR    Multicast group address        [default: 239.1.1.1]
  --port       PORT    UDP port                        [default: 5001]
  --rate       RATE    Send rate (mps for sockperf,    [default: 1000]
                       pps for mcast, kbps for iperf3)
  --duration   SECS    Benchmark duration in seconds   [default: 60]
  --tool       TOOL[,..] Benchmark tool(s); repeatable or comma-separated.
                       Choices:
                         mcast    — TGW multicast one-way latency (PHC HW timestamps)
                         sockperf — unicast RTT baseline (no TGW)
                         iperf3   — unicast UDP throughput (no TGW, no latency)
                       [default: mcast,sockperf]
                       Tools always run in canonical order: mcast → sockperf → iperf3.
  --instance-type TYPE EC2 instance type (for report)  [default: auto-detect]
  --placement  STRAT   Placement strategy              [default: single-az-cpg]
  --notes      TEXT    Additional notes for report
  --save-report        Save markdown report to runs/   [default: off]

Examples:
  # default (mcast + sockperf)
  scripts/run_benchmark.sh --stack-name MyBenchStack

  # only TGW one-way latency
  scripts/run_benchmark.sh --stack-name MyBenchStack --tool mcast

  # all three tools, comma-separated
  scripts/run_benchmark.sh --stack-name MyBenchStack --tool mcast,sockperf,iperf3

  # repeatable form
  scripts/run_benchmark.sh --stack-name MyBenchStack --tool mcast --tool iperf3
EOF
  exit 1
}

# ---------------------------------------------------------------------------
# Parse CLI arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --stack-name)    STACK_NAME="$2";    shift 2 ;;
    --group)         GROUP="$2";         shift 2 ;;
    --port)          PORT="$2";          shift 2 ;;
    --rate)          RATE="$2";          shift 2 ;;
    --duration)      DURATION="$2";      shift 2 ;;
    --tool)
      # Accept both repeatable (--tool a --tool b) and comma-separated (--tool a,b).
      IFS=',' read -ra _parts <<< "$2"
      for _t in "${_parts[@]}"; do
        _t="$(echo "$_t" | tr -d '[:space:]')"
        [[ -z "$_t" ]] && continue
        TOOLS+=("$_t")
      done
      shift 2
      ;;
    --instance-type) INSTANCE_TYPE="$2"; shift 2 ;;
    --placement)     PLACEMENT="$2";     shift 2 ;;
    --notes)         NOTES="$2";         shift 2 ;;
    --save-report)   SAVE_REPORT=true;   shift ;;
    -h|--help)       usage ;;
    *)               echo "Unknown option: $1" >&2; usage ;;
  esac
done

if [[ -z "$STACK_NAME" ]]; then
  echo "ERROR: --stack-name is required" >&2
  usage
fi

# Default tools if user did not pass --tool at all.
if [[ ${#TOOLS[@]} -eq 0 ]]; then
  TOOLS=(mcast sockperf)
fi

# Validate every tool against the allow-list.
for _t in "${TOOLS[@]}"; do
  _ok=false
  for _v in "${VALID_TOOLS[@]}"; do
    [[ "$_t" == "$_v" ]] && { _ok=true; break; }
  done
  if [[ "$_ok" != "true" ]]; then
    echo "ERROR: unknown --tool '$_t'. Valid choices: ${VALID_TOOLS[*]}" >&2
    exit 1
  fi
done

# Build the final run-order list: deduplicated, in canonical order.
RUN_TOOLS=()
for _v in "${TOOL_ORDER[@]}"; do
  for _t in "${TOOLS[@]}"; do
    if [[ "$_t" == "$_v" ]]; then
      RUN_TOOLS+=("$_v")
      break
    fi
  done
done

# Membership helper: tools_has <name> → exit 0 if requested, 1 otherwise.
tools_has() {
  local needle="$1"
  for _t in "${RUN_TOOLS[@]}"; do
    [[ "$_t" == "$needle" ]] && return 0
  done
  return 1
}

# ---------------------------------------------------------------------------
# Output capture for --save-report
# ---------------------------------------------------------------------------
if [[ "$SAVE_REPORT" == "true" ]]; then
  RAW_OUTPUT_FILE=$(mktemp /tmp/benchmark_raw_XXXXXXXX)
  exec > >(tee "$RAW_OUTPUT_FILE") 2>&1
fi

echo "=== TGW Multicast Benchmark ==="
echo "Stack:    $STACK_NAME"
echo "Tools:    ${RUN_TOOLS[*]}"
echo "Group:    $GROUP"
echo "Port:     $PORT"
echo "Rate:     $RATE"
echo "Duration: ${DURATION}s"
echo ""

# ---------------------------------------------------------------------------
# Helper: get a single CloudFormation output value by key
# ---------------------------------------------------------------------------
get_stack_output() {
  local key="$1"
  aws cloudformation describe-stacks \
    --stack-name "$STACK_NAME" \
    --query "Stacks[0].Outputs[?OutputKey=='${key}'].OutputValue" \
    --output text
}

# ---------------------------------------------------------------------------
# Helper: run an SSM command on one or more instances, return command-id
# ---------------------------------------------------------------------------
ssm_send_command() {
  local targets="$1"  # comma-separated instance IDs
  local commands="$2"
  local timeout="$3"

  # Build instance-id list as JSON array
  local id_array
  id_array=$(echo "$targets" | tr ',' '\n' | sed 's/.*/"&"/' | paste -sd ',' -)

  aws ssm send-command \
    --instance-ids "[${id_array}]" \
    --document-name "AWS-RunShellScript" \
    --parameters "{\"commands\":[\"${commands}\"],\"executionTimeout\":[\"${timeout}\"]}" \
    --query "Command.CommandId" \
    --output text
}

# ---------------------------------------------------------------------------
# Helper: wait for an SSM command to finish on a specific instance
# ---------------------------------------------------------------------------
ssm_wait_command() {
  local command_id="$1"
  local instance_id="$2"
  local max_wait="$3"

  local elapsed=0
  while [[ $elapsed -lt $max_wait ]]; do
    local status
    status=$(aws ssm get-command-invocation \
      --command-id "$command_id" \
      --instance-id "$instance_id" \
      --query "Status" \
      --output text 2>/dev/null || echo "Pending")

    case "$status" in
      Success)  return 0 ;;
      Failed|TimedOut|Cancelled)
        echo "ERROR: SSM command $command_id on $instance_id finished with status: $status" >&2
        return 1
        ;;
    esac

    sleep 5
    elapsed=$((elapsed + 5))
  done

  echo "ERROR: Timed out waiting for SSM command $command_id on $instance_id" >&2
  return 1
}

# ---------------------------------------------------------------------------
# Helper: get the private IP of an instance (needed for multicast interface)
# ---------------------------------------------------------------------------
get_private_ip() {
  local instance_id="$1"
  aws ec2 describe-instances \
    --instance-ids "$instance_id" \
    --query "Reservations[0].Instances[0].PrivateIpAddress" \
    --output text
}

# ---------------------------------------------------------------------------
# Step 1: Query CloudFormation stack outputs
# ---------------------------------------------------------------------------
echo ">>> Querying CloudFormation stack outputs..."

PUBLISHER_ID=$(get_stack_output "PublisherInstanceId")
SUBSCRIBER_IDS_CSV=$(get_stack_output "SubscriberInstanceIds")
S3_BUCKET=$(get_stack_output "S3BucketName")

# Also fetch multicast config from stack outputs for informational logging.
# CLI args (--group, --port) take precedence over stack outputs.
echo "  Stack multicast group: $(get_stack_output "MulticastGroup")"
echo "  Stack multicast port:  $(get_stack_output "MulticastPort")"

IFS=',' read -ra SUBSCRIBER_IDS <<< "$SUBSCRIBER_IDS_CSV"
ALL_INSTANCE_IDS=("$PUBLISHER_ID" "${SUBSCRIBER_IDS[@]}")

echo "  Publisher:   $PUBLISHER_ID"
echo "  Subscribers: ${SUBSCRIBER_IDS[*]}"
echo "  S3 Bucket:   $S3_BUCKET"
echo ""

# ---------------------------------------------------------------------------
# Step 2: Wait for all instances to be SSM-ready
# ---------------------------------------------------------------------------
echo ">>> Waiting for all instances to be SSM-ready..."

for instance_id in "${ALL_INSTANCE_IDS[@]}"; do
  attempt=0
  while [[ $attempt -lt $SSM_POLL_MAX_ATTEMPTS ]]; do
    ping_status=$(aws ssm describe-instance-information \
      --filters "Key=InstanceIds,Values=${instance_id}" \
      --query "InstanceInformationList[0].PingStatus" \
      --output text 2>/dev/null || echo "None")

    if [[ "$ping_status" == "Online" ]]; then
      echo "  $instance_id — SSM online"
      break
    fi

    attempt=$((attempt + 1))
    if [[ $attempt -ge $SSM_POLL_MAX_ATTEMPTS ]]; then
      echo "ERROR: $instance_id did not become SSM-ready after $((SSM_POLL_MAX_ATTEMPTS * SSM_POLL_INTERVAL))s" >&2
      exit 1
    fi
    sleep "$SSM_POLL_INTERVAL"
  done
done

echo ""

# ---------------------------------------------------------------------------
# Step 3: Wait for /opt/benchmark-env-ready on all instances
# ---------------------------------------------------------------------------
echo ">>> Waiting for benchmark environment readiness on all instances..."

for instance_id in "${ALL_INSTANCE_IDS[@]}"; do
  attempt=0
  while [[ $attempt -lt $READY_POLL_MAX_ATTEMPTS ]]; do
    cmd_id=$(ssm_send_command "$instance_id" "test -f /opt/benchmark-env-ready && which sockperf >/dev/null 2>&1 && which iperf3 >/dev/null 2>&1 && echo READY || echo NOTREADY" "30")
    sleep 3  # give the command a moment to execute

    result=$(aws ssm get-command-invocation \
      --command-id "$cmd_id" \
      --instance-id "$instance_id" \
      --query "StandardOutputContent" \
      --output text 2>/dev/null || echo "")

    if [[ "$result" == *"READY"* ]]; then
      echo "  $instance_id — environment ready"
      break
    fi

    attempt=$((attempt + 1))
    if [[ $attempt -ge $READY_POLL_MAX_ATTEMPTS ]]; then
      echo "ERROR: $instance_id environment not ready after $((READY_POLL_MAX_ATTEMPTS * READY_POLL_INTERVAL))s" >&2
      exit 1
    fi
    sleep "$READY_POLL_INTERVAL"
  done
done

echo ""

# ---------------------------------------------------------------------------
# Step 4: PTP pre-flight — abort if any instance lacks PHC sync
#
# Strict checks (each FAILs the run):
#   * /sys/module/ena/parameters/phc_enable == 1
#   * a /sys/class/ptp/*/clock_name contains "ena-ptp"
#   * ethtool -T <iface> advertises "hardware-receive"
#   * chrony's active source ("^*"-prefixed) is a PHC reference
#     (was a warning before; now a hard failure to prevent silent
#      one-way-latency miscalibration when chrony falls back to NTP)
# ---------------------------------------------------------------------------
echo ">>> PTP pre-flight: verifying hardware clock on all instances..."

# shellcheck disable=SC2016
PTP_PREFLIGHT_CMD='set -e; \
PHC_ENABLE=$(cat /sys/module/ena/parameters/phc_enable 2>/dev/null || echo 0); \
echo "phc_enable=$PHC_ENABLE"; \
if [ "$PHC_ENABLE" != "1" ]; then echo FAIL:phc_not_enabled; exit 1; fi; \
PTP_DEV=""; \
for f in /sys/class/ptp/*/clock_name; do \
  if [ -f "$f" ] && grep -q ena-ptp "$f"; then \
    PTP_DEV=/dev/$(basename $(dirname $f)); \
    break; \
  fi; \
done; \
if [ -z "$PTP_DEV" ]; then echo FAIL:no_ena_ptp_device; exit 1; fi; \
echo "ptp_device=$PTP_DEV"; \
IFACE=$(ip -o -4 route show to default | awk "{print \$5}" | head -1); \
ETHTOOL=$(ethtool -T $IFACE 2>&1); \
if ! echo "$ETHTOOL" | grep -q hardware-receive; then \
  echo FAIL:no_hw_rx_capability; \
  echo "$ETHTOOL"; \
  exit 1; \
fi; \
ACTIVE_LINE=$(chronyc sources 2>/dev/null | grep "^\^\*\|^#\*" | head -1 || true); \
if [ -z "$ACTIVE_LINE" ]; then \
  echo FAIL:chrony_no_active_source; \
  chronyc sources 2>&1 | head -20; \
  exit 1; \
fi; \
ACTIVE_NAME=$(echo "$ACTIVE_LINE" | awk "{print \$2}"); \
echo "active_source=$ACTIVE_NAME"; \
if ! echo "$ACTIVE_LINE" | grep -q PHC; then \
  echo FAIL:chrony_not_synced_to_phc; \
  echo "active=$ACTIVE_LINE"; \
  echo "(falling back to NTP would invalidate the one-way TGW latency measurement)"; \
  exit 1; \
fi; \
echo "sync=PHC"; \
echo PASS'

PTP_PREFLIGHT_PARAMS=$(python3 -c "import json,sys; print(json.dumps({'commands':[sys.argv[1]],'executionTimeout':['15']}))" "$PTP_PREFLIGHT_CMD")

PTP_FAIL=0
for instance_id in "${ALL_INSTANCE_IDS[@]}"; do
  ptp_cmd_id=$(aws ssm send-command \
    --instance-ids "$instance_id" \
    --document-name "AWS-RunShellScript" \
    --parameters "$PTP_PREFLIGHT_PARAMS" \
    --query "Command.CommandId" --output text)

  # Poll up to ~30s for completion instead of a fixed sleep.
  ptp_status="Pending"
  for _ in $(seq 1 15); do
    sleep 2
    ptp_status=$(aws ssm get-command-invocation \
      --command-id "$ptp_cmd_id" \
      --instance-id "$instance_id" \
      --query "Status" \
      --output text 2>/dev/null || echo "Pending")
    case "$ptp_status" in
      Success|Failed|Cancelled|TimedOut) break ;;
    esac
  done

  ptp_output=$(aws ssm get-command-invocation \
    --command-id "$ptp_cmd_id" \
    --instance-id "$instance_id" \
    --query "StandardOutputContent" \
    --output text 2>/dev/null || echo "")

  if [[ "$ptp_status" != "Success" ]] || [[ "$ptp_output" != *"PASS"* ]]; then
    echo "  FAIL $instance_id — PTP/PHC not ready:" >&2
    echo "    $ptp_output" >&2
    PTP_FAIL=1
  else
    echo "  $instance_id — PHC verified ($(echo "$ptp_output" | grep ptp_device | head -1), $(echo "$ptp_output" | grep active_source | head -1))"
  fi
done

if [[ $PTP_FAIL -ne 0 ]]; then
  echo "" >&2
  echo "ERROR: PTP pre-flight failed. Hardware timestamps will not be available" >&2
  echo "       OR chrony is not disciplining CLOCK_REALTIME from the local PHC." >&2
  echo "       Either condition makes the TGW one-way latency measurement invalid." >&2
  echo "       Run: scripts/check_ptp.sh --stack-name $STACK_NAME for diagnostics." >&2
  exit 1
fi

echo "  All instances passed PTP pre-flight (PHC + chrony-disciplined)."
echo ""

# ---------------------------------------------------------------------------
# Step 5+: Run requested tools in canonical order
# ---------------------------------------------------------------------------
# mcast:    multicast over TGW with PHC HW RX timestamps (one-way latency).
# sockperf: unicast ping-pong RTT baseline (no TGW in data path).
# iperf3:   unicast UDP throughput (no latency stats).

SENDER_TIMEOUT=$((DURATION + 30))
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---------------------------------------------------------------------------
# mcast — TGW multicast one-way latency
# ---------------------------------------------------------------------------
if tools_has mcast; then
  # --- Upload helper scripts to instances ---
  echo ">>> Uploading multicast helper scripts..."
  RECV_SCRIPT=$(cat "$SCRIPT_DIR/mcast_recv.py")
  SEND_SCRIPT=$(cat "$SCRIPT_DIR/mcast_send.py")

  RECV_UPLOAD=$(printf '%s' "$RECV_SCRIPT" | python3 -c "import sys,json; print(json.dumps('cat > /tmp/mcast_recv.py << '+'PYEOF'+chr(10)+sys.stdin.read()+chr(10)+'PYEOF'+chr(10)+'chmod +x /tmp/mcast_recv.py'))")
  SEND_UPLOAD=$(printf '%s' "$SEND_SCRIPT" | python3 -c "import sys,json; print(json.dumps('cat > /tmp/mcast_send.py << '+'PYEOF'+chr(10)+sys.stdin.read()+chr(10)+'PYEOF'+chr(10)+'chmod +x /tmp/mcast_send.py'))")

  # Upload to all instances
  for inst_id in "${ALL_INSTANCE_IDS[@]}"; do
    aws ssm send-command \
      --instance-ids "$inst_id" \
      --document-name "AWS-RunShellScript" \
      --parameters "{\"commands\":[$RECV_UPLOAD],\"executionTimeout\":[\"10\"]}" \
      --query "Command.CommandId" --output text >/dev/null
    aws ssm send-command \
      --instance-ids "$inst_id" \
      --document-name "AWS-RunShellScript" \
      --parameters "{\"commands\":[$SEND_UPLOAD],\"executionTimeout\":[\"10\"]}" \
      --query "Command.CommandId" --output text >/dev/null
  done
  sleep 3
  echo "  Scripts uploaded."
  echo ""

  echo ">>> Phase A: Multicast one-way latency (${RATE} pps, ${DURATION}s)..."

  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    recv_cmd="nohup python3 /tmp/mcast_recv.py ${GROUP} ${PORT} ${SENDER_TIMEOUT} > /tmp/mcast_result.json 2>&1 &"
    ssm_send_command "$sub_id" "$recv_cmd" "300" >/dev/null
    echo "  $sub_id — multicast receiver started"
  done

  sleep "$RECEIVER_SETTLE_SECS"

  pub_ip=$(get_private_ip "$PUBLISHER_ID")
  send_cmd="python3 /tmp/mcast_send.py ${GROUP} ${PORT} ${RATE} ${DURATION} ${pub_ip}"
  echo "  Sending ${RATE} pps multicast for ${DURATION}s..."
  MCAST_CMD_ID=$(ssm_send_command "$PUBLISHER_ID" "$send_cmd" "$SENDER_TIMEOUT")
  ssm_wait_command "$MCAST_CMD_ID" "$PUBLISHER_ID" "$SENDER_TIMEOUT"
  echo "  Multicast send complete."
  echo ""

  # Collect multicast loss results
  sleep 3
  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    collect_cmd_id=$(ssm_send_command "$sub_id" "cat /tmp/mcast_result.json 2>/dev/null || echo {}" "30")
    ssm_wait_command "$collect_cmd_id" "$sub_id" 30
    mcast_result=$(aws ssm get-command-invocation \
      --command-id "$collect_cmd_id" \
      --instance-id "$sub_id" \
      --query "StandardOutputContent" \
      --output text)
    echo "  $sub_id multicast loss: $mcast_result"
  done
  echo ""
fi

# ---------------------------------------------------------------------------
# sockperf — unicast RTT baseline (no TGW)
# ---------------------------------------------------------------------------
if tools_has sockperf; then
  echo ">>> Phase B: Unicast latency measurement (sockperf under-load)..."

  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    sub_ip=$(get_private_ip "$sub_id")

    recv_cmd="pkill -f 'sockperf server' 2>/dev/null || true; pkill -f 'mcast_recv' 2>/dev/null || true; fuser -k ${PORT}/udp 2>/dev/null || true; sleep 2; nohup sockperf server --ip ${sub_ip} --port ${PORT} > /tmp/sockperf_server.txt 2>&1 &"
    ssm_send_command "$sub_id" "$recv_cmd" "300" >/dev/null
    echo "  $sub_id ($sub_ip) — sockperf server started"
  done

  sleep "$RECEIVER_SETTLE_SECS"

  # Verify sockperf servers are listening
  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    verify_cmd_id=$(ssm_send_command "$sub_id" "cat /tmp/sockperf_server.txt 2>/dev/null; ss -ulnp | grep ${PORT}" "10")
    sleep 3
    verify_out=$(aws ssm get-command-invocation \
      --command-id "$verify_cmd_id" \
      --instance-id "$sub_id" \
      --query "StandardOutputContent" \
      --output text 2>/dev/null || echo "")
    echo "  $sub_id server status: $verify_out"
  done

  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    sub_ip=$(get_private_ip "$sub_id")

    send_cmd="sockperf under-load --ip ${sub_ip} --port ${PORT} --mps ${RATE} --time ${DURATION} --full-rtt 2>&1 | tee /tmp/sockperf_result.txt"
    echo "  Running sockperf under-load → $sub_id ($sub_ip)..."
    SEND_CMD_ID=$(ssm_send_command "$PUBLISHER_ID" "$send_cmd" "$SENDER_TIMEOUT")
    ssm_wait_command "$SEND_CMD_ID" "$PUBLISHER_ID" "$SENDER_TIMEOUT"

    # Collect results from publisher (sender has the stats)
    collect_cmd_id=$(ssm_send_command "$PUBLISHER_ID" "cat /tmp/sockperf_result.txt" "60")
    ssm_wait_command "$collect_cmd_id" "$PUBLISHER_ID" 60
    result_output=$(aws ssm get-command-invocation \
      --command-id "$collect_cmd_id" \
      --instance-id "$PUBLISHER_ID" \
      --query "StandardOutputContent" \
      --output text)

    echo "--- SUBSCRIBER $sub_id ---"
    echo "$result_output"
    echo "--- END $sub_id ---"
    echo ""

    # Stop sockperf server
    ssm_send_command "$sub_id" "pkill -f 'sockperf server' || true" "10" >/dev/null
  done
fi

# ---------------------------------------------------------------------------
# iperf3 — unicast UDP throughput (no TGW, no latency)
# ---------------------------------------------------------------------------
if tools_has iperf3; then
  echo ">>> Phase C: Unicast UDP throughput (iperf3)..."

  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    sub_ip=$(get_private_ip "$sub_id")

    recv_cmd="pkill -f 'iperf3 -s' 2>/dev/null || true; fuser -k ${PORT}/udp 2>/dev/null || true; sleep 2; nohup iperf3 -s -p ${PORT} > /tmp/benchmark_result.txt 2>&1 &"
    ssm_send_command "$sub_id" "$recv_cmd" "300" >/dev/null
    echo "  $sub_id ($sub_ip) — iperf3 server started"
  done

  sleep "$RECEIVER_SETTLE_SECS"

  for sub_id in "${SUBSCRIBER_IDS[@]}"; do
    sub_ip=$(get_private_ip "$sub_id")

    send_cmd="iperf3 -c ${sub_ip} -u -p ${PORT} -b ${RATE}k -t ${DURATION} 2>&1 | tee /tmp/iperf3_result.txt"
    echo "  Running iperf3 → $sub_id ($sub_ip)..."
    SEND_CMD_ID=$(ssm_send_command "$PUBLISHER_ID" "$send_cmd" "$SENDER_TIMEOUT")
    ssm_wait_command "$SEND_CMD_ID" "$PUBLISHER_ID" "$SENDER_TIMEOUT"

    stop_cmd="pkill -f 'iperf3 -s' || true; sleep 1; cat /tmp/benchmark_result.txt"
    collect_cmd_id=$(ssm_send_command "$sub_id" "$stop_cmd" "60")
    ssm_wait_command "$collect_cmd_id" "$sub_id" 60
    result_output=$(aws ssm get-command-invocation \
      --command-id "$collect_cmd_id" \
      --instance-id "$sub_id" \
      --query "StandardOutputContent" \
      --output text)

    echo "--- SUBSCRIBER $sub_id ---"
    echo "$result_output"
    echo "--- END $sub_id ---"
    echo ""
  done
fi

echo "=== Benchmark complete ==="
echo "Results above can be piped to scripts/collect_results.py for aggregation"
echo "S3 bucket for reports: $S3_BUCKET"

# ---------------------------------------------------------------------------
# Save markdown report (if --save-report)
# ---------------------------------------------------------------------------
if [[ "$SAVE_REPORT" == "true" ]]; then
  # Give tee a moment to flush
  sleep 1

  REPORT_ARGS=(--input "$RAW_OUTPUT_FILE" --placement "$PLACEMENT")
  [[ -n "$INSTANCE_TYPE" ]] && REPORT_ARGS+=(--instance-type "$INSTANCE_TYPE")
  [[ -n "$NOTES" ]] && REPORT_ARGS+=(--notes "$NOTES")

  echo ""
  echo ">>> Generating markdown report..."
  python3 "$SCRIPT_DIR/save_report.py" "${REPORT_ARGS[@]}"
  rm -f "$RAW_OUTPUT_FILE"
fi
