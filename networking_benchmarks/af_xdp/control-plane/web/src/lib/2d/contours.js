// 2d/contours.js — nested topology contours as a STRICT tree Account ⊃ Region ⊃
// VPC ⊃ AZ, using per-parent path cells (option E): an AZ that spans two VPCs is
// drawn as one cell per VPC. The group-aware layout (see grouplayout.js) has
// already separated sibling clusters, so these boxes nest cleanly and never
// intersect. CPG is a per-node badge, not a contour.

import { nodeRadius, esc } from './palette.js';
import { HIER, pathKeyOf } from '../grouplayout.js';

// Loop/reduce-based min/max — avoids `Math.min(...arr)` spread, which can blow
// the call stack for large groups (mirrors the guard used in index.js).
const minOf = (arr) => arr.reduce((m, v) => (v < m ? v : m), Infinity);
const maxOf = (arr) => arr.reduce((m, v) => (v > m ? v : m), -Infinity);

export function renderContours(ctx) {
  const { fleet, root, svg, positions } = ctx;
  // Path-scoped cells for a tier depth (0=account…3=az): key = full ancestor
  // path so cells are per-parent. Each entry carries the leaf name + indices.
  const cellsOf = (depth) => {
    const g = {};
    fleet.nodes.forEach((node, i) => {
      const leaf = node[HIER[depth]]; if (!leaf || leaf === 'unknown') return;
      const key = pathKeyOf(node, depth);
      (g[key] = g[key] || { leaf, idx: [] }).idx.push(i);
    });
    return g;
  };
  const PAD_BASE = 10, STEP = 12;
  // Extra top padding so the contour label (top:-11px) never clips above the canvas.
  const LABEL_H = 18;
  const contourDefs = [
    { depth: 3, cls: 'az',      prefix: 'AZ',      pad: PAD_BASE },
    { depth: 2, cls: 'vpc',     prefix: 'VPC',     pad: PAD_BASE + STEP },
    { depth: 1, cls: 'region',  prefix: 'Region',  pad: PAD_BASE + STEP * 2 },
    { depth: 0, cls: 'account', prefix: 'Account', pad: PAD_BASE + STEP * 3 },
  ];
  const vpcBoxes = [];
  contourDefs.forEach(def => {
    const cells = cellsOf(def.depth);
    Object.keys(cells).forEach(key => {
      const idx = cells[key].idx; if (idx.length === 0) return;
      const xs = idx.map(i => positions[i].x), ys = idx.map(i => positions[i].y), radii = idx.map(i => nodeRadius(fleet.nodes[i]));
      const left = minOf(xs.map((x, k) => x - radii[k])) - def.pad;
      const right = maxOf(xs.map((x, k) => x + radii[k])) + def.pad;
      // Pull top down enough that the label is never above the canvas origin.
      const rawTop = minOf(ys.map((y, k) => y - radii[k])) - def.pad;
      const top = Math.max(rawTop, LABEL_H);
      const bottom = maxOf(ys.map((y, k) => y + radii[k])) + def.pad;
      const el = document.createElement('div'); el.className = 'contour ' + def.cls;
      el.style.left = left + 'px'; el.style.top = top + 'px'; el.style.width = (right - left) + 'px'; el.style.height = (bottom - top) + 'px';
      el.innerHTML = '<span class="label">' + esc(def.prefix + ': ' + cells[key].leaf) + '</span>';
      root.appendChild(el);
      if (def.cls === 'vpc') vpcBoxes.push({ cx: (left + right) / 2, cy: (top + bottom) / 2, hw: (right - left) / 2, hh: (bottom - top) / 2 });
    });
  });
  if (vpcBoxes.length >= 2) {
    const edgePoint = (P, dx, dy) => { const adx = Math.abs(dx) || 1e-6, ady = Math.abs(dy) || 1e-6, t = Math.min(P.hw / adx, P.hh / ady); return { x: P.cx + dx * t, y: P.cy + dy * t }; };
    for (let a = 0; a < vpcBoxes.length; a++) for (let b = a + 1; b < vpcBoxes.length; b++) {
      const A = vpcBoxes[a], B = vpcBoxes[b], dx = B.cx - A.cx, dy = B.cy - A.cy;
      const p1 = edgePoint(A, dx, dy), p2 = edgePoint(B, -dx, -dy);
      const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      line.setAttribute('x1', p1.x); line.setAttribute('y1', p1.y); line.setAttribute('x2', p2.x); line.setAttribute('y2', p2.y);
      line.setAttribute('class', 'peering-line'); svg.appendChild(line);
      const lbl = document.createElement('div'); lbl.className = 'peering-label';
      lbl.style.left = ((p1.x + p2.x) / 2) + 'px'; lbl.style.top = ((p1.y + p2.y) / 2) + 'px'; lbl.textContent = 'VPC Peering'; lbl.style.display = 'none';
      root.appendChild(lbl);
      const hit = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      hit.setAttribute('x1', p1.x); hit.setAttribute('y1', p1.y); hit.setAttribute('x2', p2.x); hit.setAttribute('y2', p2.y);
      hit.setAttribute('class', 'peering-hit');
      hit.addEventListener('mouseenter', () => { lbl.style.display = ''; });
      hit.addEventListener('mouseleave', () => { lbl.style.display = 'none'; });
      svg.appendChild(hit);
    }
  }
}
