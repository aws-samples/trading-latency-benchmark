// mock/server.mjs — zero-dependency mock AF_XDP control-plane for local UI work.
//
// Speaks the exact wire contract the Svelte app expects (see backend/api.go):
//   GET  /api/events   SSE: {type:"snapshot"} then node/edge/job deltas + keepalive
//   GET  /api/fleet    full {nodes,edges} snapshot (late joiners / ?data path parity)
//   POST /api/run      start a campaign; streams job running/progress/done + fresh edges
//   POST /api/cancel   abort the running campaign at its next boundary
//   POST /api/cmd      ad-hoc agent command (echoes an ok result)
//   GET  /api/measurements  flat, store-backed measurement rows for the report
//        page (Latest measurements, per-mode heatmaps, All measurements) -
//        the general-purpose counterpart to /api/mcast-replicators, covering
//        ucast too. Mirrors backend store.LatestMeasurements: one row per
//        distinct (kind,variation,src,dst,replicator) edge, replicator_ip=""
//        for ucast. This is what the report reads instead of the live
//        edgeStore/fleet.matrix, which can only ever hold ONE value per
//        (src,dst) pair.
//   GET  /api/mcast-replicators  per-replicator mcast history for the report's
//        "Per-replicator paths" section - the fleet has TWO replicators per
//        region (different PG/AZ), so this is seeded on startup and grows as
//        mcast campaigns run.
//   GET  /api/errors   node error registry (always empty here - mock campaigns
//        never fail; present for API-surface parity, not called by the web UI)
//   GET  /healthz      ok
//
// Vite's dev proxy forwards /api/{events,run,cancel,cmd,measurements,mcast-replicators,errors}
// here (CP_URL, default :8080), so `npm run dev` gets a live, streaming
// multi-region fleet with no real backend. Run:  node mock/server.mjs   (PORT overrides 8080)
//
// Emits richer job/edge deltas on /api/run so the Test-Latency buttons, the
// Show-combo dropdown, mcast replicator-split rendering, and live re-render are
// all exercised end to end.

import http from 'node:http';
import { buildFleet } from './topology.mjs';

const PORT = parseInt(process.env.PORT || '8080', 10);
const { nodes, edges } = buildFleet();

// edge store keyed like the Go collector: kind|variation|src|dst
const edgeKey = (e) => `${e.kind}|${e.variation}|${e.src}|${e.dst}`;
const edgeStore = new Map(edges.map((e) => [edgeKey(e), e]));

// ── /api/measurements store, mirroring backend store.LatestMeasurements ──────
// Flat rows keyed by (kind,variation,src,dst,replicator_ip) - the general
// counterpart to mcastReplicatorResults below, covering ucast too. Unlike
// edgeStore (one value per (kind,variation,src,dst), overwritten by whichever
// replicator measured last), this keeps every replicator's numbers distinct,
// same as the real SQLite-backed endpoint.
const measurementRows = new Map(); // key -> row

function nodeTopo(ip) {
  const n = byIP(ip);
  return n ? { role: n.role, az: n.az, vpc: n.vpc_id, pg: n.placement_group, region: n.region } : {};
}

function recordMeasurement(kind, variation, srcIp, dstIp, p50, replicator, hop1p50, hop2p50) {
  const replicatorIp = replicator ? replicator.private_ip : '';
  const key = `${kind}|${variation}|${srcIp}|${dstIp}|${replicatorIp}`;
  const st = nodeTopo(srcIp), dt = nodeTopo(dstIp);
  const row = {
    run_id: 0, unix: Math.floor(Date.now() / 1000), kind, variation,
    src_ip: srcIp, dst_ip: dstIp,
    p50, p90: Math.round(p50 * 1.35), p99: Math.round(p50 * 2.1), p999: Math.round(p50 * 3.4), max: Math.round(p50 * 5.2),
    loss_pct: 0, messages: kind === 'mcast' ? 20000 : 10000,
    replicator_id: replicator ? replicator.instance_id : '', replicator_ip: replicatorIp,
    replicator_pg: replicator ? replicator.placement_group : '', replicator_az: replicator ? replicator.az : '',
    replicator_vpc: replicator ? replicator.vpc_id : '',
    src_role: st.role || '', dst_role: dt.role || '', src_az: st.az || '', dst_az: dt.az || '',
    src_vpc: st.vpc || '', dst_vpc: dt.vpc || '', src_pg: st.pg || '', dst_pg: dt.pg || '',
    src_region: st.region || '', dst_region: dt.region || '',
    // hop1_us/hop2_us on the real mcast_receive tool - source->replicator and
    // replicator->destination legs of the one-way path. Only present for
    // mcast measurements that came with a replicator (has_replicator_ts in
    // the real C++ tool); null (omitted) for ucast, matching the real
    // backend's nullable columns.
    hop1_p50: hop1p50 != null ? hop1p50 : null,
    hop1_p99: hop1p50 != null ? Math.round(hop1p50 * 1.6) : null,
    hop2_p50: hop2p50 != null ? hop2p50 : null,
    hop2_p99: hop2p50 != null ? Math.round(hop2p50 * 1.6) : null,
  };
  measurementRows.set(key, row);
}

// Seed from the existing edgeStore (ucast only - mcast rows always need a
// replicator, provided separately by seedMcastReplicatorResults) so
// /api/measurements has ucast data even before any live run in this session.
function seedMeasurementsFromEdges() {
  for (const e of edgeStore.values()) {
    if (e.kind !== 'ucast') continue;
    recordMeasurement(e.kind, e.variation, e.src, e.dst, e.metrics.service_rtt_us.p50, null);
  }
}

// ── per-replicator mcast history, mirroring backend store.LatestMcastReplicatorResults ──
// The live edgeStore above is keyed only by (kind,variation,src,dst) and gets
// overwritten as each replicator in a sweep measures the same destination -
// exactly the limitation the real store-backed endpoint exists to route around.
// This list is what GET /api/mcast-replicators serves; the report page reads
// it, not edgeStore, for its "Per-replicator paths" section.
let mcastRunSeq = 0;
const mcastReplicatorResults = [];
function recordMcastReplicatorResult(mode, relay, src, dst) {
  const base = relay.az !== src.az ? 130 : 40; // cross-AZ replicator path costs more, like the real fleet
  const p50 = jitter(base);
  // hop1 (source->replicator) and hop2 (replicator->destination) split the
  // one-way path, mirroring mcast_receive.cpp's real hop1_us/hop2_us. Split
  // roughly evenly (hop2 costs a bit more, matching the tools/README's
  // measured ~26us/~31-37us hop1/hop2 shape) rather than reusing the combined
  // p50 for both legs, which would silently make hop1+hop2 != the one-way total.
  const hop1p50 = Math.max(6, Math.round(p50 * 0.42));
  const hop2p50 = Math.max(6, p50 - hop1p50);
  mcastReplicatorResults.push({
    run_id: ++mcastRunSeq,
    started_at: Math.floor(Date.now() / 1000),
    mode,
    replicator_id: relay.instance_id,
    replicator_ip: relay.private_ip,
    replicator_pg: relay.placement_group,
    replicator_az: relay.az,
    replicator_vpc: relay.vpc_id,
    src_ip: src.private_ip,
    dst_ip: dst.private_ip,
    p50, p90: Math.round(p50 * 1.35), p99: Math.round(p50 * 2.1), p999: Math.round(p50 * 3.4), max: Math.round(p50 * 5.2),
    loss_pct: 0,
    unix: Math.floor(Date.now() / 1000),
    hop1_p50: hop1p50, hop1_p99: Math.round(hop1p50 * 1.6),
    hop2_p50: hop2p50, hop2_p99: Math.round(hop2p50 * 1.6),
  });
  // Same event, recorded into the general measurements store too (mode is the
  // mcast "variation" in both backend schemas).
  recordMeasurement('mcast', mode, src.private_ip, dst.private_ip, p50, relay, hop1p50, hop2p50);
}

// Seed one result per (replicator, mode, destination) so the report's
// "Per-replicator paths" section has data to show even before a live mcast
// campaign runs in this session - matches how a real fleet already has
// history in the store from prior campaigns. Invoked below, once `jitter` is
// defined (recordMcastReplicatorResult needs it).
function seedMcastReplicatorResults() {
  for (const region of ['us-east-1', 'eu-west-1']) {
    const src = nodes.find((n) => n.region === region && n.role === 'source');
    const replicators = nodes.filter((n) => n.region === region && n.role === 'replicator');
    const dests = nodes.filter((n) => n.region === region && n.role === 'destination');
    if (!src || !replicators.length) continue;
    for (const mode of ['copy', 'inplace']) {
      for (const relay of replicators) {
        for (const d of dests) recordMcastReplicatorResult(mode, relay, src, d);
      }
    }
  }
}

// ── SSE subscriber hub ───────────────────────────────────────────────────────
const clients = new Set();
function broadcast(obj) {
  const line = `data: ${JSON.stringify(obj)}\n\n`;
  for (const res of clients) { try { res.write(line); } catch { /* dropped */ } }
}
const sse = (res, obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

// ── campaign engine (one at a time, cancellable) ─────────────────────────────
let running = null;   // { cancel:boolean }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const byIP = (ip) => nodes.find((n) => n.private_ip === ip);
const jitter = (v) => Math.max(6, Math.round(v * (1 + (Math.random() - 0.5) * 0.06)));
seedMeasurementsFromEdges(); // needs `byIP`, defined above
seedMcastReplicatorResults(); // needs `jitter`, defined above

function bumpEdge(kind, variation, src, dst) {
  const e = edgeStore.get(`${kind}|${variation}|${src}|${dst}`);
  const now = Math.floor(Date.now() / 1000);
  // Only ucast records here: mcast's attributed measurement is written by
  // recordMcastReplicatorResult (which knows the replicator), called
  // separately in runMcast right after this. Recording an unattributed mcast
  // row here too would create noise /api/measurements doesn't have in the
  // real backend (mcast always has a replicator; there is no "no replicator"
  // mcast measurement).
  const recordHere = kind === 'ucast';
  if (e) {
    const m = e.metrics.service_rtt_us, p50 = jitter(m.p50);
    e.metrics.service_rtt_us = { ...m, p50, p90: Math.round(p50 * 1.35), p99: Math.round(p50 * 2.1), p999: Math.round(p50 * 3.4), max: Math.round(p50 * 5.2) };
    e.unix = now;
    broadcast({ type: 'edge', data: e });
    if (recordHere) recordMeasurement(kind, variation, src, dst, p50, null);
    return;
  }
  // Synthesize an edge if the pair wasn't pre-seeded for this variation.
  const a = byIP(src), b = byIP(dst); if (!a || !b) return;
  const p50 = jitter(a.region !== b.region ? 70000 : a.az !== b.az ? 130 : 40);
  const ne = { src, dst, kind, variation, unix: now,
    metrics: { service_rtt_us: { min: p50 - 5, mean: p50 + 3, p50, p90: Math.round(p50 * 1.35), p95: Math.round(p50 * 1.6), p99: Math.round(p50 * 2.1), p999: Math.round(p50 * 3.4), max: Math.round(p50 * 5.2) }, messages: 5000, lost: 0, loss_pct: 0 } };
  edgeStore.set(edgeKey(ne), ne);
  broadcast({ type: 'edge', data: ne });
  if (recordHere) recordMeasurement(kind, variation, src, dst, p50, null);
}

async function runUcast(variation) {
  const online = nodes.filter((n) => n.online);
  const pairs = [];
  for (const a of online) for (const b of online) if (a !== b) pairs.push([a, b]);
  broadcast({ type: 'job', data: { status: 'running', kind: 'ucast', variation, pairs: pairs.length, rounds: 1 } });
  let done = 0;
  for (const [a, b] of pairs) {
    if (running?.cancel) { broadcast({ type: 'job', data: { status: 'cancelled', kind: 'ucast', variation, done, total: pairs.length } }); return; }
    bumpEdge('ucast', variation, a.private_ip, b.private_ip);
    done++;
    broadcast({ type: 'job', data: { status: 'progress', kind: 'ucast', variation, done, total: pairs.length, src: a.private_ip, dst: b.private_ip, ok: true } });
    await sleep(18);
  }
  broadcast({ type: 'job', data: { status: 'done', kind: 'ucast', variation, done, total: pairs.length } });
}

async function runMcast(mode) {
  for (const region of ['us-east-1', 'eu-west-1']) {
    if (running?.cancel) break;
    const src = nodes.find((n) => n.region === region && n.role === 'source');
    const replicators = nodes.filter((n) => n.region === region && n.role === 'replicator');
    const dests = nodes.filter((n) => n.region === region && n.role === 'destination');
    if (!src || !replicators.length) continue;
    // Mirrors the real backend's top-level "running" event shape: a
    // "replicators" list of formatted strings, emitted once before the sweep,
    // followed by a per-replicator "progress"/"phase":"replicator" event as
    // each one starts (see orchestrator.go RunMcastMatrix/runMcastForReplicator).
    const replDesc = replicators.map((r) => `${r.private_ip}(pg=${r.placement_group},az=${r.az})`);
    broadcast({ type: 'job', data: { status: 'running', kind: 'mcast', mode, source: src.private_ip,
      replicators: replDesc, dests: dests.length } });
    for (const [ri, relay] of replicators.entries()) {
      if (running?.cancel) break;
      broadcast({ type: 'job', data: { status: 'progress', kind: 'mcast', mode, phase: 'replicator',
        replicator: relay.private_ip, replicator_pg: relay.placement_group, replicator_az: relay.az,
        msg: `replicator ${ri + 1}/${replicators.length}: ${relay.private_ip} (pg=${relay.placement_group}, az=${relay.az})` } });
      for (const d of dests) {
        if (running?.cancel) { broadcast({ type: 'job', data: { status: 'cancelled', kind: 'mcast', mode } }); return; }
        bumpEdge('mcast', mode, src.private_ip, d.private_ip);
        recordMcastReplicatorResult(mode, relay, src, d);
        broadcast({ type: 'job', data: { status: 'progress', kind: 'mcast', mode, src: src.private_ip, dst: d.private_ip,
          replicator: relay.private_ip, ok: true } });
        await sleep(120);
      }
      broadcast({ type: 'job', data: { status: 'replicator_done', kind: 'mcast', mode, replicator: relay.private_ip, ok: true } });
    }
  }
  broadcast({ type: 'job', data: { status: 'done', kind: 'mcast', mode } });
}

async function runCampaign(body) {
  running = { cancel: false };
  try {
    if (body.kind === 'mcast') {
      for (const m of (body.modes && body.modes.length ? body.modes : ['copy'])) {
        if (running.cancel) break;
        await runMcast(m);
      }
    } else {
      await runUcast(body.variation && body.variation !== 'all' ? body.variation : 'kernel');
    }
  } finally { running = null; }
}

// ── HTTP ─────────────────────────────────────────────────────────────────────
function readBody(req) {
  return new Promise((resolve) => {
    let b = ''; req.on('data', (c) => (b += c)); req.on('end', () => { try { resolve(JSON.parse(b || '{}')); } catch { resolve({}); } });
  });
}
const json = (res, code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' }); res.end(JSON.stringify(obj)); };

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const path = url.pathname;

  if (path === '/healthz') { res.writeHead(200); res.end('ok'); return; }

  if (path === '/api/fleet' && req.method === 'GET') {
    return json(res, 200, { generated_unix: Math.floor(Date.now() / 1000), nodes, edges: [...edgeStore.values()] });
  }

  if (path === '/api/mcast-replicators' && req.method === 'GET') {
    return json(res, 200, { results: mcastReplicatorResults });
  }

  if (path === '/api/measurements' && req.method === 'GET') {
    const kindFilter = url.searchParams.get('kind');
    const sinceUnix = Number(url.searchParams.get('since_unix')) || 0;
    let results = [...measurementRows.values()];
    if (kindFilter) results = results.filter((r) => r.kind === kindFilter);
    if (sinceUnix) results = results.filter((r) => r.unix >= sinceUnix);
    return json(res, 200, { results, since_unix: sinceUnix });
  }

  if (path === '/api/events' && req.method === 'GET') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive', 'Access-Control-Allow-Origin': '*' });
    sse(res, { type: 'snapshot', data: { nodes, edges: [...edgeStore.values()] } });
    clients.add(res);
    const ka = setInterval(() => { try { res.write(': keepalive\n\n'); } catch { /* */ } }, 20000);
    req.on('close', () => { clearInterval(ka); clients.delete(res); });
    return;
  }

  if (path === '/api/run' && req.method === 'POST') {
    const body = await readBody(req);
    if (running) return json(res, 202, { status: 'rejected', reason: 'a campaign is already running' });
    runCampaign(body);   // fire-and-forget; progress streams over SSE
    return json(res, 202, { status: 'started', kind: body.kind || 'ucast', variation: body.variation, modes: body.modes });
  }

  if (path === '/api/cancel' && req.method === 'POST') {
    if (running) running.cancel = true;
    return json(res, 202, { status: 'cancelling' });
  }

  if (path === '/api/cmd' && req.method === 'POST') {
    const body = await readBody(req);
    return json(res, 200, { cmd_id: body?.command?.cmd_id || 'mock', instance_id: body.instance_id || '', ok: true, text: 'mock ack' });
  }

  if (path === '/api/errors' && req.method === 'GET') {
    // The real backend's errorreg is empty until a command actually fails; the
    // mock's campaigns never fail, so this always returns empty. Present for
    // API-surface parity (nothing in the web UI calls this today).
    const nodeID = url.searchParams.get('node');
    return json(res, 200, nodeID ? [] : {});
  }

  res.writeHead(404, { 'Access-Control-Allow-Origin': '*' });
  res.end('not found');
});

server.listen(PORT, () => {
  console.log(`mock control-plane on http://localhost:${PORT}`);
  console.log(`  ${nodes.length} nodes across ${new Set(nodes.map((n) => n.region)).size} regions, ${new Set(nodes.map((n) => n.az)).size} AZs, ${new Set(nodes.map((n) => n.vpc_id)).size} VPCs`);
  console.log(`  ${edgeStore.size} seeded edges (ucast x2 variations [kernel,xdp] + mcast x2 modes)`);
});
