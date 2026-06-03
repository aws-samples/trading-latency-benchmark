#!/bin/bash
set -euo pipefail

# run_latency_breakdown.sh — Run latency breakdown analysis via SSM
#
# Usage:
#   ./scripts/run_latency_breakdown.sh --stack-name TgwMulticastBenchmark [--rate 1000] [--duration 10]

STACK_NAME=""
GROUP="239.1.1.1"
PORT=5001
RATE=1000
DURATION=10

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stack-name) STACK_NAME="$2"; shift 2 ;;
    --group)      GROUP="$2";      shift 2 ;;
    --port)       PORT="$2";       shift 2 ;;
    --rate)       RATE="$2";       shift 2 ;;
    --duration)   DURATION="$2";   shift 2 ;;
    *) echo "Usage: $0 --stack-name <name> [--rate 1000] [--duration 10]" >&2; exit 1 ;;
  esac
done

[[ -z "$STACK_NAME" ]] && { echo "ERROR: --stack-name required" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SENDER_TIMEOUT=$((DURATION + 30))

get_output() {
  aws cloudformation describe-stacks --stack-name "$STACK_NAME" \
    --query "Stacks[0].Outputs[?OutputKey=='${1}'].OutputValue" --output text
}

ssm_send() {
  local targets="$1" commands="$2" timeout="$3"
  local id_array
  id_array=$(echo "$targets" | tr ',' '\n' | sed 's/.*/"&"/' | paste -sd ',' -)
  aws ssm send-command \
    --instance-ids "[${id_array}]" \
    --document-name "AWS-RunShellScript" \
    --parameters "{\"commands\":[\"${commands}\"],\"executionTimeout\":[\"${timeout}\"]}" \
    --query "Command.CommandId" --output text
}

ssm_wait() {
  local cmd_id="$1" inst_id="$2" max_wait="$3" elapsed=0
  while [[ $elapsed -lt $max_wait ]]; do
    local status
    status=$(aws ssm get-command-invocation --command-id "$cmd_id" --instance-id "$inst_id" \
      --query "Status" --output text 2>/dev/null || echo "Pending")
    case "$status" in
      Success) return 0 ;;
      Failed|TimedOut|Cancelled) echo "ERROR: $status" >&2; return 1 ;;
    esac
    sleep 5; elapsed=$((elapsed + 5))
  done
  echo "ERROR: timeout" >&2; return 1
}

get_private_ip() {
  aws ec2 describe-instances --instance-ids "$1" \
    --query "Reservations[0].Instances[0].PrivateIpAddress" --output text
}

PUB_ID=$(get_output "PublisherInstanceId")
SUB_ID=$(get_output "SubscriberInstanceIds" | cut -d',' -f1)
PUB_IP=$(get_private_ip "$PUB_ID")
SUB_IP=$(get_private_ip "$SUB_ID")

echo "=== Latency Breakdown ==="
echo "Publisher: $PUB_ID ($PUB_IP)"
echo "Subscriber: $SUB_ID ($SUB_IP)"
echo "Rate: ${RATE} pps, Duration: ${DURATION}s"
echo ""

# Upload the breakdown script
BREAKDOWN_SCRIPT=$(cat "$SCRIPT_DIR/latency_breakdown.py")
UPLOAD_CMD=$(printf '%s' "$BREAKDOWN_SCRIPT" | python3 -c "import sys,json; print(json.dumps('cat > /tmp/latency_breakdown.py << '+'PYEOF'+chr(10)+sys.stdin.read()+chr(10)+'PYEOF'))")

for inst in "$PUB_ID" "$SUB_ID"; do
  aws ssm send-command --instance-ids "$inst" --document-name "AWS-RunShellScript" \
    --parameters "{\"commands\":[$UPLOAD_CMD],\"executionTimeout\":[\"10\"]}" \
    --query "Command.CommandId" --output text >/dev/null
done
sleep 3
echo "Scripts uploaded."

# Start receiver
echo "Starting receiver on $SUB_ID..."
RECV_CMD="python3 /tmp/latency_breakdown.py recv ${GROUP} ${PORT} ${SENDER_TIMEOUT} > /tmp/breakdown_recv.json 2>&1"
RECV_CMD_ID=$(ssm_send "$SUB_ID" "nohup bash -c '$RECV_CMD' &" "300")
sleep 3

# Start sender
echo "Starting sender on $PUB_ID (${RATE} pps, ${DURATION}s)..."
SEND_CMD="python3 /tmp/latency_breakdown.py send ${GROUP} ${PORT} ${RATE} ${DURATION} ${PUB_IP} > /tmp/breakdown_send.json 2>&1"
SEND_CMD_ID=$(ssm_send "$PUB_ID" "$SEND_CMD" "$SENDER_TIMEOUT")
ssm_wait "$SEND_CMD_ID" "$PUB_ID" "$SENDER_TIMEOUT"
echo "Sender complete."

# Wait for receiver to finish
sleep 5

# Collect results via S3 (SSM output has 24KB limit)
echo "Collecting results via S3..."
mkdir -p /tmp/breakdown

S3_BUCKET=$(get_output "S3BucketName")

# Upload results to S3 from instances
UPLOAD_SEND=$(ssm_send "$PUB_ID" "aws s3 cp /tmp/breakdown_send_full.json s3://${S3_BUCKET}/breakdown/sender.json" "30")
UPLOAD_RECV=$(ssm_send "$SUB_ID" "aws s3 cp /tmp/breakdown_recv_full.json s3://${S3_BUCKET}/breakdown/receiver.json" "30")
ssm_wait "$UPLOAD_SEND" "$PUB_ID" 30
ssm_wait "$UPLOAD_RECV" "$SUB_ID" 30

# Download locally
aws s3 cp "s3://${S3_BUCKET}/breakdown/sender.json" /tmp/breakdown/sender.json
aws s3 cp "s3://${S3_BUCKET}/breakdown/receiver.json" /tmp/breakdown/receiver.json

echo ""
echo "=== Analysis ==="
python3 "$SCRIPT_DIR/latency_breakdown.py" analyze \
  --sender /tmp/breakdown/sender.json \
  --receiver /tmp/breakdown/receiver.json
