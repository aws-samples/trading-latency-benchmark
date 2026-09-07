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
| `/etc/sysctl.d/99-rt-sched.conf` | `kernel.sched_rt_runtime_us = -1` - RT bandwidth throttling disabled (see below) |
| `/etc/sysctl.d/99-network-bench.conf` | rp_filter off, mc_forwarding off, igmp_qrv=1, backlog 10K, rmem_max/wmem_max=16MB |
| `/etc/profile.d/af-xdp.sh` | PATH + LIBXDP_OBJECT_PATH |
| `/etc/default/replicator` | Replicator mode/port/group/zero-copy config (default: ucast mode) |
| `/usr/local/bin/start-replicator.sh` | Mode-switching wrapper (reads `/etc/default/replicator`) |
| `/etc/default/afxdp-agent` | Agent env file (AGENT_NATS_URL, AGENT_ROLE, AGENT_BIN_DIR) |
| `/usr/local/bin/afxdp-agent-preflight.sh` | Agent preflight: resolves NATS URL+token from SSM with retries |

### RT Bandwidth Throttling (disabled)

`/etc/sysctl.d/99-rt-sched.conf` sets `kernel.sched_rt_runtime_us = -1`, turning off RT bandwidth control entirely.

The fleet runs SCHED_FIFO busy-poll threads (`mcast_receive`, `mcast_send`, `rtt`) pinned to `isolcpus` + `nohz_full` cores. On a tickless core the scheduler tick that normally charges RT runtime continuously is stopped, so `update_curr_rt()` runs only at sporadic scheduling events. `rt_time` then accumulates in large deferred lumps instead of smoothly, overshoots the default 950ms/1000ms budget, and the kernel throttles the runqueue to pay it back - descheduling the busy-poll thread for hundreds of milliseconds.

Measured live on an affected run:

| Symptom | Observed |
|---------|----------|
| `rt_time` vs `rt_runtime` | 1704ms vs 950ms (1.8x over) |
| `.rt_throttled` | flipped to 1 five times in a single failing run |
| Packet loss | ~3700 dropped |
| hop2 latency | p99 73ms, max 93ms against a normal p50 near 18us |

Disabling bandwidth control entirely is the standard configuration for RT + `nohz_full`, precisely because the accounting is unreliable on tickless cores. Tightening `sched_rt_period_us` instead was tried and made it **worse** (more frequent lumpy enforcement).

Tradeoff accepted: no last-resort kernel protection against a runaway RT thread monopolising a core. Mitigated by `isolcpus` (those threads never run on the CPU0 housekeeping core, so the OS and SSH stay responsive), each tool's own idle deadline, and the agent wrapping every measurement in `timeout`. See `dev/roadmap/fix.md` (RT-throttling section) for the full investigation.

### Systemd Units

| Unit | Purpose |
|------|---------|
| `ena-coalescing.service` | Disable interrupt coalescing (`adaptive-rx off`, rx-usecs=0, tx-usecs=0) |
| `ena-xdp-queues.service` | Pin RSS to queue 0 (`ethtool -X equal 1`) - all RX to the AF_XDP socket's queue |
| `ena-mtu.service` | Set MTU 3498 (ENA native XDP single-page requirement) |
| `ena-irq-affinity.service` | Pin ENA NIC IRQs to the first isolated CPU (off contended CPU0) |
| `cpu-performance.service` | Pin scaling governor to `performance` (no P-state ramp latency) |
| `ena-rx-lowlat.service` | Low-latency NAPI: `napi_defer_hard_irqs=2` + `gro_flush_timeout=10000` ns |
| `chrony-force-sync.service` | Block until the clock has converged, then step any residual offset (see below) |
| `replicator.service` | Packet replicator daemon (mode from `/etc/default/replicator`); ordered `After=chrony-force-sync.service` |
| `afxdp-agent.service` | Control-plane agent (NATS-driven; enabled only if binary compiled) |

### Forced Clock Convergence (`chrony-force-sync.service`)

A `Type=oneshot` unit (`RemainAfterExit=yes`, `TimeoutStartSec=35`) that runs `chronyc waitsync 30 0.0001 0 0` then `chronyc makestep`. It is ordered `After=chronyd.service network-online.target` and `Before=replicator.service afxdp-agent.service`, so nothing latency-sensitive starts against a skewed clock.

Why the stock config is not enough: chrony here relies on `makestep 1.0 3` (steps only during the first 3 updates, and only if the offset exceeds 1s) plus `refclock PHC ... trust`. Neither guarantees a converged clock by the time the benchmark units start. A post-boot PHC offset under 1s falls through to gradual slewing bounded by `maxslewrate 500` (500ppm), so a real offset seen live on this fleet (53.7ms slow) needs ~107s to correct - long enough to run several benchmark cycles against a skewed clock. `waitsync` blocks until chrony reports converged, but has a timeout so it cannot hang a boot indefinitely; `makestep` then steps any residual offset outright.

**Operational warning:** this unit exists only in the AMI bake script, so any instance launched from an AMI baked **before** this change does not have it. Nodes restarted from such an AMI came up 0.62-0.65s slow and produced bogus one-way measurements (hop1 reading 0, hop2 around 665000us) until `sudo chronyc makestep` was run by hand. Re-bake the AMI so both this unit and the RT sysctl are baked in.

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
