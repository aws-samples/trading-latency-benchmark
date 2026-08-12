// 2d/index.js — 2D latency topology map. Orchestrates the concern-modules,
// which all operate on a shared `ctx`. Self-contained and independent of the 3D
// renderer (injects its own scoped styles; consumes the shared fleet.json model).
//
//   ctx = { fleet, matrix, N, W, H, CX, CY, region, root, svg, deselectBtn,
//           statsEl, positions, stress, ranges{...}, selected, nodeEls,
//           edgeElements, edgeLabelEls, disposers, unpinAll }

import { CSS } from './styles.js';
import { edgeSigma, isCrossRegion} from './palette.js';
import { computePositions } from './layout.js';
import { renderContours } from './contours.js';
import { renderEdges } from './edges.js';
import { renderNodes } from './nodes.js';
import { renderPanels } from './panels.js';
import { applySel } from './selection.js';
import { applyZoom, resetZoom } from './zoom.js';

const STYLE_ID = 'topology2d-styles';

export function mountTopology2D(container, fleet, opts = {}) {
  if (!document.getElementById(STYLE_ID)) {
    const style = document.createElement('style'); style.id = STYLE_ID; style.textContent = CSS; document.head.appendChild(style);
  }
  const root = document.createElement('div'); root.className = 't2d-root'; container.appendChild(root);
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.classList.add('edges'); root.appendChild(svg);
  const deselectBtn = document.createElement('button'); deselectBtn.className = 'deselect-btn'; deselectBtn.textContent = 'Deselect all'; root.appendChild(deselectBtn);
  const statsEl = document.createElement('div'); statsEl.className = 'stats'; root.appendChild(statsEl);

  const W = container.clientWidth || window.innerWidth;
  const H = container.clientHeight || window.innerHeight;
  const N = fleet.nodes.length, matrix = fleet.matrix;

  const ctx = {
    fleet, matrix, N, W, H, CX: W / 2, CY: H / 2, region: fleet.region || 'us-east-1',
    root, svg, deselectBtn, statsEl,
    selected: new Set(), nodeEls: [], edgeElements: [], edgeLabelEls: [], disposers: [],
    targetIds: opts.targetIds || new Set(),
    onToggleTarget: opts.onToggleTarget || (() => {}),
  };

  // ── global ranges ──────────────────────────────────────────────────────────
  const allP50 = [], allP99 = [], allSigma = [];
  // Cross-region cells are millisecond-scale; including them would crush every
    // intra-region cell onto the green stop, so they stay out of the range.
    const nodesArr = fleet.nodes || [];
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) if (matrix[i] && matrix[i][j]) {
      if (isCrossRegion(nodesArr[i], nodesArr[j])) continue;
      allP50.push(matrix[i][j].p50); allP99.push(matrix[i][j].p99);
    }
  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) { const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i]; if (!ab && !ba) continue; allSigma.push(edgeSigma(ab, ba)); }
  // Loop-based min/max avoids stack overflow from Math.min(...100k+ element array).
  const arrMin = (a) => { let m = Infinity;  for (let k = 0; k < a.length; k++) if (a[k] < m) m = a[k]; return a.length ? m : 0; };
  const arrMax = (a) => { let m = -Infinity; for (let k = 0; k < a.length; k++) if (a[k] > m) m = a[k]; return a.length ? m : 0; };
  // Pre-sort allP50 for percentile computation in edges.js (shared via ctx.ranges).
  allP50.sort((a, b) => a - b);
  ctx.ranges = {
    allP50,
    minP50: arrMin(allP50), maxP50: arrMax(allP50),
    minP99: arrMin(allP99), maxP99: arrMax(allP99),
    minSigma: arrMin(allSigma), maxSigma: arrMax(allSigma),
  };

  const { positions, stress } = computePositions(ctx);
  ctx.positions = positions; ctx.stress = stress;

  renderContours(ctx);
  renderEdges(ctx);
  renderNodes(ctx);
  renderPanels(ctx);
  applySel(ctx, -1);

  // ── pan: drag the empty canvas to move the whole map (panels stay put) ──────
  // Map layers (edges svg + node/contour/edge-label/peering-label) live in a
  // viewport that we translate; the info panels + deselect button remain fixed
  // in the root above it.
  const viewport = document.createElement('div'); viewport.className = 't2d-viewport';
  root.insertBefore(viewport, root.firstChild);
  viewport.appendChild(svg);
  root.querySelectorAll('.node, .contour, .edge-label, .peering-label').forEach((el) => viewport.appendChild(el));
  ctx.viewport = viewport;
  ctx.zoomScale = () => zoomState.scale;
  // A live update remounts the view, so zoom and pan are restored from the
  // caller's saved state instead of resetting to the default.
  const v0 = opts.view || {};
  let panX = v0.panX || 0, panY = v0.panY || 0, panning = false, psx = 0, psy = 0, pox = 0, poy = 0;
  // Don't start a pan when the press lands on something interactive (nodes,
  // panels, edge/peering labels) — those own their clicks/drags.
  const NO_PAN = '.node, .stats, .vis-legend, .instance-legend, .node-tooltip, .deselect-btn, .edge-label, .peering-hit, .peering-line';
  const onPanDown = (e) => {
    if (e.button !== 0 || (e.target.closest && e.target.closest(NO_PAN))) return;
    panning = true; psx = e.clientX; psy = e.clientY; pox = panX; poy = panY;
    root.style.cursor = 'grabbing'; e.preventDefault();
  };
  const onPanMove = (e) => { if (!panning) return; panX = pox + (e.clientX - psx); panY = poy + (e.clientY - psy); viewport.style.transform = 'translate(' + panX + 'px,' + panY + 'px)'; };
  const onPanUp = () => { if (panning) { panning = false; root.style.cursor = ''; } };
  root.addEventListener('mousedown', onPanDown);
  window.addEventListener('mousemove', onPanMove); window.addEventListener('mouseup', onPanUp);
  ctx.disposers.push(() => { root.removeEventListener('mousedown', onPanDown); window.removeEventListener('mousemove', onPanMove); window.removeEventListener('mouseup', onPanUp); });

  // ── zoom: wheel/pinch scales the viewport about the cursor ─────────────────
  const zoomState = { scale: v0.scale || 1, tx: v0.tx || 0, ty: v0.ty || 0 };
  const syncTransform = () => {
    viewport.style.transform = `translate(${panX + zoomState.tx}px,${panY + zoomState.ty}px) scale(${zoomState.scale})`;
  };
  // Override the pan transform to compose with zoom
  const onPanMoveZ = (e) => { if (!panning) return; panX = pox + (e.clientX - psx); panY = poy + (e.clientY - psy); syncTransform(); };
  const onPanUpZ = () => { if (panning) { panning = false; root.style.cursor = ''; } };
  window.removeEventListener('mousemove', onPanMove); window.removeEventListener('mouseup', onPanUp);
  window.addEventListener('mousemove', onPanMoveZ); window.addEventListener('mouseup', onPanUpZ);
  ctx.disposers.push(() => { window.removeEventListener('mousemove', onPanMoveZ); window.removeEventListener('mouseup', onPanUpZ); });

  const onWheel = (e) => {
    e.preventDefault();
    const rect = root.getBoundingClientRect();
    const cx = e.clientX - rect.left - panX - zoomState.tx;
    const cy = e.clientY - rect.top - panY - zoomState.ty;
    applyZoom(zoomState, e.deltaY, cx, cy);
    syncTransform();
  };
  root.addEventListener('wheel', onWheel, { passive: false });
  ctx.disposers.push(() => root.removeEventListener('wheel', onWheel));

  // Double-click on empty canvas resets zoom
  const onDblClick = (e) => {
    if (e.target.closest && e.target.closest(NO_PAN)) return;
    resetZoom(zoomState);
    panX = 0; panY = 0;
    syncTransform();
  };
  root.addEventListener('dblclick', onDblClick);
  ctx.disposers.push(() => root.removeEventListener('dblclick', onDblClick));

  deselectBtn.addEventListener('click', () => { ctx.selected.clear(); applySel(ctx, -1); if (ctx.unpinAll) ctx.unpinAll(); });

  // Write the restored pan/zoom to the element: the transform is otherwise only
  // set by the event handlers, so a remounted view would ignore it.
  syncTransform();
  // Pins are restored last: they need the edges, nodes and viewport in place.
  if (ctx.restorePins) ctx.restorePins();

  return {
    getView() { return { scale: zoomState.scale, tx: zoomState.tx, ty: zoomState.ty, panX, panY }; },
    dispose() { ctx.disposers.forEach(fn => fn()); if (root.parentNode) root.parentNode.removeChild(root); },
  };
}
