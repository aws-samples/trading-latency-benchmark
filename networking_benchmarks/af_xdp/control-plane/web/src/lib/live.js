// live.js — Live control-plane client. Consumes the backend SSE stream
// (/api/events: snapshot + node/edge/job deltas), keeps the fleet {nodes,edges}
// state, and ADAPTS it into the afxdp.topology/v1 `fleet.json` schema the
// existing 2D/3D viz renders — for a chosen kind (ucast|mcast) + variation.
// Also exposes run-campaign + ad-hoc command POST helpers.

export function createLive({ onUpdate, onJob } = {}) {
  let nodes = [];
  const edges = new Map(); // key: kind|variation|src|dst -> edge
  let es = null;

  const key = (e) => `${e.kind}|${e.variation}|${e.src}|${e.dst}`;

  function apply(msg) {
    switch (msg.type) {
      case 'snapshot':
        nodes = msg.data.nodes || [];
        edges.clear();
        (msg.data.edges || []).forEach((e) => edges.set(key(e), e));
        onUpdate && onUpdate();
        break;
      case 'node': {
        const n = msg.data;
        const i = nodes.findIndex((x) => x.instance_id === n.instance_id);
        if (i >= 0) nodes[i] = n; else nodes.push(n);
        onUpdate && onUpdate();
        break;
      }
      case 'edge':
        edges.set(key(msg.data), msg.data);
        onUpdate && onUpdate();
        break;
      case 'job':
        onJob && onJob(msg.data);
        break;
      case 'error': {
        const id = msg.data && msg.data.instance_id;
        if (id) {
          if (!errorNodes.has(id)) errorNodes.set(id, []);
          errorNodes.get(id).push(msg.data.error || msg.data);
          onUpdate && onUpdate();
        }
        break;
      }
    }
  }

  function connect() {
    es = new EventSource('/api/events');
    es.onmessage = (ev) => { try { apply(JSON.parse(ev.data)); } catch (_) { /* ignore */ } };
    // EventSource auto-reconnects on error; nothing to do.
  }
  connect();

  return {
    close() { if (es) es.close(); },
    nodes: () => nodes,

    // Live counts for the control panel's real-time readout.
    stats() {
      let online = 0;
      for (const n of nodes) if (n.online) online++;
      return { nodes: nodes.length, online, edges: edges.size };
    },

    // Distinct {kind,variation} combos present in the data, each tagged with the
    // latest measurement time (unix), newest first (drives the time-sorted selector).
    combos() {
      const m = new Map(); // kind|variation -> max unix
      for (const e of edges.values()) {
        const k = `${e.kind}|${e.variation}`, u = e.unix || 0;
        if (!m.has(k) || u > m.get(k)) m.set(k, u);
      }
      return [...m.entries()]
        .sort((a, b) => b[1] - a[1])
        .map(([k, unix]) => { const [kind, variation] = k.split('|'); return { kind, variation, unix }; });
    },

    // Kinds present, newest first. At most two (ucast, mcast) - the view selector
    // shows these, and each unifies every variation of that kind.
    kinds() {
      const m = new Map(); // kind -> max unix
      for (const e of edges.values()) {
        const u = e.unix || 0;
        if (!m.has(e.kind) || u > m.get(e.kind)) m.set(e.kind, u);
      }
      return [...m.entries()].sort((a, b) => b[1] - a[1]).map(([kind, unix]) => ({ kind, unix }));
    },

    // Adapt current state into the fleet.json schema for one kind+variation.
    // Only ONLINE nodes are included — the CDK stack may have many more instances
    // than are currently running; showing offline nodes bloats the heatmap with
    // empty cells and confuses the user into thinking the orchestrator is trying
    // to reach them.
    toFleet(kind, variation) {
      const online = nodes.filter((n) => n.online);
      const order = [...online].sort((a, b) => (a.private_ip || '').localeCompare(b.private_ip || ''));
      const idx = new Map(order.map((n, i) => [n.private_ip, i]));
      // Copy an optional field through only when the backend actually reports it,
      // so renderers that show raw specs don't print "undefined" for a node that
      // never provided one.
      const opt = (dst, src, keys) => { for (const k of keys) if (src[k] != null) dst[k] = src[k]; };
      const fnodes = order.map((n, i) => {
        const fn = {
          index: i,
          name: n.private_ip,
          ec2_name: n.role || n.instance_id || n.private_ip,
          type: n.instance_type || 'unknown',
          private_ip: n.private_ip,
          public_ip: n.public_ip || '',
          az: n.az || 'unknown',
          region: n.region || 'us-east-1',
          // Topology grouping fields the 2D contours (VPC/Region/Account) and 3D
          // volumes group by. The agent self-reports vpc_id from IMDS; account is
          // not modelled and falls back to 'unknown', which skips that contour.
          account: n.account || 'unknown',
          vpc_id: n.vpc_id || 'unknown',
          cpg_name: n.placement_group || 'unknown',
          // Prefer the backend's own pg_type; else infer cluster from PG presence.
          pg_type: n.pg_type || (n.placement_group ? 'cluster' : 'unknown'),
          role: n.role || '',
          online: !!n.online,
          metal: n.metal != null ? !!n.metal : /\.metal$/.test(n.instance_type || ''),
        };
        // Hardware specs shown in the 3D per-node panel — carried through when present.
        opt(fn, n, ['vcpus', 'mem_gb', 'bw_gbps', 'pps_mpps', 'enis', 'nitro_gen', 'stack']);
        return fn;
      });
      const N = order.length;
      const matrix = Array.from({ length: N }, () => Array(N).fill(null));
      // mcast is a fan-out THROUGH the replicator, so render the physical
      // source → replicator → destination path (two hops), not a direct
      // source → destination line. The end-to-end one-way metric is attributed
      // to the measured last leg (replicator → dest) and mirrored on the shared
      // first leg (source → replicator) so the path is always honoured.
      const relayIdx = kind === 'mcast' ? order.findIndex((n) => n.role === 'replicator') : -1;
      for (const e of edges.values()) {
        if (e.kind !== kind) continue;
        // variation omitted => unify across all variations of this kind.
        if (variation && e.variation !== variation) continue;
        const i = idx.get(e.src), j = idx.get(e.dst);
        if (i == null || j == null) continue;
        const m = e.metrics && e.metrics.service_rtt_us;
        if (!m) continue;
        const cell = {
          p50: m.p50, p90: m.p90, p99: m.p99, p999: m.p999, max: m.max,
          loss: +(e.metrics.loss_pct || 0).toFixed(3),
          unix: e.unix || 0,
          variation: e.variation,
        };
        // When unifying a kind, the freshest measurement wins the cell.
        const fresher = (prev) => !prev || (cell.unix || 0) >= (prev.unix || 0);
        if (relayIdx >= 0 && i !== relayIdx && j !== relayIdx) {
          matrix[i][relayIdx] = matrix[i][relayIdx] || cell;   // source → replicator (shared)
          if (fresher(matrix[relayIdx][j])) matrix[relayIdx][j] = cell; // replicator → destination
        } else if (fresher(matrix[i][j])) {
          matrix[i][j] = cell;
        }
      }
      return {
        schema: 'afxdp.topology/v1',
        region: (fnodes[0] && fnodes[0].region) || 'us-east-1',
        account: (fnodes[0] && fnodes[0].account) || 'unknown',
        generated_at: new Date().toISOString(),
        nodes: fnodes, matrix,
      };
    },
  };
}

export async function runCampaign(body) {
  const res = await fetch('/api/run', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  });
  return res.json();
}

// Ask the backend to abort the running campaign at its next safe boundary.
export async function cancelCampaign() {
  try { await fetch('/api/cancel', { method: 'POST' }); } catch (_) { /* ignore */ }
}

export async function sendCommand(instanceId, command) {
  const res = await fetch('/api/cmd', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ instance_id: instanceId, command }),
  });
  return res.json();
}
