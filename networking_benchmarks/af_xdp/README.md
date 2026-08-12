# AF_XDP Network Latency Benchmark

High-performance network latency measurement suite using AF_XDP kernel bypass.
Measures round-trip and one-way latency between EC2 instances at microsecond precision.

## Structure

```
af_xdp/
├── src/            Core replicator engine (AF_XDP + echo-mode) + eBPF (ucast.o/mcast.o)
├── tools/          Measurement instruments (rtt, mcast_send/receive, replicator_ctl, udp_send)
├── deploy/         Infrastructure (CDK stacks + benchmark/runtime Ansible playbooks)
│   ├── cdk/        Fleet deployment + AMI builder
│   └── ansible/    run_ucast / run_mcast / configure_mcast + inventory
├── report/         Topology report — gen/ (report.py heatmap + fleet_json.py → fleet.json) + web/ (Vite + Svelte + three.js, 2D/3D)
├── tests/          pytest integration suite (echo-mode; run from the af_xdp root)
├── dev/            Dev tooling: Docker build harness (dev/Dockerfile) + sync/provision playbooks
└── Makefile        Build system (all, echo-mode, full, mcast targets)
```

## Quick Start

```bash
# Build (on EC2 with libxdp installed)
make full

# Build without libxdp (containers, CI, macOS cross-compile check)
make echo-mode

# Run tests
pip install pytest && pytest -v
```

## Documentation

| Directory | README | Description |
|-----------|--------|-------------|
| [`src/`](src/README.md) | Architecture, control protocol, build modes | Core C++ replicator + eBPF XDP programs |
| [`tools/`](tools/README.md) | Usage, CLI flags, timestamp modes | RTT client, multicast tools, control CLI |
| [`tests/`](tests/README.md) | Integration suite, test classes, fixtures | 45 echo-mode integration tests |
| [`dev/`](dev/README.md) | Dev tooling: Docker harness, sync/provision | Local build+test, fleet hot-deploy |
| [`deploy/`](deploy/README.md) | Deployment flows, instance roles | CDK + Ansible orchestration |
| [`deploy/cdk/`](deploy/cdk/README.md) | Fleet spec, scenarios, parameters | Infrastructure as code |
| [`deploy/ansible/`](deploy/ansible/README.md) | Playbooks, inventory, variables | Runtime provisioning |

## Modes

| Mode | Command | Root? | Use case |
|------|---------|:-----:|----------|
| AF_XDP (full) | `sudo replicator eth0 <ip> <port>` | Yes | Production — zero-copy, ~32µs p50 |
| AF_XDP + m2u | `sudo replicator eth0 <mcast> <port> --mcast` | Yes | Multicast fan-out via m2u tunnel |
| Echo | `replicator --echo-mode <ip> <port>` | No | Testing, containers, ~200µs p50 |

## Multicast data path (m2u)

Three roles, two hops. EC2 VPCs don't forward raw multicast, so an 8-byte **m2u**
tunnel header rides *inside* a plain unicast UDP datagram and the replicator fans it out:

```
 mcast_send            mcast.o + Replicator            mcast_receive
  (source)  ── UDP[m2u|payload] ─▶ (replicator) ── UDP[m2u|payload] ─▶ (destinations)
   q1 ZC-TX          XDP redirect → XSK → ZC fan-out           XDP redirect → XSK
```

- **m2u header** — wire framing is `Eth|IP|UDP|m2u|payload`, where `m2u` = `magic
  0x4D324355 ("M2CU")` + 4-byte multicast group. The logical stream is keyed by
  `{UDP dst port, m2u group}`.
- **Source** (`mcast_send`) — AF_XDP zero-copy TX on a **non-RSS queue (queue 1)**;
  stamps `ts_ns` immediately before ring submit.
- **Replicator** — `mcast.o` matches `{group,port}` against `config_map` and
  `bpf_redirect_map`s the frame to the AF_XDP socket on RSS **queue 0**; userspace
  stamps `replicator_ns` at RX entry, then emits one unicast copy per **registered**
  destination (per-group fan-out) via zero-copy TX, one driver kick per batch.
- **Destination** (`mcast_receive`) — its own `mcast.o` redirects the matching frame
  to its XSK. Reports one-way = `rx_ns − ts_ns`, split into **hop1** (`replicator_ns −
  ts_ns`, source→replicator) and **hop2** (`rx_ns − replicator_ns`, replicator→dest).
- All timestamps are `CLOCK_REALTIME`, disciplined to the common **AWS Time Sync NTP**
  source (`169.254.169.123`, xleave) → sub-µs inter-instance; a clock-sync gate hard-fails
  a run above `max_clock_offset_us` (default 10µs).

## Latency optimizations

Measured on the 3-node same-AZ + cluster-placement-group fleet (10k msgs @ 200µs, 0% loss).

| Optimization | Mechanism | Impact |
|---|---|---|
| **Small `gro_flush_timeout` (10µs)** | Backstop timer of the deferred-IRQ + busy-poll regime. NOT `0` (strands packets in busy-poll gaps → multi-second bursts) and NOT the old `200µs` (becomes the primary delivery path → ~200µs/hop). | **234µs → ~60µs** one-way — the dominant win |
| **In-app NAPI busy-poll** | `SO_PREFER_BUSY_POLL`+`SO_BUSY_POLL` on the XSK fd; the RX loop issues `recvfrom`/`poll` on an empty peek so NAPI runs in the pinned thread instead of waiting for a deferred hard IRQ. Applied to the replicator (`XdpSocket::receive`) **and** `mcast_receive`. | dest-RX becomes gro-independent; robust, burst-free |
| **CPU isolation + pinning** | `isolcpus=1-3`; ENA IRQ → first isolated CPU, apps → isolated+1, OS/SSH on CPU0. The busy-poll thread is never preempted by the hard IRQ. `irqbalance` disabled. | removes P99 jitter/stalls |
| **Hugetlb UMEM** | `MAP_HUGETLB` (2 MB pages) for the UMEM with a clean 4 KB fallback. | fewer TLB misses on the packet buffers |
| **Non-RSS TX queue (queue 1)** | `mcast_send` binds AF_XDP TX off RSS queue 0 (which carries SSH/control); a ZC bind on queue 0 wedges the host NIC. | correctness + host stability |
| **Hot-path micro-opts** | cache the source IP once (kills per-packet `inet_aton`); drain TX completions once per fan-out batch; single driver kick for all K destinations. | trims per-packet + per-fan-out cost |

**Floor:** on virtualized ENA, ~26µs/hop is NIC/hypervisor-bound — CPU-side tuning
(busy-poll, IRQ pinning) beyond the gro fix shows diminishing returns. `dev/roadmap.md`
covers the gro/busy-poll mechanics in depth and the deferred kernel-side forward
(`XDP_TX`) design that would attack the remaining userspace-replicator hop.

## Multicast workflow

```bash
cd deploy/ansible
export AWS_PROFILE=<profile> SSH_KEY_FILE=~/.ssh/<key>.pem ANSIBLE_HOST_KEY_CHECKING=False

# 1. Configure — clock-sync gate, replicator mcast mode, destination joins, datapath probe
ansible-playbook -i inventory.aws_ec2.yml configure_mcast.yaml -e replicator_private_ip=<repl_ip>

# 2. Run — NIC tuning (gro=10µs, IRQ pin, defer=2) + isolated-CPU-pinned receivers/sender + report
ansible-playbook -i inventory.aws_ec2.yml run_mcast.yaml \
  -e replicator_private_ip=<repl_ip> -e count=10000 -e interval_us=200 -e timeout_sec=30
```
Outputs land in `results/<date>/<hh-mm-ss>-mcast/`: per-pair `<src_ip>-<dst_ip>.json`,
`matrix_report.html` (heatmap), `fleet.json` (topology for the web viewer), and
`matrix_summary.json`. See [`report/`](report/) for the 2D/3D viewer.

## Measured Results (c7i.xlarge CPG, us-east-1)

| Metric | Value |
|--------|-------|
| p50 | 32 µs |
| p90 | 34 µs |
| p99 | 37 µs |
| p99.9 | 45 µs |
| Loss | 0% |

Kernel software timestamps (SO_TIMESTAMP), 100K messages at 10K msg/sec, AF_XDP mode.

**Multicast (m2u), same-AZ + cluster-PG, 10k msgs @ 200µs, 0% loss:**

| Metric | one-way | hop1 (src→repl) | hop2 (repl→dest) |
|--------|:---:|:---:|:---:|
| min | ~38 µs | — | — |
| p50 | ~60–65 µs | ~26 µs | ~31–37 µs |
| p99 | ~85 µs | ~42 µs | ~49 µs |

Two hops (source→replicator→dest); ~26µs/hop is the virtualized-ENA floor. 

## Deployment

Two paths:

**Baked AMI (recommended):** Build once (~10 min), deploy instantly. Instances boot with replicator running in unicast (AF_XDP) mode, ready for unicast RTT tests with zero configuration.

**Dev provisioning:** Deploy stock AL2023, run `ansible-playbook provision.yaml` (~8 min).

See [`deploy/README.md`](deploy/README.md) for full workflow.

## Build Targets

| Target | Links | XDP? | Container-safe? |
|--------|-------|:----:|:---:|
| `make all` | `-lxdp -lbpf -lelf -lpthread` | ✅ | ❌ |
| `make echo-mode` | `-lpthread` only | ❌ | ✅ |
| `make full` | same as `all` + mcast | ✅ | ❌ |
| `make mcast` | mcast tools + mcast.o | ✅ | ❌ |

## Key Design Decisions

- **Universal AMI** — one image for all roles (source/replicator/destination)
- **Mode-switching replicator** — single binary, config-driven via `/etc/default/replicator`
- **Echo-mode for CI** — full integration test suite runs without XDP/root
- **Fleet-driven CDK** — arbitrary topologies from JSON, no hardcoded instance counts
- **NTP clock sync** — chrony disciplined to the common AWS Time Sync NTP source (`169.254.169.123`, xleave) for sub-µs *inter-instance* offset. (Per-node ENA PHC refclock was rejected: it disciplines each host to its own PHC, leaving ~200µs between instances — fatal for one-way measurement.)
