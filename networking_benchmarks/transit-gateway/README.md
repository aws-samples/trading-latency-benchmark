# `transit-gateway/` — TGW Multicast Benchmark CDK App

Self-contained AWS CDK (Python) app that provisions an environment to measure
**AWS Transit Gateway multicast** one-way latency, packet loss, and throughput.
Copy this folder into another project and it works given AWS credentials and a
CDK bootstrap.

---

## Table of contents

- [What this stack builds](#what-this-stack-builds)
- [Architecture](#architecture)
- [Deployment flow](#deployment-flow)
- [Runtime / measurement flow](#runtime--measurement-flow)
- [CDK context parameters](#cdk-context-parameters)
- [Placement strategies](#placement-strategies)
- [Instance-type gating (PHC requirement)](#instance-type-gating-phc-requirement)
- [User-data bootstrap](#user-data-bootstrap)
- [Stack outputs](#stack-outputs)
- [Setup, deploy and teardown](#setup-deploy-and-teardown)
- [Results](#results)
- [Test setup](#test-setup)
- [Repository layout](#repository-layout)

---

## What this stack builds

Single CloudFormation stack `TgwMulticastBenchmark`:

| Layer         | Resource(s)                                                                 |
|---------------|-----------------------------------------------------------------------------|
| Networking    | VPC `10.0.0.0/16`, IGW, public RT, 1–3 public subnets, VPC flow log → CW Logs |
| Transit GW    | TGW (multicast on), VPC attachment, multicast domain (IGMPv2 **off**, static sources **on**), per-subnet associations |
| Compute       | 1 publisher + N subscribers (default 3, max 256), each with own ENI, gp3 encrypted root |
| Placement     | Cluster placement group (only `single-az-cpg`)                              |
| Multicast     | Publisher ENI = source; each subscriber ENI = member                        |
| IAM           | `AmazonSSMManagedInstanceCore` + `s3:PutObject` (results bucket) + `cloudwatch:PutMetricData` |
| Storage       | Results S3 bucket, `aws:SecureTransport` deny policy                        |
| Security      | One SG: inbound UDP/`<multicast_port>` + TCP/22 from `0.0.0.0/0`, all egress |
| Observability | VPC flow log (7-day retention), user-data log to `/var/log/benchmark-setup.log` |
| Compliance    | `cdk-nag` `AwsSolutionsChecks` aspect at synth time, suppressions inline    |

---

## Architecture

```
                            ┌─────────────────────────────────────────────┐
                            │                Transit Gateway              │
                            │      (multicast_support = "enable")          │
                            │                                             │
                            │   Multicast Domain                          │
                            │   ├─ Igmpv2Support       = disable           │
                            │   └─ StaticSourcesSupport = enable           │
                            │                                             │
                            │   group 239.1.1.1                            │
                            │   ├─ source : Publisher ENI                  │
                            │   └─ members: Subscriber-0..N ENIs           │
                            └──────────────▲──────────────▲───────────────┘
                                           │              │
                          TGW VPC attachment (subnets)    │
                                           │              │
        ┌──────────────────────────────────┴──────────────┴───────────────────┐
        │                              VPC 10.0.0.0/16                        │
        │                                                                     │
        │   ┌────────── Subnet(s) (1 for single-az*, 3 for cross-az) ──────┐  │
        │   │                                                              │  │
        │   │   ┌────────────┐        UDP multicast        ┌────────────┐  │  │
        │   │   │ Publisher  │ ─────────► TGW ───────────► │ Subscriber │  │  │
        │   │   │  EC2 + ENI │   send (mcast_send.py)      │  EC2 + ENI │  │  │
        │   │   └─────┬──────┘                              └─────┬──────┘  │  │
        │   │         │                                           │         │  │
        │   │         │       Phase B: unicast UDP (RTT)          │         │  │
        │   │         └───────────────────────────────────────────┘         │  │
        │   │                                                              │  │
        │   └──────────────────────────────────────────────────────────────┘  │
        │                                                                     │
        │   IGW ──► default route 0.0.0.0/0   (public subnets, public IPs)     │
        └─────────────────────────────────────────────────────────────────────┘
                  │                                          │
                  ▼                                          ▼
            SSM Run Command                      S3 results bucket  +  CloudWatch metrics
            (orchestration plane)                (TLS-only,          (TGWMulticastBenchmark
                                                  scripts/         namespace, written by
                                                  collect_results.py) collect_results.py)
```

Two independent paths between publisher and subscribers: **multicast through
the TGW** (the path under measurement; subnets don't bridge multicast) and
**direct VPC unicast** (used as the baseline that gets subtracted). Control
plane is out-of-band over SSM and AWS APIs.

---

## Deployment flow

`cdk synth/deploy` reads context from `app.py` and runs `BenchmarkStack`,
which provisions resources in this order:

1. Validate instance family (PHC gate, fail-fast)
2. VPC + flow log + IGW + RT
3. Subnets per `placement_strategy` (1 or 3) + cluster PG (cpg only)
4. Security group
5. TGW + multicast domain + VPC attachment + per-subnet associations
6. Validate `num_subscribers ≤ 256`
7. AMI lookup (AL2023 unless `base_ami` given)
8. Results S3 bucket + TLS-only policy
9. EC2 IAM role + instance profile + user-data
10. 1× publisher + N× subscriber EC2/ENI
11. Multicast group source + N members (**explicit `add_dependency` on
    domain associations** — avoids a known race in
    `CreateTransitGatewayMulticastGroupSource`)
12. CfnOutputs + `cdk-nag` suppressions

`AwsSolutionsChecks` runs at synth. EC2 user-data executes after deploy — see
[User-data bootstrap](#user-data-bootstrap).

---

## Runtime / measurement flow

After `/opt/benchmark-env-ready` exists on every host, `scripts/run_benchmark.sh`
drives measurement via SSM:

```
  run_benchmark.sh
         │
         │  (1) describe-stacks  → publisher id, subscriber ids, S3 bucket
         ▼
  SSM Run Command
         │
         │  (2) wait for SSM-ready & /opt/benchmark-env-ready on all hosts
         ▼
  Phase A — Multicast one-way latency + loss (TGW path)
         │     · upload mcast_send.py / mcast_recv.py
         │     · subscribers: mcast_recv.py joins 239.1.1.1, opens HW RX
         │       timestamping (SO_TIMESTAMPING + SIOCSHWTSTAMP) on the PHC
         │     · publisher : mcast_send.py emits sequenced UDP packets with
         │       embedded send-side timestamps at --rate pps
         │     · receiver computes (rx_ts - tx_ts) per packet → one-way latency
         │     · aggregates min/median/mean/p95/p99 + packet loss
         ▼
  Phase B — Unicast RTT baseline (direct VPC, no TGW)
         │     · sockperf under-load from publisher → each subscriber
         │     · prints avg / min / p50 / p99 RTT
         ▼
  stdout (per-subscriber blocks)
         │
         │  pipe / save → scripts/collect_results.py
         ▼
  collect_results.py
         · parses each block
         · computes aggregate stats
         · writes JSON report to S3 (falls back to ./benchmark-results/)
         · publishes MeanLatencyUs / P99LatencyUs / PacketLossPct
           to CloudWatch namespace TGWMulticastBenchmark
```

TGW overhead ≈ `multicast_one_way − unicast_RTT/2`.

---

## CDK context parameters

Pass via `-c key=value` on `cdk synth/deploy`. Defaults in `app.py`.

| Parameter            | Type   | Default            | Notes                                                          |
|----------------------|--------|--------------------|----------------------------------------------------------------|
| `num_subscribers`    | int    | `3`                | Max **256** (TGW group member limit). Synth-time `ValueError` above. |
| `instance_type`      | string | `m7i.large`        | Must be PHC-capable — see below.                              |
| `multicast_group`    | string | `239.1.1.1`        | IPv4 group address.                                            |
| `multicast_port`     | int    | `5001`             | UDP port; opened in SG.                                        |
| `s3_bucket_name`     | string | *(auto)*           | Optional explicit results-bucket name.                         |
| `placement_strategy` | string | `single-az-cpg`    | `single-az-cpg` \| `single-az` \| `cross-az`. Else `ValueError`. |
| `base_ami`           | string | *(latest AL2023)*  | Optional override; resolved at synth otherwise.               |

```bash
npx cdk deploy \
  -c num_subscribers=10 \
  -c instance_type=c7i.large \
  -c placement_strategy=cross-az \
  -c multicast_port=9999
```

---

## Placement strategies

| Strategy        | Subnets          | Placement group | Intent                                                 |
|-----------------|------------------|-----------------|--------------------------------------------------------|
| `single-az-cpg` | 1 / 1 AZ         | Cluster         | Lowest baseline; isolates pure TGW overhead.           |
| `single-az`     | 1 / 1 AZ         | None            | Quantifies the cluster-PG benefit.                     |
| `cross-az`      | 3 / 3 AZs        | None            | Adds cross-AZ propagation. Subscribers round-robin: `i % 3`. Publisher always in `subnets[0]`. |

---

## Instance-type gating (PHC requirement)

The receiver takes hardware RX timestamps via `SO_TIMESTAMPING` +
`SIOCSHWTSTAMP` against the **ENA PTP Hardware Clock (PHC)**. Without PHC,
software timestamps add microsecond-class noise that swamps the signal.

`BenchmarkStack._validate_phc_instance()` enforces this at synth. Allowed
family prefixes:

```
m7, c7, r7, i7, i8, c8, m8, r8, x8
```

Examples accepted: `m7i`, `m7g`, `m7a`, `c7i`, `c7g`, `r7i`, `r7iz`, `i8g`,
`c8g`, `m8g`, `x8g`. Examples rejected: `c6in`, `m6i`, `m5`, `c5n`, `t3`.

To bypass, edit `PHC_FAMILY_PREFIXES` in `stacks/benchmark_stack.py`.

---

## User-data bootstrap

Same user-data on publisher and subscribers. Logs to
`/var/log/benchmark-setup.log`.

| Phase | What it does                                                                                                                         |
|------:|---------------------------------------------------------------------------------------------------------------------------------------|
| **0** | Sysctl: `rmem_max=wmem_max=26214400`, `rp_filter=0`, `busy_poll=busy_read=50`.                                                       |
| **1** | `yum install` iperf3 + toolchain; build sockperf from `Mellanox/sockperf` to `/usr/local`.                                           |
| **2** | Load `ptp` module, write `options ena phc_enable=1`, reload ENA driver, find `ena-ptp` PHC, create `/dev/ptp_ena` via udev, configure `chrony` (`refclock PHC /dev/ptp_ena prefer`). Falls back to NTP via `169.254.169.123` if no PHC. |
| **2b**| `ethtool -T <iface>` for diagnostic.                                                                                                  |
| **3** | `touch /opt/benchmark-env-ready` — gate polled by `run_benchmark.sh`.                                                                |

Phase 1 runs before Phase 2 so the ENA reload's brief network drop can't starve `yum`.

---

## Stack outputs

Used by `run_benchmark.sh` (`describe-stacks`):

| Output                  | Description                                  |
|-------------------------|----------------------------------------------|
| `PublisherInstanceId`   | Publisher EC2 instance ID.                   |
| `SubscriberInstanceIds` | Comma-separated subscriber instance IDs.     |
| `S3BucketName`          | Results bucket (auto-generated unless overridden). |
| `MulticastGroup`        | Multicast group address.                     |
| `MulticastPort`         | UDP port.                                    |

---

## Setup, deploy and teardown

```bash
cd transit-gateway

# One-time
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt -r requirements-dev.txt
npx cdk bootstrap

# Synth / deploy
npx cdk synth
npx cdk deploy
npx cdk deploy -c num_subscribers=5 -c placement_strategy=single-az-cpg -c instance_type=m7i.4xlarge

# Diagnose PTP
scripts/check_ptp.sh --stack-name TgwMulticastBenchmark

# Run
scripts/run_benchmark.sh --stack-name TgwMulticastBenchmark \
  --rate 1000 --duration 60 --instance-type m7i.large --save-report

# Tear down
npx cdk destroy
```

Runtime deps (`requirements.txt`): `aws-cdk-lib`, `constructs`, `cdk-nag`.
Dev deps (`requirements-dev.txt`): `pytest`, `hypothesis`.

---

## Results

Each invocation of `scripts/run_benchmark.sh` produces results in **three
places**, depending on how it was invoked:

### 1. stdout (always)

`run_benchmark.sh` streams a per-phase log followed by one JSON blob per
subscriber for Phase A and a sockperf block per subscriber for Phase B. Pipe
or save it to a file; this is the canonical raw measurement.

Phase A line per subscriber (multicast one-way latency):

```text
i-0abc...  multicast loss: {"iface":"enp39s0",
  "so_timestamping_variant":"SO_TIMESTAMPING_NEW",
  "hw_timestamping_enabled":true,
  "total_expected":59985, "total_received":59985, "packet_loss_count":0,
  "hw_timestamp_count":59986, "end_marker_received":true,
  "min_latency_us":140.9, "max_latency_us":4675.6, "mean_latency_us":162.9,
  "median_latency_us":153.0, "p95_latency_us":171.8, "p99_latency_us":223.2}
```

`min/median/mean/p95/p99/max_latency_us` are the **TGW one-way latency**
distribution measured as `rx_phc_hw_ts (subscriber NIC) − tx_clock_realtime
(publisher user-space)` over the run window — see
[Measurement caveats](#measurement-caveats) for what biases this includes.

Phase B per-subscriber sockperf block contains `avg-rtt`, `min`, `p50`, `p99`
in microseconds — round-trip unicast, **no TGW in the path** (used as a
baseline to subtract).

### 2. `runs/` (if `--save-report`)

Passing `--save-report` invokes `save_report.py`, which writes two files into
`runs/<role>_<ami>_<UTC-timestamp>.{md,raw.txt}`:

| File         | Contents                                                                           |
|--------------|------------------------------------------------------------------------------------|
| `*.md`       | Markdown report with run config + Phase A / Phase B tables (per-subscriber and aggregate). Human-readable; tracked by git for run-to-run comparison. |
| `*.raw.txt`  | Verbatim copy of stdout for the run. Re-parseable by `save_report.py` / `collect_results.py` / `plot_runs.py`. |

These files are **machine-local** — they don't leave your workstation unless
you commit them to the repo or upload them yourself. `runs/.gitkeep` is
checked in; saved reports are not committed by default.

### 3. S3 + CloudWatch (if you pipe stdout into `collect_results.py`)

`collect_results.py` parses the raw stdout, computes aggregates, and:

- **Uploads** a JSON report to `s3://<S3BucketName>/<prefix>/<run-id>.json`
  (`<S3BucketName>` is the CloudFormation output; `<prefix>` defaults to
  `reports/`). Falls back to `./benchmark-results/` if the upload fails or no
  bucket is given.
- **Publishes** three CloudWatch metrics to the **`TGWMulticastBenchmark`**
  namespace (configurable via `--cw-namespace`):

  | Metric            | Unit         | Source                         |
  |-------------------|--------------|--------------------------------|
  | `MeanLatencyUs`   | microseconds | aggregate Phase A mean         |
  | `P99LatencyUs`    | microseconds | aggregate Phase A p99          |
  | `PacketLossPct`   | percent      | summed loss / summed expected  |

> **Aggregation note:** `collect_results.py` currently averages
> per-subscriber percentiles (e.g., `mean(per_sub_p99)`), not the percentile
> of the pooled distribution. With `N>1` subscribers this **underestimates
> the tail**; inspect the per-subscriber JSON in stdout / `*.raw.txt` for the
> true distribution shape.

### Quick comparison plotting

`scripts/plot_runs.py` parses one or more `*.raw.txt` files (or any captured
stdout) and produces a 4-panel matplotlib figure (median / p99 / max / loss)
with one bar per (subscriber, run):

```bash
python3 scripts/plot_runs.py runs/run1.raw.txt runs/run2.raw.txt runs/run3.raw.txt \
  --out plot.png --title "m7i.4xlarge × 6 subs, single-az-cpg"
```

---

## Test setup

All tests in `tests/` are pure CDK synthesis tests — no AWS calls. They build
a `Template` from synthesized JSON and assert on it.

```bash
cd transit-gateway && source .venv/bin/activate
python -m pytest tests/ -v
```

Coverage:

- TGW: `MulticastSupport=enable`.
- Multicast domain: `Igmpv2Support=disable`, `StaticSourcesSupport=enable`
  (PascalCase enforced via `add_property_override`).
- SG: inbound UDP/`<multicast_port>`, TCP/22, all egress.
- User-data substrings: `chrony`, `/dev/ptp_ena`, `phc_enable=1`, `ethtool -T`,
  `sockperf`, `iperf3`.
- PHC gate: parametrized accept/reject lists.
- `num_subscribers=257` → `ValueError` matching `256`.
- `base_ami` used verbatim.
- Placement: subnet/PG counts, invalid-strategy rejection.
- Hypothesis property test: `num_subscribers ∈ [1,10]` → exactly N subscriber
  instances + N TGW members + N+1 total instances.

Orchestration scripts have their own property tests under `scripts/tests/`.

---

## Repository layout

```
transit-gateway/
├── app.py                                  CDK entry point; reads context, applies AwsSolutionsChecks
├── cdk.json
├── requirements.txt / requirements-dev.txt
├── stacks/
│   └── benchmark_stack.py                  VPC, TGW, multicast, EC2, IAM, S3, user-data, PHC gate
├── scripts/
│   ├── run_benchmark.sh                    Orchestrator: Phase A + B via SSM
│   ├── check_ptp.sh                        PTP pre-flight diagnostic
│   ├── mcast_recv.py                       Receiver, NIC HW RX timestamping
│   ├── mcast_send.py                       Sender, sequenced UDP + embedded TX ts
│   ├── collect_results.py                  Aggregate → S3 + CloudWatch
│   ├── save_report.py                      Markdown report → runs/
│   ├── latency_breakdown.py                Three-mode latency decomposition
│   ├── run_latency_breakdown.sh
│   └── tests/
│       ├── test_mcast_recv_smoke.py
│       └── test_property_aggregate_stats.py
├── runs/                                   Saved benchmark reports
├── tests/
│   ├── test_benchmark_stack.py
│   ├── test_placement_strategy.py
│   └── test_property_subscriber_count.py
└── cdk.out/                                Synth artifacts (generated)
```
