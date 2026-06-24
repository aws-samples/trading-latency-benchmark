# mcast2ucast Automated Benchmark Harness

End-to-end automation for the mcast2ucast latency benchmark on AWS EC2.

## Prerequisites (operator workstation)

- AWS CLI v2 configured with credentials for the target account
- `jq`, `ssh`, `scp`
- `npx` (Node.js) — the `aws-cdk@2.1125.0` CLI is fetched on demand
- `python3` >= 3.10
- `shellcheck` (only required for development / linting)
- An existing EC2 keypair in the target region; PEM at `~/.ssh/<keypair>.pem`
- A subnet ID and a security group (in any VPC) — used by `bake-ami` only. The
  security group **must allow inbound SSH (tcp/22) from your workstation's public
  IP** (`<your-ip>/32`, or a CIDR that covers it); otherwise the bake hangs waiting
  for SSH and fails. See *Discovering resource IDs* below to find/verify the rule.

### Discovering resource IDs

All commands take `--region <r>` (set it to your target region):

```bash
# Base AMI — current AL2023 x86_64 (needed for bake-ami)
aws ssm get-parameter --region us-east-1 \
    --name /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64 \
    --query 'Parameter.Value' --output text

# Keypairs (the --key value; PEM must be at ~/.ssh/<keypair>.pem)
aws ec2 describe-key-pairs --region us-east-1 --query 'KeyPairs[].KeyName' --output text

# Default VPC
aws ec2 describe-vpcs --region us-east-1 \
    --filters Name=isDefault,Values=true --query 'Vpcs[].VpcId' --output text

# Public subnets in the default VPC (pick one — bake-ami needs a public IP)
aws ec2 describe-subnets --region us-east-1 --filters Name=default-for-az,Values=true \
    --query 'Subnets[].{Subnet:SubnetId,AZ:AvailabilityZone,Public:MapPublicIpOnLaunch}' --output table

# Security groups (list candidates)
aws ec2 describe-security-groups --region us-east-1 \
    --query 'SecurityGroups[].{ID:GroupId,Name:GroupName,VPC:VpcId}' --output table

# Your public IP — the SG must allow tcp/22 from this address
curl -s https://checkip.amazonaws.com

# Inspect a group's SSH ingress: confirm a tcp/22 rule whose CidrIp covers your IP
aws ec2 describe-security-groups --region us-east-1 --group-ids sg-XXXXXXXX \
    --query 'SecurityGroups[0].IpPermissions[?FromPort==`22`]' --output json
```

If no security group allows SSH from your IP, add an ingress rule (tcp/22 from `<your-ip>/32`):

```bash
aws ec2 authorize-security-group-ingress --region us-east-1 \
    --group-id sg-XXXXXXXX --protocol tcp --port 22 --cidr "$(curl -s https://checkip.amazonaws.com)/32"
```

## One-time: bake the AMI

`--base-ami` must be a stock **Amazon Linux 2023 (AL2023) x86_64** AMI. IDs are
region-specific and rotate per release — fetch the current one from SSM:

```bash
aws ssm get-parameter --region us-east-1 \
    --name /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64 \
    --query 'Parameter.Value' --output text
```

```bash
./tools/bake_ami.sh \
    --base-ami ami-XXXXXXXX  \
    --key my-keypair \
    --subnet-id subnet-XXXXXXXX \
    --security-group-id sg-XXXXXXXX \
    --region us-east-1
```

The last line of stdout is the new AMI ID. Use it for `--ami-id` below. The
AMI bakes `tune_os.sh --grub` so the per-run instances boot with the right
kernel cmdline (isolcpus, idle=poll, etc.).

After modifying any of the on-instance scripts (`bootstrap.sh`,
`tune_os.sh`, `setup_ena_bypass.sh`, `end_to_end_ptp.sh`) or the benchmark
binaries (`latency_sender`, `latency_receiver`), re-run `bake_ami.sh` to
produce a fresh AMI.

## Run a benchmark

```bash
./orchestrate.sh deploy \
    --topology cpg --sender-type m8a.2xlarge --receiver-type m8a.2xlarge \
    --ami-id ami-XXXXXXXX --key my-keypair
./orchestrate.sh setup
./orchestrate.sh verify
./orchestrate.sh run --pps 1000 --count 1000 --payload 64
./orchestrate.sh collect
./orchestrate.sh teardown --yes
```

Or all-in-one (no teardown):

```bash
./orchestrate.sh all \
    --topology cpg --sender-type m8a.2xlarge --receiver-type m8a.2xlarge \
    --ami-id ami-XXXXXXXX --key my-keypair
```

Per-run output lands in `results/<UTC-timestamp>/`:
- `sender-stdout.log` — sender progress
- `recv-h<i>-p<port>.log` — receiver percentile reports (one per daemon × host)
- `recv-h<i>-p<port>.csv` — per-packet `seq,rx_ns` (used by analyze_spread.py)
- `sub<i>-mcast2ucast.log` — daemon logs (post-mortem)
- `topology.json` — what was deployed
- `summary.json` — parsed percentiles + run metadata + fan-out spread stats

## Scaling subscriber count

The default `--num-subscriber-hosts 1` gives a point-to-point single-subscriber
benchmark. Raise `--num-subscriber-hosts H` to measure how latency scales as the
sender fans out to more hosts.

H is bounded 1..32. Each subscriber host is a separate EC2 instance with its
own secondary ENI; the sender unicasts one copy of each packet to each host.

**Sweep to plot latency vs subscriber count:**

```bash
for H in 1 2 4 6 8 10; do
  ./orchestrate.sh deploy --topology cpg \
    --sender-type c7a.2xlarge --receiver-type c7a.2xlarge \
    --ami-id ami-XXXXXXXX --key my-keypair \
    --num-subscriber-hosts $H
  ./orchestrate.sh setup
  ./orchestrate.sh verify
  ./orchestrate.sh run --pps 1000 --count 1000
  ./orchestrate.sh collect
  ./orchestrate.sh teardown --yes
done
```

Or use `deploy/sweep.sh` which handles SSH readiness and benchmark rebuilds automatically:

```bash
./sweep.sh --ami-id ami-XXXXXXXX 1 2 4 6 8 10
```

Each `summary.json` contains a `fanout_spread_us` block with
`mean / std_dev / p50 / p90 / p99 / max` (in µs) plus a
`per_subscriber_loss[]` array. Aggregate the p50s across runs to plot vs
`num_subscriber_hosts`.

## Topology modes

| `--topology` | Placement |
|---|---|
| `cpg` | Same AZ, cluster placement group |
| `same-az` | Same AZ, no CPG |
| `multi-az` | Different AZs (`--sender-az` and `--receiver-az`) |

## Debugging

- `./orchestrate.sh ssh sender` (or `subscriber-<i>`) drops into the host
- `--keep-logs` on `run` skips post-run cleanup so you can inspect `/tmp/recv-*.log` and `/tmp/sender.log`
- The `verify` step is idempotent — re-run if chrony hasn't converged
- Verify exit codes: 40 = NIC capability missing, 41 = chronyd / Leap / parse, 42 = chrony offset above threshold

## Cost

A typical `c7a.2xlarge` × 2 (H=1), ~10-minute end-to-end run costs ~$0.40.
For an H=10 sweep at `c7a.2xlarge` (11 instances total), figure ~$2.20
per ten-minute cycle — about ~$22 for a 10-point sweep with teardown between every run.
