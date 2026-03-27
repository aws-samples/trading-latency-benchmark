# mcast_gre — GRE-based Market Data Distribution

Low-latency market data fan-out across AWS VPCs using AF_XDP kernel bypass on the feeder and GRE tunnelling on the exchange. AWS VPC does not support native IP multicast routing between instances or across regions, so the system tunnels multicast traffic inside GRE unicast and fans it out in userspace.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Single VPC  (e.g. eu-central-1, Cluster Placement Group)                   │
│                                                                             │
│   ┌──────────────┐   multicast UDP      ┌────────────────────────────┐      │
│   │   Exchange   │   224.0.31.50:5000   │         Feeder             │      │
│   │    (Mock)    │ ──── gre_feed ──────►│  gre_filter.o  (XDP)       │      │
│   │              │   GRE/IP proto 47    │  AF_XDP zero-copy RX       │      │
│   └──────────────┘                      │  extractUdpPayloadGre()    │      │
│                                         │  per-group fan-out         │      │
│                                         └──────┬──────────┬──────────┘      │
│                                                │          │  GRE unicast    │
│                                      ┌─────────▼────┐  ┌──▼──────────┐      │
│                                      │ Subscriber 0 │  │ Subscriber 1│      │
│                                      │ AF_XDP RX    │  │ AF_XDP RX   │      │
│                                      │              │  │ ...         │      │
│                                      │ receiver.cpp │  │             │      │
│                                      └──────────────┘  └─────────────┘      │
└─────────────────────────────────────────────────────────────────────────────┘
```

> **Cross-region support:** The CDK stacks (`FeederStack` + `SubscriberStack` + `PeeringStack`)
> for cross-region deployment (feeder in one VPC, subscribers in another connected via VPC
> peering) are implemented. A cross-region deploy script is not yet available; see the
> single-region `deploy.sh` as a reference for the Ansible steps.

### Why GRE?

AWS VPC does not route IP multicast between instances (no IGMP snooping, no
multicast routing table).  Transit Gateway Multicast Domains cover single-region
same-TGW topologies but add ~100 µs overhead and are unavailable across peered
VPCs in different regions.

GRE (RFC 2784, IP protocol 47) carries the original inner multicast IP datagram
inside a unicast outer IP packet.  The VPC routes it like any other unicast
traffic — across subnets, AZs, and VPC peering connections.

---

## Packet flow

### 1. Exchange → Feeder (ingress GRE)

```
Exchange kernel:
  App sends UDP to 224.0.31.50:5000
  → ip route: 224.0.0.0/4 dev gre_feed
  → kernel GRE encapsulation:
      Eth | outer IPv4 (src=exchange, dst=feeder, proto=47)
          | GRE (flags=0x0000, proto=0x0800)
          | inner IPv4 (src=exchange, dst=224.0.31.50, proto=17)
          | UDP (dst=5000)
          | payload
  → NIC TX → VPC → Feeder NIC RX
```

### 2. Feeder XDP intercept (`gre_filter.o`)

The BPF program runs at the NIC driver level before the kernel network stack:

```
gre_filter (XDP):
  parse Eth → outer IPv4 (proto=47?)
           → GRE (proto=0x0800 IPv4?)
           → inner IPv4 (proto=17 UDP?)
           → inner IPv4 dst in 224.0.0.0/4?
           → scan config_map[0..15]:
               inner_ip.daddr == entry.target_ip
               AND udp.dest   == entry.target_port
           → match: bpf_redirect_map(xsks_map, rx_queue_index)
                    → AF_XDP socket (zero-copy, no kernel stack)
           → no match: XDP_PASS (kernel handles it normally)
```

`config_map` is a BPF array of up to 16 `{target_ip, target_port}` entries.
The feeder writes entries at startup (seed group from CLI) and dynamically as
subscribers join via `CTRL_MCAST_JOIN` control messages.

### 3. Feeder userspace fan-out (`PacketReplicator`)

```
AF_XDP RX:
  extractUdpPayloadGre():
    strip Eth + outer IPv4 + GRE + inner IPv4 headers
    → inner IP datagram ptr + len, group_nbo = inner IP daddr

  getCachedGroupDestinations(group_nbo):
    thread-local cache (100 ms TTL) of group → [Destination]
    populated from group_destinations_[group_nbo]

  for each Destination:
    sendToDestinationWithQueue():
      ARP resolved? → createGrePacket() → AF_XDP TX (zero-copy)
                    → fallback: SOCK_RAW/IPPROTO_GRE kernel socket
```

**Output packet layout** (feeder → subscriber):
```
Eth | outer IPv4 (src=feeder, dst=subscriber, proto=47)
    | GRE (4 bytes: flags=0x0000, proto=0x0800)
    | inner IP datagram verbatim (unchanged from exchange)
```

The inner IP datagram is forwarded without modification.  The inner UDP
destination port is always the exchange data port (default 5000).  Per-subscriber
port remapping is not performed — the kernel would need to rewrite headers and
recompute checksums, adding latency.

### 4. Subscriber receive

`receiver` attaches `subscriber_filter.o` (XDP program) to the NIC via
direct `bpf_xdp_attach` (libbpf, not the libxdp dispatcher which fails on some
kernels with `EINVAL`), then opens an AF_XDP socket on queue 0.  The XDP program
redirects all GRE frames (IP proto 47) into the socket via `bpf_redirect_map`,
bypassing the kernel IP stack entirely.

```
XDP (subscriber_filter.o):
  parse Eth → outer IPv4 (proto=47 GRE?)
  match → bpf_redirect_map(xsks_map, rx_queue_index, XDP_PASS)
           → AF_XDP socket (zero-copy)
  no match → XDP_PASS (SSH, ARP etc. unaffected)

AF_XDP RX ring poll:
  parse Eth | outer IPv4 | GRE | inner IPv4 | UDP | payload
  filter on inner UDP dst port
  latency = CLOCK_REALTIME.now() − payload.tx_ns
```

The NIC must have `combined queues = 1` so that RSS delivers all traffic to
queue 0 — the only `xsks_map` slot that is populated.  On metal instances with
many queues the default `MAX/2` setting disperses GRE across 16+ queues, and all
but queue 0 fall back to `XDP_PASS` and are silently dropped by the kernel.

---

## Multicast group management

### Seed group

`packet_replicator` takes `listen_ip` as a positional argument.  In GRE mode
this is the inner multicast group; in unicast mode it is the feeder's unicast
address.  The value is written to `config_map[0]` at startup — the XDP program
immediately starts redirecting matching frames.

### Subscriber registration (control protocol)

Subscribers register via UDP control messages to the feeder on port 12345:

| Command | Wire format | Effect |
|---------|-------------|--------|
| `add <ip> <port>` | `[0x01][4B ip][2B port]` | Add unicast destination |
| `remove <ip> <port>` | `[0x02][4B ip][2B port]` | Remove destination |
| `list` | `[0x03]` | Returns destination list |
| `mcast <group>` | `[0x04][4B group]` | CTRL_MCAST_JOIN: subscribe to group, feeder infers subscriber IP from UDP source |
| `mcast-leave <group>` | `[0x05][4B group]` | CTRL_MCAST_LEAVE |

Subscribers always receive traffic on the exchange data port (`listen_port` argument
to `packet_replicator`, default 5000) — the inner UDP dst is preserved verbatim in the
GRE frame.

---

## Per-group fan-out

Each multicast group has an independent subscriber set:

```
group_destinations_[224.0.31.50] → { sub0_ip → Dest, sub1_ip → Dest }
group_destinations_[224.0.31.51] → { sub1_ip → Dest }
```

`all_destinations_` is a flat IP-keyed map used only by the `list` command.  It
counts unique subscriber IPs, not (group × subscriber) pairs.

A single subscriber can join multiple groups; it will receive packets for each
group on the same port (merged stream).  Use separate processes or packet
filtering to demultiplex.

---

## AWS network constraints

| Constraint | Workaround |
|------------|------------|
| No VPC IP multicast routing | GRE unicast tunnel exchange→feeder |
| No multicast across VPC peering | Feeder re-sends GRE unicast per subscriber |
| ENA requires XDP TX queue headroom (feeder) | `tune_feeder.yaml` halves combined queues to `MAX/2` **before** `packet_replicator` starts; on metal (e.g. c7i.metal-24xl) the default queue count is too high and ENA rejects XDP attach with "no enough space for allocating XDP queues" — `tune_feeder.yaml` must always precede `configure.yaml` |
| ENA RSS distributes GRE across all queues (subscriber) | `configure.yaml` sets `combined 1` on subscriber; AF_XDP socket is on queue 0 only — all other queues fall back to `XDP_PASS` and are silently dropped by the kernel; feeder keeps `MAX/2` for multi-queue busy-poll throughput |
| ENA native XDP requires MTU ≤ 3498 | `configure.yaml` sets MTU 3498 on subscriber (4 KB page − headroom); otherwise ENA falls back to `xdpgeneric` (SKB mode, not kernel bypass) |
| ENA does not support `adaptive-rx off` / `adaptive-tx off` | Use `ethtool -C rx-usecs 0 tx-usecs 0` only; `adaptive-*` flags cause `netlink error` on ENA |
| ENA does not support RSS protocol-level flow steering | Cannot use `ethtool -N flow-type ip4 proto 47 action 0`; use `combined 1` instead |
| XDP_ZEROCOPY needs ENA native mode | Automatic fallback to `XDP_SKB` if native attach fails |
| libxdp dispatcher fails on some kernels | `receiver` uses direct `bpf_xdp_attach` (libbpf), not the libxdp multi-prog dispatcher |
| RPF drops asymmetric-routed multicast control traffic | `rp_filter=0` set by `tune_feeder.yaml` |
| ENA default interrupt coalescing adds 20–84 µs batching delay | `configure.yaml` sets `rx-usecs 0 tx-usecs 0` on all nodes |

---

## Deployment

### Single region (exchange + feeder + subscribers in one VPC)

```bash
./deploy.sh \
    --region eu-central-1 \
    --key-pair my-key \
    --ssh-key ~/.ssh/my-key.pem
```

Creates `SingleRegionStack` (CDK): exchange + feeder + N subscribers in a
Cluster Placement Group for minimum intra-VPC latency.

### Cross region (feeder in one region, subscribers in another)

A cross-region deploy script is not yet available.  Deploy the CDK stacks
(`FeederStack`, `SubscriberStack`, `PeeringStack`) manually via `cdk deploy`,
then run `tune_feeder.yaml` and `configure.yaml` with the appropriate
inventories and `feeder_private_ip` extra-var.

Creates `FeederStack` + `SubscriberStack` + `PeeringStack` (VPC peering with
route tables and security groups).

Both scripts:
1. Deploy CDK stacks (skip if already exists), using a pre-tuned AMI built with
   `build-tuned-ami.sh` (runs `tune_os.yaml` at AMI creation time — CPU isolation,
   IRQ affinity, network stack tuning are baked in, not applied at deploy time)
2. Fetch CloudFormation outputs (IPs)
3. Apply feeder-specific XDP tuning via `tune_feeder.yaml` (BPF JIT, rp_filter=0,
   halve ENA combined queues to `MAX/2`) — **must run before `configure.yaml`**,
   otherwise `packet_replicator` fails to attach XDP with
   `"no enough space for allocating XDP queues"` on high-queue-count metal instances
4. Run `configure.yaml` — builds xdp-tools + benchmark on all nodes, configures GRE tunnel on exchange, sets combined queues to 1 + MTU 3498 on subscribers, disables interrupt coalescing everywhere, starts `packet_replicator` service on feeder
5. Register each subscriber with its own multicast group

### Re-provision after code change

```bash
# All nodes (rebuild + reconfigure everything)
ansible-playbook -i inventory.aws_ec2.yml configure.yaml \
    --extra-vars "feeder_private_ip=<feeder-ip>"

# Limit to a single role if needed (exchange, feeder, or subscribers)
ansible-playbook -i inventory.aws_ec2.yml configure.yaml \
    --extra-vars "feeder_private_ip=<feeder-ip>" \
    --limit exchange   # or: --limit feeder  --limit subscribers
```

---

## Key defaults

| Parameter | Default | Override |
|-----------|---------|----------|
| Multicast group (seed) | `224.0.31.50` | `--multicast-group` |
| Data port | `5000` | `--data-port` |
| Control port | `12345` | hardcoded in `PacketReplicator.hpp` |
| Feeder VPC CIDR | `10.61.0.0/16` | `--feeder-vpc-cidr` |
| Subscriber VPC CIDR | `10.62.0.0/16` | `--subscriber-vpc-cidr` |
| Instance type | `c7i.4xlarge` | `--instance-type` / `--feeder-type` |
| Max simultaneous groups | 16 | `MAX_GROUPS` in `gre_filter.c` |

---

## Further development

### Cross-region end-to-end test case
cdk stack is implemented, deployment is missing.
Expected outcome: Hop 1 (exchange→feeder, intra-region) stays
~14 µs; Hop 2 (feeder→subscriber, cross-region) reflects inter-region RTT
(typically 10–80 µs depending on region pair).

### Per-subscriber port remapping
Currently the inner IP datagram is forwarded verbatim, so all subscribers
receive traffic on the exchange data port (5000).  Supporting per-subscriber
port remapping would let different consumers bind to different ports on the
same host — useful when a single subscriber machine runs multiple strategy
processes.  Implementation requires the feeder to rewrite the inner UDP
destination port and recompute the UDP checksum before re-encapsulating in
GRE.  The `CTRL_MCAST_JOIN` wire format was intentionally kept at 5 bytes
(`[0x04][4B group]`) to leave room for a future `[0x04][4B group][2B port]`
extension without breaking existing subscribers.

### `multicast_filter.o` — native multicast mode (no GRE)
For deployments where real IP multicast routing is available (TGW Multicast
Domain, same-subnet, or co-location), a `multicast_filter.c` eBPF program
that matches `224.0.0.0/4` + target port would eliminate the GRE
encapsulation overhead on the exchange and the header-stripping on the feeder.
The `PacketReplicator` code already has the `gre_mode_ = false` path; only the
BPF object and a source file are needed.

### Cross-region deploy script
A `deploy.cross_region.sh` that mirrors `deploy.sh` but targets
`cross-region-stack.ts` — deploying feeder and subscriber stacks in separate
regions, setting up VPC peering via `PeeringStack`, and running
`configure.yaml` with both inventories.

### Dynamic subscriber discovery via DNS / service registry
Today subscribers must know the feeder's private IP at registration time and
call `control_client mcast <group>` manually (or via `configure.yaml` Play 5).
Integrating with AWS Cloud Map or a simple Route 53 private zone would allow
new subscriber instances to self-register without re-running Ansible.

### Hardware timestamping baseline comparison
Add a `latency_sender` mode that uses kernel `SO_TXTIME` / `ETF` qdisc to
stamp packets at NIC TX rather than before ring submission.  This would
separate the ~1–2 µs NIC kick latency from the feeder-side software path and
provide a tighter Hop 1 floor.

### `SO_PREFER_BUSY_POLL` vs pure spin-loop comparison
The feeder RX loop is currently a pure user-space spin (`xsk_ring_cons__peek`
+ `_mm_pause` on empty ring — zero syscalls).  `SO_PREFER_BUSY_POLL` /
`SO_BUSY_POLL_BUDGET` would instead run the NIC NAPI poll loop inside the
kernel on each `recvfrom` call, eliminating the hardware-interrupt →
softirq → NAPI-schedule latency path.  On a fully dedicated feeder core the
pure spin loop should win; on a shared or power-capped core kernel busy-poll
may be lower.  Worth benchmarking both under `-i 100` burst conditions and
comparing P50 / P99 Hop 1.

---

## Directory structure

```
mcast_gre/
├── README.md                          ← this file
├── deploy.sh                          ← single-region CDK + Ansible deploy
├── configure.yaml                     ← unified provisioning: all nodes (build,
│                                         GRE tunnel, NIC tuning, services, registration)
├── tune_feeder.yaml                   ← BPF JIT, rp_filter, ENA queue headroom (feeder only)
├── inventory.aws_ec2.yml
├── benchmark/                         ← latency measurement tools
│   ├── README.md                      ← sender/receiver usage, prerequisites, tuning
│   ├── sender.cpp                     ← AF_XDP TX sender; injects GRE frames at L2
│   ├── receiver.cpp                   ← AF_XDP RX; measures one-way latency
│   ├── subscriber_filter.c            ← XDP program: redirects GRE → AF_XDP socket
│   ├── ptp.sh                         ← tightens NTP (chrony) for sub-10µs clock sync
│   └── Makefile                       ← requires clang for BPF compilation
└── cdk/                               ← AWS CDK TypeScript stacks
    └── lib/
        ├── single-region-stack.ts     ← SingleRegionStack (CPG)
        └── cross-region-stack.ts      ← SourceStack, SubscriberStack, PeeringStack
```
