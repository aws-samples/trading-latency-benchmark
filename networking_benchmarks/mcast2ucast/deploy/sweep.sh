#!/usr/bin/env bash
# sweep.sh — Drive a multi-cell latency sweep across subscriber counts against a baked AMI.
#
# Usage:
#   ./sweep.sh --ami-id <id> [options] [H1 H2 ...]
#
# Options:
#   --ami-id <id>          AMI to deploy (required)
#   --key <keypair>        EC2 key pair name (default: my-keypair)
#   --topology <t>         cpg|same-az|multi-az (default: cpg)
#   --sender-type <t>      sender instance type (default: c7a.2xlarge)
#   --receiver-type <t>    subscriber instance type (default: c7a.2xlarge)
#   --pps <n>              packets per second (default: 1000)
#   --count <n>            packet count per run (default: 1000)
#   --payload <n>          payload bytes (default: 64)
#   --region <r>           AWS region (default: us-east-1)
#   --log-dir <dir>        where to write per-cell logs (default: /tmp/m2u-sweep-<ts>)
#
# Positional args — list of subscriber host counts to sweep, e.g.:  1 2 4 8 10
# If none given, defaults to: 1 2 4 8 (a quick sanity sweep).
#
# Each cell runs: deploy → wait-for-ssh → setup → rebuild-benchmarks →
#                 verify → run → collect → teardown
# On any step failure the script tears down the partial stack and exits non-zero.
#
# Why rebuild-benchmarks?
#   The baked AMI can carry stale macOS (Mach-O) latency_sender/latency_receiver
#   binaries from the operator workstation. setup_instance.sh only checks the
#   mcast2ucast daemon binary, not the bench tools. This step forces a clean
#   Linux rebuild on every host before the run.
#
# Why wait-for-ssh?
#   CDK CREATE_COMPLETE fires when RunInstances is accepted, not when sshd is
#   ready. Without an explicit probe loop, the subsequent scp in setup fails
#   immediately on cold instances (especially in cluster placement groups where
#   all hosts boot simultaneously).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ORCHESTRATE="$HERE/orchestrate.sh"

# --- defaults ---
AMI=""
KEY="my-keypair"
TOPOLOGY="cpg"
SENDER_TYPE="c7a.2xlarge"
RECEIVER_TYPE="c7a.2xlarge"
PPS=1000
COUNT=1000
PAYLOAD=64
REGION="us-east-1"
LOG_DIR=""

# --- arg parsing ---
CELLS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ami-id)        AMI="$2";           shift 2 ;;
        --key)           KEY="$2";           shift 2 ;;
        --topology)      TOPOLOGY="$2";      shift 2 ;;
        --sender-type)   SENDER_TYPE="$2";   shift 2 ;;
        --receiver-type) RECEIVER_TYPE="$2"; shift 2 ;;
        --pps)           PPS="$2";           shift 2 ;;
        --count)         COUNT="$2";         shift 2 ;;
        --payload)       PAYLOAD="$2";       shift 2 ;;
        --region)        REGION="$2";        shift 2 ;;
        --log-dir)       LOG_DIR="$2";       shift 2 ;;
        [0-9]*)          CELLS+=("$1");      shift ;;
        -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; exit 2 ;;
    esac
done

[[ -z "$AMI" ]] && { echo "ERROR: --ami-id is required" >&2; exit 2; }

if [[ ${#CELLS[@]} -eq 0 ]]; then
    CELLS=("1" "2" "4" "8")
fi

[[ -z "$LOG_DIR" ]] && LOG_DIR="/tmp/m2u-sweep-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$LOG_DIR"

cd "$HERE"

echo "[sweep] AMI=$AMI topology=$TOPOLOGY sender=$SENDER_TYPE receiver=$RECEIVER_TYPE"
echo "[sweep] pps=$PPS count=$COUNT payload=$PAYLOAD region=$REGION"
echo "[sweep] cells: ${CELLS[*]}"
echo "[sweep] logs: $LOG_DIR"

# --- helpers ---

run_step() {
    local tag="$1" step="$2"; shift 2
    local cell_log="$LOG_DIR/$tag.log"
    echo "[sweep:$tag] $step" | tee -a "$cell_log"
    if ! "$ORCHESTRATE" "$step" "$@" >> "$cell_log" 2>&1; then
        echo "[sweep:$tag] FAILED at $step — last 60 lines:" >&2
        tail -60 "$cell_log" >&2
        echo "[sweep:$tag] attempting teardown ..." >&2
        "$ORCHESTRATE" teardown --yes >> "$cell_log" 2>&1 || \
            echo "[sweep:$tag] WARN teardown also failed" >&2
        exit 11
    fi
}

# Probe SSH on every host in topology.json (up to 30 min).
# CDK CREATE_COMPLETE does not mean sshd is accepting connections.
wait_for_ssh() {
    local tag="$1"
    local cell_log="$LOG_DIR/$tag.log"
    local key
    key=$(jq -r '.ssh_key_path' "$HERE/topology.json")
    local -a ips
    ips+=( "$(jq -r '.sender.primary_ip' "$HERE/topology.json")" )
    while IFS= read -r ip; do ips+=("$ip"); done < <(
        jq -r '.subscriber_hosts[].primary_ip' "$HERE/topology.json"
    )
    echo "[sweep:$tag] probing SSH (180×10s = up to 30min) on ${#ips[@]} hosts" \
        | tee -a "$cell_log"
    local SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
                    -o ConnectTimeout=15 -o BatchMode=yes
                    -o ServerAliveInterval=15 -o ServerAliveCountMax=8
                    -i "$key")
    local ip rc
    for ip in "${ips[@]}"; do
        rc=1
        for _ in $(seq 1 180); do
            if ssh "${SSH_OPTS[@]}" ec2-user@"$ip" 'echo ready' >/dev/null 2>&1; then
                rc=0; break
            fi
            sleep 10
        done
        if [[ "$rc" -ne 0 ]]; then
            echo "[sweep:$tag] SSH to $ip never came up" >&2
            "$ORCHESTRATE" teardown --yes >> "$cell_log" 2>&1 || true
            exit 14
        fi
        echo "[sweep:$tag] ssh-ready: $ip" | tee -a "$cell_log"
    done
}

# Rebuild latency_sender + latency_receiver from source on every host.
# The AMI may contain stale macOS Mach-O binaries copied from the operator
# workstation during bake; make clean ensures Linux binaries are built.
rebuild_benchmarks() {
    local tag="$1"
    local cell_log="$LOG_DIR/$tag.log"
    local key
    key=$(jq -r '.ssh_key_path' "$HERE/topology.json")
    local -a ips
    ips+=( "$(jq -r '.sender.primary_ip' "$HERE/topology.json")" )
    while IFS= read -r ip; do ips+=("$ip"); done < <(
        jq -r '.subscriber_hosts[].primary_ip' "$HERE/topology.json"
    )
    local SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
                    -o ConnectTimeout=15 -o ServerAliveInterval=15
                    -o ServerAliveCountMax=8 -i "$key")
    local ip
    for ip in "${ips[@]}"; do
        echo "[sweep:$tag] rebuilding benchmarks on $ip" | tee -a "$cell_log"
        if ! ssh "${SSH_OPTS[@]}" ec2-user@"$ip" \
                'cd /home/ec2-user/mcast2ucast/benchmarks && make clean && make' \
                >> "$cell_log" 2>&1; then
            echo "[sweep:$tag] FAILED rebuild on $ip" >&2
            tail -40 "$cell_log" >&2
            "$ORCHESTRATE" teardown --yes >> "$cell_log" 2>&1 || true
            exit 15
        fi
    done
}

# --- main loop ---

declare -a SUMMARY_FILES=()

for H in "${CELLS[@]}"; do
    tag="H${H}"
    echo
    echo "============================================================"
    echo "[sweep] CELL $tag  (num-subscriber-hosts=$H)"
    echo "============================================================"

    run_step "$tag" deploy \
        --topology "$TOPOLOGY" \
        --sender-type "$SENDER_TYPE" \
        --receiver-type "$RECEIVER_TYPE" \
        --ami-id "$AMI" --key "$KEY" \
        --region "$REGION" \
        --num-subscriber-hosts "$H"
    wait_for_ssh "$tag"
    run_step "$tag" setup
    rebuild_benchmarks "$tag"
    run_step "$tag" verify
    run_step "$tag" run --pps "$PPS" --count "$COUNT" --payload "$PAYLOAD"
    run_step "$tag" collect

    last_run=$(ls -1dt "$HERE/results"/*/ 2>/dev/null | head -1)
    if [[ -d "$last_run" && -f "${last_run}summary.json" ]]; then
        echo "[sweep:$tag] summary -> ${last_run}summary.json"
        SUMMARY_FILES+=("${last_run}summary.json")
    fi

    run_step "$tag" teardown --yes
done

echo
echo "============================================================"
echo "[sweep] DONE — ${#SUMMARY_FILES[@]}/${#CELLS[@]} cells succeeded"
echo "============================================================"
for s in "${SUMMARY_FILES[@]}"; do
    jq -r '"  H=\(.num_subscriber_hosts)"
          + "  p50=\(.first_receiver.p50_us // "—")µs"
          + "  p99=\(.first_receiver.p99_us // "—")µs"
          + "  [\(.started_utc // "")]"' "$s" 2>/dev/null
done
echo
echo "[sweep] logs in $LOG_DIR"
