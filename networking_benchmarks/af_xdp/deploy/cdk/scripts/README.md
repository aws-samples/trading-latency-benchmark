# scripts/

AMI bake script executed as EC2 UserData during the `ami-builder` deployment.

## bake-ami.sh

Runs on a temporary c7i.xlarge instance, takes ~9-10 minutes, produces a universal AMI for all roles.

### Execution flow

1. Install build toolchain (gcc, clang, libbpf-devel, kernel-headers)
2. Build xdp-tools from source (with AL2023 stdbool.h patch)
3. Clone repo + `make full` → install binaries to `/opt/af-xdp/`
4. Write system configs
5. Install systemd units
6. Cleanup (remove build artifacts, dnf cache)
7. Signal CloudFormation success/failure via WaitConditionHandle URL

### Configs written

| File | Purpose |
|------|---------|
| `/etc/modprobe.d/ena-phc.conf` | ENA PHC + LLQ enable (activates `/dev/ptp0` on boot) |
| `/etc/chrony.d/aws-phc.conf` | chrony refclock PHC — ±50-500ns clock sync |
| `/etc/chrony.d/aws-ptp.conf` | Tight NTP fallback (minpoll 2, xleave, maxslewrate 500) |
| `/etc/sysctl.d/99-bpf-xdp.conf` | BPF JIT enable, harden off |
| `/etc/sysctl.d/99-network-bench.conf` | rp_filter off, mc_forwarding off, igmp_qrv=1, backlog 10K, `rmem_max`/`wmem_max`=16M |
| `/etc/profile.d/af-xdp.sh` | PATH + LIBXDP_OBJECT_PATH |
| `/etc/default/replicator` | Replicator mode/port/group/zero-copy config (default: ucast mode) |
| `/usr/local/bin/start-replicator.sh` | Mode-switching wrapper (reads `/etc/default/replicator`) |

### Systemd units

| Unit | Purpose |
|------|---------|
| `ena-coalescing.service` | Disable interrupt coalescing (`adaptive-rx off`, rx-usecs=0, tx-usecs=0) |
| `ena-xdp-queues.service` | Pin RSS to queue 0 (`ethtool -X equal 1`) — all RX lands on the AF_XDP socket's queue |
| `ena-mtu.service` | Set MTU 3498 (ENA native XDP single-page requirement) |
| `ena-irq-affinity.service` | Pin ENA NIC IRQs to CPU0 (keep isolated cores 1-3 quiet); irqbalance disabled |
| `replicator.service` | Packet replicator daemon (mode from `/etc/default/replicator`) |

### CPU isolation (grub cmdline)

The bake appends `isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3 nosmt` to
`GRUB_CMDLINE_LINUX_DEFAULT` (active after first boot). On a 4-physical-core
`c7i.2xlarge`: core 0 = OS + NIC IRQs, core 1 = replicator poll thread, core 2 =
rtt receiver, core 3 = rtt sender — non-competing busy-polling.

### Binaries installed (`/opt/af-xdp/`)

| Binary | Description |
|--------|-------------|
| `replicator` | AF_XDP zero-copy replicator + kernel-mode echo |
| `rtt` | RTT measurement client (kernel `sendto` or `--xdp-tx` AF_XDP send; SO_TIMESTAMP RX + TSC) |
| `mcast_send` | Multicast sender |
| `mcast_receive` | Multicast receiver |
| `replicator_ctl` | Control protocol client (add/remove/list) |
| `udp_send` | UDP connectivity probe |
| `xdp/ucast.o` | Unicast XDP filter (eBPF) |
| `xdp/mcast.o` | Multicast XDP filter (eBPF) |

### Error handling

- `trap cleanup EXIT` — always signals CFN + stops instance on any failure
- TAIL_LOG grep captures error/fail lines for the CFN failure reason
- Instance auto-stops; Lambda handles AMI creation + termination
