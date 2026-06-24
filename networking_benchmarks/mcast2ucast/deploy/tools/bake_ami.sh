#!/bin/bash
# bake_ami.sh — One-shot: launch a stock AL2023 instance, run bootstrap.sh
# + tune_os.sh --grub, snapshot the root volume into an AMI.
#
# Usage:
#   ./bake_ami.sh \
#       --base-ami ami-XXXXXXXX \
#       --key <keypair> \
#       [--instance-type t3.medium] \
#       [--region us-east-1] \
#       [--subnet-id subnet-XXX] \
#       [--security-group-id sg-XXX] \
#       [--ssh-key-path ~/.ssh/<key>.pem]
#
# On success, prints the new AMI ID on the LAST line of stdout (operator
# pastes into cdk/cdk.json).
#
# Exit codes:
#   0   success
#   10  RunInstances failed / instance never reaches running
#   11  bootstrap.sh exited non-zero
#   12  CreateImage timed out

set -euo pipefail

BASE_AMI=""
KEY=""
INSTANCE_TYPE="c7a.2xlarge"
REGION=""
SUBNET_ID=""
SG_ID=""
SSH_KEY_PATH=""

REPO_ROOT="$(cd "$(dirname "$0")/../../" && pwd)"   # mcast2ucast/

while [ $# -gt 0 ]; do
    case "$1" in
        --base-ami)            BASE_AMI="$2"; shift 2 ;;
        --key)                 KEY="$2"; shift 2 ;;
        --instance-type)       INSTANCE_TYPE="$2"; shift 2 ;;
        --region)              REGION="$2"; shift 2 ;;
        --subnet-id)           SUBNET_ID="$2"; shift 2 ;;
        --security-group-id)   SG_ID="$2"; shift 2 ;;
        --ssh-key-path)        SSH_KEY_PATH="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,30p' "$0"; exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

[ -z "$BASE_AMI" ] && { echo "ERROR: --base-ami required" >&2; exit 1; }
[ -z "$KEY" ]      && { echo "ERROR: --key required" >&2; exit 1; }
[ -z "$SUBNET_ID" ] && { echo "ERROR: --subnet-id required (any default-VPC subnet)" >&2; exit 1; }
[ -z "$SG_ID" ]     && { echo "ERROR: --security-group-id required (must allow SSH from operator)" >&2; exit 1; }
[ -z "$SSH_KEY_PATH" ] && SSH_KEY_PATH="$HOME/.ssh/${KEY}.pem"

AWS_REGION_ARGS=()
[ -n "$REGION" ] && AWS_REGION_ARGS=(--region "$REGION")

TIMESTAMP=$(date -u +%Y%m%d-%H%M%S)
AMI_NAME="mcast2ucast-bench-${TIMESTAMP}"

echo "[bake] launching helper instance ($INSTANCE_TYPE, $BASE_AMI) ..."
INSTANCE_ID=$(aws ec2 run-instances "${AWS_REGION_ARGS[@]}" \
    --image-id "$BASE_AMI" \
    --instance-type "$INSTANCE_TYPE" \
    --key-name "$KEY" \
    --subnet-id "$SUBNET_ID" \
    --security-group-ids "$SG_ID" \
    --associate-public-ip-address \
    --block-device-mappings 'DeviceName=/dev/xvda,Ebs={VolumeSize=100,VolumeType=gp3,DeleteOnTermination=true}' \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=mcast2ucast-ami-builder-${TIMESTAMP}},{Key=Project,Value=mcast2ucast-bench}]" \
    --query 'Instances[0].InstanceId' --output text 2>&1) || { echo "ERROR: run-instances failed: $INSTANCE_ID" >&2; exit 10; }
echo "[bake] instance: $INSTANCE_ID"

cleanup() {
    # Capture the inherited exit code BEFORE we run any commands that
    # could mutate $?. The `|| true` below is intentional (we don't want
    # the terminate to mask the original failure) but it would otherwise
    # zero out $? and the implicit `exit` would report success.
    local rc=$?
    echo "[bake] terminating $INSTANCE_ID"
    aws ec2 terminate-instances "${AWS_REGION_ARGS[@]}" --instance-ids "$INSTANCE_ID" >/dev/null || true
    exit "$rc"
}
trap cleanup EXIT

echo "[bake] waiting for instance running + status checks ..."
aws ec2 wait instance-status-ok "${AWS_REGION_ARGS[@]}" --instance-ids "$INSTANCE_ID" || { echo "ERROR: status checks failed" >&2; exit 10; }

PUBLIC_IP=$(aws ec2 describe-instances "${AWS_REGION_ARGS[@]}" \
    --instance-ids "$INSTANCE_ID" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
echo "[bake] public ip: $PUBLIC_IP"

# AWS CLI prints the literal "None" when no public IP is assigned (e.g. the
# subnet has map_public_ip_on_launch=false and we forgot the auto-assign flag).
if [ -z "$PUBLIC_IP" ] || [ "$PUBLIC_IP" = "None" ]; then
    echo "ERROR: instance has no public IP — wrong subnet?" >&2
    exit 10
fi

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 \
          -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -i "$SSH_KEY_PATH")

# Wait for SSH (30 × 5s = 150s total)
for _ in $(seq 1 30); do
    if ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'echo ready' >/dev/null 2>&1; then
        break
    fi
    sleep 5
done
if ! ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'echo ready' >/dev/null 2>&1; then
    echo "ERROR: SSH to $PUBLIC_IP did not become available within 150s" >&2
    exit 10
fi

echo "[bake] copying source tree (tar over ssh)..."
# We mirror the local repo's mcast2ucast/ directory to the instance under
# ~/trading-latency-benchmark/mcast2ucast/. bootstrap.sh checks whether
# ~/trading-latency-benchmark exists and SKIPS the github clone when it
# does — so the copied files are what gets built. Don't add a .git/ probe
# to bootstrap.sh; the parent-dir-exists check is the contract.
#
# Use tar | ssh tar (instead of scp -r or rsync) so we can exclude heavy
# artifacts: the raw mcast2ucast/ tree is ~830 MB with deploy.bak/ and
# the CDK venv, but the bake-relevant slice is <5 MB.
#
# Why not rsync: macOS Sequoia ships openrsync, which negotiates with the
# AL2023 receiver's rsync 3.4.0 in a way that triggers a "buffer overflow:
# recv_rules" abort with multiple --exclude rules. tar avoids the whole
# rsync/openrsync compatibility surface.
ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'mkdir -p ~/trading-latency-benchmark/mcast2ucast'
tar -C "$REPO_ROOT" \
    --exclude='deploy.bak' \
    --exclude='deploy/cdk/.venv' \
    --exclude='deploy/cdk/cdk.out' \
    --exclude='deploy/cdk/.cdk.staging' \
    --exclude='deploy/cdk/.pytest_cache' \
    --exclude='deploy/results' \
    --exclude='deploy/topology.json' \
    --exclude='deploy/.last-*' \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude='build' \
    -czf - . | \
ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" \
    'tar -C ~/trading-latency-benchmark/mcast2ucast -xzf - && chmod +x ~/trading-latency-benchmark/mcast2ucast/*.sh ~/trading-latency-benchmark/mcast2ucast/deploy/scripts/*.sh ~/trading-latency-benchmark/mcast2ucast/deploy/tools/*.sh 2>/dev/null' \
    || { echo "ERROR: tar over ssh failed" >&2; exit 11; }

echo "[bake] copying remote bootstrap runner..."
# Write the inner script to a file and SCP it. Avoids shell-quoting hell
# from heredoc-inside-bash-c-inside-single-quoted-ssh-arg. AL2023 cleanup
# rationale (do NOT touch /etc/systemd/network or machine-id; minimal
# cloud-init clean only) is documented in this file as comments.
cat > /tmp/bake-remote-runner.sh <<'REMOTE_EOF'
#!/bin/bash
set -e
# Use absolute path: under sudo, $HOME=/root, not /home/ec2-user.
cd /home/ec2-user/trading-latency-benchmark/mcast2ucast
./bootstrap.sh > /tmp/bootstrap.log 2>&1
# Release hugepages so next boot starts clean (setup_instance.sh re-allocates).
echo 0 > /proc/sys/vm/nr_hugepages
# No cloud-init or network cleanup. The vendor 80-ec2.network handles DHCP
# for ENA on new instances. Any cleanup (cloud-init clean, wiping network
# files, truncating machine-id) breaks the NIC initialization on next boot.
echo BOOTSTRAP_OK
REMOTE_EOF
scp "${SSH_OPTS[@]}" /tmp/bake-remote-runner.sh ec2-user@"$PUBLIC_IP":/tmp/bake-remote-runner.sh

echo "[bake] launching bootstrap.sh detached on instance ..."
# Detach via setsid so the SSH session can drop without killing bootstrap
# (DPDK build is ~15 min; a transient idle-disconnect previously aborted
# the bake). The remote writes /tmp/bootstrap.exit when done; we poll it.
ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" '
    rm -f /tmp/bootstrap.exit
    chmod +x /tmp/bake-remote-runner.sh
    sudo nohup setsid bash -c "/tmp/bake-remote-runner.sh >> /tmp/bootstrap.log 2>&1; echo \$? > /tmp/bootstrap.exit" < /dev/null > /dev/null 2>&1 &
    sleep 2
    echo LAUNCHED
' < /dev/null > /tmp/bake-ssh.log 2>&1
if ! grep -q LAUNCHED /tmp/bake-ssh.log; then
    echo "ERROR: failed to launch detached bootstrap" >&2
    cat /tmp/bake-ssh.log >&2
    exit 11
fi

echo "[bake] polling bootstrap progress (up to 30 min)..."
DEADLINE=$(( $(date +%s) + 1800 ))
LAST_LINES=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    EXIT_RC=$(ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'cat /tmp/bootstrap.exit 2>/dev/null || echo POLLING' 2>/dev/null || echo SSH_ERR)
    if [ "$EXIT_RC" != "POLLING" ] && [ "$EXIT_RC" != "SSH_ERR" ] && [ -n "$EXIT_RC" ]; then
        break
    fi
    # Print incremental log lines so the operator can see progress
    CUR_LINES=$(ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'wc -l < /tmp/bootstrap.log 2>/dev/null || echo 0' 2>/dev/null || echo 0)
    if [ "$CUR_LINES" -gt "$LAST_LINES" ]; then
        ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" "tail -n +$((LAST_LINES + 1)) /tmp/bootstrap.log 2>/dev/null | grep -E '^=== |Linking target' | tail -5" 2>/dev/null || true
        LAST_LINES="$CUR_LINES"
    fi
    sleep 30
done

if [ "$EXIT_RC" = "POLLING" ] || [ "$EXIT_RC" = "SSH_ERR" ] || [ -z "$EXIT_RC" ]; then
    echo "ERROR: bootstrap timed out after 30 min" >&2
    ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'tail -n 50 /tmp/bootstrap.log' >&2 || true
    exit 11
fi
if [ "$EXIT_RC" != "0" ]; then
    echo "ERROR: bootstrap.sh exited rc=$EXIT_RC" >&2
    echo "--- last 50 lines of remote /tmp/bootstrap.log ---" >&2
    ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'tail -n 50 /tmp/bootstrap.log' >&2 || true
    exit 11
fi
# Belt + suspenders: confirm BOOTSTRAP_OK marker landed in the log
if ! ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'grep -q BOOTSTRAP_OK /tmp/bootstrap.log' 2>/dev/null; then
    echo "ERROR: BOOTSTRAP_OK marker missing despite rc=0 — incomplete bootstrap" >&2
    ssh "${SSH_OPTS[@]}" ec2-user@"$PUBLIC_IP" 'tail -n 50 /tmp/bootstrap.log' >&2 || true
    exit 11
fi
echo "[bake] bootstrap complete"

echo "[bake] rebooting instance to persist network state to EBS ..."
aws ec2 reboot-instances "${AWS_REGION_ARGS[@]}" --instance-ids "$INSTANCE_ID"
sleep 10
aws ec2 wait instance-status-ok "${AWS_REGION_ARGS[@]}" --instance-ids "$INSTANCE_ID"
echo "[bake] instance back up after reboot"

echo "[bake] creating AMI '$AMI_NAME' (no reboot) ..."
AMI_ID=$(aws ec2 create-image "${AWS_REGION_ARGS[@]}" \
    --instance-id "$INSTANCE_ID" \
    --name "$AMI_NAME" \
    --description "mcast2ucast benchmark AMI baked $TIMESTAMP" \
    --no-reboot \
    --query 'ImageId' --output text)
echo "[bake] ami: $AMI_ID  (waiting up to 10min for available)"

if ! aws ec2 wait image-available "${AWS_REGION_ARGS[@]}" --image-ids "$AMI_ID"; then
    echo "ERROR: CreateImage timed out" >&2
    exit 12
fi

echo "[bake] tagging AMI"
aws ec2 create-tags "${AWS_REGION_ARGS[@]}" \
    --resources "$AMI_ID" \
    --tags Key=Project,Value=mcast2ucast-bench Key=BakedAt,Value="$TIMESTAMP" >/dev/null || true

# Last line is the AMI ID — operator/orchestrator parses this.
echo "$AMI_ID"
