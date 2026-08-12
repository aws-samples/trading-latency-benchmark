<script>
  import { onMount, onDestroy, afterUpdate } from 'svelte';
  import { mountTopology2D } from './lib/2d/index.js';
  import { mountTopology3D } from './lib/topology3d.js';
  import { createLive, runCampaign, cancelCampaign } from './lib/live.js';
  import { mountControls } from './lib/controls.js';
  import { buildCombinedReportBody, buildCombinedReportHTML, REPORT_CSS, reportInteractions } from './lib/report-combined.js';
  import { prunedTargets, countPairs, SCOPE_AMONG, SCOPE_FANOUT, resolvePreset } from './lib/pairs.js';

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

  // ── Live report overlay state ──
  let reportOverlayOpen = false;
  let reportRetry = null;
  let reportOverlayEl = null;

  // ── Target set — scopes the next run to a subset of nodes ──
  let targetIds = new Set();
  let scope = 'among';
  let activePreset = null;   // highlighted chip
  let targetAnchor = null;   // the marked instance presets cluster around

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

  // ── Live report overlay ──
  function getReportViews() {
    const combos = (conn ? conn.combos() : []).filter((c) => c.kind === kind);
    const views = combos.length
      ? combos.map((c) => ({ ...c, fleet: conn.toFleet(c.kind, c.variation) }))
      : (fleet ? [{ kind, variation, fleet }] : []);
    return views;
  }

  function openReportOverlay() {
    reportOverlayOpen = true;
    rerenderReportOverlay();
  }

  // The browser's print header prints document.title; blank it so the page is
  // not stamped with "AF_XDP topology". The URL half of that header is a print
  // dialog setting and cannot be suppressed from CSS.
  function printReport() {
    const views = getReportViews();
    if (!views.length) return;
    const doc = buildCombinedReportHTML(views, panel?.timezone?.() || '')
      .replace('</head>', `<style>
        @page { size: landscape; margin: 0; }
        @media print {
          html, body { background: #fff !important; color: #111 !important; }
          /* @page margin is 0 so browsers drop their header/footer; put the
             page margin back on the content instead. */
          body { padding: 12mm 10mm !important; }
          table { break-inside: auto; page-break-inside: auto; }
          tr, th, td { break-inside: avoid; page-break-inside: avoid; }
          thead { display: table-header-group; }
          details { display: block; }
          details > * { display: block; }
          .heat td, .heat th, td, th { border-color: #bbb !important; }
          th { background: #f1f5f9 !important; color: #334155 !important; }
          td { color: #111 !important; }
          .method, .coverage, .selbar { background: #f8fafc !important; color: #1e293b !important;
            border-color: #ddd !important; }
          .mode-badge { background: #e5e7eb !important; color: #1f2937 !important; }
        }
      </style></head>`);
    const f = document.createElement('iframe');
    f.setAttribute('aria-hidden', 'true');
    f.style.cssText = 'position:fixed;right:0;bottom:0;width:0;height:0;border:0';
    document.body.appendChild(f);
    f.contentDocument.open();
    f.contentDocument.write(doc);
    f.contentDocument.close();
    const go = () => {
      // A print stylesheet cannot force a <details> open - the UA hides the
      // contents until the attribute is present - so set it on the printed copy.
      f.contentDocument.querySelectorAll('details').forEach((d) => d.setAttribute('open', ''));
      f.contentWindow.focus();
      f.contentWindow.print();
      setTimeout(() => f.remove(), 1000);
    };
    if (f.contentDocument.readyState === 'complete') go();
    else f.contentWindow.addEventListener('load', go, { once: true });
  }


  function closeReportOverlay() {
    reportOverlayOpen = false;
    if (reportOverlayEl) reportOverlayEl.innerHTML = '';
  }

  function ensureReportCss() {
    if (document.getElementById('afxdp-report-css')) return;
    const st = document.createElement('style');
    st.id = 'afxdp-report-css';
    st.textContent = REPORT_CSS;
    document.head.appendChild(st);
  }

  function rerenderReportOverlay() {
    if (!reportOverlayOpen) return;
    // The element is bound by Svelte only after the {#if} flushes, and the first
    // SSE event can arrive before that. Returning silently left the tab blank
    // until a manual reload happened to order those two the other way round, so
    // retry on the next tick and let whichever arrives last trigger the render.
    if (!reportOverlayEl || !getReportViews().length) {
      if (!reportRetry) {
        reportRetry = setTimeout(function () { reportRetry = null; rerenderReportOverlay(); }, 120);
      }
      return;
    }
    ensureReportCss();
    const views = getReportViews();
    // Preserve scroll position and IP selection across re-renders
    const scrollTop = reportOverlayEl.scrollTop;
    const selectedIPs = new Set();
    reportOverlayEl.querySelectorAll('#inv-table tr.sel').forEach((tr) => {
      if (tr.dataset.ip) selectedIPs.add(tr.dataset.ip);
    });

    const body = buildCombinedReportBody(views, panel?.timezone?.() || '');
    const contentEl = reportOverlayEl.querySelector('.report-content');
    if (contentEl) {
      contentEl.innerHTML = body;
    } else {
      reportOverlayEl.innerHTML = `<div class="report-content">${body}</div>`;
    }

    const root = reportOverlayEl.querySelector('.report-content');
    reportInteractions(root);

    // Restore IP selection
    if (selectedIPs.size > 0) {
      root.querySelectorAll('#inv-table tr[data-ip]').forEach((tr) => {
        if (selectedIPs.has(tr.dataset.ip)) tr.click();
      });
    }
    // Restore scroll
    reportOverlayEl.scrollTop = scrollTop;
  }

  function remount() {
    if (!fleet || !container) return;
    // Preserve the 3D camera across remounts so live updates don't reset zoom/pan.
    if (mode === '3d' && handle && handle.getView) { try { view3d = handle.getView(); } catch (_) { /* keep last */ } }
    if (handle) handle.dispose();
    container.innerHTML = '';
    try {
      handle = (mode === '2d')
        ? mountTopology2D(container, fleet, { targetIds, onToggleTarget })
        : mountTopology3D(container, fleet, { view: view3d, targetIds, onToggleTarget });
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
      // Infer kind from the data: if nodes have source+replicator+destination roles → mcast.
      const roles = new Set((fleet.nodes || []).map(n => n.role).filter(Boolean));
      if (roles.has('source') && roles.has('replicator') && roles.has('destination')) {
        kind = 'mcast';
      } else if ((label || url).includes('mcast')) {
        kind = 'mcast';
      } else {
        kind = 'ucast';
      }
      loading = false; remount();
      panel?.setStats(statsFromFleet(fleet));
      panel?.setStatus(`loaded ${label || url} (${kind})`);
    } catch (e) { loading = false; error = e.message || String(e); panel?.setStatus('load failed: ' + error); }
  }

  // Rebuild the viz from live state for the current kind+variation (debounced).
  function liveRerender() {
    if (!conn) return;
    // At most two entries: ucast and/or mcast, each unifying its variations.
    panel?.setCombos(conn.kinds(), { kind });
    fleet = conn.toFleet(kind, variation);
    // Prune targetIds: a terminated/offline node cannot silently scope a run.
    const pruned = prunedTargets(targetIds, fleet.nodes);
    if (pruned.size !== targetIds.size) targetIds = pruned;
    updateTargetPanel();
    loading = false; error = '';
    // A report tab renders no map, so skip the mount, and render the report
    // before anything optional so it cannot be starved by a later failure.
    if (reportOverlayOpen) rerenderReportOverlay();
    else remount();
    const s = conn.stats();
    panel?.setStats({ ...s, updated: Date.now() });
  }
  function scheduleRerender() {
    if (rerenderTimer) return;
    rerenderTimer = setTimeout(() => { rerenderTimer = null; liveRerender(); }, 500);
  }

  // Push current target state to the control panel.
  function updateTargetPanel() {
    if (!panel) return;
    const N = fleet ? fleet.nodes.filter((n) => n.online !== false).length : 0;
    const k = targetIds.size;
    const pairs = countPairs(N, k, scope);
    panel.setTargets({ count: k, pairs, scope, totalNodes: N, preset: activePreset });
  }

  // Target toggle handler — called from 2D checkbox / shift+click and 3D shift+click.
  function onToggleTarget(instanceId) {
    // A manual pick no longer corresponds to a preset, so drop the highlight.
    activePreset = null;
    // This instance becomes the anchor the group presets expand from.
    targetAnchor = targetIds.has(instanceId) ? null : instanceId;
    if (targetIds.has(instanceId)) targetIds.delete(instanceId);
    else targetIds.add(instanceId);
    // D3: auto-switch scope when among yields 0 pairs (k<2).
    if (scope === SCOPE_AMONG && targetIds.size === 1) scope = SCOPE_FANOUT;
    updateTargetPanel();
    remount();
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
        for (const v of ['kernel', 'xdp']) {
          if (runCancelled) break;
          panel?.setStatus(`${pfx}running ucast/${v}…`);
          await runCampaign({ ...body, variation: v, nodes: [...targetIds], scope });
          await waitForDone();
        }
        if (!runCancelled) panel?.setStatus('done ucast/all');
      } else {
        panel?.setStatus(`${pfx}launching ${body.kind}/${body.variation || (body.modes || []).join('+')}…`);
        await runCampaign({ ...body, nodes: [...targetIds], scope });
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
    const ms = Math.max(30, body.intervalSec || 30) * 1000;
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
      onSelectView: ({ kind: k }) => { kind = k; variation = null; liveRerender(); },
      onPickResult: (p) => { if (p) load(`/api/fleet?path=${encodeURIComponent(p)}`, p); },
      onRun: doRun,
      onClearTargets: () => {
        targetIds = new Set(); activePreset = null; targetAnchor = null; scope = SCOPE_AMONG;
        updateTargetPanel(); remount();
      },
      onScopeChange: (s) => { scope = s; updateTargetPanel(); remount(); },
      onPreset: (name) => {
        // Presets are group expansions of the MARKED instance: PG selects every
        // instance sharing its placement group, AZ every one in its AZ, and so
        // on. Pressing the active preset again collapses back to just that
        // instance, keeping the anchor so another grouping can be tried without
        // re-marking. The buttons are disabled while nothing is marked.
        const anchor = targetAnchor || (targetIds.size ? [...targetIds][0] : null);
        if (!anchor) return;
        if (activePreset === name) {
          targetIds = new Set([anchor]);
          activePreset = null;
          updateTargetPanel(); remount(); return;
        }
        targetIds = new Set(resolvePreset(name, fleet?.nodes || [], anchor));
        activePreset = targetIds.size ? name : null;
        if (scope === SCOPE_AMONG && targetIds.size === 1) scope = SCOPE_FANOUT;
        updateTargetPanel(); remount();
      },
    });

    const params = new URLSearchParams(location.search);

    // If opened as a report tab (?report=<kind>), render only the live report
    // overlay. The tab opens its own SSE connection (fresh app instance) so it
    // stays live. No topology is rendered.
    if (params.get('report')) {
      kind = params.get('report');
      connect();
      reportOverlayOpen = true;
      return;
    }

    // Default: connect to the live backend (shows recent test or a blank map).
    // Only load a static fleet.json when explicitly requested via ?data=.
    if (params.get('data')) await load(params.get('data'), params.get('data'));
    else connect();

    try { const res = await fetch('/api/results'); if (res.ok) { runs = await res.json(); panel?.setResults(runs); } } catch { /* no dev API */ }
  });
  afterUpdate(() => {
    if (reportOverlayOpen && reportOverlayEl && !reportOverlayEl.querySelector('.report-content')) {
      rerenderReportOverlay();
    }
  });

  onDestroy(() => { if (handle) handle.dispose(); stopHeartbeat(); if (conn) conn.close(); if (panel) panel.dispose(); });
</script>

<div class="controls-host" bind:this={controlsHost}></div>
<div class="root" bind:this={container}></div>

{#if reportOverlayOpen}
<div class="report-overlay" data-report-overlay>
  <div class="report-toolbar">
    <button class="report-toolbar-btn" on:click={printReport}>Save as PDF</button>
  </div>
  <div class="report-body" bind:this={reportOverlayEl}></div>
</div>
{/if}

{#if loading}<div class="msg">Loading topology…</div>{/if}
{#if error}<div class="msg err">Failed to load: {error}</div>{/if}

<style>
  .root { position: fixed; inset: 0; }
  .controls-host { position: fixed; inset: 0; pointer-events: none; z-index: 9999; }
  .controls-host :global(.cp-panel) { pointer-events: auto; }
  .msg { position: fixed; bottom: 16px; right: 16px; z-index: 9999; color: #e6edf3;
    background: rgba(22,27,34,.92); border: 1px solid #30363d; border-radius: 6px; padding: 8px 12px;
    font: 13px -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }
  .msg.err { color: #f85149; border-color: #f85149; }

  .report-overlay { position: fixed; inset: 0; z-index: 10000; display: flex; flex-direction: column;
    background: #0d1117; }
  .report-toolbar { display: flex; gap: 8px; padding: 8px 16px; background: #161b22;
    border-bottom: 1px solid #30363d; flex-shrink: 0; }
  .report-toolbar-btn { background: #21262d; color: #e6edf3; border: 1px solid #30363d;
    border-radius: 6px; padding: 6px 14px; cursor: pointer; font: 600 13px system-ui; }
  .report-toolbar-btn:hover { background: #30363d; color: #fff; }
  .report-body { flex: 1; overflow: auto; padding: 22px;
    font-family: system-ui, -apple-system, sans-serif; color: #e6edf3; }

  @media print {
    /* Landscape: the measurements table is wide. */
    @page { size: landscape; margin: 10mm; }
    .controls-host, .root, .report-toolbar, .msg { display: none !important; }
    /* Undo the whole screen layout. While the overlay is fixed + flex with an
       overflow:auto body, printing captures only the visible viewport, which is
       why just the first page came out. */
    :global(html), :global(body) { height: auto !important; overflow: visible !important;
      background: #fff !important; }
    .report-overlay { position: static !important; inset: auto !important; display: block !important;
      height: auto !important; overflow: visible !important; z-index: auto !important;
      background: #fff; }
    .report-body { flex: none !important; height: auto !important; max-height: none !important;
      overflow: visible !important; padding: 0; color: #111; background: #fff; }
    .report-body :global(table) { break-inside: avoid; page-break-inside: avoid; }
    .report-body :global(details) { display: block; }
    .report-body :global(details[open]), .report-body :global(details) { open: true; }
    .report-body :global(details > *) { display: block; }
    .report-body :global(details > summary) { display: list-item; }
    .report-body :global(.coverage) { background: #fefce8; color: #92400e; border-color: #d4d4d4; }
    .report-body :global(.method) { background: #f8fafc; color: #1e293b; border-color: #d4d4d4; }
    .report-body :global(th) { background: #f1f5f9; color: #334155; }
    .report-body :global(td), .report-body :global(th) { border-color: #cbd5e1; }
    .report-body :global(.meta) { color: #475569; }
    .report-body :global(h1), .report-body :global(h2) { color: #111; }
  }
</style>
