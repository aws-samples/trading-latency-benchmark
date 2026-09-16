# ansible/

Runtime configuration and provisioning playbooks for the AF_XDP benchmark.

> **Note:** the dev/iteration playbooks (`provision.yaml`, `sync.yaml`,
> `run_tests.yaml`) have moved to **`af_xdp/dev/ansible/`**. Run them from there
> (`cd ../../dev/ansible`); the shared `inventory.aws_ec2.yml` is symlinked into
> that folder so `-i inventory.aws_ec2.yml` still works. This directory keeps the
> benchmark/runtime playbooks (`run_ucast.yaml`, `run_mcast.yaml`,
> `configure_mcast.yaml`) and the shared inventory. Report generators live in `../../report/`.

## Two usage modes

### 1. With CDK FleetStack (default)

CDK deploys instances with `Role` tags. Ansible discovers them automatically via dynamic inventory:

```bash
ansible-playbook -i inventory.aws_ec2.yml provision.yaml        # stock AL2023 only
ansible-playbook -i inventory.aws_ec2.yml configure_mcast.yaml  # mcast topology
```

### 2. Without CDK — self-hosted instances (BYOI)

Use these playbooks on **any** EC2 instances you manage yourself (existing fleet, shared accounts, on-prem-like setups). Requirements:

1. **Tag your instances** with the `Role` tag so inventory can discover them:
   - `Role: source` — market data origin
   - `Role: replicator` — packet replicator node
   - `Role: destination` — latency measurement endpoint

2. **Ensure connectivity:**
   - SSH access from your control machine (port 22 or SSM)
   - Intra-fleet UDP 5000 + TCP 12345 open between nodes
   - Public IP or bastion for ansible to reach them

3. **Run playbooks** with a static or dynamic inventory:

```bash
# Option A: Use the dynamic inventory (discovers by Role tag)
export AWS_DEFAULT_REGION=us-east-1
ansible-playbook -i inventory.aws_ec2.yml provision.yaml

# Option B: Use a static inventory file
ansible-playbook -i hosts.ini provision.yaml
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

After provisioning, binaries are installed to `/opt/af-xdp/` and the `replicator.service` systemd unit is active (ucast-mode by default).

## Files

| File | Purpose | When to use |
|------|---------|-------------|
| `configure_mcast.yaml` | Multicast setup (self-contained) | Adapts source/dest nodes (stops replicator to free the AF_XDP queue), replicator mcast mode, registration, ARP seed, datapath probe |
| `run_ucast.yaml` | Run unicast NxN RTT benchmark + generate report | After provisioning — serial pairwise measurement, then local HTML/JSON report |
| `run_mcast.yaml` | Run multicast fan-out benchmark | After configure_mcast — source→replicator→destinations |
| `inventory.aws_ec2.yml` | Dynamic EC2 inventory by Role tag | With CDK-deployed or manually-tagged instances |
| _report generators_ | Moved to `../../report/` | `gen/` (Python: `report.py` — heatmap `matrix_report.html` + shared loaders; `fleet_json.py` — emits `fleet.json`; `run_ucast.yaml` Play 4 runs both) + `web/` (Vite + Svelte + three.js — renders `fleet.json` in 2D/3D) |
| **`../../dev/ansible/`** (moved) | | |
| `provision.yaml` | Full install from scratch | Stock AL2023 — self-hosted or CDK without baked AMI |

## Inventory

Uses `amazon.aws.aws_ec2` plugin. Groups instances by `Role` tag:

| Tag `Role` | Ansible group |
|------------|---------------|
| `source` | `source` |
| `replicator` | `replicator` |
| `destination` | `destination` |

Requires:
```bash
export SSH_KEY_FILE=~/.ssh/your-key.pem
export AWS_DEFAULT_REGION=us-east-1
```

## Workflows

### Unicast (no ansible needed after provisioning)

Instances boot with `replicator.service` in unicast-mode. Run RTT tests directly:

```bash
ssh ec2-user@<nodeA> '/opt/af-xdp/rtt <nodeB_ip> 5000 <nodeA_ip> 19020 1000 1000 100 0 1'
```

### Multicast

`configure_mcast.yaml` is self-contained — it adapts the ucast-baked source/dest
nodes (stops the replicator to free the AF_XDP queue) as the first step of each
play, then wires the topology:

```bash
# 1. Adapt nodes + replicator mcast mode + registration + ARP seed
ansible-playbook -i inventory.aws_ec2.yml configure_mcast.yaml -e replicator_private_ip=10.0.1.20
# 2. Fan-out latency benchmark (source → replicator → destinations)
ansible-playbook -i inventory.aws_ec2.yml run_mcast.yaml -e replicator_private_ip=10.0.1.20
```

### Rebuild binaries (after code change)

```bash
ansible-playbook -i inventory.aws_ec2.yml provision.yaml -e rebuild=true
```

## configure_mcast.yaml — Plays

| Play | Hosts | What it does |
|------|-------|--------------|
| 1 | `source` | Stops+disables replicator (frees AF_XDP queue) + detaches stale XDP (no kernel tunnel; mcast_send builds m2u) |
| 2 | `replicator` | Writes `/etc/default/replicator` (mcast mode) + restarts service |
| 3 | `destination` | Stops+disables replicator + detaches stale XDP + registers (CTRL_MCAST_JOIN) + seeds ARP toward the replicator so fan-out frames get a real dst MAC |
| 4 | `replicator` | Best-effort datapath probe: `mcast_send` m2u burst, checks the XDP redirect counter moved (non-fatal) |

### Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `replicator_private_ip` | (required) | Private IP of the replicator instance |
| `mcast_group` | `224.0.31.50` | Base multicast group |
| `base_mcast_group` | `224.0.31.50` | Per-destination group base (last octet incremented) |
| `data_port` | `5000` | UDP data port |

### `run_ucast.yaml` — dynamic CPU pinning

Unicast runs on `c7i.4xlarge` (see [deploy README → Instance sizing](../README.md#instance-sizing-cost-vs-cores)).
Core pins are derived dynamically per host, so no size-specific tuning is needed.

| Variable | Default | Description |
|----------|---------|-------------|
| `auto_pin` | `true` | Derive `send_cpu`/`recv_cpu` per host from the isolated range: `recv = lo+2`, `send = lo+3` (above the ENA-IRQ core `lo` and the replicator-poll core `lo+1`). Adapts across `c7i.4xlarge` → metal. Set `false` to force the literals below. |
| `send_cpu` / `recv_cpu` | `4` / `3` | Literal rtt pins used only when `auto_pin=false`. |

## Multicast: groups & destinations (capability + future dev)

**Replicator core (supported today):** the replicator handles **up to 16 multicast
groups** (`config_map` has `MAX_GROUPS = 16` slots; one slot per distinct group,
ref-counted join/leave) and, per group, an arbitrary **set of destinations** —
each matching packet is fanned out to *every* destination registered for that
group (`group_destinations_[group]`). The `mcast.o` XDP filter loops the 16 slots
matching inner `{group, port}` and redirects to the AF_XDP socket. So a single
replicator supports **16 groups × N destinations each**.

**Orchestration (current limitation):** the playbooks + `mcast_receive` currently
only wire up the **single-group** case coherently. Three things must be aligned
before the general multi-group/multi-destination case works end-to-end:

1. **Group assignment.** `configure_mcast` assigns each destination a *different*
   group (`base_mcast_group + destination_index` → .50, .51, …). That models
   "one group per destination", not "one source stream fanned to many
   destinations on a shared group".
2. **Receiver group.** `mcast_receive -g <group>` seeds exactly one `config_map`
   slot (slot 0). A single receiver cannot yet listen on multiple groups (would
   need to populate multiple slots).
3. **Source ↔ receiver group match.** `run_mcast` uses one `mcast_group` var for
   both the source's `mcast_send -g` and every receiver's `-g`, which contradicts
   the per-destination assignment above. With >1 destination the receiver ends up
   on the wrong group → `mcast.o` `XDP_PASS`es → nothing received.

**Two models to implement (future dev):**

- **Shared group (simple fan-out):** all destinations join one group; the source
  sends that group; all receivers use the same `-g`. Change `configure_mcast` to
  register every destination on `mcast_group` (drop the per-destination index).
- **Per-group (independent streams):** N groups; align each destination's `-g`
  and `config_map` entry with the source group feeding it (needs per-destination
  vars in `run_mcast`, and multi-slot `config_map` seeding in `mcast_receive` for
  a receiver that listens on several groups).

**Also note:** mcast latency is a **one-way** measurement (`rx_ns − tx_ns` across
two hosts), so it requires PHC/chrony clock sync between source and destination
(the AMI configures `refclock PHC /dev/ptp0`). `mcast_receive` reports a per-hop
split (source→replicator, replicator→destination) using the replicator's embedded
timestamp when present.

## provision.yaml — What it installs

- Build deps (gcc, clang, libbpf-devel, kernel-headers)
- xdp-tools from source (with AL2023 stdbool.h patch)
- Benchmark binaries → `/opt/af-xdp/`
- ENA PHC + chrony refclock PHC (±50-500ns sync)
- BPF JIT + network sysctl tuning
- systemd units (coalescing, queue headroom, MTU, replicator)
- Reboot for PHC activation

Pass `-e rebuild=true` to skip deps/configs and only rsync + rebuild binaries.

## Supported platforms

- Amazon Linux 2023 (x86_64) — primary target
- Any RHEL 9 / Fedora derivative with `dnf` should work (untested)
- Requires ENA NIC for AF_XDP mode; echo-mode works on any Linux



## Dev - Rsync + Instance upgrade

# 1. Push to fork (from your laptop)
git push fork feature/afxdp-latency-improvements --force

# Check logs
ansible all -i inventory.aws_ec2.yml -b -m shell \
-a 'journalctl -u replicator -n 10 --no-pager' \
--ssh-extra-args="-o StrictHostKeyChecking=no"

# Reset
# Reboot if doesn't help
ansible all -i inventory.aws_ec2.yml -b -m reboot --ssh-extra-args="-o StrictHostKeyChecking=no"

ansible all -i inventory.aws_ec2.yml -b -m shell -a 'systemctl restart replicator && sleep 3 && \
systemctl is-active replicator' --ssh-extra-args="-o StrictHostKeyChecking=no"

# Dev playbook
ansible-playbook -i inventory.aws_ec2.yml sync.yaml
ansible-playbook -i inventory.aws_ec2.yml run_ucast.yaml
ansible-playbook -i inventory.aws_ec2.yml run_ucast.yaml

# For host restart - SSH host key check 
 \ --ssh-extra-args="-o StrictHostKeyChecking=no"