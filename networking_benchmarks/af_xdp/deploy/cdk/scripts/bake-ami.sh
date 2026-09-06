#!/bin/bash
# AMI Bake Script — runs as UserData on the builder instance.
# Installs all static dependencies, builds binaries, writes configs, then signals CFN.
#
# Environment variables (set by CDK UserData):
#   STACK_NAME, REGION, WAIT_HANDLE_URL, GIT_REPO, GIT_REF
set -uo pipefail
exec > /var/log/bake-ami.log 2>&1

echo "=== AMI Bake started at $(date -u) ==="

# ── 0. Self-stop + CFN signal on exit (success or failure) ────────────────────
INSTANCE_ID=$(TOKEN=$(curl -s -X PUT "http://169.254.169.254/latest/api/token" -H "X-aws-ec2-metadata-token-ttl-seconds: 60") && curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/instance-id)
BAKE_EXIT=1  # assume failure unless explicitly set to 0

cleanup() {
  echo "=== Signalling CFN (exit=$BAKE_EXIT) and stopping instance ==="
  # Signal CFN via WaitConditionHandle URL (not --resource)
  TAIL_LOG=$(grep -i "error\|fail\|fatal\|denied\|===\|Step" /var/log/bake-ami.log 2>/dev/null | tail -20 | tr '"' "'" | tr '\n' '|' | cut -c1-1000)
  if [ "$BAKE_EXIT" -eq 0 ]; then
    curl -s -X PUT -H "Content-Type:" --data-binary "{\"Status\":\"SUCCESS\",\"UniqueId\":\"bake\",\"Data\":\"complete\"}" "$WAIT_HANDLE_URL" || true
  else
    curl -s -X PUT -H "Content-Type:" --data-binary "{\"Status\":\"FAILURE\",\"UniqueId\":\"bake\",\"Reason\":\"${TAIL_LOG:-no log}\"}" "$WAIT_HANDLE_URL" || true
  fi
}
trap cleanup EXIT

set -e  # fail-fast after trap is registered

# ── 1. Build dependencies ─────────────────────────────────────────────────────
dnf install -y \
  git clang llvm libbpf-devel elfutils-libelf-devel \
  kernel-headers kernel-devel iproute ethtool \
  make gcc gcc-c++ pkgconfig m4 libpcap-devel rsync \
  python3 python3-pip

# ── 2. xdp-tools ─────────────────────────────────────────────────────────────
if [ ! -f /usr/local/lib/libxdp.so ]; then
  git clone --depth 1 https://github.com/xdp-project/xdp-tools.git /opt/xdp-tools
  cd /opt/xdp-tools

  # AL2023 gcc default is pre-C23: xdp-tools uses bare bool/true/false
  grep -rlZ --include='*.c' --include='*.h' \
      -e '\bbool\b' -e '\btrue\b' -e '\bfalse\b' lib headers 2>/dev/null \
    | while IFS= read -r -d '' f; do
        grep -q 'stdbool.h' "$f" || sed -i '1i #include <stdbool.h>' "$f"
      done

  ./configure
  echo 'CFLAGS += -Wno-error' >> config.mk
  make -j"$(nproc)"
  make install > /dev/null
  ldconfig

  mkdir -p /usr/lib64/bpf
  cp -f lib/libxdp/xdp-dispatcher.o /usr/lib64/bpf/ 2>/dev/null || true
fi

# ── 3. Benchmark binaries ─────────────────────────────────────────────────────
REPO_URL="$GIT_REPO"
REF="$GIT_REF"

echo "=== Step 3: Clone + build benchmark ==="
echo "Cloning $REPO_URL (ref: $REF)..."
if ! git clone --depth 1 --branch "$REF" "$REPO_URL" /tmp/build-src; then
  echo "ERROR: git clone failed. Repo may not be public or ref may not exist."
  echo "Skipping binary build — AMI will have xdp-tools but no benchmark binaries."
  echo "Use ansible rsync to deploy binaries at runtime."
  mkdir -p /opt/af-xdp/xdp
else
  cd /tmp/build-src/networking_benchmarks/af_xdp
  make full
  mkdir -p /opt/af-xdp/xdp
  cp -f replicator rtt mcast_send mcast_receive replicator_ctl udp_send /opt/af-xdp/ 2>/dev/null || true
  cp -f src/xdp/*.o /opt/af-xdp/xdp/ 2>/dev/null || true
fi

# Ensure ec2-user owns everything for dev iteration (rsync + make as ec2-user)
chown -R ec2-user:ec2-user /opt/af-xdp /tmp/build-src 2>/dev/null || true

# ── 3b. Control-plane agent (Go) ──────────────────────────────────────────────
# Build the NATS-driven Go agent from the same checkout so fleet nodes can be
# driven by the central control plane. Uses the latest stable Go from go.dev.
# FATAL on failure: a baked AMI without a working agent silently breaks the
# control plane, so we fail the bake (CFN gets a FAILURE signal) instead.
CP_DIR=/tmp/build-src/networking_benchmarks/af_xdp/control_plane
if [ -d "$CP_DIR/agent" ]; then
  echo "=== Step 3b: build control-plane agent ==="
  [ -f "$CP_DIR/go.mod" ] || { echo "FATAL: $CP_DIR/go.mod missing (gitignored?) — cannot build agent"; exit 1; }
  GOVER=$(curl -sL "https://go.dev/VERSION?m=text" 2>/dev/null | head -1)
  [ -n "$GOVER" ] || { echo "FATAL: could not resolve Go version from go.dev"; exit 1; }
  curl -fsSL "https://go.dev/dl/${GOVER}.linux-amd64.tar.gz" -o /tmp/go.tgz || { echo "FATAL: Go toolchain download failed"; exit 1; }
  rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/go.tgz
  ( cd "$CP_DIR" && GOFLAGS="-mod=mod -buildvcs=false" GOCACHE=/tmp/gocache GOPATH=/tmp/go PATH=/usr/local/go/bin:$PATH \
      go build -o /opt/af-xdp/afxdp-agent ./agent )
  [ -x /opt/af-xdp/afxdp-agent ] || { echo "FATAL: afxdp-agent did not build"; exit 1; }
  echo "afxdp-agent built"
  chown ec2-user:ec2-user /opt/af-xdp/afxdp-agent 2>/dev/null || true
fi

# ── 4. System configs ─────────────────────────────────────────────────────────

# ENA PHC (hardware timestamping)
cat > /etc/modprobe.d/ena-phc.conf <<'EOF'
options ena enable_llq=1 phc_enable=1
EOF

# chrony: ensure include directive exists (AL2023 default omits it)
grep -qE '^include /etc/chrony\.d/\*\.conf' /etc/chrony.conf 2>/dev/null || \
  echo 'include /etc/chrony.d/*.conf' >> /etc/chrony.conf

# chrony: comment out default NTP server (our config takes over)
sed -i 's/^server 169\.254\.169\.123/# server 169.254.169.123/' /etc/chrony.conf 2>/dev/null || true

# chrony: tighter NTP fallback (active until PHC is available post-reboot)
mkdir -p /etc/chrony.d
cat > /etc/chrony.d/aws-ptp.conf <<'EOF'
# AWS Nitro time sync — aggressive NTP polling as PHC fallback.
server 169.254.169.123 prefer iburst minpoll 2 maxpoll 2 xleave polltarget 16
maxslewrate 500
rtcsync
EOF

# chrony: refclock PHC (activated after reboot with phc_enable=1)
cat > /etc/chrony.d/aws-phc.conf <<'EOF'
# ENA PHC (Nitro hypervisor clock) — ±50-500ns accuracy.
# Bypasses NTP-UDP; reads PHC device directly.
# Requires phc_enable=1 (set in /etc/modprobe.d/ena-phc.conf).
# Do NOT use hwtimestamp on ENA — SIOCSHWTSTAMP not supported.
refclock PHC /dev/ptp0 poll 0 dpoll -2 trust prefer
EOF

# BPF JIT
cat > /etc/sysctl.d/99-bpf-xdp.conf <<'EOF'
net.core.bpf_jit_enable = 1
net.core.bpf_jit_harden = 0
net.core.bpf_jit_kallsyms = 1
EOF

# Network tuning (feeder-safe, harmless on other roles)
cat > /etc/sysctl.d/99-network-bench.conf <<'EOF'
net.ipv4.conf.all.mc_forwarding = 0
net.ipv4.conf.default.mc_forwarding = 0
net.ipv4.igmp_qrv = 1
net.ipv4.conf.all.rp_filter = 0
net.ipv4.conf.default.rp_filter = 0
net.core.netdev_max_backlog = 10000
# Socket buffer ceilings. rtt requests SO_RCVBUF=4MB; the kernel silently
# clamps it to rmem_max (AL2023 default 208KB), which caused ~0.6% UDP
# RcvbufErrors (client-side reply drops) under coalesced micro-bursts. Raise the
# ceiling so the 4MB request sticks.
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
EOF

# LIBXDP_OBJECT_PATH for xdp-dispatcher.o lookup
cat > /etc/profile.d/af-xdp.sh <<'EOF'
export LIBXDP_OBJECT_PATH=/usr/lib64/bpf
export PATH=/opt/af-xdp:$PATH
EOF

# ── 4b. CPU isolation for non-competing busy-polling ──────────────────────────
# Core layout (any instance with >=5 online cores, e.g. c7i.4xlarge with nosmt,
# or m8a.2xlarge which has no SMT to disable in the first place):
#   core 0 : OS + NIC IRQs (housekeeping)
#   core 1 : replicator AF_XDP busy-poll thread (queue 0 → core 1)
#   core 2 : receiver (SCHED_FIFO)
#   core 3 : sender   (SCHED_FIFO)
#   core 4 : spare
# isolcpus removes 1-4 from the scheduler's load balancer; nohz_full stops the
# scheduler tick on them; rcu_nocbs offloads RCU callbacks; nosmt disables HT
# siblings on vendors that have SMT (Intel) so each isolated core is a full
# physical core - a no-op on vendors without SMT (AMD m8a, one vCPU per core).
# This 4-core literal is fixed regardless of instance size or vendor; the
# runtime narrows it to what the workload needs and never targets an offline
# core (see initializeCpuCores / derivePins), but it does NOT scale up to use
# more cores on a larger instance (e.g. m8a.metal-24xl's 96 cores) - that is a
# deliberate tradeoff for core-count parity with comparable DPDK benchmarks,
# not an oversight.
ISOL="isolcpus=1-4 nohz_full=1-4 rcu_nocbs=1-4 nosmt intel_idle.max_cstate=0 processor.max_cstate=1 default_hugepagesz=2M hugepagesz=2M hugepages=512"
if ! grep -q "isolcpus=" /etc/default/grub 2>/dev/null; then
  sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=\"|GRUB_CMDLINE_LINUX_DEFAULT=\"${ISOL} |" /etc/default/grub
  grub2-mkconfig -o /boot/grub2/grub.cfg || true
fi

echo "=== CPU isolation cmdline applied (active after reboot) ==="

# ── LIBXDP_OBJECT_PATH end ────────────────────────────────────────────────────

# ── 5. Systemd units ──────────────────────────────────────────────────────────

# Interrupt coalescing (rx-usecs=0 tx-usecs=0)
cat > /etc/systemd/system/ena-coalescing.service <<'EOF'
[Unit]
Description=Disable ENA interrupt coalescing for minimum latency
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'IFACE=$(ip -4 route show default | awk '"'"'{print $5}'"'"' | head -1); ethtool -C "${IFACE:-eth0}" adaptive-rx off || true; ethtool -C "${IFACE:-eth0}" rx-usecs 0 tx-usecs 0 || true'

[Install]
WantedBy=multi-user.target
EOF

# ENA queue: redirect ALL RSS traffic to queue 0 (where AF_XDP socket is bound)
# ENA doesn't support combined=1, so we set indirection table instead.
cat > /etc/systemd/system/ena-xdp-queues.service <<'EOF'
[Unit]
Description=Set ENA RSS indirection to queue 0 for AF_XDP
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'IFACE=$(ip -4 route show default | awk '"'"'{print $5}'"'"' | head -1); ethtool -X "${IFACE:-eth0}" equal 1'

[Install]
WantedBy=multi-user.target
EOF

# MTU 3498 for native XDP (ENA single-page frame requirement)
cat > /etc/systemd/system/ena-mtu.service <<'EOF'
[Unit]
Description=Set MTU 3498 for ENA native XDP
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'IFACE=$(ip -4 route show default | awk '"'"'{print $5}'"'"' | head -1); ip link set "${IFACE:-eth0}" mtu 3498'

[Install]
WantedBy=multi-user.target
EOF

# Pin ENA NIC IRQs to CPU0 so isolated cores 1-4 stay free of interrupt work.
cat > /etc/systemd/system/ena-irq-affinity.service <<'EOF'
[Unit]
Description=Pin ENA NIC IRQs to the first isolated CPU (off contended CPU0; apps run on isolated+1)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'IFACE=$(ip -4 route show default | awk '"'"'{print $5}'"'"' | head -1); ISOL=$(cat /sys/devices/system/cpu/isolated 2>/dev/null); CPU=${ISOL%%-*}; CPU=${CPU:-0}; for irq in $(grep "$IFACE" /proc/interrupts | awk -F: "{print \$1}"); do echo "$CPU" > /proc/irq/$irq/smp_affinity_list 2>/dev/null || true; done'

[Install]
WantedBy=multi-user.target
EOF

# Pin CPU scaling governor to performance (no P-state ramp latency).
cat > /etc/systemd/system/cpu-performance.service <<'EOF'
[Unit]
Description=Pin CPU scaling governor to performance for low latency
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done'

[Install]
WantedBy=multi-user.target
EOF

# Low-latency NAPI for AF_XDP busy-poll RX: the RX consumers (XdpSocket in the
# replicator, and mcast_receive) now busy-poll NAPI in-app via recvfrom/poll with
# SO_BUSY_POLL, so we DEFER hard IRQs (napi_defer_hard_irqs) and let busy-poll own
# the NAPI. gro_flush_timeout is the backstop for busy-poll gaps: 10us — NOT 0
# (strands packets in gaps -> multi-second bursts) and NOT 200us (dominates hop
# latency). See dev/roadmap.md for the full mechanics + measurements.
cat > /etc/systemd/system/ena-rx-lowlat.service <<'EOF'
[Unit]
Description=Low-latency NAPI for AF_XDP busy-poll RX (defer hard IRQs + 10us gro backstop)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'IFACE=$(ip -4 route show default | awk '"'"'{print $5}'"'"' | head -1); echo 2 > /sys/class/net/${IFACE:-eth0}/napi_defer_hard_irqs 2>/dev/null || true; echo 10000 > /sys/class/net/${IFACE:-eth0}/gro_flush_timeout 2>/dev/null || true'

[Install]
WantedBy=multi-user.target
EOF

# Replicator start script (mode/port/group from /etc/default/replicator)
cat > /usr/local/bin/start-replicator.sh <<'EOF'
#!/bin/bash
# Reads config from /etc/default/replicator (or env vars).
# Defaults: ucast mode, port 5000, mcast group 224.0.31.50
set -euo pipefail

CONF=/etc/default/replicator
[ -f "$CONF" ] && . "$CONF"

MODE="${REPLICATOR_MODE:-ucast}"
PORT="${REPLICATOR_PORT:-5000}"
MCAST_GROUP="${REPLICATOR_MCAST_GROUP:-224.0.31.50}"
ZC="${REPLICATOR_ZEROCOPY:-true}"   # AF_XDP zero-copy (ENA supports ZC); 'true'|'false'

IFACE=$(ip -4 route show default | awk '{print $5}' | head -1)
IP=$(ip -4 addr show "$IFACE" | awk '/inet /{print $2}' | cut -d/ -f1)

case "$MODE" in
  echo)    exec /opt/af-xdp/replicator --echo-mode "$IP" "$PORT" ;;
  ucast)   exec /opt/af-xdp/replicator "$IFACE" "$IP" "$PORT" "$ZC" ;;
  mcast)   exec /opt/af-xdp/replicator "$IFACE" "$MCAST_GROUP" "$PORT" "$ZC" --mcast ;;
  *) echo "Unknown REPLICATOR_MODE=$MODE" >&2; exit 1 ;;
esac
EOF
chmod +x /usr/local/bin/start-replicator.sh

# Default config (echo mode — works everywhere, override at runtime)
cat > /etc/default/replicator <<'EOF'
# Replicator configuration — sourced by start-replicator.sh
# Override via ansible, cloud-init, or manual edit.
REPLICATOR_MODE=ucast       # echo | ucast | mcast
REPLICATOR_PORT=5000
REPLICATOR_MCAST_GROUP=224.0.31.50
REPLICATOR_ZEROCOPY=true    # AF_XDP zero-copy (ENA-supported); set false to force copy/DRV mode
EOF

# Replicator systemd service
cat > /etc/systemd/system/replicator.service <<'EOF'
[Unit]
Description=AF_XDP packet replicator
After=network-online.target ena-xdp-queues.service ena-mtu.service
Wants=network-online.target

[Service]
Type=simple
User=root
EnvironmentFile=-/etc/default/replicator
WorkingDirectory=/opt/af-xdp
ExecStart=/usr/local/bin/start-replicator.sh
Restart=on-failure
RestartSec=5
Environment=LIBXDP_OBJECT_PATH=/usr/lib64/bpf

[Install]
WantedBy=multi-user.target
EOF

# ── Control-plane agent unit (NATS-driven; inert until AGENT_NATS_URL resolves) ─
# Reads AGENT_NATS_URL from /etc/default/afxdp-agent, or discovers it from SSM
# /af-xdp/nats-url at start (written by the ControlPlaneStack). AGENT_ROLE comes
# from the instance Role tag via IMDS when instance-metadata-tags are enabled.
cat > /etc/default/afxdp-agent <<'EOF'
# afxdp-agent config. Set AGENT_NATS_URL to the control plane, e.g.:
#   AGENT_NATS_URL=nats://bench.example.com:4222
# AGENT_ROLE (source|replicator|destination) — only if IMDS tags are unavailable.
# AGENT_BIN_DIR=/opt/af-xdp
EOF

cat > /usr/local/bin/afxdp-agent-preflight.sh <<'EOF'
#!/bin/bash
# Resolve AGENT_NATS_URL + AGENT_NATS_TOKEN from SSM (written by ControlPlaneStack)
# when not already set in the env file. Retries until BOTH resolve (the control
# plane may still be booting), then REQUIRES them — so the agent never starts
# half-configured (which manifested as 'no servers' / 'Authorization Violation').
set -uo pipefail
. /etc/default/afxdp-agent 2>/dev/null || true
TOKEN=$(curl -s -X PUT "http://169.254.169.254/latest/api/token" -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null)
REGION=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/placement/region 2>/dev/null)
ssm_get() { aws ssm get-parameter --region "${REGION:-us-east-1}" --name "$1" --query Parameter.Value --output text 2>/dev/null; }
: > /run/afxdp-agent.env
URL="${AGENT_NATS_URL:-}"; TOK="${AGENT_NATS_TOKEN:-}"
for i in $(seq 1 30); do
  [ -z "$URL" ] && { v=$(ssm_get /af-xdp/nats-url);   [ -n "$v" ] && [ "$v" != "None" ] && URL="$v"; }
  [ -z "$TOK" ] && { v=$(ssm_get /af-xdp/nats-token); [ -n "$v" ] && [ "$v" != "None" ] && TOK="$v"; }
  { [ -n "$URL" ] && [ -n "$TOK" ]; } && break
  sleep 2
done
[ -n "$URL" ] && echo "AGENT_NATS_URL=$URL" >> /run/afxdp-agent.env
[ -n "$TOK" ] && echo "AGENT_NATS_TOKEN=$TOK" >> /run/afxdp-agent.env
case "$URL" in tls://*) echo "AGENT_NATS_INSECURE=1" >> /run/afxdp-agent.env;; esac
# Require both before the agent starts; systemd (Restart=always) retries this.
{ [ -n "$URL" ] && [ -n "$TOK" ]; } || { echo "preflight: NATS url/token not resolved yet (control plane up?)" >&2; exit 1; }
exit 0
EOF
chmod +x /usr/local/bin/afxdp-agent-preflight.sh

cat > /etc/systemd/system/afxdp-agent.service <<'EOF'
[Unit]
Description=AF_XDP control-plane agent (NATS-driven)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
ExecStartPre=/usr/local/bin/afxdp-agent-preflight.sh
EnvironmentFile=-/etc/default/afxdp-agent
EnvironmentFile=-/run/afxdp-agent.env
Environment=LIBXDP_OBJECT_PATH=/usr/lib64/bpf
WorkingDirectory=/opt/af-xdp
ExecStart=/opt/af-xdp/afxdp-agent
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
# Disable irqbalance so it can't migrate NIC IRQs onto the isolated cores.
systemctl disable --now irqbalance 2>/dev/null || true
systemctl enable ena-coalescing.service ena-xdp-queues.service ena-mtu.service ena-irq-affinity.service cpu-performance.service ena-rx-lowlat.service replicator.service
# Enable the control-plane agent only if it built (best-effort in Step 3b).
[ -x /opt/af-xdp/afxdp-agent ] && systemctl enable afxdp-agent.service || echo "afxdp-agent not built; unit installed but not enabled"

# ── 6. Cleanup ────────────────────────────────────────────────────────────────
rm -rf /tmp/build-src /opt/xdp-tools
dnf clean all
rm -rf /var/cache/dnf

echo "=== AMI Bake complete at $(date -u) ==="

# ── 7. Mark success (trap handles CFN signal + instance stop) ─────────────────
BAKE_EXIT=0
