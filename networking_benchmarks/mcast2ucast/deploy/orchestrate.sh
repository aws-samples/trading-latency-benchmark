#!/bin/bash
# orchestrate.sh — Drive the mcast2ucast benchmark lifecycle from the
# operator workstation.  See subcommand list at the bottom of usage().
#
# Required tools on the operator workstation: aws, jq, ssh, scp, npx (or cdk).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TOPOLOGY_JSON="$HERE/topology.json"
CDK_DIR="$HERE/cdk"
SCRIPTS_DIR="$HERE/scripts"
TOOLS_DIR="$HERE/tools"
RESULTS_DIR="$HERE/results"

# Defaults — overridable via flags
DEFAULT_PRIMARY_NIC=""    # empty → setup_instance.sh / verify_ptp_hwtstamp.sh auto-detect
DEFAULT_SECONDARY_PCI=""  # empty → setup_instance.sh auto-detects (different across c7a/m6i/metal)
DEFAULT_MAX_OFFSET_US=10
DEFAULT_PPS=1000
DEFAULT_COUNT=1000
DEFAULT_PAYLOAD=64
DEFAULT_RUN_TIMEOUT_S=30
DEFAULT_MCAST_GROUP="224.0.0.101"  # recorded in topology.json; single source of truth

# The variables above are consumed by cmd_* implementations in Tasks 11-16.
# Reference them here so shellcheck does not flag them as unused while the
# stubs are placeholders.
: "${CDK_DIR}" "${SCRIPTS_DIR}" "${RESULTS_DIR}" \
  "${DEFAULT_PRIMARY_NIC}" "${DEFAULT_SECONDARY_PCI}" \
  "${DEFAULT_MAX_OFFSET_US}" "${DEFAULT_PPS}" "${DEFAULT_COUNT}" \
  "${DEFAULT_PAYLOAD}" "${DEFAULT_RUN_TIMEOUT_S}"

usage() {
    cat <<'EOF'
Usage: orchestrate.sh <subcommand> [args]

Subcommands:
  bake-ami   --base-ami <id> --key <kp> --subnet-id <sn> --security-group-id <sg>
             [--instance-type t3.medium] [--region <r>] [--ssh-key-path <p>]
  deploy     --topology cpg|same-az|multi-az
             --sender-type <type> --receiver-type <type>
             --ami-id <id> --key <kp>
             [--region <r>] [--single-az <az>] [--sender-az <az>] [--receiver-az <az>]
             [--ssh-key-path <p>]
             [--num-subscriber-hosts H]   (default 1, range 1..32)
  setup      [--primary-nic ens5] [--pci 28:00.0]
  verify     [--max-offset-us 10]
  run        [--pps 1000] [--count 1000] [--payload 64] [--keep-logs]
  collect
  teardown   [--yes]
  all        deploy + setup + verify + run + collect (no teardown)
  ssh        sender|subscriber-<i>
EOF
}

# ---- helpers ---------------------------------------------------------

err() { echo "ERROR: $*" >&2; }

require_topology_json() {
    if [ ! -f "$TOPOLOGY_JSON" ]; then
        err "topology.json not found at $TOPOLOGY_JSON — run 'orchestrate.sh deploy' first"
        exit 30
    fi
}

require_tools() {
    for t in "$@"; do
        if ! command -v "$t" >/dev/null 2>&1; then
            err "missing required tool: $t"
            exit 1
        fi
    done
}

require_positive_int() {
    # require_positive_int <field-name> <value>
    #
    # jq prints "null" for absent keys, and `for ((i=0; i<null; i++))` is a
    # silent no-op — so a malformed topology.json could false-pass a run/verify
    # or produce an empty collect. Guard every count read out of topology.json.
    local name="$1" val="$2"
    if ! [[ "$val" =~ ^[0-9]+$ ]] || [ "$val" -lt 1 ]; then
        err "topology.json: $name is '$val' — not a positive integer"
        exit 30
    fi
}

ssh_to() {
    # ssh_to <host_ip> <ssh_key_path> <remote-cmd-string>
    #
    # <remote-cmd-string> is passed verbatim to the remote shell — callers
    # must quote shell metacharacters themselves.  StrictHostKeyChecking=no
    # is intentional: ephemeral EC2 hosts have no stable host key.
    local ip="$1" key="$2" cmd="$3"
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=60 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 \
        -i "$key" ec2-user@"$ip" "$cmd"
}

scp_to() {
    # scp_to <host_ip> <ssh_key_path> <local_path> <remote_path>
    local ip="$1" key="$2" local_path="$3" remote_path="$4"
    scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=60 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 \
        -i "$key" "$local_path" ec2-user@"$ip":"$remote_path"
}

scp_from() {
    # scp_from <host_ip> <ssh_key_path> <remote_path> <local_path>
    local ip="$1" key="$2" remote_path="$3" local_path="$4"
    scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=60 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 \
        -i "$key" ec2-user@"$ip":"$remote_path" "$local_path"
}

# ---- subcommands -----------------------------------------------------

cmd_bake_ami()  { "$TOOLS_DIR/bake_ami.sh" "$@"; }
cmd_deploy() {
    require_tools aws jq npx python3

    local topology="" sender_type="" receiver_type="" ami_id="" key=""
    local region="" single_az="" sender_az="" receiver_az="" receiver_azs="" ssh_key_path=""
    local num_hosts="" num_daemons=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --topology)       topology="$2"; shift 2 ;;
            --sender-type)    sender_type="$2"; shift 2 ;;
            --receiver-type)  receiver_type="$2"; shift 2 ;;
            --ami-id)         ami_id="$2"; shift 2 ;;
            --key)            key="$2"; shift 2 ;;
            --region)         region="$2"; shift 2 ;;
            --single-az)      single_az="$2"; shift 2 ;;
            --sender-az)      sender_az="$2"; shift 2 ;;
            --receiver-az)    receiver_az="$2"; shift 2 ;;
            --receiver-azs)   receiver_azs="$2"; shift 2 ;;
            --ssh-key-path)   ssh_key_path="$2"; shift 2 ;;
            --num-subscriber-hosts)            num_hosts="$2"; shift 2 ;;
            --num-subscriber-daemons-per-host) num_daemons="$2"; shift 2 ;;
            *) err "unknown flag: $1"; exit 2 ;;
        esac
    done
    for v in topology sender_type receiver_type ami_id key; do
        if [ -z "${!v}" ]; then err "--${v//_/-} required"; exit 2; fi
    done
    [ -z "$region" ]        && region="us-east-1"
    [ -z "$ssh_key_path" ]  && ssh_key_path="$HOME/.ssh/${key}.pem"
    if [ "$topology" = "multi-az" ]; then
        [ -z "$sender_az" ]   && sender_az="${region}a"
        [ -z "$receiver_az" ] && receiver_az="${region}b"
    elif [ "$topology" = "spread-az" ]; then
        [ -z "$sender_az" ]   && sender_az="${region}a"
        [ -z "$receiver_azs" ] && receiver_azs="${region}a,${region}b,${region}c"
    else
        [ -z "$single_az" ]   && single_az="${region}a"
    fi

    [ -z "$num_hosts" ]   && num_hosts=1
    [ -z "$num_daemons" ] && num_daemons=1
    if ! [[ "$num_hosts" =~ ^([1-9]|[12][0-9]|3[0-2])$ ]]; then
        err "--num-subscriber-hosts must be 1..32, got '$num_hosts'"; exit 2
    fi
    if ! [[ "$num_daemons" =~ ^([1-9]|1[0-6])$ ]]; then
        err "--num-subscriber-daemons-per-host must be 1..16, got '$num_daemons'"; exit 2
    fi
    if [ $((num_hosts * num_daemons)) -gt 128 ]; then
        err "H*D must be <= 128, got $num_hosts*$num_daemons=$((num_hosts*num_daemons))"; exit 2
    fi

    echo "[deploy] cdk deploy ($topology, sender=$sender_type, receiver=$receiver_type)"
    (
        # set -e is suppressed for the LHS of `||`, so re-enable it here
        # explicitly so pip-install or activate failures still abort the
        # subshell and trigger the error branch below.
        set -euo pipefail
        cd "$CDK_DIR"
        if [ ! -d .venv ]; then python3 -m venv .venv; fi
        # shellcheck disable=SC1091
        . .venv/bin/activate
        pip install -q -r requirements.txt
        local args=(
            --require-approval never
            --context topology="$topology"
            --context senderInstanceType="$sender_type"
            --context receiverInstanceType="$receiver_type"
            --context amiId="$ami_id"
            --context keyPairName="$key"
            --context region="$region"
            --context numSubscriberHosts="$num_hosts"
            --context numSubscriberDaemonsPerHost="$num_daemons"
        )
        if [ "$topology" = "multi-az" ]; then
            args+=(--context senderAz="$sender_az" --context receiverAz="$receiver_az")
        elif [ "$topology" = "spread-az" ]; then
            args+=(--context senderAz="$sender_az" --context receiverAzs="$receiver_azs")
        else
            args+=(--context singleAz="$single_az")
        fi
        npx --yes "aws-cdk@${CDK_CLI_VERSION:-2.1125.0}" deploy "${args[@]}"
        deactivate
    ) || { err "cdk deploy failed"; exit 20; }

    echo "[deploy] reading CFN outputs"
    local out_json
    out_json=$(aws cloudformation describe-stacks \
        --region "$region" --stack-name Mcast2UcastBenchStack \
        --query 'Stacks[0].Outputs' --output json) \
        || { err "describe-stacks failed"; exit 21; }

    o() { echo "$out_json" | jq -r --arg k "$1" '.[] | select(.OutputKey==$k) | .OutputValue'; }

    local sender_instance_id sender_primary_ip sender_secondary_eni sender_secondary_ip sender_az_out
    sender_instance_id=$(o SenderInstanceId)
    sender_primary_ip=$(o SenderPublicIp)
    sender_secondary_eni=$(o SenderSecondaryEniId)
    sender_secondary_ip=$(o SenderSecondaryIp)
    sender_az_out=$(o SenderAz)

    for v in sender_instance_id sender_primary_ip sender_secondary_eni sender_secondary_ip sender_az_out; do
        if [ -z "${!v}" ] || [ "${!v}" = "null" ]; then
            err "missing CFN output for $v"; exit 21
        fi
    done

    local -a sub_instance_ids sub_primary_ips sub_secondary_enis sub_secondary_ips sub_secondary_macs sub_azs
    for ((i=0; i<num_hosts; i++)); do
        sub_instance_ids[i]=$(o "Subscriber${i}InstanceId")
        sub_primary_ips[i]=$(o "Subscriber${i}PublicIp")
        sub_secondary_enis[i]=$(o "Subscriber${i}SecondaryEniId")
        sub_secondary_ips[i]=$(o "Subscriber${i}SecondaryIp")
        sub_azs[i]=$(o "Subscriber${i}Az")
        for val in "${sub_instance_ids[i]}" "${sub_primary_ips[i]}" \
                   "${sub_secondary_enis[i]}" "${sub_secondary_ips[i]}" "${sub_azs[i]}"; do
            if [ -z "$val" ] || [ "$val" = "null" ]; then
                err "missing/null CFN output for a Subscriber${i}* key"; exit 21
            fi
        done
    done

    echo "[deploy] resolving secondary ENI MACs"
    local sender_secondary_mac
    sender_secondary_mac=$(aws ec2 describe-network-interfaces --region "$region" \
        --network-interface-ids "$sender_secondary_eni" \
        --query 'NetworkInterfaces[0].MacAddress' --output text) \
        || { err "describe-network-interfaces (sender) failed"; exit 21; }
    # AWS CLI --output text returns the literal "None" for missing attrs
    if [ -z "$sender_secondary_mac" ] || [ "$sender_secondary_mac" = "None" ]; then
        err "failed to resolve sender_secondary_mac from describe-network-interfaces"; exit 21
    fi

    for ((i=0; i<num_hosts; i++)); do
        sub_secondary_macs[i]=$(aws ec2 describe-network-interfaces --region "$region" \
            --network-interface-ids "${sub_secondary_enis[i]}" \
            --query 'NetworkInterfaces[0].MacAddress' --output text) \
            || { err "describe-network-interfaces (subscriber-$i) failed"; exit 21; }
        if [ -z "${sub_secondary_macs[i]}" ] || [ "${sub_secondary_macs[i]}" = "None" ]; then
            err "failed to resolve subscriber-$i secondary mac"; exit 21
        fi
    done

    echo "[deploy] writing $TOPOLOGY_JSON"
    # AZs come from the CDK outputs (authoritative per-host), not re-derived
    # from the CLI flags — for spread-az each subscriber lands in a distinct
    # AZ that a single scalar cannot represent.
    local sender_az_resolved="$sender_az_out"

    # Note: env-var prefix is a command-prefix assignment, not a `local`
    # declaration, so SC2155 does not apply here. Newlines (not NULs!) are
    # used as the array separator: bash $(...) strips NULs by POSIX
    # mandate, so `printf '%s\0'` would silently concatenate elements.
    NUM_HOSTS="$num_hosts" \
    NUM_DAEMONS="$num_daemons" \
    MCAST_GROUP="$DEFAULT_MCAST_GROUP" \
    TOPOLOGY="$topology" \
    KEY_NAME="$key" \
    SSH_KEY_PATH="$ssh_key_path" \
    AMI_ID="$ami_id" \
    REGION="$region" \
    SENDER_INSTANCE_ID="$sender_instance_id" \
    SENDER_TYPE="$sender_type" \
    SENDER_AZ="$sender_az_resolved" \
    SENDER_PRIMARY_IP="$sender_primary_ip" \
    SENDER_SECONDARY_ENI="$sender_secondary_eni" \
    SENDER_SECONDARY_IP="$sender_secondary_ip" \
    SENDER_SECONDARY_MAC="$sender_secondary_mac" \
    RECEIVER_TYPE="$receiver_type" \
    SUB_INSTANCE_IDS="$(printf '%s\n' "${sub_instance_ids[@]}")" \
    SUB_PRIMARY_IPS="$(printf '%s\n' "${sub_primary_ips[@]}")" \
    SUB_SECONDARY_ENIS="$(printf '%s\n' "${sub_secondary_enis[@]}")" \
    SUB_SECONDARY_IPS="$(printf '%s\n' "${sub_secondary_ips[@]}")" \
    SUB_SECONDARY_MACS="$(printf '%s\n' "${sub_secondary_macs[@]}")" \
    SUB_AZS="$(printf '%s\n' "${sub_azs[@]}")" \
    DEPLOYED_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    python3 - "$TOPOLOGY_JSON" <<'PYEOF'
import json, sys, os

out_path = sys.argv[1]
num_hosts = int(os.environ["NUM_HOSTS"])
num_daemons = int(os.environ["NUM_DAEMONS"])

# splitlines() handles both the trailing newline that printf '%s\n'
# always emits AND any platform-specific line-ending oddities.
sub_instance_ids = os.environ["SUB_INSTANCE_IDS"].splitlines()[:num_hosts]
sub_primary_ips = os.environ["SUB_PRIMARY_IPS"].splitlines()[:num_hosts]
sub_secondary_enis = os.environ["SUB_SECONDARY_ENIS"].splitlines()[:num_hosts]
sub_secondary_ips = os.environ["SUB_SECONDARY_IPS"].splitlines()[:num_hosts]
sub_secondary_macs = os.environ["SUB_SECONDARY_MACS"].splitlines()[:num_hosts]
sub_azs = os.environ["SUB_AZS"].splitlines()[:num_hosts]

ports = list(range(5001, 5001 + num_daemons))

doc = {
    "stack_name": "Mcast2UcastBenchStack",
    "topology": os.environ["TOPOLOGY"],
    "key_name": os.environ["KEY_NAME"],
    "ssh_key_path": os.environ["SSH_KEY_PATH"],
    "ami_id": os.environ["AMI_ID"],
    "region": os.environ["REGION"],
    "num_subscriber_hosts": num_hosts,
    "num_subscriber_daemons_per_host": num_daemons,
    "mcast_group": os.environ["MCAST_GROUP"],
    "sender": {
        "instance_id": os.environ["SENDER_INSTANCE_ID"],
        "instance_type": os.environ["SENDER_TYPE"],
        "az": os.environ["SENDER_AZ"],
        "primary_ip": os.environ["SENDER_PRIMARY_IP"],
        "secondary_eni_id": os.environ["SENDER_SECONDARY_ENI"],
        "secondary_ip": os.environ["SENDER_SECONDARY_IP"],
        "secondary_mac": os.environ["SENDER_SECONDARY_MAC"],
        "tap_ip": "10.99.0.1",
    },
    "subscriber_hosts": [
        {
            "index": i,
            "instance_id": sub_instance_ids[i],
            "instance_type": os.environ["RECEIVER_TYPE"],
            "az": sub_azs[i],
            "primary_ip": sub_primary_ips[i],
            "secondary_eni_id": sub_secondary_enis[i],
            "secondary_ip": sub_secondary_ips[i],
            "secondary_mac": sub_secondary_macs[i],
            "tap_ip": f"10.99.0.{i + 2}",
            "daemon_ports": ports,
        }
        for i in range(num_hosts)
    ],
    "deployed_utc": os.environ["DEPLOYED_UTC"],
}

with open(out_path, "w") as f:
    json.dump(doc, f, indent=2)
PYEOF

    echo "[deploy] OK"
    cat "$TOPOLOGY_JSON"
}
cmd_setup() {
    require_tools jq ssh scp
    require_topology_json

    local primary_nic="$DEFAULT_PRIMARY_NIC" pci="$DEFAULT_SECONDARY_PCI"
    while [ $# -gt 0 ]; do
        case "$1" in
            --primary-nic) primary_nic="$2"; shift 2 ;;
            --pci)         pci="$2"; shift 2 ;;
            *) err "unknown flag: $1"; exit 2 ;;
        esac
    done

    local key_path
    key_path=$(jq -r '.ssh_key_path' "$TOPOLOGY_JSON")
    local sender_ip sender_tap
    sender_ip=$(jq -r '.sender.primary_ip' "$TOPOLOGY_JSON")
    sender_tap=$(jq -r '.sender.tap_ip' "$TOPOLOGY_JSON")
    local num_hosts mcast_group
    num_hosts=$(jq -r '.num_subscriber_hosts' "$TOPOLOGY_JSON")
    require_positive_int num_subscriber_hosts "$num_hosts"
    mcast_group=$(jq -r ".mcast_group // \"$DEFAULT_MCAST_GROUP\"" "$TOPOLOGY_JSON")

    # Build the sender's deploy.conf with H*D subscriber lines and SCP it
    # ahead of running setup_instance.sh on the sender host. setup_instance.sh
    # on the SENDER overwrites $MCAST_DIR/deploy.conf with a single-line file
    # by default (as written in Task 8). We override that by:
    #   - writing the H*D file LOCALLY to /tmp/sender-deploy.conf
    #   - SCPing it to the sender's $MCAST_DIR/deploy.conf
    #   - running setup_instance.sh with --skip-deploy-conf (NEW flag)
    #
    # OR: pre-generate the file and pass it as an arg. Chose the SCP-then-skip
    # approach because the file content is wholly client-derived from
    # topology.json and doesn't depend on any host-side state.
    # Per-run temp dir so concurrent orchestrate invocations on one workstation
    # don't clobber each other's deploy.conf / setup logs mid-scp.
    local tmpdir
    tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/orch-setup.XXXXXX")
    local deploy_conf="$tmpdir/sender-deploy.conf"
    {
        echo "# Auto-generated by orchestrate.sh — sender side, H*D fan-out."
        local i d
        for ((i=0; i<num_hosts; i++)); do
            local sip mac
            sip=$(jq -r ".subscriber_hosts[$i].secondary_ip" "$TOPOLOGY_JSON")
            mac=$(jq -r ".subscriber_hosts[$i].secondary_mac" "$TOPOLOGY_JSON")
            for d in $(jq -r ".subscriber_hosts[$i].daemon_ports[]" "$TOPOLOGY_JSON"); do
                echo "subscriber $mcast_group $sip $d $mac"
            done
        done
    } > "$deploy_conf"

    local total_subs
    total_subs=$(grep -c '^subscriber ' "$deploy_conf")
    echo "[setup] deploy.conf has $total_subs lines (H=$num_hosts, D=$(jq -r '.num_subscriber_daemons_per_host' "$TOPOLOGY_JSON"))"

    setup_host() {
        # role: "sender" or "subscriber"
        # peer_*: only relevant for sender; for subscribers we pass empty.
        local role="$1" ip="$2" tap_ip="$3" peer_tap_ip="$4" peer_sip="$5" peer_mac="$6"
        echo "[setup:$role:$ip] copying setup_instance.sh"
        scp_to "$ip" "$key_path" "$SCRIPTS_DIR/setup_instance.sh" /tmp/setup_instance.sh

        # Build NIC/PCI flag list — omit when empty so setup_instance.sh
        # falls back to its on-host auto-detection (works across c7a/m6i/metal).
        local nic_pci_flags=""
        [ -n "$primary_nic" ] && nic_pci_flags+=" --primary-nic $primary_nic"
        [ -n "$pci" ]         && nic_pci_flags+=" --pci $pci"

        local cmd
        if [ "$role" = "sender" ]; then
            # SCP the pre-generated multi-line deploy.conf, then run setup
            # with --skip-deploy-conf so setup_instance.sh keeps our file.
            scp_to "$ip" "$key_path" "$deploy_conf" /tmp/sender-deploy.conf
            cmd="sudo cp /tmp/sender-deploy.conf /home/ec2-user/mcast2ucast/deploy.conf && sudo chown ec2-user:ec2-user /home/ec2-user/mcast2ucast/deploy.conf && chmod +x /tmp/setup_instance.sh && sudo /tmp/setup_instance.sh --role sender ${nic_pci_flags} --tap-ip ${tap_ip} --peer-tap-ip ${peer_tap_ip} --peer-secondary-ip ${peer_sip} --peer-secondary-mac ${peer_mac} ${gateway_mac_flag} --skip-deploy-conf"
        else
            cmd="chmod +x /tmp/setup_instance.sh && sudo /tmp/setup_instance.sh --role receiver $nic_pci_flags --tap-ip $tap_ip --peer-tap-ip $peer_tap_ip"
        fi
        echo "[setup:$role:$ip] running"
        ssh_to "$ip" "$key_path" "$cmd"
    }

    # First subscriber host's MAC + IP are passed to the sender's setup
    # only as the --peer-* args. setup_instance.sh's --skip-deploy-conf
    # mode means these are unused for sender, but the script still
    # requires the flags to be present. Pass the first subscriber's
    # values as a placeholder.
    local first_sub_ip first_sub_mac first_sub_tap
    first_sub_ip=$(jq -r '.subscriber_hosts[0].secondary_ip' "$TOPOLOGY_JSON")
    first_sub_mac=$(jq -r '.subscriber_hosts[0].secondary_mac' "$TOPOLOGY_JSON")
    first_sub_tap=$(jq -r '.subscriber_hosts[0].tap_ip' "$TOPOLOGY_JSON")

    # For multi-az topology, discover the VPC gateway MAC so the sender's
    # DPDK daemon can route unicast packets across subnets. The gateway IP
    # is subnet_base + 1 (AWS VPC convention).
    local gateway_mac_flag=""
    local topology
    topology=$(jq -r '.topology // "cpg"' "$TOPOLOGY_JSON")
    if [ "$topology" = "multi-az" ] || [ "$topology" = "spread-az" ]; then
        echo "[setup] multi-az: discovering gateway MAC from sender..."
        local gw_mac
        gw_mac=$(ssh_to "$sender_ip" "$key_path" '
            GW_IP=$(ip route show default | awk "/default/{print \$3}")
            if [ -z "$GW_IP" ]; then
                echo "ERROR: no default gateway" >&2; exit 1
            fi
            ping -c 1 -W 1 "$GW_IP" >/dev/null 2>&1
            arp -n "$GW_IP" | awk "/ether/{print \$3; exit}"
        ')
        gw_mac=$(echo "$gw_mac" | tr -d '\r\n ')
        if [ -z "$gw_mac" ]; then
            err "could not discover gateway MAC"; exit 30
        fi
        echo "[setup] gateway MAC: $gw_mac"
        gateway_mac_flag="--gateway-mac $gw_mac"
    fi

    # Launch sender + all subscriber hosts in parallel
    setup_host sender "$sender_ip" "$sender_tap" "$first_sub_tap" "$first_sub_ip" "$first_sub_mac" \
        > "$tmpdir/setup-sender.log" 2>&1 &
    local pids=($!)

    local i
    for ((i=0; i<num_hosts; i++)); do
        local sub_ip sub_tap
        sub_ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
        sub_tap=$(jq -r ".subscriber_hosts[$i].tap_ip" "$TOPOLOGY_JSON")
        setup_host subscriber "$sub_ip" "$sub_tap" "$sender_tap" "" "" \
            > "$tmpdir/setup-sub-$i.log" 2>&1 &
        pids+=($!)
    done

    local rc=0
    for pid in "${pids[@]}"; do
        wait "$pid" || { rc=$?; }
    done

    echo "----- $tmpdir/setup-sender.log (tail 30) -----"
    tail -n 30 "$tmpdir/setup-sender.log" || true
    for ((i=0; i<num_hosts; i++)); do
        echo "----- $tmpdir/setup-sub-$i.log (tail 30) -----"
        tail -n 30 "$tmpdir/setup-sub-$i.log" || true
    done

    if [ "$rc" -ne 0 ]; then
        err "setup failed (rc=$rc)"; exit 30
    fi
    echo "[setup] OK"
}
cmd_verify() {
    require_tools jq ssh scp
    require_topology_json

    local max_offset_us="$DEFAULT_MAX_OFFSET_US"
    local primary_nic="$DEFAULT_PRIMARY_NIC"
    while [ $# -gt 0 ]; do
        case "$1" in
            --max-offset-us) max_offset_us="$2"; shift 2 ;;
            --primary-nic)   primary_nic="$2"; shift 2 ;;
            *) err "unknown flag: $1"; exit 2 ;;
        esac
    done

    local key_path num_hosts
    key_path=$(jq -r '.ssh_key_path' "$TOPOLOGY_JSON")
    num_hosts=$(jq -r '.num_subscriber_hosts' "$TOPOLOGY_JSON")
    require_positive_int num_subscriber_hosts "$num_hosts"

    local nic_flag=""
    [ -n "$primary_nic" ] && nic_flag="--primary-nic $primary_nic"

    # Verify a single host: scp script, run, check exit code, extract offset.
    # Args: $1=label $2=ip
    _verify_host() {
        local label="$1" ip="$2"
        echo "[verify:$label:$ip] copying verify_ptp_hwtstamp.sh"
        scp_to "$ip" "$key_path" "$SCRIPTS_DIR/verify_ptp_hwtstamp.sh" /tmp/verify_ptp_hwtstamp.sh

        echo "[verify:$label:$ip] running gate (max_offset_us=$max_offset_us)"
        local out rc=0
        out=$(ssh_to "$ip" "$key_path" "chmod +x /tmp/verify_ptp_hwtstamp.sh && /tmp/verify_ptp_hwtstamp.sh $nic_flag --max-offset-us $max_offset_us") || rc=$?

        echo "$out"
        if [ "$rc" -ne 0 ]; then
            case "$rc" in
                1) err "verify ($label): NIC missing hardware-receive — exit 40"; exit 40 ;;
                2) err "verify ($label): chronyd not active or bad arg — exit 41"; exit 41 ;;
                3) err "verify ($label): chrony Leap not Normal — exit 41"; exit 41 ;;
                4) err "verify ($label): chrony offset above ${max_offset_us}us — exit 42"; exit 42 ;;
                5) err "verify ($label): could not parse chronyc output (format changed?) — exit 41"; exit 41 ;;
                *) err "verify ($label): unexpected rc=$rc"; exit 40 ;;
            esac
        fi

        local offset_us
        offset_us=$(echo "$out" | sed -n 's/^OK offset_us=\([0-9]*\).*/\1/p')
        [ -z "$offset_us" ] && offset_us=0
        if [ "$offset_us" -gt "$worst_offset" ]; then
            worst_offset="$offset_us"
        fi
    }

    local worst_offset=0

    # Verify sender (stamps tx_ns — clock drift here directly biases latency)
    local sender_ip
    sender_ip=$(jq -r '.sender.primary_ip' "$TOPOLOGY_JSON")
    _verify_host "sender" "$sender_ip"

    # Verify all subscriber hosts
    local i
    for ((i=0; i<num_hosts; i++)); do
        local ip
        ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
        _verify_host "sub-$i" "$ip"
    done

    echo "$worst_offset" > "$HERE/.last-chrony-offset-us"
    echo "[verify] OK worst_offset_us=$worst_offset (across sender + $num_hosts subscriber hosts)"
}
cmd_run() {
    require_tools jq ssh
    require_topology_json

    local pps="$DEFAULT_PPS" count="$DEFAULT_COUNT" payload="$DEFAULT_PAYLOAD"
    local keep_logs=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --pps)        pps="$2"; shift 2 ;;
            --count)      count="$2"; shift 2 ;;
            --payload)    payload="$2"; shift 2 ;;
            --keep-logs)  keep_logs=1; shift ;;
            *) err "unknown flag: $1"; exit 2 ;;
        esac
    done

    if [ "$pps" -lt 1 ]; then err "--pps must be >= 1"; exit 2; fi

    # shellcheck disable=SC2155
    local interval_us=$(( 1000000 / pps ))
    local expected_s=$(( count / pps ))
    [ "$expected_s" -lt 1 ] && expected_s=1
    local recv_timeout_s=$(( DEFAULT_RUN_TIMEOUT_S > expected_s * 2 ? DEFAULT_RUN_TIMEOUT_S : expected_s * 2 ))

    local key_path sender_ip num_hosts num_daemons mcast_group
    key_path=$(jq -r '.ssh_key_path'         "$TOPOLOGY_JSON")
    sender_ip=$(jq -r '.sender.primary_ip'   "$TOPOLOGY_JSON")
    num_hosts=$(jq -r '.num_subscriber_hosts' "$TOPOLOGY_JSON")
    num_daemons=$(jq -r '.num_subscriber_daemons_per_host' "$TOPOLOGY_JSON")
    mcast_group=$(jq -r ".mcast_group // \"$DEFAULT_MCAST_GROUP\"" "$TOPOLOGY_JSON")

    require_positive_int num_subscriber_hosts "$num_hosts"
    require_positive_int num_subscriber_daemons_per_host "$num_daemons"

    local now
    now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    echo "$now" > "$HERE/.last-run-started-utc"
    echo "$pps $count $payload $interval_us $recv_timeout_s" > "$HERE/.last-run-args"

    echo "[run] killing prior latency_sender + latency_receiver across all hosts"
    ssh_to "$sender_ip" "$key_path" 'pkill -f latency_sender || true' || true
    local i
    for ((i=0; i<num_hosts; i++)); do
        local sub_ip
        sub_ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
        ssh_to "$sub_ip" "$key_path" 'pkill -f latency_receiver || true' || true
    done

    echo "[run] starting $((num_hosts * num_daemons)) latency_receivers (H=$num_hosts D=$num_daemons, count=$count, timeout=${recv_timeout_s}s)"
    # Start all receivers in parallel to minimize the time gap between
    # first and last receiver joining the multicast group. Sequential
    # SSH startup creates a ~20s window where early receivers' IGMP
    # memberships can expire before the sender fires.
    for ((i=0; i<num_hosts; i++)); do
        local sub_ip
        sub_ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
        local d port
        for ((d=0; d<num_daemons; d++)); do
            port=$(( 5001 + d ))
            # shellcheck disable=SC2155
            local cpu=$(( 6 + (d % 2) ))
            local tap_ip
            tap_ip=$(jq -r ".subscriber_hosts[$i].tap_ip" "$TOPOLOGY_JSON")
            ssh_to "$sub_ip" "$key_path" "nohup taskset -c $cpu /home/ec2-user/mcast2ucast/benchmarks/latency_receiver \
                -g $mcast_group -I $tap_ip -p $port -c $count -t $recv_timeout_s \
                --csv-out /tmp/recv-h${i}-p${port}.csv \
                > /tmp/recv-h${i}-p${port}.log 2>&1 & echo \$! > /tmp/recv-h${i}-p${port}.pid" &
        done
    done
    wait
    sleep 3

    echo "[run] starting latency_sender on $sender_ip (pps=$pps, payload=$payload)"
    # Self-contained — re-reads from topology.json so it can be safely
    # invoked outside of cmd_run's frame (e.g. via trap EXIT in future).
    _cleanup_receivers() {
        local _key _nh _j _ip
        _key=$(jq -r '.ssh_key_path' "$TOPOLOGY_JSON")
        _nh=$(jq -r '.num_subscriber_hosts' "$TOPOLOGY_JSON")
        for ((_j=0; _j<_nh; _j++)); do
            _ip=$(jq -r ".subscriber_hosts[$_j].primary_ip" "$TOPOLOGY_JSON")
            ssh_to "$_ip" "$_key" 'pkill -f latency_receiver || true' || true
        done
    }
    ssh_to "$sender_ip" "$key_path" "taskset -c 6 /home/ec2-user/mcast2ucast/benchmarks/latency_sender \
        -I mcast0 -g $mcast_group -p 5001 \
        -c $count -i $interval_us -s $payload \
        > /tmp/sender.log 2>&1" \
        || {
            err "sender exited non-zero — dumping /tmp/sender.log from $sender_ip"
            ssh_to "$sender_ip" "$key_path" 'cat /tmp/sender.log 2>/dev/null || echo "(no /tmp/sender.log)"' >&2 || \
                echo "WARN: could not retrieve sender.log (ssh failed)" >&2
            _cleanup_receivers
            exit 50
        }

    echo "[run] sender done — waiting for all receivers to drain (max ${recv_timeout_s}s)"
    local waited=0
    while [ "$waited" -lt "$recv_timeout_s" ]; do
        local any_alive=0
        for ((i=0; i<num_hosts; i++)); do
            local sub_ip
            sub_ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
            if ssh_to "$sub_ip" "$key_path" 'pgrep -f latency_receiver >/dev/null'; then
                any_alive=1; break
            fi
        done
        [ "$any_alive" -eq 0 ] && break
        sleep 2; waited=$(( waited + 2 ))
    done

    # Forced kill of any stragglers after timeout
    for ((i=0; i<num_hosts; i++)); do
        local sub_ip
        sub_ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
        ssh_to "$sub_ip" "$key_path" 'pkill -f latency_receiver || true' || true
    done

    local now_end
    now_end=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    echo "$now_end" > "$HERE/.last-run-ended-utc"

    if [ "$keep_logs" -eq 0 ]; then
        echo "[run] run done (logs left on hosts; use --keep-logs to retain after collect)"
    fi
    echo "[run] OK"
}
cmd_collect() {
    require_tools jq ssh scp python3
    require_topology_json

    local key_path sender_ip topology s_type r_type s_az r_az ami_id num_hosts num_daemons
    key_path=$(jq -r '.ssh_key_path'           "$TOPOLOGY_JSON")
    sender_ip=$(jq -r '.sender.primary_ip'     "$TOPOLOGY_JSON")
    topology=$(jq -r '.topology'               "$TOPOLOGY_JSON")
    s_type=$(jq -r '.sender.instance_type'     "$TOPOLOGY_JSON")
    r_type=$(jq -r '.subscriber_hosts[0].instance_type' "$TOPOLOGY_JSON")
    s_az=$(jq -r '.sender.az'                  "$TOPOLOGY_JSON")
    r_az=$(jq -r '.subscriber_hosts[0].az'     "$TOPOLOGY_JSON")
    ami_id=$(jq -r '.ami_id'                   "$TOPOLOGY_JSON")
    num_hosts=$(jq -r '.num_subscriber_hosts'  "$TOPOLOGY_JSON")
    num_daemons=$(jq -r '.num_subscriber_daemons_per_host' "$TOPOLOGY_JSON")

    require_positive_int num_subscriber_hosts "$num_hosts"
    require_positive_int num_subscriber_daemons_per_host "$num_daemons"

    local ts run_dir
    ts=$(date -u +%Y%m%dT%H%M%SZ)
    run_dir="$RESULTS_DIR/$ts"
    mkdir -p "$run_dir"

    echo "[collect] scp sender logs"
    scp_from "$sender_ip" "$key_path" /tmp/sender.log "$run_dir/sender-stdout.log" \
        || { err "scp sender.log failed"; exit 60; }
    scp_from "$sender_ip" "$key_path" /tmp/mcast2ucast.log "$run_dir/sender-mcast2ucast.log" \
        || true

    echo "[collect] scp $((num_hosts * num_daemons)) receiver logs + CSVs"
    local i d port sub_ip
    for ((i=0; i<num_hosts; i++)); do
        sub_ip=$(jq -r ".subscriber_hosts[$i].primary_ip" "$TOPOLOGY_JSON")
        for ((d=0; d<num_daemons; d++)); do
            port=$(( 5001 + d ))
            scp_from "$sub_ip" "$key_path" "/tmp/recv-h$i-p$port.log" "$run_dir/recv-h$i-p$port.log" \
                || { err "scp recv h$i p$port log failed"; exit 60; }
            scp_from "$sub_ip" "$key_path" "/tmp/recv-h$i-p$port.csv" "$run_dir/recv-h$i-p$port.csv" \
                || { err "scp recv h$i p$port csv failed"; exit 60; }
        done
        # Best-effort daemon log per host
        scp_from "$sub_ip" "$key_path" /tmp/mcast2ucast.log "$run_dir/sub$i-mcast2ucast.log" \
            || true
    done

    cp "$TOPOLOGY_JSON" "$run_dir/topology.json"

    local pps count payload interval_us recv_timeout_s
    if [ -f "$HERE/.last-run-args" ]; then
        # shellcheck disable=SC2034
        read -r pps count payload interval_us recv_timeout_s < "$HERE/.last-run-args"
    else
        pps="$DEFAULT_PPS"; count="$DEFAULT_COUNT"; payload="$DEFAULT_PAYLOAD"
        interval_us=$(( 1000000 / pps ))
        # shellcheck disable=SC2034
        recv_timeout_s="$DEFAULT_RUN_TIMEOUT_S"   # set under set -u for any future use
    fi
    local started_utc ended_utc offset_us
    started_utc=$(cat "$HERE/.last-run-started-utc" 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)
    ended_utc=$(cat   "$HERE/.last-run-ended-utc"   2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)
    offset_us=$(cat   "$HERE/.last-chrony-offset-us" 2>/dev/null || echo 0)

    echo "[collect] parsing first receiver percentile table"
    # Parse recv-h0-p5001.log as the representative single-receiver stat.
    # Pre-check rather than relying on the python heredoc to surface a
    # FileNotFoundError — gives the operator a targeted "did cmd_run
    # complete?" message instead of an opaque traceback.
    if [ ! -f "$run_dir/recv-h0-p5001.log" ]; then
        err "recv-h0-p5001.log missing in $run_dir — did cmd_run complete?"
        exit 61
    fi
    local pcts_json
    pcts_json=$(python3 - "$run_dir/recv-h0-p5001.log" <<'PYEOF'
import json, re, sys
text = open(sys.argv[1]).read()
def find_int(pat, default=0):
    m = re.search(pat, text); return int(m.group(1)) if m else default
received = find_int(r'Received:\s+(\d+)/')
lost     = find_int(r'Lost:\s+(\d+)\s+packets')
ooo      = find_int(r'Out of order:\s+(\d+)')
pcts = {}
for p in [0,10,20,30,40,50,60,70,80,90,95,99]:
    m = re.search(rf'P{p}\b\s+([\d.]+)', text)
    if m: pcts[f"p{p}_us"] = float(m.group(1))
out = {"received": received, "lost": lost, "out_of_order": ooo, **pcts}
print(json.dumps(out))
PYEOF
) || { err "first-receiver percentile parse failed"; exit 61; }

    echo "[collect] running analyze_spread.py"
    local spread_json
    spread_json=$(python3 "$SCRIPTS_DIR/analyze_spread.py" "$run_dir/topology.json" "$run_dir" "$count") \
        || { err "analyze_spread.py failed"; exit 61; }

    # Compose summary.json
    jq -n \
        --argjson pcts "$pcts_json" \
        --argjson spread "$spread_json" \
        --arg topology "$topology" \
        --arg s_type "$s_type" \
        --arg r_type "$r_type" \
        --arg s_az "$s_az" \
        --arg r_az "$r_az" \
        --argjson pps "$pps" \
        --argjson interval_us "$interval_us" \
        --argjson count "$count" \
        --argjson payload "$payload" \
        --argjson offset_us "$offset_us" \
        --arg started "$started_utc" \
        --arg ended "$ended_utc" \
        --arg ami_id "$ami_id" \
        '{
          topology: $topology,
          sender_instance_type: $s_type,
          receiver_instance_type: $r_type,
          sender_az: $s_az, receiver_az: $r_az,
          pps: $pps, interval_us: $interval_us,
          count: $count, payload_bytes: $payload,
          chrony_offset_us_at_start: $offset_us,
          started_utc: $started, ended_utc: $ended,
          ami_id: $ami_id,
          stack_name: "Mcast2UcastBenchStack",
          first_receiver: $pcts
        } + $spread' \
        > "$run_dir/summary.json"

    echo "[collect] OK $run_dir/summary.json"
    cat "$run_dir/summary.json"
}
cmd_teardown() {
    require_tools npx jq

    local yes=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --yes) yes=1; shift ;;
            *) err "unknown flag: $1"; exit 2 ;;
        esac
    done

    local region="us-east-1"
    if [ -f "$TOPOLOGY_JSON" ]; then
        region=$(jq -r '.region // "us-east-1"' "$TOPOLOGY_JSON")
    fi

    if [ "$yes" -eq 0 ]; then
        read -r -p "Destroy stack Mcast2UcastBenchStack in $region? [y/N] " ans
        case "$ans" in y|Y) ;; *) echo "aborted"; exit 0 ;; esac
    fi

    (
        # set -e is suppressed for the LHS of `||`, so re-enable here.
        set -euo pipefail
        cd "$CDK_DIR"
        # shellcheck disable=SC1091
        if [ -d .venv ]; then . .venv/bin/activate; fi
        npx --yes "aws-cdk@${CDK_CLI_VERSION:-2.1125.0}" destroy --force \
            --context topology=cpg \
            --context senderInstanceType=placeholder \
            --context receiverInstanceType=placeholder \
            --context amiId=ami-placeholder \
            --context keyPairName=placeholder \
            --context region="$region" \
            --context singleAz="${region}a" \
            --context numSubscriberHosts=1 \
            --context numSubscriberDaemonsPerHost=1
        if [ -d .venv ]; then deactivate; fi
    ) || { err "cdk destroy failed"; exit 70; }

    rm -f "$TOPOLOGY_JSON" "$HERE/.last-chrony-offset-us" \
          "$HERE/.last-run-started-utc" "$HERE/.last-run-ended-utc" "$HERE/.last-run-args"
    echo "[teardown] OK"
}

cmd_all() {
    cmd_deploy "$@"
    cmd_setup
    cmd_verify
    cmd_run
    cmd_collect
}

cmd_ssh() {
    require_tools jq ssh
    require_topology_json
    local target="${1:-}"
    local key_path ip num_hosts
    key_path=$(jq -r '.ssh_key_path' "$TOPOLOGY_JSON")
    num_hosts=$(jq -r '.num_subscriber_hosts' "$TOPOLOGY_JSON")
    case "$target" in
        sender)
            ip=$(jq -r '.sender.primary_ip' "$TOPOLOGY_JSON")
            ;;
        subscriber-[0-9]*)
            local idx="${target#subscriber-}"
            if [ "$idx" -ge "$num_hosts" ]; then
                err "subscriber index $idx out of range (have $num_hosts hosts)"; exit 2
            fi
            ip=$(jq -r ".subscriber_hosts[$idx].primary_ip" "$TOPOLOGY_JSON")
            ;;
        *)
            err "usage: orchestrate.sh ssh sender | subscriber-<i> (i=0..$((num_hosts-1)))"; exit 2
            ;;
    esac
    exec ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -i "$key_path" ec2-user@"$ip"
}

# ---- entry point -----------------------------------------------------

if [ $# -eq 0 ]; then usage; exit 2; fi
SUBCMD="$1"; shift
case "$SUBCMD" in
    bake-ami)  cmd_bake_ami "$@" ;;
    deploy)    cmd_deploy "$@" ;;
    setup)     cmd_setup "$@" ;;
    verify)    cmd_verify "$@" ;;
    run)       cmd_run "$@" ;;
    collect)   cmd_collect "$@" ;;
    teardown)  cmd_teardown "$@" ;;
    all)       cmd_all "$@" ;;
    ssh)       cmd_ssh "$@" ;;
    -h|--help) usage; exit 0 ;;
    *) err "unknown subcommand: $SUBCMD"; usage; exit 2 ;;
esac
