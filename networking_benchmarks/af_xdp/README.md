# AF_XDP Network Latency Benchmark

High-performance network latency measurement suite using AF_XDP kernel bypass.
Measures round-trip (unicast) and one-way (multicast fan-out) latency between EC2
instances at microsecond precision, with a push-button control plane (agents +
web UI) for driving campaigns and rendering live results.

## Structure

```
af_xdp/
├── src/              Core replicator engine (AF_XDP + echo-mode) + eBPF (ucast.o / mcast.o)
│   ├── common/       Shared headers
│   ├── Replicator/   Packet replicator files - key binary running on each instance
│   ├── xdp/          eBPF C programs
├── tools/            Measurement instruments (rtt, mcast_send/receive, replicator_ctl, udp_send)
├── deploy/           Infrastructure
│   ├── cdk/          Fleet + AMI builder + control-plane CDK stacks (scenarios/, scripts/bake-ami.sh)
│   └── ansible/      run_ucast / run_mcast / configure_mcast + dynamic inventory
├── control_plane/    Centralized orchestration + live monitoring
│   ├── agent/        Per-node sidecar (IMDS self-register, runs rtt/mcast, streams telemetry)
│   ├── backend/      Registry + NxN collector + orchestrator + HTTP/SSE API (serves web/)
│   └── cmd/afxdpctl/ One CLI: up / sync / down / run / cancel / report / fleet
│   ├── gen/          Offline: per-pair JSON → heatmap (report.py) + fleet.json (fleet_json.py)
│   ├── mcp/          Read-only MCP server (exposes results DB to AI tooling)
│   ├── proto/        Shared NATS wire contract (subjects + message schemas)
│   ├── web/          Svelte + three.js live 2D/3D topology + control panel
├── tests/            pytest integration suite (echo-mode; run from the af_xdp root)
├── dev/              Dev tooling: Docker build harness (dev/Dockerfile) + sync/provision playbooks
└── Makefile          Build system (all, echo-mode, full, mcast targets)
```

## Quick Start

```bash
# Build on EC2 with libxdp installed (production datapath)
make full

# Build without libxdp (containers, CI, macOS cross-compile check)
make echo-mode

# Run the integration suite (echo-mode; no root/XDP needed)
pip install pytest && pytest -v
```

## Running benchmarks - three ways

**1. Web UI (recommended).** A Go **agent** on every fleet node connects
*outbound* to a central **backend** over a **NATS** bus; the backend serves a web
UI + JSON/SSE API. You launch tests from the browser, results stream back live into a 2D/3D topology. No SSH in the hot path, no creds in the
loop. See [`control_plane/`](control_plane/README.md).

**2. CLI Tool.**
```bash
# One CLI for the whole loop (control_plane/cmd/afxdpctl):
afxdpctl up   --key virginia --git-repo <url> --git-ref <branch> --scenario ucast-az-cpg-3 --bake
afxdpctl sync --key ~/.ssh/virginia.pem            # hot-deploy local code to the fleet
afxdpctl run  ucast kernel                          # or: run mcast copy,inplace,bpf_tx
afxdpctl report -o run.html                         # heatmap + all latencies
afxdpctl down --key virginia
```

**3. Ansible playbooks (no control plane).** Drive a fleet directly with
`run_ucast.yaml` / `run_mcast.yaml`; results are written to `results/` and turned
into a heatmap + `fleet.json` by `control_plane/gen/`. See the multicast workflow
below and [`deploy/ansible/`](deploy/ansible/README.md).

## Documentation

| Directory | Description |
|-----------|-------------|
| [`src/`](src/README.md) | Core C++ replicator + eBPF XDP programs; datapaths, control protocol, build modes |
| [`tools/`](tools/README.md) | RTT client, multicast tools, control CLI - usage, flags, timestamp modes |
| [`control_plane/`](control_plane/README.md) | NATS + agents + backend + web; what happens behind the web UI; `afxdpctl` |
| [`control_plane/web/`](control_plane/web/README.md) | Live 2D/3D viewer + control panel internals |
| [`deploy/`](deploy/README.md) | Deployment flows and instance roles |
| [`deploy/cdk/`](deploy/cdk/README.md) | Fleet spec, scenarios, deployment types, context parameters |
| [`deploy/ansible/`](deploy/ansible/README.md) | Runtime playbooks, inventory, variables |
| [`dev/`](dev/README.md) | Docker build harness, sync/provision, dev loop |
| [`tests/`](tests/README.md) | Echo-mode integration suite - test classes, fixtures, run modes |

## Modes

| Mode | Command | Root? | Use case |
|------|---------|:-----:|----------|
| AF_XDP (full) | `sudo replicator eth0 <ip> <port>` | Yes | Production - zero-copy, ~32µs p50 |
| AF_XDP + m2u | `sudo replicator eth0 <mcast> <port> --mcast` | Yes | Multicast fan-out via m2u tunnel |
| Echo | `replicator --echo-mode <ip> <port>` | No | Testing, containers, ~200µs p50 |

## Multicast data path (m2u)

Three roles, two hops. EC2 VPCs don't forward raw multicast, so an 8-byte **m2u**
tunnel header (inspired by [mcast2ucast](../mcast2ucast/)) rides *inside* a plain unicast UDP datagram and the replicator fans
it out:

![multicast (m2u) datapath](tools/assets/mcast-datapath.svg)

- **m2u header** - wire framing is `Eth|IP|UDP|m2u|payload`, where `m2u` = `magic
  0x4D324355 ("M2CU")` + 4-byte multicast group. The logical stream is keyed by
  `{UDP dst port, m2u group}`.
- **Source** (`mcast_send`) - AF_XDP zero-copy TX on a **non-RSS queue (queue 1)**;
  stamps `ts_ns` immediately before ring submit.
- **Replicator** - `mcast.o` matches `{group,port}` against `config_map` and
  `bpf_redirect_map`s the frame to the AF_XDP socket on RSS **queue 0**; userspace
  stamps `replicator_ns` at RX entry, then emits one unicast copy per **registered**
  destination (per-group fan-out) via zero-copy TX, one driver kick per batch.
- **Destination** (`mcast_receive`) - its own `mcast.o` redirects the matching frame
  to its XSK. Reports one-way = `rx_ns − ts_ns`, split into **hop1** (`replicator_ns −
  ts_ns`, source→replicator) and **hop2** (`rx_ns − replicator_ns`, replicator→dest).
- All timestamps are `CLOCK_REALTIME`, disciplined to the common **AWS Time Sync NTP**
  source (`169.254.169.123`, xleave) → sub-µs inter-instance; a clock-sync gate hard-fails
  a run above `max_clock_offset_us` (default 10µs).

## Latency optimizations

Measured on the 3-node same-AZ + cluster-placement-group fleet (10k msgs @ 200µs, 0% loss).

| Optimization | Mechanism | Impact |
|---|---|---|
| **Small `gro_flush_timeout` (10µs)** | Backstop timer of the deferred-IRQ + busy-poll regime. NOT `0` (strands packets in busy-poll gaps → multi-second bursts) and NOT a high value (e.g. `200µs`) - as it becomes the primary delivery path → ~200µs/hop. | tight latency one-way |
| **In-app NAPI busy-poll** | `SO_PREFER_BUSY_POLL`+`SO_BUSY_POLL` on the XSK fd; the RX loop issues `recvfrom`/`poll` on an empty peek so NAPI runs in the pinned thread instead of waiting for a deferred hard IRQ. Applied to the replicator (`XdpSocket::receive`) **and** `mcast_receive`. | dest-RX becomes gro-independent; robust, burst-free |
| **CPU isolation + pinning** | `isolcpus=1-4`; ENA IRQ → first isolated CPU, apps → isolated+1, OS/SSH on CPU0. The busy-poll thread is never preempted by the hard IRQ. `irqbalance` disabled. | removes P99 jitter/stalls |
| **Hugetlb UMEM** | `MAP_HUGETLB` (2 MB pages) for the UMEM with a clean 4 KB fallback. | fewer TLB misses on the packet buffers |
| **Non-RSS TX queue (queue 1)** | `mcast_send` binds AF_XDP TX off RSS queue 0 (which carries SSH/control); a ZC bind on queue 0 wedges the host NIC. | correctness + host stability |
| **Hot-path micro-opts** | cache the source IP once (kills per-packet `inet_aton`); drain TX completions once per fan-out batch; single driver kick for all K destinations. | trims per-packet + per-fan-out cost |

**Floor:** on virtualized ENA, ~23µs/hop is NIC/hypervisor-bound - CPU-side tuning
(busy-poll, IRQ pinning) beyond the gro fix shows diminishing returns.

## Multicast workflow (ansible)

```bash
cd deploy/ansible
export AWS_PROFILE=<profile> SSH_KEY_FILE=~/.ssh/<key>.pem ANSIBLE_HOST_KEY_CHECKING=False

# 1. Configure - clock-sync gate, replicator mcast mode, destination joins, datapath probe
ansible-playbook -i inventory.aws_ec2.yml configure_mcast.yaml -e replicator_private_ip=<repl_ip>

# 2. Run - NIC tuning (gro=10µs, IRQ pin, defer=2) + isolated-CPU-pinned receivers/sender + report
ansible-playbook -i inventory.aws_ec2.yml run_mcast.yaml \
  -e replicator_private_ip=<repl_ip> -e count=10000 -e interval_us=200 -e timeout_sec=30
```
Outputs land in `results/<date>/<hh-mm-ss>-mcast/`: per-pair `<src_ip>-<dst_ip>.json`,
`matrix_report.html` (heatmap), `fleet.json` (topology for the web viewer), and
`matrix_summary.json`. The same `fleet.json` schema is what the control-plane web
viewer renders - see [`control_plane/`](control_plane/README.md).

## Measured Results (best case, c7i cluster-PG)

| Path | p50 | p99 | Loss |
|---|---|---|---|
| Unicast RTT, same-AZ + cluster-PG (dedicated 3-node run, us-east-1) | 32 µs | 37 µs | 0% |
| Multicast one-way, same-AZ + cluster-PG (live 8-node fleet, eu-central-1) | 51 µs | 64 µs | 0% |

Full topology-tier breakdown (dedicated host, same-AZ, cross-AZ,
cross-region; hop1/hop2 splits; per-replicator/destination multicast
matrix; methodology notes) is in
[`assets/blog/topology_latency.md`](../../assets/blog/topology_latency.md).


## Deployment

Three CDK deployment types (`--context deploymentType=…`, default `fleet`):

| Type | What it creates |
|------|-----------------|
| `ami-builder` | Builds a pre-tuned AMI (binaries, tuning, systemd units, **baked agent**) and publishes its id to SSM. ~10–20 min. |
| `fleet` | An EC2 fleet from a JSON scenario (placement groups, multi-AZ, cross-region). Baked instances boot ready; the agent self-registers. |
| `control-plane` | A small EC2 running NATS + the backend (serves the web UI); publishes the NATS endpoint/token to SSM for agents to discover. |

Fleet nodes either use the **baked AMI** (instant readiness) or stock AL2023 +
`ansible-playbook provision.yaml`. Iterate on a running fleet with
`ansible-playbook sync.yaml` (rsync → rebuild tools **and the Go agent** → restart).
The `afxdpctl` CLI wraps all of this. See [`deploy/README.md`](deploy/README.md).

## Accessing the control plane

The web UI and JSON/SSE API listen on `8080` with no authentication and no TLS,
so that port is **not exposed**. Reach it through SSM Session Manager port
forwarding, which authorises on IAM and needs no inbound rule, no certificate
and no DNS name:

```bash
# One-time: install the plugin (macOS)
brew install --cask session-manager-plugin

# Forward the control plane's 8080 to localhost:8080
aws ssm start-session --region <region> --target <control-plane-instance-id> \
  --document-name AWS-StartPortForwardingSession \
  --parameters '{"portNumber":["8080"],"localPortNumber":["8080"]}'

# then browse http://localhost:8080
```

Find the instance id with:

```bash
aws ec2 describe-instances --region <region> \
  --filters "Name=tag:Name,Values=af-xdp-control-plane" \
             "Name=instance-state-name,Values=running" \
  --query "Reservations[].Instances[].InstanceId" --output text
```

A shell on the same instance, without SSH or an open port:

```bash
aws ssm start-session --region <region> --target <control-plane-instance-id>
```

Every session is authenticated as your IAM principal and recorded in CloudTrail.

### Direct access instead

To reach `8080` and `22` from one address rather than over SSM, deploy the
control plane with `adminCidr`:

```bash
npx cdk deploy XdpStack-ControlPlane --context deploymentType=control-plane \
  --context region=<region> --context keyPairName=<key> \
  --context adminCidr=$(curl -s https://checkip.amazonaws.com)/32
```

Without `adminCidr`, `8080` has no ingress rule at all and `22` stays open -
narrow it yourself if the account is not otherwise restricted.

### What stays exposed

`4222` (NATS) follows `clientCidr`, default `0.0.0.0/0`, because agents dial in
from every region a fleet spans and a cross-region fleet reaches the control
plane over the public internet. A token in SSM protects it. Closing it needs
private connectivity between the fleet VPCs and the control plane rather than a
security group change, so treat a world-reachable `4222` as the remaining
exposure and set `clientCidr` when the fleet is single-region.


## Build Targets

| Target | Links | XDP? | Container-safe? |
|--------|-------|:----:|:---:|
| `make all` | `-lxdp -lbpf -lelf -lpthread` | ✅ | ❌ |
| `make echo-mode` | `-lpthread` only | ❌ | ✅ |
| `make full` | same as `all` + mcast | ✅ | ❌ |
| `make mcast` | mcast tools + mcast.o | ✅ | ❌ |

## Key Design Decisions

- **Universal AMI** - one image for all roles (source/replicator/destination).
- **Mode-switching replicator** - single binary, config-driven via `/etc/default/replicator`.
- **Echo-mode for CI** - the full integration suite runs without XDP/root (containers, macOS).
- **Fleet-driven CDK** - arbitrary topologies from JSON, no hardcoded instance counts.
- **Agent-outbound control plane** - agents open ONE outbound NATS connection; no inbound
  ports on fleet nodes and no SSH in the hot path, so a runaway XDP program can't lock you out.
- **PHC clock sync** - chrony disciplines each node to the **ENA PHC hardware clock**
  (`refclock PHC /dev/ptp0`, `phc_enable=1`) reading the Nitro hypervisor clock directly
  (observed RMS offset tens of nanoseconds). AWS Time Sync NTP (`169.254.169.123`, xleave,
  `minpoll 2`) is the fallback until the PHC device is available after reboot.

## License

MIT-0. See the repository `LICENSE`.
