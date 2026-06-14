# Clock Bound Measure

Automated deployment and measurement of [ClockBound](https://github.com/aws/clock-bound) clock error bounds across EC2 instances, comparing PTP (Precision Time Protocol) vs NTP (Network Time Protocol) time synchronization.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  CDK Stack: ClockBoundMeasureStack                               │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  VPC (10.60.0.0/16) — 2 AZs                               │  │
│  │                                                            │  │
│  │  ┌─────────────────────┐    ┌─────────────────────┐       │  │
│  │  │  Instance 1          │    │  Instance 2          │       │  │
│  │  │  (configurable type) │    │  (configurable type) │       │  │
│  │  │                      │    │                      │       │  │
│  │  │  ENA + PHC detection │    │  ENA + PHC detection │       │  │
│  │  │  ClockBound daemon   │    │  ClockBound daemon   │       │  │
│  │  │  Example client      │    │  Example client      │       │  │
│  │  └─────────────────────┘    └─────────────────────┘       │  │
│  │                                                            │  │
│  │  NAT Gateway (outbound: GitHub, rustup)                    │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  IAM Role: AmazonSSMManagedInstanceCore (no SSH keys required)   │
│  Security Group: egress-only                                     │
└──────────────────────────────────────────────────────────────────┘

Local machine
  └── scripts/run_clock_bound.sh
        └── AWS SSM send-command → each instance
              ├── /opt/clock-bound/status.json  (sync mechanism)
              ├── systemctl status clockbound   (daemon health)
              └── clock-bound-client            (bounded timestamp)
```

### Instance Bootstrap Flow

On first boot, each instance executes `user_data/bootstrap.sh`:

```
1. Detect NIC interface and ENA driver
2. PTP Detection:
   ├── ENA driver? → enable phc_enable=1 (modprobe.d + reload)
   ├── /dev/ptpN exists? → SYNC_MECHANISM=PTP
   │   └── Configure chrony: refclock PHC /dev/ptpN poll 0 delay 0.000010 prefer
   └── No PHC? → SYNC_MECHANISM=NTP
3. Install ClockBound daemon:
   ├── Try RPM from GitHub releases
   └── Fallback: build from source (cargo build --release --features daemon)
4. Install Rust toolchain (if not present)
5. Build example client (examples/client/rust)
6. Write /opt/clock-bound/status.json
```

When PTP is detected, chrony is configured with the PHC as a preferred reference clock per [AWS documentation](https://aws.amazon.com/blogs/compute/its-about-time-microsecond-accurate-clocks-on-amazon-ec2-instances/):

```bash
refclock PHC /dev/ptp0 poll 0 delay 0.000010 prefer
```

This gives chrony direct PTP access with ±5µs delay accounting for OS latency, enabling the tightest possible synchronization.

## ClockBound Integration

### What ClockBound Provides

ClockBound reports a **deterministic error bound** — not a statistical estimate:

```
[earliest, latest] = system_time ± clock_error_bound
```

True time is guaranteed to exist within this interval. This differs from standard deviation (which only gives probabilistic coverage).

### ClockBound 3.0 Feed-Forward Algorithm

Version 3.0.0-alpha.1 replaces chrony-based synchronization with a built-in feed-forward clock discipline:

- **Auto-detects** available time sources (PHC, NTP, VMClock)
- **Disciplines the system clock** directly — no chrony dependency
- **Reports bounds via shared memory** — 13-14M reads/sec, no syscall overhead
- **More conservative bounds** than chrony+PHC formula — accounts for TSC drift between samples

### PTP vs NTP — Measured Results

| Instance Type | Sync | ClockBound | Chrony+PHC Formula | Throughput |
|---------------|------|------------|-------------------|------------|
| m7i.4xlarge | PTP | ±63–78µs | ±25–34µs | ~14M calls/sec |
| m5.4xlarge | NTP | ±1391–1448µs | N/A | ~11M calls/sec |
| c6in.4xlarge | NTP | ±1422–1744µs | N/A | ~13M calls/sec |

PTP delivers **~20x tighter bounds** than NTP. The Chrony+PHC formula confirms the AWS blog's "under 40µs" claim for PTP-enabled instances.

### PTP-Capable Instance Families

PHC support requires:
- **Instance families:** M7a, M7g, M7i, R7a, R7g, R7i, I8g
- **ENA driver:** ≥ 2.10.0
- **Supported regions:** us-east-1, us-east-2, us-west-2, ap-northeast-1, eu-north-1, and others

The bootstrap script auto-detects PHC availability at runtime — no configuration needed.

## Usage

### Prerequisites

- AWS CLI v2 with valid credentials
- AWS Session Manager plugin (`session-manager-plugin`)
- Python 3.9+
- Node.js (for CDK CLI)

### Deploy

```bash
cd networking_benchmarks/clock_bound_measure
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

# Deploy to specific region (PTP requires supported region)
npx cdk deploy -c region=us-east-1 \
    --parameters InstanceType1=m7i.4xlarge \
    --parameters InstanceType2=m5.4xlarge
```

### Query Results

```bash
# Wait for bootstrap (~5-7 min for Rust compilation), then:
./scripts/run_clock_bound.sh --region us-east-1

# With full daemon logs and client output:
./scripts/run_clock_bound.sh --region us-east-1 --verbose
```

### End-to-End Test

```bash
# Deploy, query, destroy in one command:
./scripts/e2e_test.sh --region us-east-1 \
    --instance-type-1 m7i.4xlarge \
    --instance-type-2 m5.4xlarge

# Keep stack running after test:
./scripts/e2e_test.sh --region us-east-1 \
    --instance-type-1 m7i.4xlarge \
    --instance-type-2 m5.4xlarge \
    --no-cleanup
```

### Event Ordering Demo

Demonstrates ClockBound's core value proposition: determining whether two events on different instances can be definitively ordered.

```bash
# Requires a running stack (use --no-cleanup above)
./scripts/demo_event_ordering.sh --region us-east-1
```

The demo measures actual clock error bounds from both instances, then simulates event ordering at increasing delays until the bounded timestamp intervals no longer overlap:

```
── Delay: 0µs (simultaneous) ────────────────────────────────
  A: [===]  ±72µs
  B: [============================================================]  ±1390µs
     ↑ OVERLAP ↑
  Verdict: AMBIGUOUS — cannot determine order

── Delay: 100µs ─────────────────────────────────────────────
  A: [==]  ±72µs
  B:     [========================================================]  ±1390µs
       ↑ GAP ↑
  Verdict: ORDERED — A happened before B
```

### Teardown

```bash
npx cdk destroy -c region=us-east-1
```

## Troubleshooting

### Understanding the Two Error Bound Measurements

The `run_clock_bound.sh` script reports two different error bounds:

| Column | Source | Typical PTP Value | What It Means |
|--------|--------|-------------------|---------------|
| **ClockBound** | ClockBound 3.0 daemon | ±63–78µs | Conservative bound including TSC drift modeling |
| **Chrony+PHC** | Blog formula | ±25–34µs | `SYSTEM_TIME + 0.5*ROOT_DELAY + ROOT_DISPERSION + PHC_ERROR_BOUND` |

ClockBound 3.0's feed-forward algorithm reports wider bounds because it models TSC frequency drift between PHC polling intervals — uncertainty that chrony ignores. Both are valid; ClockBound is more conservative.

To validate the chrony-based calculation manually:
```bash
# PHC error bound from Nitro sysfs (nanoseconds):
PCI=$(cat /sys/class/net/<iface>/device/uevent | grep PCI_SLOT_NAME | cut -d= -f2)
cat /sys/bus/pci/devices/$PCI/phc_error_bound

# Chrony tracking values (System time, Root delay, Root dispersion):
chronyc tracking
```

Reference: [AWS Blog — Measuring your clock accuracy](https://aws.amazon.com/blogs/compute/its-about-time-microsecond-accurate-clocks-on-amazon-ec2-instances/)

### PTP Not Detected on Expected Instance Type

**Problem:** Instance reports `sync_mechanism: NTP` despite being a PTP-capable type.

**Checks:**
```bash
# Verify ENA PHC is enabled:
cat /sys/module/ena/parameters/phc_enable    # Should be 1

# Check for PTP device:
ethtool -T <interface> | grep "PTP Hardware Clock"
ls /dev/ptp*

# Verify ENA driver version (needs ≥ 2.10.0):
modinfo ena | grep version

# Verify chrony is using PHC:
chronyc sources    # Should show #* PHC0
```

**Common causes:**
- Instance type not in supported family (M7a/M7g/M7i/R7a/R7g/R7i/I8g)
- Region doesn't support PHC
- ENA driver too old (AL2023 ships 2.16.x which supports PHC)
- `phc_enable` parameter didn't persist after ENA module reload

### Bootstrap Script Failed

**Problem:** `/opt/clock-bound/status.json` never appears.

**Debug:**
```bash
# Check bootstrap log:
aws ssm send-command --instance-ids <id> --region <region> \
    --document-name "AWS-RunShellScript" \
    --parameters 'commands=["cat /var/log/clock-bound-bootstrap.log | tail -50"]'
```

**Common failures:**
- `dnf` package cache corruption — fixed with `dnf clean all` before install
- `source /root/.cargo/env` fails — requires `export HOME=/root` (cloud-init has minimal env)
- Cargo package name: daemon binary is `clock-bound` with `--features daemon`, producing `clockbound`
- Example client path: `examples/client/rust`, producing `clock-bound-vmclock-client-example`

### SSM Commands Return Empty Output

**Problem:** `run_clock_bound.sh` shows `UNKNOWN` sync and `N/A` for clock error bound.

**Cause:** The clock-bound-client binary runs 100M iterations (~7 seconds). If the SSM command hasn't completed when results are fetched, output is empty.

**Fix:** The script polls for command completion (up to 60s). If still failing:
```bash
# Manually check command status:
aws ssm get-command-invocation --command-id <id> --instance-id <id> --query Status
```

### Capacity Errors During Deployment

**Problem:** `InsufficientInstanceCapacity` for requested instance type in the target AZ.

**Solutions:**
- The stack uses `max_azs=2` to spread across availability zones
- Try a different instance size (e.g., `m7i.2xlarge` instead of `m7i.4xlarge`)
- Deploy to a different region

## Project Structure

```
clock_bound_measure/
├── app.py                      # CDK app entry point
├── cdk.json                    # CDK configuration
├── requirements.txt            # Python deps (aws-cdk-lib, constructs)
├── stacks/
│   ├── __init__.py
│   └── clock_bound_stack.py    # Stack: VPC, EC2, IAM, SG
├── user_data/
│   └── bootstrap.sh            # Instance bootstrap (PTP, ClockBound, Rust)
└── scripts/
    ├── run_clock_bound.sh      # Query instances via SSM
    ├── e2e_test.sh             # Deploy + query + optional cleanup
    └── demo_event_ordering.sh  # Event ordering visualization
```

## References

- [ClockBound GitHub](https://github.com/aws/clock-bound)
- [AWS Blog: Microsecond-Accurate Clocks on EC2](https://aws.amazon.com/blogs/compute/its-about-time-microsecond-accurate-clocks-on-amazon-ec2-instances/)
- [EC2 User Guide: Configure Amazon Time Sync Service](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/configure-ec2-ntp.html)
- [EC2 User Guide: Compare Timestamps with ClockBound](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/compare-timestamps-with-clockbound.html)
- [ENA Driver PHC Support](https://github.com/amzn/amzn-drivers/tree/master/kernel/linux/ena)
