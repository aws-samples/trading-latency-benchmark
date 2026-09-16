// 2d/panels.js — shared panel behaviour: drag, fold, proportional resize.
//
// The h3 header is OUTSIDE the scaler (its height never changes from resize).
// On fold: panel shrinks to header text width, resize disabled.
// On unfold: restores original position and size.

import { fmtLat, fmtRange, capabilityColor, buildCapabilityScale, CAP_GRADIENT_CSS, esc } from './palette.js';
import { applySel } from './selection.js';

// Resize floor is the panel's ORIGINAL size (1.0×) — a panel may grow up to
// MAX_K× but never shrink below what it was first laid out at.
const MIN_K = 1.0, MAX_K = 2.0;

// Registry of all enhanced panels so a single control (the control panel's
// fold-all button) can collapse/expand every panel at once. Each entry exposes
// setCollapsed(want) and isCollapsed(); entries unregister themselves on cleanup.
const foldables = new Set();
export function foldAllPanels(collapse) { setAllFolded(collapse); }
export function anyPanelExpanded() { for (const f of foldables) if (!f.isCollapsed()) return true; return false; }
export function resetAllPanels() { foldables.forEach((f) => f.reset && f.reset()); }

// ── Shared corner placement ──────────────────────────────────────────────────
// The 2D map and the 3D scene both scatter their info panels across the four
// viewport corners (control panel = top-left, always). This is the single
// source of truth for that distribution so both renderers stay in lock-step.
// corner ∈ 'tl' | 'tr' | 'bl' | 'br'. Sets inline top/left/right/bottom (which
// override any CSS default) so a panel lands — and, after a fold/reset, returns
// — to the same corner in either view.
export const PANEL_MARGIN = 16;
const CORNER_SIDES = { tl: ['top', 'left'], tr: ['top', 'right'], bl: ['bottom', 'left'], br: ['bottom', 'right'] };
export function placePanel(el, corner) {
  el.style.top = el.style.bottom = el.style.left = el.style.right = 'auto';
  const [v, h] = CORNER_SIDES[corner] || CORNER_SIDES.tr;
  el.style[v] = PANEL_MARGIN + 'px';
  el.style[h] = PANEL_MARGIN + 'px';
}

// ── Shared boundary-visibility toggles (Account/Region/VPC/AZ) ───────────────
// Builds a labelled row of checkboxes for the legend panel. Framework-agnostic:
// the caller supplies onToggle(level, enabled) and shows/hides its own boundary
// objects (2D contour DOM / 3D scene objects). Used by both renderers so the
// control's markup + behaviour live in one place. Returns the wrapper element.
export function buildBoundaryToggles(onToggle, initial = {}, extras = []) {
  const wrap = document.createElement('div');
  wrap.className = 'boundary-toggles';
  wrap.innerHTML = '<div class="bt-title">Show</div>';
  const rowEl = document.createElement('div'); rowEl.className = 'bt-row';
  [['account', 'Account'], ['region', 'Region'], ['vpc', 'VPC'], ['az', 'AZ'], ['pg', 'PG']].forEach(([key, label]) => {
    const lab = document.createElement('label'); lab.className = 'bt-item';
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.checked = initial[key] !== false;
    cb.addEventListener('change', () => onToggle(key, cb.checked));
    lab.appendChild(cb); lab.appendChild(document.createTextNode(' ' + label));
    rowEl.appendChild(lab);
  });
  // Extra checkboxes sharing the same row (e.g. "Links"). Each: {label, onChange, checked}.
  extras.forEach(({ label, onChange, checked }) => {
    const lab = document.createElement('label'); lab.className = 'bt-item';
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.checked = checked !== false;
    cb.addEventListener('change', () => onChange(cb.checked));
    lab.appendChild(cb); lab.appendChild(document.createTextNode(' ' + label));
    rowEl.appendChild(lab);
  });
  wrap.appendChild(rowEl);
  return wrap;
}

// A live update remounts the 2D view, so panel geometry and fold state live here
// rather than on the elements, which are rebuilt each time.
import { makeFoldable, setAllFolded, unregister } from '../fold.js';

const PANEL_STATE_KEY = 't2d-panel-state';
const PANEL_STATE = (() => {
  try { return JSON.parse(localStorage.getItem(PANEL_STATE_KEY)) || {}; } catch { return {}; }
})();
const savePanelState = () => {
  try { localStorage.setItem(PANEL_STATE_KEY, JSON.stringify(PANEL_STATE)); } catch { /* ignore */ }
};
const panelKey = (el) => el.id
  || (el.className || '').split(/\s+/).filter(Boolean).join('.')
  || 'panel';

export function enhancePanel(ctx, el, track = true, corner = null) {
  const h = el.querySelector('h3');
  if (!h) return () => {};

  // Caret.
  const caret = document.createElement('span');
  caret.className = 'panel-caret'; caret.textContent = '\u2304';
  h.insertBefore(caret, h.firstChild);

  // Structure: el > h3 (immutable height) + content > scaler > [body]
  const content = document.createElement('div'); content.className = 'panel-content';
  while (h.nextSibling) content.appendChild(h.nextSibling);
  el.appendChild(content);
  const scaler = document.createElement('div'); scaler.className = 'panel-scale';
  while (content.firstChild) scaler.appendChild(content.firstChild);
  content.appendChild(scaler);

  // Initial corner placement (shared 2D/3D). Overrides CSS defaults.
  if (corner) placePanel(el, corner);

  // Proportional resize. Header stays fixed; only the body scales. Resize is
  // WIDTH-DRIVEN ONLY: dragging the width zooms the content and the panel height
  // auto-follows the scaled content, so the resulting window always shows the
  // full content — never clipped, never with empty space. (Vertical user-resize
  // is disabled below via resize:horizontal.)
  let baseW = 0;
  let collapsed = false;
  const update = () => {
    // While folded the panel is intentionally sized to its header text; the
    // ResizeObserver must not run the width clamp / height reservation or it
    // would immediately snap the folded header back to full width.
    if (collapsed) return;
    if (!baseW) { baseW = el.clientWidth || 1; scaler.style.width = baseW + 'px'; }
    let k = el.clientWidth / baseW;
    if (k < MIN_K) { k = MIN_K; el.style.width = Math.round(baseW * MIN_K) + 'px'; }
    if (k > MAX_K) { k = MAX_K; el.style.width = Math.round(baseW * MAX_K) + 'px'; }
    scaler.style.transform = 'scale(' + k + ')';
    scaler.style.transformOrigin = 'top left';
    // Reserve exactly the scaled content height. A CSS transform doesn't change
    // layout box size, so we set the wrapper's height explicitly; the panel's
    // own height is left auto (overflow:hidden) and therefore fits header +
    // content precisely. Guard against re-triggering the observer needlessly.
    const scaledH = Math.ceil(scaler.offsetHeight * k) + 'px';
    if (content.style.height !== scaledH) content.style.height = scaledH;
    el.style.maxHeight = '';
  };
  const ro = new ResizeObserver(update); ro.observe(el);
  // Also observe the content wrapper: when the body's height changes (e.g. the
  // control panel swaps normal↔live sections, or combos populate), recompute so
  // the panel sticks to its content instead of clipping / leaving empty space.
  ro.observe(scaler);
  // Only width is user-resizable; height always fits the full content.
  el.style.resize = 'horizontal';

  // ── drag ──────────────────────────────────────────────────────────────────
  let dragging = false, moved = false, sx = 0, sy = 0, ox = 0, oy = 0;
  h.addEventListener('mousedown', (e) => {
    if (e.target === caret || (e.target.closest && (e.target.closest('button') || e.target.closest('select')))) return;
    dragging = true; moved = false; sx = e.clientX; sy = e.clientY;
    const r = el.getBoundingClientRect(); ox = r.left; oy = r.top;
    el.style.right = el.style.bottom = 'auto';
    el.style.left = ox + 'px'; el.style.top = oy + 'px';
    e.preventDefault();
  });
  const onMove = (e) => {
    if (!dragging) return;
    if (Math.abs(e.clientX - sx) + Math.abs(e.clientY - sy) > 3) moved = true;
    el.style.left = (ox + e.clientX - sx) + 'px'; el.style.top = (oy + e.clientY - sy) + 'px';
  };
  const onUp = () => { dragging = false; };
  window.addEventListener('mousemove', onMove); window.addEventListener('mouseup', onUp);

  // ── fold ──────────────────────────────────────────────────────────────────
  // One shared folding path with the control panel - see lib/fold.js. A folded
  // panel keeps its position; only its content is hidden.
  const fold = makeFoldable(panelKey(el), caret, content, {
    onChange: (folded) => {
      collapsed = folded;
      el.style.minWidth = folded ? '0' : '';
      el.style.width = folded ? 'auto' : '';
      el.style.height = folded ? 'auto' : '';
      el.style.resize = folded ? 'none' : 'horizontal';
      if (!folded) update();
    },
  });
  const setCollapsed = (want) => fold.toggle(want);
  h.addEventListener('click', (e) => {
    if (moved) { moved = false; return; }
    // Ignore clicks on header controls (2D/3D, Live, fold-all) — otherwise the
    // bubbled click would toggle this panel's own fold and desync fold-all.
    if (e.target.closest && e.target.closest('button, select, a')) return;
    setCollapsed(!collapsed);
  });
  // Restore this panel to its default (CSS) position/size and expanded state.
  const reset = () => {
    if (collapsed) { collapsed = false; caret.textContent = '\u25be'; content.style.display = ''; }
    el._snap = null;
    el.style.left = el.style.right = el.style.top = el.style.bottom = '';
    el.style.width = el.style.height = el.style.minWidth = el.style.maxHeight = '';
    el.style.resize = 'horizontal';
    if (corner) placePanel(el, corner);   // return to the assigned corner, not the CSS default
    baseW = 0;
    update();
  };
  const foldEntry = { setCollapsed, isCollapsed: () => collapsed, reset };
  foldables.add(foldEntry);
  const cleanup = () => { foldables.delete(foldEntry); ro.disconnect(); window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp); };
  if (track) ctx.disposers.push(cleanup);
  return cleanup;
  // Restore this panel's saved geometry and fold, and keep the store current.
  const pk = panelKey(el), st = PANEL_STATE[pk];
  if (st) {
    if (st.left) el.style.left = st.left;
    if (st.top) el.style.top = st.top;
    if (st.width) el.style.width = st.width;
    if (st.height) el.style.height = st.height;
  }
  const record = () => {
    PANEL_STATE[pk] = {
      left: el.style.left, top: el.style.top, width: el.style.width,
      height: el.style.height,
    };
    savePanelState();
  };
  el.addEventListener('mouseup', record);
  const mo = new MutationObserver(record);
  mo.observe(el, { attributes: true, attributeFilter: ['class', 'style'] });
  ctx.disposers.push(() => mo.disconnect());

}

// Dedicated enhancer for pinned latency panels. Unlike enhancePanel it does NOT
// restructure the DOM (no scaler, no caret) — the panel stays a pixel-faithful
// clone of the hover tooltip (inherits its padding/nowrap/auto-size), so pinning
// never changes the content's style, indent, or size. It only adds: drag by the
// header, and participation in fold-all (fold hides the body; reset restores the
// original position + expanded state). Returns a cleanup fn.
export function enhancePinned(el, opts = {}) {
  const h = el.querySelector('h3');
  if (!h) return () => {};
  h.style.cursor = 'move';
  h.style.userSelect = 'none';

  // The panel lives inside the zoomed viewport, so a pointer delta is divided by
  // the scale to move it by the same visual distance.
  const scaleOf = () => (opts.scale && opts.scale()) || 1;
  let dragging = false, sx = 0, sy = 0, ox = 0, oy = 0;

  const down = (e) => {
    if (e.target.closest && e.target.closest('button,select,a')) return;
    dragging = true;
    sx = e.clientX; sy = e.clientY;
    ox = parseFloat(el.style.left) || 0;
    oy = parseFloat(el.style.top) || 0;
    e.preventDefault();
  };
  const move = (e) => {
    if (!dragging) return;
    const k = scaleOf();
    el.style.left = (ox + (e.clientX - sx) / k) + 'px';
    el.style.top = (oy + (e.clientY - sy) / k) + 'px';
  };
  const up = () => {
    if (!dragging) return;
    dragging = false;
    // Report the resting place so it survives the next remount.
    if (opts.onMove) opts.onMove(parseFloat(el.style.left) || 0, parseFloat(el.style.top) || 0);
  };

  h.addEventListener('mousedown', down);
  window.addEventListener('mousemove', move);
  window.addEventListener('mouseup', up);
  return () => {
    h.removeEventListener('mousedown', down);
    window.removeEventListener('mousemove', move);
    window.removeEventListener('mouseup', up);
  };
}

// ── Shared panel content builders (used by both 2D and 3D renderers) ─────────

// Build the Summary panel's inner HTML. `opts`: { N, pairs, minP50, maxP50,
// minP99, maxP99, minSigma, maxSigma, nodes[], stress? }.
// Instructions collapse behind their own chevron, shared by the 2D and 3D
// legends so both behave the same. Takes the hint rows already built.
export function instructionsHTML(rowsHtml) {
  return '<div class="ux-instr">'
    + '<div class="instr-head" data-instr-toggle>'
    + '<span class="instr-chevron">\u2304</span> Instructions</div>'
    + '<div class="ux-hint" data-instr-body style="display:none">' + rowsHtml + '</div>'
    + '</div>';
}

// Bind the toggle inside a legend. Safe to call again after a repaint.
export function wireInstructions(el) {
  const head = el && el.querySelector('[data-instr-toggle]');
  const body = el && el.querySelector('[data-instr-body]');
  if (!head || !body || head.dataset.instrBound) return;
  head.dataset.instrBound = '1';
  head.classList.add('collapsed');
  head.addEventListener('click', (e) => {
    e.stopPropagation();
    const open = body.style.display !== 'none';
    body.style.display = open ? 'none' : '';
    head.classList.toggle('collapsed', open);
  });
}

export function buildSummaryHTML(opts) {
  const { N, pairs, minP50, maxP50, minP99, maxP99, minSigma, maxSigma, nodes, stress } = opts;
  const stat = (label, val) => '<div class="stat"><span>' + label + '</span><span class="val">' + esc(val) + '</span></div>';
  const statList = (label, arr) => '<div class="stat"><span>' + label + '</span><span class="val val-list">' + arr.map(esc).join('<br>') + '</span></div>';

  const uniq = (k) => [...new Set((nodes || []).map(n => n[k]))].filter(v => v && v !== 'unknown');
  const uRegions = uniq('region'), uAZs = uniq('az'), uCPGs = uniq('cpg_name'), uAccounts = uniq('account');
  let scopeHtml = '';
  if (uCPGs.length === 1) scopeHtml += stat('Placement Group', uCPGs[0]); else if (uCPGs.length > 1) scopeHtml += statList('PGs', uCPGs);
  if (uAZs.length === 1) scopeHtml += stat('AZ', uAZs[0]); else if (uAZs.length > 1) scopeHtml += stat('AZs', uAZs.length);
  if (uRegions.length === 1) scopeHtml += stat('Region', uRegions[0]); else if (uRegions.length > 1) scopeHtml += statList('Regions', uRegions);
  if (uAccounts.length === 1) scopeHtml += stat('Account', uAccounts[0]); else if (uAccounts.length > 1) scopeHtml += stat('Accounts', uAccounts.length);

  let html = '<h3>Summary</h3>'
    + stat('Nodes', N) + stat('Pairs', pairs)
    + stat('p50', fmtRange(minP50, maxP50))
    + stat('p99', fmtRange(minP99, maxP99))
    + stat('Jitter \u03c3', fmtRange(minSigma, maxSigma));
  if (scopeHtml) html += '<div style="margin-top:8px;border-top:1px solid #30363d;padding-top:6px">' + scopeHtml + '</div>';
  if (stress != null) html += '<div class="stress">Layout fidelity: <span class="val">' + (100 - stress * 100).toFixed(1) + '%</span></div>';
  return html;
}

// Build Instance Types panel rows HTML. Returns '' if no known types.
export function buildInstanceTypesHTML(nodes, region, capScale) {
  const seen = new Map(); (nodes || []).forEach(n => { if (!seen.has(n.type)) seen.set(n.type, n); });
  let rows = '';
  for (const [type, node] of seen) {
    if (type === 'unknown') continue;
    const colors = capabilityColor(node, capScale), r = 13, family = type.split('.')[0];
    const eType = esc(type);
    const specs = [];
    if (node.vcpus) specs.push(esc(node.vcpus) + 'vCPU');
    if (node.mem_gb) specs.push(esc(node.mem_gb) + 'GB');
    if (node.bw_gbps) specs.push(esc(node.bw_gbps) + 'Gbps');
    if (node.pps_mpps) specs.push(esc(node.pps_mpps) + 'Mpps');
    if (node.enis) specs.push(esc(node.enis) + ' ENIs');
    if (node.nitro_gen) specs.push('Nitro ' + esc(node.nitro_gen));
    const specsHtml = specs.length ? '<div class="type-specs">' + specs.join(' \u00b7 ') + '</div>' : '';
    rows += '<div class="type-row"><div class="type-dot" style="width:' + (r*2) + 'px;height:' + (r*2) + 'px;background:' + colors.bg + ';border:2px solid ' + colors.border + '"></div>'
      + '<div class="type-info"><div class="type-name">' + eType + '</div>' + specsHtml + '</div>'
      + '<a href="https://instances.vantage.sh/?selected=' + encodeURIComponent(type) + '&region=' + encodeURIComponent(region) + '" target="_blank" rel="noopener noreferrer">specs\u2197</a>'
      + '<a href="https://aws.amazon.com/ec2/instance-types/' + encodeURIComponent(family) + '/" target="_blank" rel="noopener noreferrer">family\u2197</a></div>';
  }
  return rows;
}

export function renderPanels(ctx) {
  const { fleet, root, statsEl, N, region, stress } = ctx;
  const { minP50, maxP50, minP99, maxP99, minSigma, maxSigma, allP50 } = ctx.ranges;
  const capScale = buildCapabilityScale(fleet.nodes);   // uniform blue→green over present types

  // Instance Types panel (shared logic via buildInstanceTypesHTML).
  const itHtml = buildInstanceTypesHTML(fleet.nodes, region, capScale);
  if (itHtml) {
    const el = document.createElement('div'); el.className = 'instance-legend';
    el.innerHTML = '<h3>Instance Types</h3>' + itHtml; root.appendChild(el);
  }

  (function () {
    const el = document.createElement('div'); el.className = 'vis-legend';
    el.innerHTML = '<h3>Legend</h3>'
      + '<div class="ux-instr"><div class="instr-head" data-instr-toggle><span class="instr-chevron">\u2304</span> Instructions</div>'
      + '<div class="ux-hint" data-instr-body style="display:none">'
      + '<div class="hint-row"><b>Hover</b> a node \u2014 show its edge labels</div>'
      + '<div class="hint-row"><b>Click</b> a node \u2014 pin its latency table</div>'
      + '<div class="hint-row"><b>Drag</b> a panel title to move it; click to fold; drag its corner to resize</div>'
      + '</div></div>'
      + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,#9abe5a,#f0883e,#f85149)"></div><span>Edge color = p50 (green=fast, red=slow)</span></div>'
      + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,rgba(57,211,83,0.7),rgba(57,211,83,0.07))"></div><span>Edge opacity = p50 (faster = more opaque)</span></div>'
      + '<div class="row"><div class="swatch" style="background:' + CAP_GRADIENT_CSS + '"></div><span>Node color = capability (blue=basic \u2192 green=metal/top-net)</span></div>'
      + '<div class="row"><span>Distance \u221d p50 \u2014 stress ' + (stress * 100).toFixed(1) + '%</span></div>'
      + '<div class="contour-samples">'
      + '<span style="border:1.5px dashed rgba(88,166,255,0.3);color:#58a6ff">VPC</span>'
      + '<span style="border:2px dashed rgba(163,113,247,0.4);color:#c084fc">AZ</span>'
      + '<span style="border:1.5px dashed rgba(57,211,83,0.3);color:#39d353">Region</span>'
      + '<span style="border:1.5px solid rgba(248,81,73,0.5);color:#f85149">Account</span></div>'
      + '</div>';
    // Shared Boundaries toggles — show/hide contour levels (and VPC peering lines).
    // Shared Show toggles — boundary levels + Links (edge) visibility in one row.
    el.appendChild(buildBoundaryToggles((key, on) => {
      const disp = on ? '' : 'none';
      if (key === 'pg') { root.querySelectorAll('.pg-badge').forEach((c) => { c.style.display = disp; }); return; }
      root.querySelectorAll('.contour.' + key).forEach((c) => { c.style.display = disp; });
      if (key === 'vpc') {
        root.querySelectorAll('.peering-line, .peering-hit').forEach((c) => { c.style.display = disp; });
        if (!on) root.querySelectorAll('.peering-label').forEach((c) => { c.style.display = 'none'; });
      }
    }, {}, [
      { label: 'Links', checked: true, onChange: (on) => { ctx.linksHidden = !on; applySel(ctx, -1); } },
      // Matches the 3D legend: IPs and the role badge hide together, so the two
      // views expose the same control.
      { label: 'Node data', checked: true, onChange: (on) => {
        const disp = on ? '' : 'none';
        root.querySelectorAll('.node .ip, .node .role-badge').forEach((c) => { c.style.display = disp; });
      } },
    ]));
    
  // Instructions collapse behind their own chevron header.
  wireInstructions(el);
  root.appendChild(el);
  })();

  // Summary panel (shared logic via buildSummaryHTML).
  statsEl.innerHTML = buildSummaryHTML({
    N, pairs: allP50.length, minP50, maxP50, minP99, maxP99, minSigma, maxSigma,
    nodes: fleet.nodes, stress,
  });

  // Scatter the info panels across the corners (shared placement): summary
  // top-right, instance types bottom-left, legend bottom-right (control panel
  // owns top-left). Same distribution as the 3D view.
  root.querySelectorAll('.stats, .vis-legend, .instance-legend').forEach(el => {
    const corner = el.classList.contains('stats') ? 'tr' : el.classList.contains('instance-legend') ? 'bl' : 'br';
    enhancePanel(ctx, el, true, corner);
  });
}
