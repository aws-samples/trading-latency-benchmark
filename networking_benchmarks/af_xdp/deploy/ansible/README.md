# ansible/

Runtime configuration and benchmark playbooks for the AF_XDP benchmark.

> **Note:** dev/iteration playbooks (`provision.yaml`, `sync.yaml`, `run_tests.yaml`)
> live in **`af_xdp/dev/ansible/`**. The shared `inventory.aws_ec2.yml` is symlinked
> into that folder so `-i inventory.aws_ec2.yml` works from either location.

## Two Usage Modes

### 1. With CDK FleetStack (default)

CDK deploys instances with `Role` tags. Ansible discovers them automatically via dynamic inventory:

```bash
export SSH_KEY_FILE=~/.ssh/virginia.pem
export AWS_DEFAULT_REGION=us-east-1

# Multicast setup + run:
ansible-playbook -i inventory.aws_ec2.yml configure_mcast.yaml \
  -e replicator_private_ip=10.61.0.5
ansible-playbook -i inventory.aws_ec2.yml run_mcast.yaml \
  -e replicator_private_ip=10.61.0.5

# Unicast (no config needed — baked AMI is test-ready):
ansible-playbook -i inventory.aws_ec2.yml run_ucast.yaml
```

### 2. Without CDK — Self-hosted Instances (BYOI)

Use these playbooks on **any** EC2 instances you manage yourself. Requirements:

1. **Tag your instances** with the `Role` tag:
   - `Role: source` — market data origin
   - `Role: replicator` — packet replicator node
   - `Role: destination` — latency measurement endpoint

2. **Ensure connectivity:**
   - SSH access from your control machine (port 22)
   - Intra-fleet UDP 5000 + TCP 12345 open between nodes
   - Public IP or bastion for ansible to reach them

3. **Run playbooks** with a static or dynamic inventory:

```bash
# Option A: dynamic inventory (discovers by Role tag)
export AWS_DEFAULT_REGION=us-east-1
ansible-playbook -i inventory.aws_ec2.yml run_ucast.yaml

# Option B: static inventory file
ansible-playbook -i hosts.ini run_ucast.yaml
```

Example static inventory (`hosts.ini`):
```ini
[source]
10.0.1.10 ansible_user=ec2-user

[replicator]
10.0.1.20 ansible_user=ec2-user

[destination]
10.0.1.30 ansible_user=ec2-user
10.0.1.31 ansible_user=ec2-user
```

## Files

| File | Purpose | When to use |
|------|---------|-------------|
| `run_ucast.yaml` | Unicast NxN RTT benchmark + generate report | After provisioning — serial pairwise measurement |
| `run_mcast.yaml` | Multicast fan-out benchmark | After `configure_mcast` — source→replicator→destinations |
| `configure_mcast.yaml` | Multicast setup (self-contained) | Adapts nodes, sets replicator mcast mode, registers dests |
| `inventory.aws_ec2.yml` | Dynamic EC2 inventory by Role tag | With CDK-deployed or manually-tagged instances |

## Inventory

Uses `amazon.aws.aws_ec2` plugin. Discovers instances in **both** `us-east-1` and `eu-west-2` (covers cross-region topologies; single-region runs get empty results from the other region).

Groups by `Role` tag:

| Tag `Role` | Ansible group |
|------------|---------------|
| `source` | `source` |
| `replicator` | `replicator` |
| `destination` | `destination` |

**Environment variables required:**
```bash
export SSH_KEY_FILE=~/.ssh/virginia.pem          # primary region key
export SSH_KEY_FILE_SECONDARY=~/.ssh/london.pem  # secondary region key (cross-region only)
export AWS_DEFAULT_REGION=us-east-1
```

The inventory auto-selects the correct key per node based on its AZ prefix.

## Workflows

### Unicast (no ansible config needed after provisioning)

Instances boot with `replicator.service` in unicast-mode. Run RTT tests directly:

```bash
# Manual single-pair:
ssh ec2-user@<nodeA> '/opt/af-xdp/rtt <nodeB_ip> 5000 <nodeA_ip> 19020 1000 1000 100 0 1'

# Full NxN benchmark via playbook:
ansible-playbook -i inventory.aws_ec2.yml run_ucast.yaml
```

### Multicast

`configure_mcast.yaml` is self-contained — it adapts the ucast-baked source/dest nodes, wires the topology, and probes the datapath:

```bash
# 1. Configure topology
ansible-playbook -i inventory.aws_ec2.yml configure_mcast.yaml \
  -e replicator_private_ip=10.61.0.5

# 2. Run fan-out benchmark
ansible-playbook -i inventory.aws_ec2.yml run_mcast.yaml \
  -e replicator_private_ip=10.61.0.5
```

### `run_ucast.yaml` — Dynamic CPU Pinning

| Variable | Default | Description |
|----------|---------|-------------|
| `auto_pin` | `true` | Derive `send_cpu`/`recv_cpu` per host from the isolated range |
| `send_cpu` / `recv_cpu` | `4` / `3` | Literal pins (used only when `auto_pin=false`) |

With `auto_pin=true`: `recv = isolated[2]`, `send = isolated[3]` (above the ENA-IRQ core and the replicator-poll core). Adapts across `c7i.4xlarge` → metal.

### `configure_mcast.yaml` — Plays

| Play | Hosts | What it does |
|------|-------|--------------|
| 1 | `source` | Stops+disables replicator (frees AF_XDP queue), detaches stale XDP |
| 2 | `replicator` | Writes `/etc/default/replicator` (mcast mode) + restarts service |
| 3 | `destination` | Stops+disables replicator, detaches XDP, registers (CTRL_MCAST_JOIN), seeds ARP |
| 4 | `replicator` | Best-effort datapath probe: checks XDP redirect counter moved |

### `configure_mcast.yaml` — Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `replicator_private_ip` | **(required)** | Private IP of the replicator instance |
| `mcast_group` | `224.0.31.50` | Base multicast group |
| `base_mcast_group` | `224.0.31.50` | Per-destination group base (last octet incremented) |
| `data_port` | `5000` | UDP data port |

## Using `afxdpctl` Instead

For a simpler workflow, use the `afxdpctl` CLI which wraps both CDK and ansible:

```bash
afxdpctl run ucast kernel         # triggers ucast benchmark via the control-plane API
afxdpctl run mcast copy,inplace   # triggers mcast benchmark
afxdpctl report -o results.html   # generates HTML report
```

See `control-plane/cmd/afxdpctl` for the full CLI reference.
