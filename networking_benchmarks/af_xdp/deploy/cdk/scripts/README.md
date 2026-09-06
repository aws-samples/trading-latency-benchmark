# scripts/

AMI bake script executed as EC2 UserData during the `ami-builder` deployment.

## bake-ami.sh

Runs on a temporary builder instance (default `m8a.2xlarge`), takes ~9-10 minutes, produces a universal AMI for all roles. The build targets `-march=x86-64-v3` (not `-march=native`), so the resulting binaries run correctly regardless of which vendor (Intel or AMD) the builder or the fleet nodes happen to be.

### Execution Flow

| Step | Duration | Action |
|------|----------|--------|
| 0 | - | Register `trap cleanup EXIT` (signals CFN + stops instance on any exit) |
| 1 | ~90s | Install build toolchain (gcc, clang, libbpf-devel, kernel-headers, etc.) |
| 2 | ~120s | Build xdp-tools from source (with AL2023 stdbool.h patch) |
| 3 | ~60s | Clone repo (`$GIT_REPO` @ `$GIT_REF`) + `make full` → install binaries to `/opt/af-xdp/` |
| 3b | ~30s | **Build control-plane agent (Go)** - downloads latest Go, builds `./agent` → `/opt/af-xdp/afxdp-agent`. **FATAL on failure** (AMI without a working agent breaks the control plane silently). |
| 4 | - | Write system configs (sysctl, chrony, modprobe, grub cmdline, profile) |
| 5 | - | Install + enable systemd units |
| 6 | - | Cleanup (remove build artifacts, dnf cache) |
| 7 | - | Mark `BAKE_EXIT=0` (trap signals CFN SUCCESS + stops instance) |

### Key Behavior Changes (current)

- **Agent build is FATAL:** Step 3b exits non-zero (which trips `set -e` → triggers the cleanup trap → CFN FAILURE signal) if the Go agent doesn't compile. This is intentional - a baked AMI without `afxdp-agent` silently breaks control-plane-driven operation.
- **Preflight retries SSM:** The baked `afxdp-agent-preflight.sh` retries SSM lookups for up to 60 seconds (30 × 2s) so that fleet nodes can tolerate the control-plane still booting when they come up.
- **Binary-less fallback:** If the git clone fails (private repo, wrong ref), the bake continues *without* benchmark binaries (xdp-tools still installed) and prints a warning. Binaries can be deployed at runtime via ansible `sync.yaml`. The agent build is still attempted if the clone *partially* succeeded.

### Configs Written

| File | Purpose |
|------|---------|
| `/etc/modprobe.d/ena-phc.conf` | ENA PHC + LLQ enable (activates `/dev/ptp0` on boot) |
| `/etc/chrony.d/aws-phc.conf` | chrony refclock PHC - ±50-500ns clock sync via Nitro hypervisor |
| `/etc/chrony.d/aws-ptp.conf` | Tight NTP fallback (minpoll 2, xleave, maxslewrate 500) |
| `/etc/sysctl.d/99-bpf-xdp.conf` | BPF JIT enable, harden off |
| `/etc/sysctl.d/99-network-bench.conf` | rp_filter off, mc_forwarding off, igmp_qrv=1, backlog 10K, rmem_max/wmem_max=16MB |
| `/etc/profile.d/af-xdp.sh` | PATH + LIBXDP_OBJECT_PATH |
| `/etc/default/replicator` | Replicator mode/port/group/zero-copy config (default: ucast mode) |
| `/usr/local/bin/start-replicator.sh` | Mode-switching wrapper (reads `/etc/default/replicator`) |
| `/etc/default/afxdp-agent` | Agent env file (AGENT_NATS_URL, AGENT_ROLE, AGENT_BIN_DIR) |
| `/usr/local/bin/afxdp-agent-preflight.sh` | Agent preflight: resolves NATS URL+token from SSM with retries |

### Systemd Units

| Unit | Purpose |
|------|---------|
| `ena-coalescing.service` | Disable interrupt coalescing (`adaptive-rx off`, rx-usecs=0, tx-usecs=0) |
| `ena-xdp-queues.service` | Pin RSS to queue 0 (`ethtool -X equal 1`) - all RX to the AF_XDP socket's queue |
| `ena-mtu.service` | Set MTU 3498 (ENA native XDP single-page requirement) |
| `ena-irq-affinity.service` | Pin ENA NIC IRQs to the first isolated CPU (off contended CPU0) |
| `cpu-performance.service` | Pin scaling governor to `performance` (no P-state ramp latency) |
| `ena-rx-lowlat.service` | Low-latency NAPI: `napi_defer_hard_irqs=2` + `gro_flush_timeout=10000` ns |
| `replicator.service` | Packet replicator daemon (mode from `/etc/default/replicator`) |
| `afxdp-agent.service` | Control-plane agent (NATS-driven; enabled only if binary compiled) |

### CPU Isolation (grub cmdline)

The bake appends to `GRUB_CMDLINE_LINUX_DEFAULT`:

```
isolcpus=1-4 nohz_full=1-4 rcu_nocbs=1-4 nosmt
intel_idle.max_cstate=0 processor.max_cstate=1
default_hugepagesz=2M hugepagesz=2M hugepages=512
```

Active after first boot. `isolcpus=1-4` is a fixed 4-core literal regardless of instance size or vendor: on an 8-online-core instance that leaves core 0 = OS + housekeeping, cores 1-4 = isolated for datapath, cores 5-7 unused. The runtime narrows this to what the workload needs (replicator `initializeCpuCores`, `run_ucast.yaml` `auto_pin`, `run_mcast` pinning) and never targets an offline core regardless of how SMT/`nosmt` shrinks the online set (see `nosmt` note above - it's a no-op on `m8a`, which has no SMT).

### Binaries Installed (`/opt/af-xdp/`)

| Binary | Description |
|--------|-------------|
| `replicator` | AF_XDP zero-copy packet replicator + echo-mode fallback |
| `rtt` | RTT measurement client (kernel or `--xdp-tx` AF_XDP send; SO_TIMESTAMP RX + TSC) |
| `mcast_send` | Multicast sender (m2u encapsulation) |
| `mcast_receive` | Multicast receiver (busy-poll AF_XDP) |
| `replicator_ctl` | Control protocol client (add/remove/list destinations) |
| `udp_send` | UDP connectivity probe |
| `afxdp-agent` | Control-plane Go agent (NATS-driven) |
| `xdp/ucast.o` | Unicast XDP filter (eBPF bytecode) |
| `xdp/mcast.o` | Multicast XDP filter (eBPF bytecode) |

### Error Handling

- `trap cleanup EXIT` - always signals CFN + stops instance on any failure
- TAIL_LOG grep captures error/fail lines for the CFN failure reason (max 1000 chars)
- Instance auto-stops; Lambda handles AMI creation + termination
- If Lambda finds the bake failed, it terminates the builder and deregisters any partial AMI
