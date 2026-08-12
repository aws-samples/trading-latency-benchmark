# dev/

Developer iteration tooling - separate from production `deploy/`. Used to build, test, and hot-deploy code to a running fleet during development.

## Structure

```
dev/
├── Dockerfile              Local build + test harness (mirrors the AMI bake)
└── ansible/                Dev playbooks + shared-inventory symlink
    ├── sync.yaml           rsync local source → fleet EC2s + control-plane, rebuild, restart
    ├── ansible.cfg         Enables amazon.aws.aws_ec2 inventory plugin + disables SSH multiplexing
    ├── provision.yaml      Full install on stock AL2023 (no baked AMI)
    ├── run_tests.yaml      Run the pytest suite on the fleet
    └── inventory.aws_ec2.yml → symlink to ../../deploy/ansible/inventory.aws_ec2.yml
```

## Recommended: `afxdpctl sync`

The `afxdpctl` CLI (at `control_plane/cmd/afxdpctl`) wraps the dev ansible for the common hot-deploy case:

```bash
# Hot-deploy local code to all fleet nodes (rsync → make → agent rebuild → restart):
afxdpctl sync --key ~/.ssh/virginia.pem --region us-east-1
```

This is equivalent to running `ansible-playbook sync.yaml` but handles env vars and paths for you.

## Dockerfile - Local Build + Test

Mirrors the AMI bake (xdp-tools + `make full`), then runs pytest in echo mode (no root/XDP). Validates that the code **compiles** exactly as the bake will, before spending ~9 min on an EC2 bake.

Build for `linux/amd64` (the Makefile targets x86_64; emulated on Apple Silicon):

```bash
# From af_xdp/ root:
docker build --platform linux/amd64 -f dev/Dockerfile -t afxdp-test .
docker run --rm --platform linux/amd64 afxdp-test            # runs pytest tests/
```

The Dockerfile:
1. Installs the same toolchain as `bake-ami.sh` (gcc, clang, libbpf-devel, kernel-headers)
2. Builds xdp-tools from source (with the AL2023 stdbool.h patch)
3. Runs `make full` (validates compilation)
4. Default CMD: `pytest tests/ -v --tb=short`

## ansible/ - Dev Iteration on a Running Fleet

Run from `dev/ansible/` (the inventory is symlinked here). Requires `SSH_KEY_FILE` and `AWS_DEFAULT_REGION`.

### Prerequisites

```bash
export SSH_KEY_FILE=~/.ssh/virginia.pem
export AWS_DEFAULT_REGION=us-east-1
# For cross-region:
export SSH_KEY_FILE_SECONDARY=~/.ssh/london.pem
```

A fleet must be running and tagged by `Role` (deployed via CDK or manually tagged).

### sync.yaml - Hot-deploy Local Code Changes

Syncs the local `af_xdp/` tree to all nodes, rebuilds, and restarts. Two
different paths run depending on the target host's group:

```bash
cd dev/ansible
ansible-playbook -i inventory.aws_ec2.yml sync.yaml

# Limit to specific role:
ansible-playbook -i inventory.aws_ec2.yml sync.yaml --limit replicator

# Limit to the control-plane only:
ansible-playbook -i inventory.aws_ec2.yml sync.yaml --limit control_plane
```

**Fleet path** (`source`/`replicator`/`destination` hosts):
1. `rsync` local source → `/home/ec2-user/af_xdp/` (excludes `.git`, `node_modules`, `cdk.out`, `*.o`, `*.d`, binaries)
2. `make clean && make full` on each node
3. Verify the built replicator supports AF_XDP (not echo-mode-only)
4. Copy binaries to `/opt/af-xdp/`
5. **Build the fleet node's Go agent** from the synced source (uses baked Go toolchain at `/usr/local/go`; skips cleanly if the `control_plane/agent` dir or toolchain is absent)
6. **Fix source tree ownership back to `ec2-user`** - the build steps above run under `become: yes`, so their output would otherwise leave root-owned files that block the *next* sync's unprivileged rsync
7. **Restart `afxdp-agent.service`** (enables it if not already enabled)
8. Detach stale XDP programs, reset interface, set RSS to queue 0
9. Restart `replicator.service`
10. Verify the replicator is active

**Control-plane path** (the single `XdpStack-ControlPlane` host, matched by its
CFN stack-name tag - it carries no `Role` tag, so it's not covered by the fleet
groups above): after the same rsync, it instead builds `afxdp-backend` (`go
build ./backend`), rebuilds the web dist (`npm run build` in `control-plane/web`),
fixes ownership the same way, and restarts `cp-backend.service` - none of the
fleet-only C++/XDP/replicator steps apply to this host.

The rsync excludes build artifacts (`*.o`, `*.d`, binaries) to prevent a stale host-compiled object (e.g. one with `-DECHO_MODE_ONLY`) from contaminating the remote build.

**`dev/ansible/ansible.cfg`** carries two fixes that make this playbook actually
work, both non-obvious from the task list above:
- `[inventory] enable_plugins` explicitly enables the `amazon.aws.aws_ec2`
  dynamic-inventory plugin - without it, `ansible-core` silently falls back to
  treating `inventory.aws_ec2.yml` as a static, unreachable host and the whole
  playbook "succeeds" against zero real hosts.
- `[ssh_connection] ssh_args` disables SSH connection multiplexing
  (`ControlMaster=no`, `ControlPersist=0`). Some local SSH client wrappers
  intermittently choke on the reused control socket mid-playbook; a fresh
  connection per task is slower but was reliable across repeated full runs
  where multiplexing was not.

### provision.yaml - Full Provision on Stock AL2023

For instances without the baked AMI (development or BYOI):

```bash
ansible-playbook -i inventory.aws_ec2.yml provision.yaml

# Rebuild only (skip deps/configs - after code changes):
ansible-playbook -i inventory.aws_ec2.yml provision.yaml -e rebuild=true

# Skip the automatic reboot (defer it yourself):
ansible-playbook -i inventory.aws_ec2.yml provision.yaml -e grub_reboot=false
```

**What it installs (~8 min first run):**
- Build deps (gcc, clang, libbpf-devel, kernel-headers, etc.)
- xdp-tools from source (with AL2023 stdbool.h patch) → `/usr/local/lib/libxdp.so`
- Benchmark binaries (`make full`) → `/opt/af-xdp/`
- ENA PHC + chrony refclock PHC (hardware timestamping)
- BPF JIT + network sysctl tuning (rmem_max/wmem_max = 16MB)
- CPU isolation: `isolcpus=1-4 nohz_full=1-4 rcu_nocbs=1-4 nosmt` (grub)
- Low-latency: `intel_idle.max_cstate=0`, hugepages, scaling governor = performance
- Systemd units (`ena-rx-lowlat`, `cpu-performance`, coalescing, queues, MTU)
- Reboot for kernel cmdline + PHC activation
- Verifies PHC active after reboot

### run_tests.yaml - Run Integration Tests

Runs the pytest suite on all fleet nodes in echo mode:

```bash
ansible-playbook -i inventory.aws_ec2.yml run_tests.yaml

# Limit to specific nodes:
ansible-playbook -i inventory.aws_ec2.yml run_tests.yaml --limit replicator
```

**What it does:**
1. Ensures `pytest` is installed
2. Stops `replicator.service` (tests spawn their own echo-mode replicator on port 23456/29000)
3. Runs `pytest tests/ -v --tb=short`
4. Always restarts `replicator.service` after tests (even on failure)
5. Fails the play if any tests failed

### inventory.aws_ec2.yml

Symlink to `../../deploy/ansible/inventory.aws_ec2.yml`. Uses the `amazon.aws.aws_ec2` plugin to discover fleet instances by `Role` tag across `us-east-1` and `eu-west-2`.

Groups:
- `source` - Role=source
- `replicator` - Role=replicator
- `destination` - Role=destination

Auto-selects the SSH key per region (primary vs secondary).

## Typical Dev Loop

```bash
# 1. Make code changes locally (edit src/, tools/, control-plane/, etc.)

# 2. Validate compilation locally (optional but fast):
docker build --platform linux/amd64 -f dev/Dockerfile -t afxdp-test . && \
docker run --rm --platform linux/amd64 afxdp-test

# 3. Hot-deploy to fleet:
afxdpctl sync --key ~/.ssh/virginia.pem

# 4. Run tests:
cd dev/ansible && ansible-playbook -i inventory.aws_ec2.yml run_tests.yaml

# 5. Run benchmarks:
afxdpctl run ucast kernel
afxdpctl report -o run.html
```
