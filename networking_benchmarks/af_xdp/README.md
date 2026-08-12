# AF_XDP Network Latency Benchmark

High-performance network latency measurement suite using AF_XDP kernel bypass.
Measures round-trip and one-way latency between EC2 instances at microsecond precision.

## Structure

```
af_xdp/
├── src/            Core replicator engine (AF_XDP + kernel-mode) + eBPF (ucast.o/mcast.o)
├── tools/          Measurement instruments (rtt, mcast_send/receive, replicator_ctl, udp_send)
├── deploy/         Infrastructure (CDK stacks + benchmark/runtime Ansible playbooks)
│   ├── cdk/        Fleet deployment + AMI builder
│   └── ansible/    run_ucast / run_mcast / configure_mcast + inventory
├── report/         Topology report — gen/ (report.py heatmap + fleet_json.py → fleet.json) + web/ (Vite + Svelte + three.js, 2D/3D)
├── dev/            Dev tooling: pytest suite (dev/tests), Docker build harness, sync/provision playbooks
└── Makefile        Build system (all, kernel-mode, full, mcast targets)
```

## Quick Start

```bash
# Build (on EC2 with libxdp installed)
make full

# Build without libxdp (containers, CI, macOS cross-compile check)
make kernel-mode

# Run tests
pip install pytest && pytest -v
```

## Documentation

| Directory | README | Description |
|-----------|--------|-------------|
| [`src/`](src/README.md) | Architecture, control protocol, build modes | Core C++ replicator + eBPF XDP programs |
| [`tools/`](tools/README.md) | Usage, CLI flags, timestamp modes | RTT client, multicast tools, control CLI |
| [`dev/`](dev/README.md) | Dev tooling: tests, Docker harness, sync/provision | 34 integration tests, local build+test |
| [`deploy/`](deploy/README.md) | Deployment flows, instance roles | CDK + Ansible orchestration |
| [`deploy/cdk/`](deploy/cdk/README.md) | Fleet spec, scenarios, parameters | Infrastructure as code |
| [`deploy/ansible/`](deploy/ansible/README.md) | Playbooks, inventory, variables | Runtime provisioning |

## Modes

| Mode | Command | Root? | Use case |
|------|---------|:-----:|----------|
| AF_XDP (full) | `sudo replicator eth0 <ip> <port>` | Yes | Production — zero-copy, ~32µs p50 |
| AF_XDP + GRE | `sudo replicator eth0 <mcast> <port> --gre` | Yes | Multicast fan-out via GRE tunnel |
| Kernel | `replicator --kernel-mode <ip> <port>` | No | Testing, containers, ~200µs p50 |

## Measured Results (c7i.xlarge CPG, us-east-1)

| Metric | Value |
|--------|-------|
| p50 | 32 µs |
| p90 | 34 µs |
| p99 | 37 µs |
| p99.9 | 45 µs |
| Loss | 0% |

Kernel software timestamps (SO_TIMESTAMP), 100K messages at 10K msg/sec, AF_XDP mode.

## Deployment

Two paths:

**Baked AMI (recommended):** Build once (~10 min), deploy instantly. Instances boot with replicator running in kernel-mode, ready for unicast RTT tests with zero configuration.

**Dev provisioning:** Deploy stock AL2023, run `ansible-playbook provision.yaml` (~8 min).

See [`deploy/README.md`](deploy/README.md) for full workflow.

## Build Targets

| Target | Links | XDP? | Container-safe? |
|--------|-------|:----:|:---:|
| `make all` | `-lxdp -lbpf -lelf -lpthread` | ✅ | ❌ |
| `make kernel-mode` | `-lpthread` only | ❌ | ✅ |
| `make full` | same as `all` + mcast | ✅ | ❌ |
| `make mcast` | mcast tools + mcast.o | ✅ | ❌ |

## Key Design Decisions

- **Universal AMI** — one image for all roles (source/replicator/destination)
- **Mode-switching replicator** — single binary, config-driven via `/etc/default/replicator`
- **Kernel-mode for CI** — full integration test suite runs without XDP/root
- **Fleet-driven CDK** — arbitrary topologies from JSON, no hardcoded instance counts
- **PHC clock sync** — chrony refclock PHC for ±50-500ns inter-host accuracy
