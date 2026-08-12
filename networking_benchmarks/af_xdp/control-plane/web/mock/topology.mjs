// mock/topology.mjs — deterministic multi-region / multi-AZ / multi-VPC / PG /
// EC2 fleet used to exercise the 2D/3D web UI locally without a real backend.
//
// Emits BOTH shapes from one source of truth:
//   • live schema   → { nodes:[Node], edges:[Edge] }  (what /api/events streams)
//   • static schema → afxdp.topology/v1 fleet.json     (what ?data= loads)
//
// The live `nodes` carry the extra topology fields (vpc_id, account, pg_type,
// metal) and hardware specs (vcpus/mem_gb/bw_gbps/pps_mpps/enis/nitro_gen) that
// proto.NodeInfo does not model yet — the frontend passes them through when
// present, so a richer backend renders fully with no UI change.

// ── tiny deterministic RNG (mulberry32) so every run is identical ────────────
function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// ── instance-type spec table (for the 3D per-node panel + volume sizing) ─────
const SPECS = {
  'c7gn.4xlarge': { vcpus: 16, mem_gb: 32,  bw_gbps: 50,  pps_mpps: 3, enis: 8,  nitro_gen: 5, metal: false },
  'c7gn.2xlarge': { vcpus: 8,  mem_gb: 16,  bw_gbps: 50,  pps_mpps: 3, enis: 4,  nitro_gen: 5, metal: false },
  'c7gn.metal':   { vcpus: 64, mem_gb: 128, bw_gbps: 200, pps_mpps: 6, enis: 15, nitro_gen: 5, metal: true  },
  'c6in.8xlarge': { vcpus: 32, mem_gb: 64,  bw_gbps: 100, pps_mpps: 5, enis: 8,  nitro_gen: 5, metal: false },
};

// ── fleet definition: 2 regions, 2 accounts, 4 VPCs, 5 AZs, cluster+spread PGs
//    Roles: one replicator per region (mcast fan-out), sources + destinations.
const DEF = [
  // region        account         az            vpc          pg          pg_type    type            role          n
  ['us-east-1',   '111122223333', 'us-east-1a', 'vpc-use1a', 'cpg-nyc-1', 'cluster', 'c7gn.4xlarge', 'source',      1],
  ['us-east-1',   '111122223333', 'us-east-1a', 'vpc-use1a', 'cpg-nyc-1', 'cluster', 'c7gn.4xlarge', 'replicator',  1],
  ['us-east-1',   '111122223333', 'us-east-1a', 'vpc-use1a', 'cpg-nyc-1', 'cluster', 'c7gn.4xlarge', 'destination', 2],
  ['us-east-1',   '111122223333', 'us-east-1a', 'vpc-use1a', 'spg-nyc-1', 'spread',  'c7gn.2xlarge', 'destination', 1],
  ['us-east-1',   '111122223333', 'us-east-1b', 'vpc-use1a', 'cpg-nyc-2', 'cluster', 'c6in.8xlarge', 'destination', 2],
  // 2nd account in the SAME region (us-east-1), its own VPC — multi-account/region.
  ['us-east-1',   '777788889999', 'us-east-1a', 'vpc-use1c', 'cpg-nyc-3', 'cluster', 'c6in.8xlarge', 'destination', 2],
  ['eu-west-1',   '444455556666', 'eu-west-1a', 'vpc-euw1a', 'cpg-dub-1', 'cluster', 'c7gn.metal',   'source',      1],
  ['eu-west-1',   '444455556666', 'eu-west-1a', 'vpc-euw1a', 'cpg-dub-1', 'cluster', 'c7gn.metal',   'replicator',  1],
  ['eu-west-1',   '444455556666', 'eu-west-1a', 'vpc-euw1a', 'cpg-dub-1', 'cluster', 'c7gn.4xlarge', 'destination', 1],
  ['eu-west-1',   '444455556666', 'eu-west-1b', 'vpc-euw1b', 'cpg-dub-2', 'cluster', 'c6in.8xlarge', 'destination', 1],
  // 2nd VPC in the SAME account+region (eu-west-1) — multi-VPC within one account.
  ['eu-west-1',   '444455556666', 'eu-west-1a', 'vpc-euw1c', 'cpg-dub-3', 'cluster', 'c7gn.4xlarge', 'destination', 2],
];

const UCAST_VARS = ['kernel', 'xdp-tx', 'xdp-rx', 'xdp-txrx'];
const MCAST_VARS = ['copy', 'inplace'];
// per-variation one-way service latency multiplier vs the kernel baseline
const VAR_MULT = { kernel: 1.0, 'xdp-tx': 0.82, 'xdp-rx': 0.74, 'xdp-txrx': 0.61, copy: 0.9, inplace: 0.8 };

// octet-per-region so private IPs are visually distinct per region/az
function buildNodes() {
  const nodes = [];
  const perRegionOctet = {};   // region -> next host octet
  const azSubnet = {};         // az -> 2nd/3rd octet pair
  let subnetSeed = 10;
  DEF.forEach((row) => {
    const [region, account, az, vpc, pg, pgType, type, role, count] = row;
    if (!(az in azSubnet)) azSubnet[az] = subnetSeed++;
    for (let k = 0; k < count; k++) {
      const rk = region;
      perRegionOctet[rk] = (perRegionOctet[rk] || 10) + 1;
      const host = perRegionOctet[rk];
      const b = azSubnet[az];
      const priv = `10.${region === 'us-east-1' ? 0 : 1}.${b}.${host}`;
      const pub = `52.${region === 'us-east-1' ? 1 : 2}.${b}.${host}`;
      const id = `i-${region.slice(0, 2)}${az.slice(-1)}${String(host).padStart(3, '0')}${k}`;
      const spec = SPECS[type] || {};
      nodes.push({
        instance_id: id,
        private_ip: priv,
        public_ip: pub,
        az, region, account,
        vpc_id: vpc,
        instance_type: type,
        placement_group: pg,
        pg_type: pgType,
        role,
        stack: `afxdp-${region}`,
        hostname: id,
        ...spec,
        // liveness/state (registry fields)
        agent_version: '0.1.0-mock',
        state: 'idle',
        clock_offset_us: 0,
        last_seen_unix: Math.floor(Date.now() / 1000),
        online: true,
      });
    }
  });
  return nodes;
}

// base one-way service RTT (µs) between two nodes, kernel baseline
function baseLatency(a, b, rand) {
  const jitter = () => 1 + (rand() - 0.5) * 0.12;   // ±6%
  if (a.region !== b.region) return Math.round((68000 + rand() * 6000) * jitter());          // cross-region
  if (a.az !== b.az)         return Math.round((110 + rand() * 60) * jitter());               // cross-AZ, same region
  if (a.placement_group !== b.placement_group) return Math.round((55 + rand() * 20) * jitter()); // same AZ, cross-PG
  return Math.round((34 + rand() * 14) * jitter());                                            // same cluster PG
}

function pct(p50) {
  // build a plausible percentile ladder + tail from a p50
  const p90 = Math.round(p50 * 1.35);
  const p95 = Math.round(p50 * 1.6);
  const p99 = Math.round(p50 * 2.1);
  const p999 = Math.round(p50 * 3.4);
  const max = Math.round(p50 * 5.2);
  const min = Math.round(p50 * 0.7);
  const mean = Math.round(p50 * 1.12);
  return { min, mean, p50, p90, p95, p99, p999, max };
}

function buildEdges(nodes) {
  const rand = rng(0xC0FFEE);
  const edges = [];
  const now = Math.floor(Date.now() / 1000);
  // Precompute a stable kernel baseline per unordered pair so variations scale it.
  const baseOf = new Map();
  const pairKey = (a, b) => (a < b ? a + '|' + b : b + '|' + a);
  for (let i = 0; i < nodes.length; i++)
    for (let j = 0; j < nodes.length; j++) {
      if (i === j) continue;
      const k = pairKey(nodes[i].private_ip, nodes[j].private_ip);
      if (!baseOf.has(k)) baseOf.set(k, baseLatency(nodes[i], nodes[j], rand));
    }

  // ── ucast NxN across all variations ──
  UCAST_VARS.forEach((variation, vi) => {
    for (let i = 0; i < nodes.length; i++)
      for (let j = 0; j < nodes.length; j++) {
        if (i === j) continue;
        const base = baseOf.get(pairKey(nodes[i].private_ip, nodes[j].private_ip));
        const p50 = Math.max(8, Math.round(base * VAR_MULT[variation] * (1 + (rand() - 0.5) * 0.08)));
        const loss = nodes[i].region !== nodes[j].region ? +(rand() * 0.05).toFixed(3) : +(rand() * 0.01).toFixed(3);
        edges.push({
          src: nodes[i].private_ip, dst: nodes[j].private_ip,
          kind: 'ucast', variation,
          unix: now - (UCAST_VARS.length - vi) * 45,   // stagger so combos sort newest-first
          metrics: { service_rtt_us: pct(p50), messages: 5000, lost: Math.round(5000 * loss / 100), loss_pct: loss },
        });
      }
  });

  // ── mcast source→destination per region (adapter splits through replicator) ──
  MCAST_VARS.forEach((variation, vi) => {
    ['us-east-1', 'eu-west-1'].forEach((region) => {
      const src = nodes.find((n) => n.region === region && n.role === 'source');
      const dests = nodes.filter((n) => n.region === region && n.role === 'destination');
      if (!src) return;
      dests.forEach((d) => {
        const base = baseOf.get(pairKey(src.private_ip, d.private_ip));
        const p50 = Math.max(10, Math.round(base * VAR_MULT[variation] * (1 + (rand() - 0.5) * 0.08)));
        const loss = +(rand() * 0.02).toFixed(3);
        edges.push({
          src: src.private_ip, dst: d.private_ip,
          kind: 'mcast', variation,
          unix: now - 10 - vi * 30,
          metrics: { service_rtt_us: pct(p50), messages: 20000, lost: Math.round(20000 * loss / 100), loss_pct: loss },
        });
      });
    });
  });

  return edges;
}

// ── static afxdp.topology/v1 fleet.json (single kind|variation matrix) ───────
export function toStaticFleet(nodes, edges, kind = 'ucast', variation = 'kernel') {
  const order = [...nodes].sort((a, b) => a.private_ip.localeCompare(b.private_ip));
  const idx = new Map(order.map((n, i) => [n.private_ip, i]));
  const fnodes = order.map((n, i) => ({
    index: i, name: n.private_ip, ec2_name: n.role || n.instance_id, type: n.instance_type,
    private_ip: n.private_ip, public_ip: n.public_ip, az: n.az, region: n.region,
    account: n.account, vpc_id: n.vpc_id, cpg_name: n.placement_group, pg_type: n.pg_type,
    role: n.role, vcpus: n.vcpus, mem_gb: n.mem_gb, bw_gbps: n.bw_gbps, pps_mpps: n.pps_mpps,
    enis: n.enis, nitro_gen: n.nitro_gen, metal: !!n.metal, online: true,
  }));
  const N = order.length;
  const matrix = Array.from({ length: N }, () => Array(N).fill(null));
  const relay = kind === 'mcast' ? order.findIndex((n) => n.role === 'replicator') : -1;
  edges.filter((e) => e.kind === kind && e.variation === variation).forEach((e) => {
    const i = idx.get(e.src), j = idx.get(e.dst); if (i == null || j == null) return;
    const m = e.metrics.service_rtt_us;
    const cell = { p50: m.p50, p90: m.p90, p99: m.p99, p999: m.p999, max: m.max, loss: e.metrics.loss_pct };
    if (relay >= 0 && i !== relay && j !== relay) { matrix[i][relay] = matrix[i][relay] || cell; matrix[relay][j] = cell; }
    else matrix[i][j] = cell;
  });
  return {
    schema: 'afxdp.topology/v1',
    generated_at: new Date().toISOString(),
    region: fnodes[0].region, account: fnodes[0].account,
    nodes: fnodes, matrix,
  };
}

export function buildFleet() {
  const nodes = buildNodes();
  const edges = buildEdges(nodes);
  return { nodes, edges };
}
