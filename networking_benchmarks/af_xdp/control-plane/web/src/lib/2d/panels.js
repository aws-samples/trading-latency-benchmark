// 2d/panels.js — shared panel behaviour: drag, fold, proportional resize.
//
// The h3 header is OUTSIDE the scaler (its height never changes from resize).
// On fold: panel shrinks to header text width, resize disabled.
// On unfold: restores original position and size.

import { fmtRange, nodeRadius, getNodeColors, esc } from './palette.js';

// Resize floor is the panel's ORIGINAL size (1.0×) — a panel may grow up to
// MAX_K× but never shrink below what it was first laid out at.
const MIN_K = 1.0, MAX_K = 2.0;

// Registry of all enhanced panels so a single control (the control panel's
// fold-all button) can collapse/expand every panel at once. Each entry exposes
// setCollapsed(want) and isCollapsed(); entries unregister themselves on cleanup.
const foldables = new Set();
export function foldAllPanels(collapse) { foldables.forEach((f) => f.setCollapsed(collapse)); }
export function anyPanelExpanded() { for (const f of foldables) if (!f.isCollapsed()) return true; return false; }
export function resetAllPanels() { foldables.forEach((f) => f.reset && f.reset()); }

export function enhancePanel(ctx, el, track = true) {
  const h = el.querySelector('h3');
  if (!h) return () => {};

  // Caret.
  const caret = document.createElement('span');
  caret.className = 'panel-caret'; caret.textContent = '\u25be';
  h.insertBefore(caret, h.firstChild);

  // Structure: el > h3 (immutable height) + content > scaler > [body]
  const content = document.createElement('div'); content.className = 'panel-content';
  while (h.nextSibling) content.appendChild(h.nextSibling);
  el.appendChild(content);
  const scaler = document.createElement('div'); scaler.className = 'panel-scale';
  while (content.firstChild) scaler.appendChild(content.firstChild);
  content.appendChild(scaler);

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
  const setCollapsed = (want) => {
    if (want === collapsed) return;
    collapsed = want;
    caret.textContent = collapsed ? '\u25b8' : '\u25be';
    if (collapsed) {
      const r = el.getBoundingClientRect();
      const vh = window.innerHeight;
      el._snap = { left: r.left, top: r.top };
      content.style.display = 'none';
      // Shrink-to-fit the header text: width:auto on a fixed/absolute element is
      // shrink-to-fit, but the panels carry a min-width in CSS that would keep
      // them full-width — override it while folded so only the title shows.
      el.style.minWidth = '0';
      el.style.width = 'auto'; el.style.height = 'auto'; el.style.maxHeight = '';
      el.style.resize = 'none';   // locked when folded
      el.style.top = el.style.bottom = 'auto'; el.style.right = 'auto';
      el.style.left = r.left + 'px';
      if (r.top + r.height / 2 < vh / 2) el.style.top = '14px';
      else el.style.bottom = '14px';
    } else {
      if (el._snap) {
        el.style.left = el.style.right = el.style.top = el.style.bottom = 'auto';
        el.style.left = el._snap.left + 'px';
        el.style.top  = el._snap.top + 'px';
        el._snap = null;
      }
      el.style.minWidth = '';
      el.style.width = ''; el.style.height = ''; el.style.maxHeight = '';
      el.style.resize = 'horizontal';
      content.style.display = '';
      update();
    }
  };
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
    baseW = 0;
    update();
  };
  const foldEntry = { setCollapsed, isCollapsed: () => collapsed, reset };
  foldables.add(foldEntry);
  const cleanup = () => { foldables.delete(foldEntry); ro.disconnect(); window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp); };
  if (track) ctx.disposers.push(cleanup);
  return cleanup;
}

// Dedicated enhancer for pinned latency panels. Unlike enhancePanel it does NOT
// restructure the DOM (no scaler, no caret) — the panel stays a pixel-faithful
// clone of the hover tooltip (inherits its padding/nowrap/auto-size), so pinning
// never changes the content's style, indent, or size. It only adds: drag by the
// header, and participation in fold-all (fold hides the body; reset restores the
// original position + expanded state). Returns a cleanup fn.
export function enhancePinned(el, def) {
  const h = el.querySelector('h3');
  if (!h) return () => {};
  h.style.cursor = 'move'; h.style.userSelect = 'none';
  const body = [...el.children].filter((n) => n !== h);

  let collapsed = false, dragging = false, moved = false, sx = 0, sy = 0, ox = 0, oy = 0;
  let snap = null;   // position before folding, restored on unfold
  const md = (e) => {
    if (e.target.closest && e.target.closest('button,select,a')) return;
    dragging = true; moved = false; sx = e.clientX; sy = e.clientY;
    const r = el.getBoundingClientRect(); ox = r.left; oy = r.top;
    el.style.right = el.style.bottom = 'auto'; el.style.left = ox + 'px'; el.style.top = oy + 'px';
    e.preventDefault();
  };
  const mm = (e) => {
    if (!dragging) return;
    if (Math.abs(e.clientX - sx) + Math.abs(e.clientY - sy) > 3) moved = true;
    el.style.left = (ox + e.clientX - sx) + 'px'; el.style.top = (oy + e.clientY - sy) + 'px';
  };
  const mu = () => { dragging = false; };
  h.addEventListener('mousedown', md);
  window.addEventListener('mousemove', mm); window.addEventListener('mouseup', mu);

  const setCollapsed = (want) => {
    if (want === collapsed) return;
    collapsed = want;
    body.forEach((n) => { n.style.display = collapsed ? 'none' : ''; });
    if (collapsed) {
      // Fold: keep only the header and stick it to the top browser edge.
      snap = { left: el.style.left, top: el.style.top };
      el.style.right = el.style.bottom = 'auto';
      el.style.top = '0px';
    } else if (snap) {
      el.style.left = snap.left; el.style.top = snap.top; snap = null;
    }
  };
  // Header click folds/unfolds (ignored right after a drag, or on inner controls).
  h.addEventListener('click', (e) => {
    if (moved) { moved = false; return; }
    if (e.target.closest && e.target.closest('button,select,a')) return;
    setCollapsed(!collapsed);
  });
  const reset = () => {
    collapsed = false; snap = null;
    body.forEach((n) => { n.style.display = ''; });
    el.style.right = el.style.bottom = 'auto'; el.style.left = def.left; el.style.top = def.top;
  };
  const entry = { setCollapsed, isCollapsed: () => collapsed, reset };
  foldables.add(entry);

  return () => { foldables.delete(entry); window.removeEventListener('mousemove', mm); window.removeEventListener('mouseup', mu); };
}

export function renderPanels(ctx) {
  const { fleet, root, statsEl, N, region, stress } = ctx;
  const { minP50, maxP50, minP99, maxP99, minSigma, maxSigma, allP50 } = ctx.ranges;

  (function () {
    const seen = new Map(); fleet.nodes.forEach(n => { if (!seen.has(n.type)) seen.set(n.type, n); });
    let rows = '';
    for (const [type, node] of seen) {
      const colors = getNodeColors(type), r = Math.round(nodeRadius(node) * 0.35), family = type.split('.')[0];
      const eType = esc(type);
      rows += '<div class="type-row"><div class="type-dot" style="width:' + (r*2) + 'px;height:' + (r*2) + 'px;background:' + colors.bg + ';border:2px solid ' + colors.border + '"></div>'
        + '<div class="type-info"><div class="type-name">' + eType + '</div><div class="type-specs">' + esc(node.vcpus) + 'vCPU \u00b7 ' + esc(node.mem_gb) + 'GB \u00b7 ' + esc(node.bw_gbps) + 'Gbps \u00b7 ' + esc(node.pps_mpps) + 'Mpps \u00b7 ' + esc(node.enis) + ' ENIs \u00b7 Nitro ' + esc(node.nitro_gen) + '</div></div>'
        + '<a href="https://instances.vantage.sh/?selected=' + encodeURIComponent(type) + '&region=' + encodeURIComponent(region) + '" target="_blank" rel="noopener noreferrer">specs\u2197</a>'
        + '<a href="https://aws.amazon.com/ec2/instance-types/' + encodeURIComponent(family) + '/" target="_blank" rel="noopener noreferrer">family\u2197</a></div>';
    }
    const el = document.createElement('div'); el.className = 'instance-legend';
    el.innerHTML = '<h3>Instance Types</h3>' + rows; root.appendChild(el);
  })();

  (function () {
    const el = document.createElement('div'); el.className = 'vis-legend';
    el.innerHTML = '<h3>Legend</h3>'
      + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,#39d353,#f0883e,#f85149)"></div><span>Edge color = p50 (green=fast, red=slow)</span></div>'
      + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,rgba(57,211,83,0.7),rgba(57,211,83,0.07))"></div><span>Edge opacity = p50 (faster = more opaque)</span></div>'
      + '<div class="row"><span>Node size = f(BW, PPS, ENIs, Nitro, CPU, Mem, metal)</span></div>'
      + '<div class="row"><span>Distance \u221d p50 \u2014 stress ' + (stress * 100).toFixed(1) + '%</span></div>'
      + '<div class="contour-samples">'
      + '<span style="border:1.5px dashed rgba(57,211,83,0.3);color:#39d353">VPC</span>'
      + '<span style="border:2px dashed rgba(163,113,247,0.4);color:#c084fc">AZ</span>'
      + '<span style="border:1.5px dashed rgba(88,166,255,0.3);color:#58a6ff">Region</span>'
      + '<span style="border:1.5px solid rgba(248,81,73,0.5);color:#f85149">Account</span></div>'
      + '<div class="ux-hint"><b>Hover</b> node \u2014 edge labels. <b>Click</b> \u2014 pin table. <b>Drag</b> title to move; click to fold; drag corner to resize.</div>';
    root.appendChild(el);
  })();

  const uniq = (k) => [...new Set(fleet.nodes.map(n => n[k]))].filter(v => v && v !== 'unknown');
  const uRegions = uniq('region'), uAZs = uniq('az'), uCPGs = uniq('cpg_name'), uAccounts = uniq('account');
  const stat = (label, val) => '<div class="stat"><span>' + label + '</span><span class="val">' + esc(val) + '</span></div>';
  // Multi-value scope (PGs/Regions): stack each name on its own row so long
  // names wrap as whole titles instead of breaking mid-spelling.
  const statList = (label, arr) => '<div class="stat"><span>' + label + '</span><span class="val val-list">' + arr.map(esc).join('<br>') + '</span></div>';
  let scopeHtml = '';
  if (uCPGs.length === 1) scopeHtml += stat('Placement Group', uCPGs[0]); else if (uCPGs.length > 1) scopeHtml += statList('PGs', uCPGs);
  if (uAZs.length === 1) scopeHtml += stat('AZ', uAZs[0]); else if (uAZs.length > 1) scopeHtml += stat('AZs', uAZs.length);
  if (uRegions.length === 1) scopeHtml += stat('Region', uRegions[0]); else if (uRegions.length > 1) scopeHtml += statList('Regions', uRegions);
  if (uAccounts.length === 1) scopeHtml += stat('Account', uAccounts[0]); else if (uAccounts.length > 1) scopeHtml += stat('Accounts', uAccounts.length);
  statsEl.innerHTML = '<h3>Summary</h3>'
    + stat('Nodes', N) + stat('Pairs', allP50.length)
    + stat('p50', fmtRange(minP50, maxP50))
    + stat('p99', fmtRange(minP99, maxP99))
    + stat('Jitter \u03c3', fmtRange(minSigma, maxSigma))
    + (scopeHtml ? '<div style="margin-top:8px;border-top:1px solid #30363d;padding-top:6px">' + scopeHtml + '</div>' : '')
    + '<div class="stress">Layout fidelity: <span class="val">' + (100 - stress * 100).toFixed(1) + '%</span></div>';

  root.querySelectorAll('.stats, .vis-legend, .instance-legend').forEach(el => enhancePanel(ctx, el));
}
