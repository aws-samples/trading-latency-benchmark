<script>
  import { onMount, onDestroy } from 'svelte';
  import { mountTopology2D } from './lib/2d/index.js';
  import { mountTopology3D } from './lib/topology3d.js';
  import { createLive, runCampaign, cancelCampaign } from './lib/live.js';
  import { mountControls } from './lib/controls.js';
  import { buildReportHTML } from './lib/report.js';

  let container;        // viz host (wiped on remount)
  let controlsHost;     // persistent overlay host for the shared panel
  let panel = null;

  let fleet = null;
  let error = '';
  let loading = true;
  let mode = '2d';
  let handle = null;
  let view3d = null;   // persisted 3D camera view, kept across live-update remounts

  let runs = [];

  // ── backend connection (SSE) — the DATA source. Always open when a backend is
  //    present; independent of "Live mode" (which is the heartbeat mode below). ──
  let conn = null;
  let kind = 'ucast';
  let variation = 'kernel';
  let rerenderTimer = null;

  // ── Live (heartbeat) mode — a DISTINCT mode that re-runs a chosen test on an
  //    interval. Launching a one-shot test does NOT enable this. ──
  let heartbeatOn = false;
  let hbTimer = null;
  let hbBody = null;
  let hbRunning = false;
  let runCancelled = false;   // set when the active run button is pressed again

  // ucast "all" sequencer: the backend runs one campaign at a time, so we must
  // wait for each variation's job 'done' before launching the next.
  let jobDoneWaiters = [];
  function resolveJobDone() { const ws = jobDoneWaiters; jobDoneWaiters = []; ws.forEach((fn) => fn()); }
  function waitForDone(timeoutMs = 180000) {
    return new Promise((res) => { const t = setTimeout(res, timeoutMs); jobDoneWaiters.push(() => { clearTimeout(t); res(); }); });
  }

  function remount() {
    if (!fleet || !container) return;
    // Preserve the 3D camera across remounts so live updates don't reset zoom/pan.
    if (mode === '3d' && handle && handle.getView) { try { view3d = handle.getView(); } catch (_) { /* keep last */ } }
    if (handle) handle.dispose();
    container.innerHTML = '';
    try {
      handle = (mode === '2d')
        ? mountTopology2D(container, fleet)
        : mountTopology3D(container, fleet, { view: view3d });
    } catch (e) {
      console.error('Viz mount failed:', e);
      container.innerHTML = '<pre style="color:#f85149;padding:20px;font:12px monospace;white-space:pre-wrap;max-height:80vh;overflow:auto">'
        + 'Render error:\n' + (e.stack || e.message || String(e)) + '</pre>';
      handle = null;
    }
  }

  function statsFromFleet(f) {
    if (!f || !Array.isArray(f.nodes)) return { nodes: 0, online: 0, edges: 0 };
    let edges = 0;
    (f.matrix || []).forEach((row) => row && row.forEach((c) => { if (c) edges++; }));
    const online = f.nodes.filter((n) => n.online !== false).length;
    return { nodes: f.nodes.length, online, edges };
  }

  function validateFleet(f) {
    if (!f || typeof f !== 'object') throw new Error('fleet is not an object');
    if (!Array.isArray(f.nodes)) throw new Error('fleet.nodes must be an array');
    if (!Array.isArray(f.matrix)) throw new Error('fleet.matrix must be an array');
    return f;
  }

  // Explicit static load (?data= override or Browse results) — not the default.
  async function load(url, label) {
    loading = true; error = '';
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`GET ${url} -> ${res.status}`);
      fleet = validateFleet(await res.json());
      loading = false; remount();
      panel?.setStats(statsFromFleet(fleet));
      panel?.setStatus(`loaded ${label || url}`);
    } catch (e) { loading = false; error = e.message || String(e); panel?.setStatus('load failed: ' + error); }
  }

  // Rebuild the viz from live state for the current kind+variation (debounced).
  function liveRerender() {
    if (!conn) return;
    const combos = conn.combos();
    panel?.setCombos(combos, { kind, variation });
    fleet = conn.toFleet(kind, variation);
    loading = false; error = ''; remount();
    const s = conn.stats();
    panel?.setStats({ ...s, updated: Date.now() });
  }
  function scheduleRerender() {
    if (rerenderTimer) return;
    rerenderTimer = setTimeout(() => { rerenderTimer = null; liveRerender(); }, 500);
  }

  // Human-readable, detailed campaign progress for the status line.
  function fmtJob(d) {
    const kv = d.kind ? d.kind + (d.variation ? '/' + d.variation : (d.mode ? '/' + d.mode : '')) : '';
    switch (d.status) {
      case 'running':
        return d.kind === 'mcast'
          ? `running ${kv} — src ${d.source} → replicator ${d.replicator} → ${d.dests} dest(s)`
          : `running ${kv} — ${d.pairs} pairs across ${d.rounds} rounds`;
      case 'progress':
        if (d.msg) return `${kv}: ${d.msg}`;
        return d.kind === 'mcast'
          ? `${kv}: ${d.src}→${d.dst} ${d.ok === false ? 'FAILED ' + (d.err || '') : 'ok'}`
          : `${d.done}/${d.total} - ${d.src}→${d.dst}${d.ok === false ? ' FAILED ' + (d.err || '') : ''}`;
      case 'mode_done': return `${kv}: mode complete${d.ok === false ? ' (failed)' : ''}`;
      case 'done': return d.done != null ? `done ${kv} (${d.done}/${d.total})` : `done ${kv}`;
      case 'cancelled': return `cancelled ${kv}${d.done != null ? ` at ${d.done}/${d.total}` : ''}`;
      case 'rejected': return `rejected — ${d.reason || ''}`;
      case 'error': return `error ${kv} — ${d.reason || d.err || ''}`;
      default: return `${d.status || ''} ${kv}`.trim();
    }
  }

  // Open the SSE data connection (idempotent). Shows the current live fleet —
  // the most recent test's results, or a blank map if none yet.
  function connect() {
    if (conn) return;
    conn = createLive({
      onUpdate: scheduleRerender,
      onJob: (d) => { panel?.setStatus(fmtJob(d)); if (d.status === 'done' || d.status === 'cancelled') resolveJobDone(); },
    });
    liveRerender();
  }

  // Sanity-check the interacting inputs and (for mcast) auto-fix an obviously
  // broken combo. count+rate drive the ucast run duration; count+interval_us the
  // mcast send window. Returns human-readable warnings; may mutate body.
  function cohesionCheck(body) {
    const w = [];
    if (body.count < 1000) w.push(`${body.count} pkts — p99.9 will be noisy`);
    if (body.kind === 'ucast') {
      const ms = body.count / Math.max(1, body.rate) * 1000;
      if (ms < 50) w.push(`~${ms.toFixed(0)}ms run (${body.count}pkts @ ${body.rate}pps) — very short`);
      if (body.rate > 200000) w.push(`${body.rate}pps is high for a QD=1 ping-pong`);
    } else {
      const secs = body.count * (body.interval_us || 0) / 1e6;
      const to = body.timeout_sec || 25;
      if (secs > to) { body.timeout_sec = Math.ceil(secs) + 5; w.push(`send ~${secs.toFixed(1)}s > timeout — raised to ${body.timeout_sec}s`); }
      if ((body.interval_us || 0) < 20) w.push(`interval ${body.interval_us}µs is very tight — may drop`);
    }
    return w;
  }

  // Launch a ONE-SHOT test. Does NOT enable Live mode. Awaits true completion
  // (the job 'done' event) so the active button stays orange for the whole run,
  // then re-enables the buttons. body === null is a CANCEL signal.
  async function doRun(body) {
    if (!body) { runCancelled = true; cancelCampaign(); return; }
    runCancelled = false;
    if (!conn) connect();
    kind = body.kind;
    variation = (body.variation && body.variation !== 'all') ? body.variation : (body.modes ? body.modes[0] : variation);
    const pfx = (() => { const w = cohesionCheck(body); return w.length ? '\u26a0 ' + w.join(' · ') + ' — ' : ''; })();
    try {
      if (body.kind === 'ucast' && body.variation === 'all') {
        for (const v of ['kernel', 'xdp-tx', 'xdp-rx', 'xdp-txrx']) {
          if (runCancelled) break;
          panel?.setStatus(`${pfx}running ucast/${v}…`);
          await runCampaign({ ...body, variation: v });
          await waitForDone();
        }
        if (!runCancelled) panel?.setStatus('done ucast/all');
      } else {
        panel?.setStatus(`${pfx}launching ${body.kind}/${body.variation || (body.modes || []).join('+')}…`);
        await runCampaign(body);
        await waitForDone();
      }
    } catch (e) {
      panel?.setStatus('run failed: ' + e);
    } finally {
      panel?.endRun();   // clear the orange button + re-enable the disabled ones
    }
  }

  // ── Live (heartbeat) mode: re-run the chosen test every interval ─────────────
  function stopHeartbeat() { if (hbTimer) { clearInterval(hbTimer); hbTimer = null; } hbBody = null; }
  function startHeartbeat(body) {
    stopHeartbeat();
    hbBody = body;
    const ms = Math.max(10, body.intervalSec || 30) * 1000;   // 10s floor keeps it resource-sane
    const tick = async () => { if (hbRunning || !hbBody) return; hbRunning = true; try { await doRun(hbBody); } finally { hbRunning = false; } };
    tick();                                    // fire immediately
    hbTimer = setInterval(tick, ms);
  }

  onMount(async () => {
    panel = mountControls(controlsHost, {
      initialMode: mode,
      onSetMode: (m) => { if (m !== mode) { mode = m; remount(); } },
      // Live toggle = heartbeat MODE (distinct from launching a test / the SSE connection).
      onToggleLive: (on) => { heartbeatOn = on; if (!on) { stopHeartbeat(); panel?.setStatus('live monitoring off'); } },
      // A heartbeat mode was chosen (or cleared) in the live panel.
      onHeartbeat: (sel) => { if (!sel) { stopHeartbeat(); panel?.setStatus('heartbeat stopped'); } else { startHeartbeat(sel); } },
      onSelectView: ({ kind: k, variation: v }) => { kind = k; variation = v; liveRerender(); },
      onPickResult: (p) => { if (p) load(`/api/fleet?path=${encodeURIComponent(p)}`, p); },
      onReport: () => {
        if (!fleet || !(fleet.nodes || []).length) { panel?.setStatus('no data to report yet'); return; }
        const html = buildReportHTML(fleet, kind, variation);
        const url = URL.createObjectURL(new Blob([html], { type: 'text/html' }));
        const a = document.createElement('a');
        a.href = url; a.download = `afxdp-report-${kind}-${variation}-${Date.now()}.html`;
        document.body.appendChild(a); a.click(); a.remove();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
        panel?.setStatus(`downloaded report ${kind}/${variation}`);
      },
      onRun: doRun,
    });

    const params = new URLSearchParams(location.search);
    // Default: connect to the live backend (shows recent test or a blank map).
    // Only load a static fleet.json when explicitly requested via ?data=.
    if (params.get('data')) await load(params.get('data'), params.get('data'));
    else connect();

    try { const res = await fetch('/api/results'); if (res.ok) { runs = await res.json(); panel?.setResults(runs); } } catch { /* no dev API */ }
  });
  onDestroy(() => { if (handle) handle.dispose(); stopHeartbeat(); if (conn) conn.close(); if (panel) panel.dispose(); });
</script>

<div class="controls-host" bind:this={controlsHost}></div>
<div class="root" bind:this={container}></div>

{#if loading}<div class="msg">Loading topology…</div>{/if}
{#if error}<div class="msg err">Failed to load: {error}</div>{/if}

<style>
  .root { position: fixed; inset: 0; }
  /* controls-host MUST be highest z-index — above the 3D CSS2DRenderer overlay
     (which is position:absolute with high z-index) and above all canvas elements.
     pointer-events:none lets clicks through to the canvas; .cp-panel is auto. */
  .controls-host { position: fixed; inset: 0; pointer-events: none; z-index: 9999; }
  .controls-host :global(.cp-panel) { pointer-events: auto; }
  .msg { position: fixed; bottom: 16px; right: 16px; z-index: 9999; color: #e6edf3;
    background: rgba(22,27,34,.92); border: 1px solid #30363d; border-radius: 6px; padding: 8px 12px;
    font: 13px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }
  .msg.err { color: #f85149; border-color: #f85149; }
</style>
