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
.cp-panel-caret{display:inline-block;transition:transform .12s;font-size:18px;color:#8b949e;margin-right:6px;transition:transform .15s;cursor:pointer;
  padding:2px 4px;border-radius:4px}
.cp-panel-caret:hover{background:rgba(88,166,255,.15);color:#58a6ff}
.cp-panel-caret.collapsed{transform:rotate(-90deg)}
.cp-panel-fold-btn{font-size:18px;line-height:1;color:#8b949e;cursor:pointer;user-select:none;padding:0 2px}
.cp-panel-fold-btn:hover{color:#e6edf3}
.cp-panel-body{padding:6px 12px 10px;background:#161b22}
.cp-panel .row{display:flex;align-items:center;gap:6px;margin:6px 0;flex-wrap:wrap}
.cp-panel .row.center{justify-content:center}
.cp-panel .grow{flex:1}
.cp-seg{display:flex;border:1px solid #30363d;border-radius:7px;overflow:hidden}
.cp-seg button{background:transparent;color:#8b949e;border:none;padding:5px 12px;cursor:pointer;font:600 12px inherit}
.cp-seg button.on{background:rgba(88,166,255,.18);color:#58a6ff}
.cp-seg a{background:transparent;color:#8b949e;border:none;padding:5px 12px;cursor:pointer;font:600 12px inherit;text-decoration:none;display:inline-block}
.cp-seg a:hover{color:#e6edf3;background:rgba(88,166,255,.12)}
.cp-seg a.disabled{opacity:.4;cursor:not-allowed;color:#6e7681}
.cp-seg a.disabled:hover{background:transparent;color:#6e7681}
.cp-live{background:transparent;color:#8b949e;border:1px solid #30363d;border-radius:7px;
  padding:5px 12px;cursor:pointer;font:600 12px inherit}
.cp-live.on{background:rgba(248,81,73,.16);color:#f85149;border-color:#da3633}
.cp-foldall{margin-left:auto;background:transparent;color:#8b949e;border:1px solid #30363d;
  border-radius:7px;padding:5px 10px;cursor:pointer;font:600 13px inherit;line-height:1}
.cp-foldall:hover{color:#e6edf3;border-color:#8b949e;background:rgba(88,166,255,.12)}
.cp-lbl{color:#6e7681;font:600 11px inherit;text-transform:uppercase;letter-spacing:.4px;flex-shrink:0;width:76px;display:inline-block;padding-right:10px;box-sizing:border-box}
.cp-section{color:#e6edf3;font:700 12px inherit;text-transform:uppercase;letter-spacing:.5px}
.cp-btn-group{display:flex;gap:4px;flex-wrap:nowrap;margin-left:4px}
.cp-stats{color:#8b949e;font:12px inherit;font-variant-numeric:tabular-nums}
.cp-stats b{color:#e6edf3}
.cp-clock{color:#8b949e;font:12px inherit;font-variant-numeric:tabular-nums;flex:1}
.cp-tz{flex:0 1 auto;width:auto;min-width:72px;max-width:150px}
.cp-sel{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px;padding:4px 6px;font:12px inherit;flex:1}
.cp-btn{background:#21262d;color:#adbac7;border:1px solid #30363d;border-radius:6px;
  padding:4px 8px;cursor:pointer;font:600 12px inherit;transition:background .15s,border-color .15s;text-decoration:none;display:inline-block}
.cp-btn:hover{background:#30363d;color:#fff}
.cp-btn:active,.cp-btn.running{background:rgba(240,136,62,.22);color:#f0883e;border-color:#f0883e}
.cp-btn:disabled,.cp-btn.disabled{opacity:.4;cursor:not-allowed;color:#6e7681;background:#1a1f26;border-color:#21262d}
.cp-btn.disabled:hover{background:#1a1f26;color:#6e7681}
.cp-btn-sm{padding:3px 7px;font-size:11px}
.cp-target-info{color:#8b949e;font:12px inherit;flex:1}
.cp-target-info.active{color:#ffd700}
.cp-presets{gap:4px}
.cp-presets .cp-cancel{margin-left:auto;margin-right:2px}
.cp-tip{color:#6e7681;font:10px inherit;font-style:italic}
.cp-presets button.on{background:rgba(240,136,62,.22);color:#f0883e;border-color:#f0883e}
.cp-cost-hint{color:#f0883e;font:10px inherit;margin-left:4px}
.cp-icon{background:#21262d;color:#adbac7;border:1px solid #30363d;border-radius:6px;padding:4px 9px;cursor:pointer;font:600 14px inherit;line-height:1;flex:0 0 auto}
.cp-icon:hover{background:#30363d;color:#fff}
.cp-num{width:72px;flex:0 0 72px;box-sizing:border-box;background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px;padding:3px 5px;font:12px inherit}
.cp-dim{color:#6e7681;font:11px inherit;margin-left:2px}
.cp-log-label{color:#6e7681;font:700 10px inherit;letter-spacing:.6px}
.cp-log-row{display:flex;align-items:center;gap:6px;margin:2px 0 3px}
.cp-log-row .cp-log-dl{margin-left:auto;padding:0 4px;font-size:11px;line-height:1.4}
.cp-status{background:#0d1117;border:1px solid #21262d;border-radius:6px;padding:6px 8px;white-space:pre-wrap;overflow-y:auto;max-height:140px;
  color:#3fb950;font:12px 'SF Mono','Fira Code',ui-monospace,Menlo,monospace;
  min-height:34px;max-height:120px;overflow:auto;white-space:pre-wrap;word-break:break-word;
  font-variant-numeric:tabular-nums}
.cp-hr{height:1px;background:#21262d;margin:4px -12px}
`;

import { enhancePanel } from './2d/panels.js';
import { makeFoldable, setAllFolded } from './fold.js';
import { esc } from './2d/palette.js';
import { SCOPES, SCOPE_AMONG, SCOPE_FANOUT, PRESETS, countPairs } from './pairs.js';

export function mountControls(host, opts = {}) {
  const OPS_LOG_MAX = 5000, LOG_TAIL = 40;
  const opsLog = [];
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

      <!-- NORMAL mode: View buttons + one-shot Run Tests -->
      <div data-normal>
        <div class="row"><span class="cp-lbl">View</span>
          <span class="cp-btn-group" data-view-seg></span>
        </div>
        <div class="cp-hr"></div>
      </div>

      <!-- Target block: shared between normal and live modes -->
      <div data-target-block>
        <div class="row center"><span class="cp-section">Targets</span><span class="cp-panel-caret collapsed" data-fold-targets>\u2304</span></div>
        <div data-targets-content style="display:none">
        <div class="row"><span class="cp-target-info" data-target-info>No selection \u2014 full mesh</span></div>
        <div class="row"><span class="cp-tip" data-target-tip>Mark an instance for a group selection</span></div>
          <div class="row cp-presets"><button class="cp-btn cp-btn-sm" data-preset="pg">PG</button><button class="cp-btn cp-btn-sm" data-preset="vpc">VPC</button><button class="cp-btn cp-btn-sm" data-preset="az">AZ</button><button class="cp-btn cp-btn-sm" data-preset="region">Region</button><button class="cp-btn cp-btn-sm" data-preset="all">All</button><button class="cp-btn cp-btn-sm cp-cancel" data-cancel-targets title="Clear the target set">Deselect</button></div>
        <div class="row"><select class="cp-sel" data-scope></select></div>
        </div>
      </div>
      <div class="cp-hr"></div>

      <div data-normal>
        <div class="row center"><span class="cp-section">Test Latency</span><span class="cp-panel-caret collapsed" data-fold-latency>\u2304</span></div>
        <div data-latency-content style="display:none">
        <div class="row"><span class="cp-lbl">Packets</span><input class="cp-num" data-count value="10000" title="Measurement packets per pair (100–1,000,000)"></div>
        <div class="row"><span class="cp-lbl">Rate</span><input class="cp-num" data-rate value="10000" title="Ucast send rate (1,000–1,000,000)"><span class="cp-dim">pps</span></div>
        <div class="row"><span class="cp-lbl">Interval</span><input class="cp-num" data-interval value="100" title="Mcast inter-packet interval (10–100,000)"><span class="cp-dim">µs</span></div>
        <div class="row"><span class="cp-lbl">Parallel</span><input class="cp-num" data-max-parallel value="4" title="Max concurrent pairs per round. Lower = more accurate (less NIC contention). 1 = fully serial (slowest, most correct)."><span class="cp-dim">pairs</span></div>
        <div class="row"><span class="cp-lbl">Warmup</span><input class="cp-num" data-warmup value="1000" title="Warmup packets before measurement begins"><span class="cp-dim">pkts</span></div>
        <div class="row"><span class="cp-lbl">Max loss</span><input class="cp-num" data-max-loss value="2" title="Reject a pair outright when its loss exceeds this percent. rtt computes percentiles ONLY from datagrams that returned, so a lossy run reports the latency of its surviving subset — a survivorship-biased number that is not comparable to a clean run. Rejected pairs are recorded as failures, not as results. Set -1 to disable (not recommended)."><span class="cp-dim">% loss</span></div>
        <div class="cp-hr"></div>
        <div class="row"><span class="cp-lbl">unicast</span></div>
        <div class="row">
          <span class="cp-btn-group">
            <button class="cp-btn" data-run-ucast="kernel" title="kernel sendto()/recvfrom() — full kernel network stack, AF_XDP echo on remote">kernel</button>
            <button class="cp-btn" data-run-ucast="xdp" title="Kernel bypass on sender - AF_XDP zero-copy TX + RX">xdp</button>
            <button class="cp-btn" data-run-ucast="all" title="Run both ucast variations sequentially (kernel then xdp)">all</button>
          </span>
        </div>
        <div class="row"><span class="cp-lbl">multicast</span></div>
        <div class="row">
          <span class="cp-btn-group">
            <button class="cp-btn" data-run-mcast="copy" title="Replicator copies each RX frame into a fresh TX buffer per destination — safest, one kernel alloc per fan-out">copy</button>
            <button class="cp-btn" data-run-mcast="inplace" title="Replicator patches destination MAC/IP in-place on the RX frame — zero-copy for the last destination, fastest fan-out">inplace</button>
            <button class="cp-btn" data-run-mcast="bpf_tx" title="XDP_TX in-kernel forward — no userspace replicator, single-destination only, lowest possible hop latency">bpf_tx</button>
            <button class="cp-btn" data-run-mcast="kernel" title="Plain UDP sockets end-to-end, no AF_XDP/eBPF anywhere — apples-to-apples kernel-stack baseline. Opt-in only: NOT included in 'all'.">kernel</button>
            <button class="cp-btn" data-run-mcast="all" title="Run the 3 AF_XDP mcast forward modes sequentially (copy → inplace → bpf_tx). kernel baseline is opt-in — run it separately.">all</button>
          </span>
        </div>
        </div>
      </div>

      <!-- LIVE mode: heartbeat — interval first, then pick a mode to re-run -->
      <div data-live-section style="display:none">
        <div class="row"><span class="cp-lbl">Every</span>
          <input class="cp-num" data-hb-interval value="30" title="Heartbeat interval - keep >= 30s (a full campaign takes several seconds)">
          <span class="cp-dim">sec (min 30)</span>
        </div>
        <div class="cp-hr"></div>
        <div class="row"><span class="cp-lbl">unicast</span></div>
        <div class="row"><span class="cp-btn-group">
          <button class="cp-btn" data-hb-ucast="kernel">kernel</button>
          <button class="cp-btn" data-hb-ucast="xdp">xdp</button>
          <button class="cp-btn" data-hb-ucast="all">all</button>
        </span></div>
        <div class="row"><span class="cp-lbl">multicast</span></div>
        <div class="row"><span class="cp-btn-group">
          <button class="cp-btn" data-hb-mcast="copy">copy</button>
          <button class="cp-btn" data-hb-mcast="inplace">inplace</button>
          <button class="cp-btn" data-hb-mcast="bpf_tx">bpf_tx</button>
          <button class="cp-btn" data-hb-mcast="kernel" title="Opt-in only: not included in 'all'">kernel</button>
          <button class="cp-btn" data-hb-mcast="all">all</button>
        </span></div>
      </div>

      <div class="cp-hr"></div>
      <div class="cp-log-row"><span class="cp-log-label">LOG</span><button class="cp-icon cp-log-dl" data-log-download title="Download the full session ops log">\u2913</button></div>
      <div class="cp-status" data-status></div>
      <div class="row" style="margin-top:6px"><button class="cp-btn cp-btn-sm" data-cancel-run disabled title="Cancel the in-flight campaign run">Cancel Run</button></div>
    </div>
  `;
  host.appendChild(el);

  // Use the shared enhancePanel for drag/fold/proportional-resize.
  const ctx = { disposers: [] };
  enhancePanel(ctx, el, false);


  const $ = (sel) => el.querySelector(sel);
  const segBtns = [...el.querySelectorAll('[data-mode]')];
  const liveBtn = $('[data-live]');
  // Fold state for both sections, bound once the markup exists.
  const foldables = [
    ['cp-targets', '[data-fold-targets]', '[data-targets-content]'],
    ['cp-latency', '[data-fold-latency]', '[data-latency-content]'],
  ].map(([key, btnSel, contentSel]) => makeFoldable(
    key, $(btnSel), $(contentSel), { defaultFolded: true },
  ));

  const foldAllBtn = $('[data-foldall]');
  // Toggle: 1st click folds EVERY panel (including this control panel); 2nd click
  // restores them ALL to their default position/size/expanded state, regardless
  // of whatever the user changed in between.
  let allFolded = false;
  foldAllBtn.addEventListener('click', () => {
    allFolded = !allFolded;
    setAllFolded(allFolded);
    foldAllBtn.textContent = allFolded ? '\u29C7' : '\u29C9';
  });
  const viewSeg = $('[data-view-seg]');
  const statsEl = $('[data-stats]');
  const statusEl = $('[data-status]');
  const num = (s) => Math.max(1, parseInt($(s).value, 10) || 0);
  // Loss threshold needs fractions and must allow -1 (disable), so it cannot use
  // num() which floors at 1.
  const numf = (s, dflt) => { const v = parseFloat($(s).value); return Number.isFinite(v) ? v : dflt; };
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
  let activeViewKind = null;
  const renderViewButtons = (kinds, sel) => {
    lastCombos = kinds; lastSel = sel;
    // Anchors styled identically to the .cp-btn mode buttons below. The report
    // is store-backed (fetches GET /api/measurements + /api/mcast-replicators
    // directly), NOT built from this live combos() list any more, so it must
    // not be gated on live SSE state having seen that kind yet - a fleet whose
    // store already has mcast history (e.g. after a backend restart, before
    // any new telemetry has streamed in) would otherwise show a permanently
    // disabled "mcast" link despite the report having real data to show.
    // Both links are always enabled; the report itself renders an empty-state
    // message if the store genuinely has nothing for that kind.
    viewSeg.innerHTML = ['ucast', 'mcast'].map((k) => {
      return `<a data-view-btn="${esc(k)}" class="cp-btn" href="?report=${esc(k)}" target="_blank" rel="noopener"`
        + ` title="Open the ${esc(k)} report in a new tab">${esc(k)}</a>`;
    }).join('');
  };

  tzSel.addEventListener('change', () => { selectedTz = tzSel.value; if (lastCombos) renderViewButtons(lastCombos, lastSel); });

  let mode = opts.initialMode || '2d';
  let liveOn = !!opts.initialLive;
  let activeHb = null;   // active heartbeat-mode button (live mode)
  const paintMode = () => segBtns.forEach((b) => b.classList.toggle('on', b.dataset.mode === mode));
  const paintLive = () => { liveBtn.classList.toggle('on', liveOn); liveBtn.textContent = liveOn ? '\u25CF Live' : 'Live'; };
  // Live mode swaps the panel body: hide the Show row + one-shot Run Tests, show
  // the heartbeat section (distinct settings). Clears any active heartbeat on exit.
  // Sections bind eagerly below; this only re-asserts after a repaint.
  function applyFoldState() { foldables.forEach((f) => f.apply()); }

  function syncSections() {
    el.querySelectorAll('[data-normal]').forEach((n) => { n.style.display = liveOn ? 'none' : ''; });
    const lv = $('[data-live-section]');
    if (lv) lv.style.display = liveOn ? '' : 'none';
    if (!liveOn && activeHb) { activeHb.classList.remove('running'); activeHb = null; }
    applyFoldState();
  }
  paintMode(); paintLive(); syncSections();

  segBtns.forEach((b) => b.addEventListener('click', () => { mode = b.dataset.mode; paintMode(); onSetMode && onSetMode(mode); }));
  liveBtn.addEventListener('click', () => { liveOn = !liveOn; paintLive(); syncSections(); onToggleLive && onToggleLive(liveOn); });
  // Track the currently running one-shot button. While a run is active it stays
  // orange; every other run button is disabled (gray) until the run finishes.
  const runBtns = [...el.querySelectorAll('[data-run-ucast],[data-run-mcast]')];
  let activeRunBtn = null;
  const cancelRunBtn = $('[data-cancel-run]');
  const endRunUI = () => {
    if (activeRunBtn) activeRunBtn.classList.remove('running');
    activeRunBtn = null;
    runBtns.forEach((b) => { b.disabled = false; });
    if (cancelRunBtn) cancelRunBtn.disabled = true;
  };
  const startRun = (btn, payload) => {
    if (activeRunBtn === btn) {          // second press on the active button = cancel
      onRun && onRun(null);
      endRunUI();
      return;
    }
    if (activeRunBtn) return;            // a run is active — others are disabled anyway
    activeRunBtn = btn;
    btn.classList.add('running');
    runBtns.forEach((b) => { if (b !== btn) b.disabled = true; });
    if (cancelRunBtn) cancelRunBtn.disabled = false;
    onRun && onRun(payload);
  };
  // Cancel Run button handler
  if (cancelRunBtn) {
    cancelRunBtn.addEventListener('click', () => {
      if (!cancelRunBtn.disabled) { onRun && onRun(null); endRunUI(); }
    });
  }

  el.querySelectorAll('[data-run-ucast]').forEach((b) => b.addEventListener('click', () => {
    startRun(b, { kind: 'ucast', variation: b.dataset.runUcast, count: clamp(num('[data-count]'), 100, 1000000), rate: clamp(num('[data-rate]'), 1000, 1000000), warmup: clamp(num('[data-warmup]'), 0, 100000), max_parallel: clamp(num('[data-max-parallel]'), 1, 100), max_loss_pct: clamp(numf('[data-max-loss]', 2), -1, 100) });
  }));
  el.querySelectorAll('[data-run-mcast]').forEach((b) => b.addEventListener('click', () => {
    const mcastMode = b.dataset.runMcast;
    const modes = mcastMode === 'all' ? ['copy', 'inplace', 'bpf_tx'] : [mcastMode];
    startRun(b, { kind: 'mcast', modes, count: clamp(num('[data-count]'), 100, 1000000), interval_us: clamp(num('[data-interval]'), 10, 100000), timeout_sec: 25 });
  }));

  $('[data-log-download]').addEventListener('click', () => {
    const blob = new Blob([opsLog.join('\n') + '\n'], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = Object.assign(document.createElement('a'), {
      href: url, download: `afxdp-ops-log-${Date.now()}.txt`,
    });
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 5000);
  });

  // ── Target set: scope select, presets, clear ────────────────────────────────
  const { onScopeChange, onPreset, onClearTargets } = opts;
  const scopeSel = $('[data-scope]');
  const targetInfo = $('[data-target-info]');
  const targetTip = $('[data-target-tip]');
  const cancelBtn = $('[data-cancel-targets]');
  // Scope options carry a live pair count, so each one states what it will
  // actually run instead of leaving the arrow notation to be decoded.
  const paintScopeOptions = (count, totalNodes) => {
    const prev = scopeSel.value;
    scopeSel.innerHTML = SCOPES.map((s) => {
      const n = count ? countPairs(totalNodes, count, s.id) : 0;
      const suffix = count ? ` \u2014 ${n} pair${n === 1 ? '' : 's'}` : '';
      return `<option value="${s.id}" title="${s.hint}">${s.label}${suffix}</option>`;
    }).join('');
    if (prev) scopeSel.value = prev;
  };
  paintScopeOptions(0, 0);
  scopeSel.disabled = true;
  scopeSel.addEventListener('change', () => { onScopeChange && onScopeChange(scopeSel.value); });
  // Chips pass the preset NAME; App resolves it against the fleet.
  cancelBtn.addEventListener('click', () => { onClearTargets && onClearTargets(); });
  el.querySelectorAll('[data-preset]').forEach((b) => b.addEventListener('click', () => {
    onPreset && onPreset(b.dataset.preset);
  }));

  let _lastTargetState = { count: 0, pairs: 0, scope: 'among', totalNodes: 0 };
  const scopeRow = scopeSel.closest('.row');
  if (scopeRow) scopeRow.style.display = 'none'; // hidden until selection exists
  const paintTargetBlock = ({ count, pairs, scope: sc, totalNodes, preset }) => {
    _lastTargetState = { count, pairs, scope: sc, totalNodes, preset };
    // Show the scope dropdown only when there is a selection
    if (scopeRow) scopeRow.style.display = count > 0 ? '' : 'none';
    // Each preset expands the marked instance into its group, so there is
    // nothing for them to act on until an instance is marked - EXCEPT 'all'
    // which selects every online node regardless of anchor.
    el.querySelectorAll('[data-preset]').forEach((b) => {
      const isAll = b.dataset.preset === 'all';
      b.disabled = isAll ? false : count === 0;
      b.classList.toggle('on', !!preset && b.dataset.preset === preset);
      b.title = (!isAll && count === 0)
        ? 'Mark an instance first'
        : `Select every instance in the same ${b.textContent.trim()} as the marked one`;
    });
    if (targetTip) targetTip.style.display = count === 0 ? '' : 'none';
    if (cancelBtn) cancelBtn.disabled = count === 0;
    scopeSel.disabled = count === 0;
    paintScopeOptions(count, totalNodes);
    scopeSel.value = sc;
    if (count === 0) {
      targetInfo.textContent = '';
      targetInfo.classList.remove('active');
    } else {
      targetInfo.textContent = `${count} selected \u00b7 ${pairs} pairs`;
      targetInfo.classList.add('active');
    }
  };

  // ── Fold state persistence (localStorage) ────────────────────────────────

  // ── Live heartbeat: choose a mode -> App re-runs it every interval (min 30s) ──
  let _targetIds = new Set();
  const hbIntervalSec = () => {
    const input = $('[data-hb-interval]');
    let v = parseInt(input.value, 10) || 30;
    if (v < 30) { v = 30; input.value = '30'; }
    return v;
  };
  const hbClick = (btn, sel) => {
    if (activeHb === btn) { btn.classList.remove('running'); activeHb = null; onHeartbeat && onHeartbeat(null); return; }
    if (activeHb) activeHb.classList.remove('running');
    btn.classList.add('running'); activeHb = btn;
    const params = { count: 1000, rate: 20000, warmup: 500, interval_us: 100, timeout_sec: 25, intervalSec: hbIntervalSec(),
      nodes: [..._targetIds], scope: scopeSel.value || 'among' };
    onHeartbeat && onHeartbeat({ ...sel, ...params });
  };
  el.querySelectorAll('[data-hb-ucast]').forEach((b) => b.addEventListener('click', () => hbClick(b, { kind: 'ucast', variation: b.dataset.hbUcast })));
  el.querySelectorAll('[data-hb-mcast]').forEach((b) => b.addEventListener('click', () => {
    const m = b.dataset.hbMcast;
    hbClick(b, { kind: 'mcast', modes: m === 'all' ? ['copy', 'inplace', 'bpf_tx'] : [m] });
  }));

  return {
    setMode(m) { mode = m; paintMode(); },
    setLive(on) { liveOn = on; paintLive(); syncSections(); },
    timezone() { return selectedTz; },
    setStatus(text) {
      // Keep the full session in the ring; display only the most recent line.
      if (text) {
        const t = new Date().toISOString().replace('T', ' ').slice(0, 19);
        opsLog.push(`${t}  ${text}`);
        if (opsLog.length > OPS_LOG_MAX) opsLog.splice(0, opsLog.length - OPS_LOG_MAX);
      }
      statusEl.textContent = opsLog.length ? opsLog[opsLog.length - 1] : '';
    },
    opsLog() { return opsLog.slice(); },
    endRun() { endRunUI(); },
    setStats({ nodes = 0, online = 0, edges = 0 } = {}) {
      statsEl.innerHTML = `<b>${online}</b>/${nodes} online &middot; <b>${edges}</b> links`;
    },
    // Populate the Show dropdown with saved-run browse results (dev-only API).
    setResults(runs) {
      // Browse results are no longer shown in a dropdown - log only.
      if (!runs || !runs.length) return;
    },
    // kinds: [{kind,unix}]; sel: {kind,variation} currently shown
    setCombos(kinds, sel) { renderViewButtons(kinds, sel);   applyFoldState();
    },
    setTargets(state) { paintTargetBlock(state); },
    setTargetIds(ids) { _targetIds = ids || new Set();   applyFoldState();
    },
    startRunUI() {
      // Mark cancel-run as enabled (for external callers / testing).
      if (cancelRunBtn) cancelRunBtn.disabled = false;
      runBtns.forEach((b) => { b.disabled = true; });
    },
    dispose() { el.remove(); },
  };
}
