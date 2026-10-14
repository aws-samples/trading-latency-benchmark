# Clock Bound Measure

Automated deployment and measurement of [ClockBound](https://github.com/aws/clock-bound) clock error bounds across EC2 instances, comparing PTP (Precision Time Protocol) vs NTP (Network Time Protocol) time synchronization.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  CDK Stack: ClockBoundMeasureStack                               │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  VPC (10.60.0.0/16) — 2 AZs                                │  │
│  │                                                            │  │
│  │  ┌─────────────────────┐    ┌─────────────────────┐        │  │
│  │  │  Instance 1 (PTPG)  │    │  Instance 2         │        │  │
│  │  │  (Gen7+, PTP)       │    │  (configurable type)│        │  │
│  │  │                     │    │                     │        │  │
│  │  │  ENA + PHC detection│    │  ENA + PHC detection│        │  │
│  │  │  ClockBound daemon  │    │  ClockBound daemon  │        │  │
│  │  │  Example client     │    │  Example client     │        │  │
│  │  └─────────────────────┘    └─────────────────────┘        │  │
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
   └── No PHC? → SYNC_MECHANISM=NTP
3. Install ClockBound daemon: build from source, disable chronyd, start clockbound
   (pinned to 3.0.0-beta.0; cargo build --release --features daemon; RPM install is a future TODO)
4. Install Rust toolchain (if not present)
5. Build example client (examples/client/rust)
6. CloudWatch publisher: pip install boto3, write and start clockbound-metrics.service
7. Write /opt/clock-bound/status.json
```

## ClockBound Integration

### What ClockBound Provides

ClockBound reports a **deterministic error bound** — not a statistical estimate:

```
[earliest, latest] = system_time ± clock_error_bound
```

True time is guaranteed to exist within this interval. This differs from standard deviation (which only gives probabilistic coverage).

### Pinned Version

`bootstrap.sh` builds ClockBound from the
[`3.0.0-beta.0`](https://github.com/aws/clock-bound/releases/tag/3.0.0-beta.0) release,
pinned to commit
[`526b8a8`](https://github.com/aws/clock-bound/commit/526b8a89d2a8b8ac461a3ab61cadc8c93d48a479)
(`CLOCKBOUND_REF`). All results below were measured on that commit. To try another
release, change `CLOCKBOUND_REF`; the example client binary name
(`clock-bound-vmclock-client-example`) and the shm1 protocol may differ between releases.

### ClockBound 3.0 Feed-Forward Algorithm

Version 3.0.0 replaces chrony-based synchronization with a built-in feed-forward clock discipline:

- **Auto-detects** available time sources (PHC, NTP, VMClock)
- **Disciplines the system clock** directly — `chronyd` is disabled (and `Conflicts=` in the unit) so only one daemon steers `CLOCK_REALTIME`
- **Reports bounds via shared memory** — 13-14M reads/sec, no syscall overhead
- **Accounts for TSC drift** between samples in the reported bound

### PTP vs NTP — Measured Results

| Instance Type | Sync | ClockBound | Throughput |
|---------------|------|------------|------------|
| m7i.4xlarge (PTPG) | PTP | ±30–36µs | ~14M calls/sec |
| m5.4xlarge | NTP | ±677–691µs | ~11M calls/sec |

PTP delivers **~20x tighter bounds** than NTP. Measured 2026-10-14 in us-east-1 with
`chronyd` disabled; the placement group made no measurable difference to the bound.

### PTP-Capable Instance Families

PHC support requires:
- **Instance families:** M7a, M7g, M7i, R7a, R7g, R7i, I8g
- **ENA driver:** ≥ 2.10.0
- **Supported regions:** us-east-1, us-east-2, us-west-2, ap-northeast-1, eu-north-1, and others

The bootstrap script auto-detects PHC availability at runtime — no configuration needed.

Instance 1 launches into a precision-time placement group (`Strategy: precision-time`),
so `InstanceType1` must be a Gen7+ precision-time-capable type (default `m7i.4xlarge`).

## Usage

### Prerequisites

- AWS CLI v2 with valid credentials
- AWS Session Manager plugin (`session-manager-plugin`)
- Python 3.9+
- Node.js (for CDK CLI)
- `cdk bootstrap` run once in the target account and region — the stack ships the
  metrics publisher as an S3 asset

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

### Continuous CloudWatch Metrics

`run_clock_bound.sh` gives a one-shot snapshot. Each instance also publishes
continuously, so degradation is visible over time instead of only when you happen
to run the script. The stack prints a `DashboardUrl` output, which
`run_clock_bound.sh` echoes after its summary table.

`clockbound-metrics.service` runs `/opt/clockbound_metrics.py`, which reads the
ClockBound protocol v3 shared memory segment at `/var/run/clockbound/shm1` once a
second and publishes one statistic set per minute:

| Metric | Namespace | Unit | Meaning |
|---|---|---|---|
| `ClockErrorBound` | `ClockBoundMeasure` | Microseconds | Segment `Bound` field, ns→µs |
| `ClockStatus` | `ClockBoundMeasure` | None | 0=Unknown, 1=Synchronized, 2=FreeRunning, 3=Disrupted |

Both carry a single `InstanceId` dimension. `SyncMechanism` is deliberately not a
dimension: CloudWatch alarms require an exact dimension set and cannot alarm on
`SEARCH()`, and PTP-vs-NTP is not resolved until the instance boots.

These are ClockBound's own numbers — ClockBound 3.x already folds the Nitro
`phc_error_bound` sysfs value into its bound, and models feed-forward drift
between updates.

Four alarms, all evaluating one 60-second period on `Maximum`:

- `ClockErrorBound` per instance against `ClockErrorBoundThreshold1` / `2`
  (defaults 200µs and 2000µs, matching a PTP / NTP instance pairing)
- `ClockStatus >= 2` per instance, catching `FreeRunning` and `Disrupted`

All four treat missing data as breaching. When the publisher cannot get a valid
bound — segment absent, torn read, or past its `Void-After` timestamp — it emits
nothing rather than a fabricated value, so a gap means "bound unknown". A gap also
occurs when an instance is stopped or the stack is destroyed.

No SNS topic is created; alarm state is visible on the dashboard and in the console.

Check the parser without deploying anything:

```bash
python3 user_data/clockbound_metrics.py --selftest
```

Publisher logs on an instance:

```bash
journalctl -u clockbound-metrics -n 50
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
├── requirements.txt            # Python deps (aws-cdk-lib, boto3, constructs)
├── stacks/
│   ├── __init__.py
│   └── clock_bound_stack.py    # Stack: VPC, EC2, IAM, SG, alarms, dashboard
├── user_data/
│   ├── bootstrap.sh            # Instance bootstrap (PTP, ClockBound, Rust)
│   └── clockbound_metrics.py   # shm1 reader → CloudWatch (--selftest)
└── scripts/
    ├── run_clock_bound.sh      # Query instances via SSM
    ├── e2e_test.sh             # Deploy + query + optional cleanup
    └── demo_event_ordering.sh  # Event ordering visualization
```

## References

- [ClockBound GitHub](https://github.com/aws/clock-bound)
- [ClockBound shared memory protocol](https://github.com/aws/clock-bound/blob/main/docs/protocol.md) — the v3 segment layout parsed by `clockbound_metrics.py`
- [AWS Blog: Manage EC2 Clock Accuracy with Time Sync and CloudWatch (Part 2)](https://aws.amazon.com/blogs/mt/manage-amazon-ec2-instance-clock-accuracy-using-amazon-time-sync-service-and-amazon-cloudwatch-part-2/) — predates ClockBound and derives the bound from `chronyc tracking`; this stack publishes ClockBound's own bound instead
- [AWS Blog: Microsecond-Accurate Clocks on EC2](https://aws.amazon.com/blogs/compute/its-about-time-microsecond-accurate-clocks-on-amazon-ec2-instances/)
- [EC2 User Guide: Configure Amazon Time Sync Service](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/configure-ec2-ntp.html)
- [EC2 User Guide: Compare Timestamps with ClockBound](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/compare-timestamps-with-clockbound.html)
- [ENA Driver PHC Support](https://github.com/amzn/amzn-drivers/tree/master/kernel/linux/ena)
