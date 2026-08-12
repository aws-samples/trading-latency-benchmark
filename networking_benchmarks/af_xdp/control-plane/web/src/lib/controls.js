// controls.js — a shared, framework-agnostic control panel mounted ONCE as a
// persistent overlay above whichever topology view (2D or 3D) is active. It
// survives the viz dispose/remount that live updates trigger, so it's the
// single source of UI for: view mode, real-time monitoring on/off, which
// kind/variation to render, and launching test campaigns. It is pure DOM +
// callbacks — the host (App) wires the callbacks to the live model + backend.

const STYLE_ID = 'cp-controls-styles';
const CSS = `
.cp-panel{position:fixed;top:14px;left:14px;z-index:9999;width:360px;
  min-width:360px;max-width:720px;min-height:40px;max-height:90vh;
  background:#161b22;border:1px solid #30363d;border-radius:10px;
  padding:0;color:#e6edf3;
  font:13px -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  box-shadow:0 8px 24px rgba(0,0,0,.5);resize:horizontal;overflow:hidden}
.cp-panel-title{display:flex;align-items:center;gap:6px;padding:8px 12px;cursor:move;
  user-select:none;border-bottom:1px solid #30363d;background:#0d1117}
.cp-panel-title:hover{background:#1c2128}
.cp-panel-caret{font-size:14px;color:#8b949e;margin-right:6px;transition:transform .15s;cursor:pointer;
  padding:2px 4px;border-radius:4px}
.cp-panel-caret:hover{background:rgba(88,166,255,.15);color:#58a6ff}
.cp-panel-caret.collapsed{transform:rotate(-90deg)}
.cp-panel-fold-btn{font-size:9px;color:#8b949e;cursor:pointer;user-select:none;padding:0 2px}
.cp-panel-fold-btn:hover{color:#e6edf3}
.cp-panel-body{padding:6px 12px 10px;background:#161b22}
.cp-panel .row{display:flex;align-items:center;gap:6px;margin:6px 0;flex-wrap:wrap}
.cp-panel .row.center{justify-content:center}
.cp-panel .grow{flex:1}
.cp-seg{display:flex;border:1px solid #30363d;border-radius:7px;overflow:hidden}
.cp-seg button{background:transparent;color:#8b949e;border:none;padding:5px 12px;cursor:pointer;font:600 12px inherit}
.cp-seg button.on{background:rgba(88,166,255,.18);color:#58a6ff}
.cp-live{background:transparent;color:#8b949e;border:1px solid #30363d;border-radius:7px;
  padding:5px 12px;cursor:pointer;font:600 12px inherit}
.cp-live.on{background:rgba(248,81,73,.16);color:#f85149;border-color:#da3633}
.cp-foldall{margin-left:auto;background:transparent;color:#8b949e;border:1px solid #30363d;
  border-radius:7px;padding:5px 10px;cursor:pointer;font:600 13px inherit;line-height:1}
.cp-foldall:hover{color:#e6edf3;border-color:#8b949e;background:rgba(88,166,255,.12)}
.cp-lbl{color:#6e7681;font:600 11px inherit;text-transform:uppercase;letter-spacing:.4px;flex-shrink:0}
.cp-section{color:#e6edf3;font:700 12px inherit;text-transform:uppercase;letter-spacing:.5px}
.cp-btn-group{display:flex;gap:4px;flex-wrap:nowrap;margin-left:4px}
.cp-stats{color:#8b949e;font:12px inherit;font-variant-numeric:tabular-nums}
.cp-stats b{color:#e6edf3}
.cp-clock{color:#8b949e;font:12px inherit;font-variant-numeric:tabular-nums;flex:1}
.cp-tz{flex:0 0 auto;width:auto;max-width:150px}
.cp-sel{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px;padding:4px 6px;font:12px inherit;flex:1}
.cp-btn{background:#21262d;color:#adbac7;border:1px solid #30363d;border-radius:6px;
  padding:4px 8px;cursor:pointer;font:600 12px inherit;transition:background .15s,border-color .15s}
.cp-btn:hover{background:#30363d;color:#fff}
.cp-btn:active,.cp-btn.running{background:rgba(240,136,62,.22);color:#f0883e;border-color:#f0883e}
.cp-btn:disabled{opacity:.4;cursor:not-allowed;color:#6e7681;background:#1a1f26;border-color:#21262d}
.cp-icon{background:#21262d;color:#adbac7;border:1px solid #30363d;border-radius:6px;padding:4px 9px;cursor:pointer;font:600 14px inherit;line-height:1;flex:0 0 auto}
.cp-icon:hover{background:#30363d;color:#fff}
.cp-num{width:58px;background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px;padding:3px 5px;font:12px inherit}
.cp-dim{color:#6e7681;font:11px inherit;margin-left:2px}
.cp-log-label{color:#6e7681;font:700 10px inherit;letter-spacing:.6px;margin:2px 0 3px}
.cp-status{background:#0d1117;border:1px solid #21262d;border-radius:6px;padding:6px 8px;
  color:#3fb950;font:12px 'SF Mono','Fira Code',ui-monospace,Menlo,monospace;
  min-height:34px;max-height:120px;overflow:auto;white-space:pre-wrap;word-break:break-word;
  font-variant-numeric:tabular-nums}
.cp-hr{height:1px;background:#21262d;margin:4px -12px}
`;

import { enhancePanel, foldAllPanels, resetAllPanels } from './2d/panels.js';
import { esc } from './2d/palette.js';

export function mountControls(host, opts = {}) {
  const { onSetMode, onToggleLive, onSelectView, onRun, onPickResult, onHeartbeat, onReport } = opts;
  if (!document.getElementById(STYLE_ID)) {
    const s = document.createElement('style'); s.id = STYLE_ID; s.textContent = CSS; document.head.appendChild(s);
  }
  const el = document.createElement('div');
  el.className = 'cp-panel';
  el.innerHTML = `
    <h3 class="cp-panel-title">
      <div class="cp-seg" data-seg>
        <button data-mode="2d">2D</button><button data-mode="3d">3D</button>
      </div>
      <button class="cp-live" data-live>Live</button>
      <button class="cp-foldall" data-foldall title="Fold / unfold all panels">\u29C9</button>
    </h3>
    <div class="cp-panel-body" data-body>
      <div class="row"><span class="cp-stats" data-stats>—</span></div>
      <div class="row"><span class="cp-lbl">Timezone</span><select class="cp-sel cp-tz" data-tz title="Display timezone for the log + Show list"></select></div>
      <div class="cp-hr"></div>

      <!-- NORMAL mode: Show selector + one-shot Run Tests -->
      <div data-normal>
        <div class="row"><span class="cp-lbl">Show</span>
          <select class="cp-sel" data-view><option value="">(no data yet)</option></select>
          <button class="cp-icon" data-report title="Download report (heatmap + all latencies) for the shown run">\u2913</button>
        </div>
        <div class="cp-hr"></div>
        <div class="row center"><span class="cp-section">Test Latency</span></div>
        <div class="row"><span class="cp-lbl">Packets</span><input class="cp-num" data-count value="5000" title="Measurement packets per pair (100–1,000,000)"></div>
        <div class="row"><span class="cp-lbl">Rate</span><input class="cp-num" data-rate value="20000" title="Ucast send rate (1,000–1,000,000)"><span class="cp-dim">pps</span></div>
        <div class="row"><span class="cp-lbl">Interval</span><input class="cp-num" data-interval value="100" title="Mcast inter-packet interval (10–100,000)"><span class="cp-dim">µs</span></div>
        <div class="cp-hr"></div>
        <div class="row"><span class="cp-lbl">unicast</span></div>
        <div class="row">
          <span class="cp-btn-group">
            <button class="cp-btn" data-run-ucast="kernel" title="Round-trip through kernel sendto (tuned busy-poll baseline)">kernel</button>
            <button class="cp-btn" data-run-ucast="xdp-tx" title="AF_XDP zero-copy TX + kernel RX">xdp-tx</button>
            <button class="cp-btn" data-run-ucast="xdp-rx" title="Kernel TX + XDP-stamped RX (instrumented)">xdp-rx</button>
            <button class="cp-btn" data-run-ucast="xdp-txrx" title="AF_XDP TX + XDP-stamped RX">txrx</button>
            <button class="cp-btn" data-run-ucast="all" title="Run all 4 ucast variations sequentially">all</button>
          </span>
        </div>
        <div class="row"><span class="cp-lbl">multicast</span></div>
        <div class="row">
          <span class="cp-btn-group">
            <button class="cp-btn" data-run-mcast="copy" title="Replicator copies frame to new TX buffer per destination">copy</button>
            <button class="cp-btn" data-run-mcast="inplace" title="Replicator patches RX frame headers in-place (zero-copy last dest)">inplace</button>
            <button class="cp-btn" data-run-mcast="kernel" title="XDP_TX forward in the kernel (single destination only)">kernel</button>
            <button class="cp-btn" data-run-mcast="all" title="Run all 3 mcast modes sequentially">all</button>
          </span>
        </div>
      </div>

      <!-- LIVE mode: heartbeat — interval first, then pick a mode to re-run -->
      <div data-live-section style="display:none">
        <div class="row"><span class="cp-lbl">Every</span>
          <input class="cp-num" data-hb-interval value="30" title="Heartbeat interval — keep >= 10s to stay resource-sane (a full campaign takes several seconds)">
          <span class="cp-dim">sec (min 10)</span>
        </div>
        <div class="cp-hr"></div>
        <div class="row"><span class="cp-lbl">unicast</span></div>
        <div class="row"><span class="cp-btn-group">
          <button class="cp-btn" data-hb-ucast="kernel">kernel</button>
          <button class="cp-btn" data-hb-ucast="xdp-tx">xdp-tx</button>
          <button class="cp-btn" data-hb-ucast="xdp-rx">xdp-rx</button>
          <button class="cp-btn" data-hb-ucast="xdp-txrx">txrx</button>
          <button class="cp-btn" data-hb-ucast="all">all</button>
        </span></div>
        <div class="row"><span class="cp-lbl">multicast</span></div>
        <div class="row"><span class="cp-btn-group">
          <button class="cp-btn" data-hb-mcast="copy">copy</button>
          <button class="cp-btn" data-hb-mcast="inplace">inplace</button>
          <button class="cp-btn" data-hb-mcast="kernel">kernel</button>
          <button class="cp-btn" data-hb-mcast="all">all</button>
        </span></div>
      </div>

      <div class="cp-hr"></div>
      <div class="cp-log-label">LOG</div>
      <div class="cp-status" data-status></div>
    </div>
  `;
  host.appendChild(el);

  // Use the shared enhancePanel for drag/fold/proportional-resize.
  const ctx = { disposers: [] };
  enhancePanel(ctx, el, false);


  const $ = (sel) => el.querySelector(sel);
  const segBtns = [...el.querySelectorAll('[data-mode]')];
  const liveBtn = $('[data-live]');
  const foldAllBtn = $('[data-foldall]');
  // Toggle: 1st click folds EVERY panel (including this control panel); 2nd click
  // restores them ALL to their default position/size/expanded state, regardless
  // of whatever the user changed in between.
  let allFolded = false;
  foldAllBtn.addEventListener('click', () => {
    if (allFolded) { resetAllPanels(); allFolded = false; foldAllBtn.textContent = '\u29C9'; }
    else { foldAllPanels(true); allFolded = true; foldAllBtn.textContent = '\u29C7'; }
  });
  const viewSel = $('[data-view]');
  const statsEl = $('[data-stats]');
  const statusEl = $('[data-status]');
  const num = (s) => Math.max(1, parseInt($(s).value, 10) || 0);
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

  // ── Display timezone (affects the Show list + any timestamps) ───────────────
  const tzSel = $('[data-tz]');
  const TZ_OPTS = [
    ['', 'Local'], ['UTC', 'UTC'], ['Europe/Berlin', 'CET Berlin'], ['Europe/London', 'London'],
    ['America/New_York', 'New York'], ['America/Chicago', 'Chicago'], ['America/Los_Angeles', 'Los Angeles'],
    ['Asia/Kolkata', 'India'], ['Asia/Singapore', 'Singapore'], ['Asia/Tokyo', 'Tokyo'], ['Australia/Sydney', 'Sydney'],
  ];
  tzSel.innerHTML = TZ_OPTS.map(([v, l]) => `<option value="${v}">${l}</option>`).join('');
  let selectedTz = '';   // '' = browser local
  const fmtTime = (ms) => {
    if (!ms) return '';
    const o = { hour: '2-digit', minute: '2-digit', hour12: false, timeZoneName: 'short' };
    if (selectedTz) o.timeZone = selectedTz;
    try { return new Intl.DateTimeFormat([], o).format(new Date(ms)); } catch { return new Date(ms).toLocaleTimeString(); }
  };
  let lastCombos = null, lastSel = null;
  const renderCombos = (combos, sel) => {
    lastCombos = combos; lastSel = sel;
    const cur = sel ? `${sel.kind}|${sel.variation}` : viewSel.value;
    if (!combos || !combos.length) { viewSel.innerHTML = '<option value="">(no data yet)</option>'; return; }
    // Newest-first, labelled "HH:MM TZ · kind/variation".
    viewSel.innerHTML = combos.map((c) => {
      const v = `${c.kind}|${c.variation}`;
      const t = c.unix ? fmtTime(c.unix * 1000) + ' · ' : '';
      return `<option value="${v}"${v === cur ? ' selected' : ''}>${t}${c.kind}/${c.variation}</option>`;
    }).join('');
  };
  tzSel.addEventListener('change', () => { selectedTz = tzSel.value; if (lastCombos) renderCombos(lastCombos, lastSel); });

  let mode = opts.initialMode || '2d';
  let liveOn = !!opts.initialLive;
  let activeHb = null;   // active heartbeat-mode button (live mode)
  const paintMode = () => segBtns.forEach((b) => b.classList.toggle('on', b.dataset.mode === mode));
  const paintLive = () => { liveBtn.classList.toggle('on', liveOn); liveBtn.textContent = liveOn ? '\u25CF Live' : 'Live'; };
  // Live mode swaps the panel body: hide the Show row + one-shot Run Tests, show
  // the heartbeat section (distinct settings). Clears any active heartbeat on exit.
  function syncSections() {
    const nrm = $('[data-normal]'), lv = $('[data-live-section]');
    if (nrm) nrm.style.display = liveOn ? 'none' : '';
    if (lv) lv.style.display = liveOn ? '' : 'none';
    if (!liveOn && activeHb) { activeHb.classList.remove('running'); activeHb = null; }
  }
  paintMode(); paintLive(); syncSections();

  segBtns.forEach((b) => b.addEventListener('click', () => { mode = b.dataset.mode; paintMode(); onSetMode && onSetMode(mode); }));
  liveBtn.addEventListener('click', () => { liveOn = !liveOn; paintLive(); syncSections(); onToggleLive && onToggleLive(liveOn); });
  viewSel.addEventListener('change', () => {
    // A browse-result option carries data-run (a results/ subdir path); a live
    // combo option carries a "kind|variation" value. The Show dropdown hosts both.
    const opt = viewSel.selectedOptions[0];
    if (opt && opt.dataset.run !== undefined) { onPickResult && onPickResult(opt.dataset.run); return; }
    const v = viewSel.value; if (!v) return;
    const [kind, variation] = v.split('|');
    onSelectView && onSelectView({ kind, variation });
  });
  // Track the currently running one-shot button. While a run is active it stays
  // orange; every other run button is disabled (gray) until the run finishes.
  const runBtns = [...el.querySelectorAll('[data-run-ucast],[data-run-mcast]')];
  let activeRunBtn = null;
  const endRunUI = () => {
    if (activeRunBtn) activeRunBtn.classList.remove('running');
    activeRunBtn = null;
    runBtns.forEach((b) => { b.disabled = false; });
  };
  const startRun = (btn, payload) => {
    if (activeRunBtn === btn) {          // second press on the active button = cancel
      onRun && onRun(null);
      endRunUI();
      return;
    }
    if (activeRunBtn) return;            // a run is active — others are disabled anyway
    activeRunBtn = btn;
    btn.classList.add('running');                                  // active = orange
    runBtns.forEach((b) => { if (b !== btn) b.disabled = true; });  // others gray + disabled
    onRun && onRun(payload);
  };

  el.querySelectorAll('[data-run-ucast]').forEach((b) => b.addEventListener('click', () => {
    startRun(b, { kind: 'ucast', variation: b.dataset.runUcast, count: clamp(num('[data-count]'), 100, 1000000), rate: clamp(num('[data-rate]'), 1000, 1000000), warmup: 1000 });
  }));
  el.querySelectorAll('[data-run-mcast]').forEach((b) => b.addEventListener('click', () => {
    const mcastMode = b.dataset.runMcast;
    const modes = mcastMode === 'all' ? ['copy', 'inplace', 'kernel'] : [mcastMode];
    startRun(b, { kind: 'mcast', modes, count: clamp(num('[data-count]'), 100, 1000000), interval_us: clamp(num('[data-interval]'), 10, 100000), timeout_sec: 25 });
  }));

  // Download report (heatmap + all latencies) for the currently-shown run.
  $('[data-report]').addEventListener('click', () => onReport && onReport());

  // ── Live heartbeat: choose a mode → App re-runs it every interval (min 10s) ──
  const hbIntervalSec = () => Math.max(10, parseInt($('[data-hb-interval]').value, 10) || 30);
  const hbClick = (btn, sel) => {
    if (activeHb === btn) { btn.classList.remove('running'); activeHb = null; onHeartbeat && onHeartbeat(null); return; }
    if (activeHb) activeHb.classList.remove('running');
    btn.classList.add('running'); activeHb = btn;
    // Heartbeat is a frequent liveness pulse — a small packet count keeps each
    // tick quick and resource-light (one-shot Test Latency uses the larger 5000).
    const params = { count: 1000, rate: 20000, warmup: 500, interval_us: 100, timeout_sec: 25, intervalSec: hbIntervalSec() };
    onHeartbeat && onHeartbeat({ ...sel, ...params });
  };
  el.querySelectorAll('[data-hb-ucast]').forEach((b) => b.addEventListener('click', () => hbClick(b, { kind: 'ucast', variation: b.dataset.hbUcast })));
  el.querySelectorAll('[data-hb-mcast]').forEach((b) => b.addEventListener('click', () => {
    const m = b.dataset.hbMcast;
    hbClick(b, { kind: 'mcast', modes: m === 'all' ? ['copy', 'inplace', 'kernel'] : [m] });
  }));

  return {
    setMode(m) { mode = m; paintMode(); },
    setLive(on) { liveOn = on; paintLive(); syncSections(); },
    setStatus(text) { statusEl.textContent = text || ''; },
    endRun() { endRunUI(); },
    setStats({ nodes = 0, online = 0, edges = 0 } = {}) {
      statsEl.innerHTML = `<b>${online}</b>/${nodes} online &middot; <b>${edges}</b> edges`;
    },
    // Populate the Show dropdown with saved-run browse results (dev-only API).
    setResults(runs) {
      if (!runs || !runs.length) return;
      viewSel.innerHTML = ['<option value="" disabled selected>Browse results\u2026 (' + runs.length + ')</option>']
        .concat(runs.map((r) => '<option data-run="' + esc(r.path) + '" value="run:' + esc(r.path) + '">' + esc(r.path) + '</option>'))
        .join('');
    },
    // combos: [{kind,variation,unix}]; sel: {kind,variation} currently shown
    setCombos(combos, sel) { renderCombos(combos, sel); },
    dispose() { el.remove(); },
  };
}
