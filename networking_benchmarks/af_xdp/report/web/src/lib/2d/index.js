// 2d/index.js — 2D latency topology map. Orchestrates the concern-modules,
// which all operate on a shared `ctx`. Self-contained and independent of the 3D
// renderer (injects its own scoped styles; consumes the shared fleet.json model).
//
//   ctx = { fleet, matrix, N, W, H, CX, CY, region, root, svg, deselectBtn,
//           statsEl, positions, stress, ranges{...}, selected, nodeEls,
//           edgeElements, edgeLabelEls, disposers, unpinAll }

import { CSS } from './styles.js';
import { edgeSigma } from './palette.js';
import { computePositions } from './layout.js';
import { renderContours } from './contours.js';
import { renderEdges } from './edges.js';
import { renderNodes } from './nodes.js';
import { renderPanels } from './panels.js';
import { applySel } from './selection.js';

const STYLE_ID = 'topology2d-styles';

export function mountTopology2D(container, fleet) {
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
  };

  // ── global ranges ──────────────────────────────────────────────────────────
  const allP50 = [], allP99 = [], allSigma = [];
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) if (matrix[i] && matrix[i][j]) { allP50.push(matrix[i][j].p50); allP99.push(matrix[i][j].p99); }
  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) { const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i]; if (!ab && !ba) continue; allSigma.push(edgeSigma(ab, ba)); }
  ctx.ranges = {
    allP50,
    minP50: allP50.length ? Math.min(...allP50) : 0, maxP50: allP50.length ? Math.max(...allP50) : 100,
    minP99: allP99.length ? Math.min(...allP99) : 0, maxP99: allP99.length ? Math.max(...allP99) : 100,
    minSigma: allSigma.length ? Math.min(...allSigma) : 0, maxSigma: allSigma.length ? Math.max(...allSigma) : 1,
  };

  const { positions, stress } = computePositions(ctx);
  ctx.positions = positions; ctx.stress = stress;

  renderContours(ctx);
  renderEdges(ctx);
  renderNodes(ctx);
  renderPanels(ctx);
  applySel(ctx, -1);

  deselectBtn.addEventListener('click', () => { ctx.selected.clear(); applySel(ctx, -1); if (ctx.unpinAll) ctx.unpinAll(); });

  return { dispose() { ctx.disposers.forEach(fn => fn()); if (root.parentNode) root.parentNode.removeChild(root); } };
}
