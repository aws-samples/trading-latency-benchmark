// mock/server.mjs — zero-dependency mock AF_XDP control-plane for local UI work.
//
// Speaks the exact wire contract the Svelte app expects (see backend/api.go):
//   GET  /api/events   SSE: {type:"snapshot"} then node/edge/job deltas + keepalive
//   GET  /api/fleet    full {nodes,edges} snapshot (late joiners / ?data path parity)
//   POST /api/run      start a campaign; streams job running/progress/done + fresh edges
//   POST /api/cancel   abort the running campaign at its next boundary
//   POST /api/cmd      ad-hoc agent command (echoes an ok result)
//   GET  /healthz      ok
//
// Vite's dev proxy forwards /api/{events,run,cancel,cmd} here (CP_URL, default
// :8080), so `npm run dev` gets a live, streaming multi-region fleet with no
// real backend. Run:  node mock/server.mjs   (PORT overrides 8080)
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

function bumpEdge(kind, variation, src, dst) {
  const e = edgeStore.get(`${kind}|${variation}|${src}|${dst}`);
  const now = Math.floor(Date.now() / 1000);
  if (e) {
    const m = e.metrics.service_rtt_us, p50 = jitter(m.p50);
    e.metrics.service_rtt_us = { ...m, p50, p90: Math.round(p50 * 1.35), p99: Math.round(p50 * 2.1), p999: Math.round(p50 * 3.4), max: Math.round(p50 * 5.2) };
    e.unix = now;
    broadcast({ type: 'edge', data: e });
    return;
  }
  // Synthesize an edge if the pair wasn't pre-seeded for this variation.
  const a = byIP(src), b = byIP(dst); if (!a || !b) return;
  const p50 = jitter(a.region !== b.region ? 70000 : a.az !== b.az ? 130 : 40);
  const ne = { src, dst, kind, variation, unix: now,
    metrics: { service_rtt_us: { min: p50 - 5, mean: p50 + 3, p50, p90: Math.round(p50 * 1.35), p95: Math.round(p50 * 1.6), p99: Math.round(p50 * 2.1), p999: Math.round(p50 * 3.4), max: Math.round(p50 * 5.2) }, messages: 5000, lost: 0, loss_pct: 0 } };
  edgeStore.set(edgeKey(ne), ne);
  broadcast({ type: 'edge', data: ne });
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
    const relay = nodes.find((n) => n.region === region && n.role === 'replicator');
    const dests = nodes.filter((n) => n.region === region && n.role === 'destination');
    if (!src || !relay) continue;
    broadcast({ type: 'job', data: { status: 'running', kind: 'mcast', mode, source: src.private_ip, replicator: relay.private_ip, dests: dests.length } });
    for (const d of dests) {
      if (running?.cancel) { broadcast({ type: 'job', data: { status: 'cancelled', kind: 'mcast', mode } }); return; }
      bumpEdge('mcast', mode, src.private_ip, d.private_ip);
      broadcast({ type: 'job', data: { status: 'progress', kind: 'mcast', mode, src: src.private_ip, dst: d.private_ip, ok: true } });
      await sleep(120);
    }
    broadcast({ type: 'job', data: { status: 'mode_done', kind: 'mcast', mode, ok: true } });
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

  res.writeHead(404, { 'Access-Control-Allow-Origin': '*' });
  res.end('not found');
});

server.listen(PORT, () => {
  console.log(`mock control-plane on http://localhost:${PORT}`);
  console.log(`  ${nodes.length} nodes across ${new Set(nodes.map((n) => n.region)).size} regions, ${new Set(nodes.map((n) => n.az)).size} AZs, ${new Set(nodes.map((n) => n.vpc_id)).size} VPCs`);
  console.log(`  ${edgeStore.size} seeded edges (ucast x4 variations + mcast x2 modes)`);
});
