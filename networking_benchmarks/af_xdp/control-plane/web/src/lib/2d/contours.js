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

// PAD_BASE: clearance from the innermost nodes to the AZ border.
// STEP: additional clearance per tier, i.e. the distance between one
// contour border and the next one out.
// A node draws badges outside its circle (.pg-badge at top:-9px, .role-badge at
// bottom:-9px, both centred so they overhang sideways too), and every contour
// draws its label at top:-11px, above its own border. Padding measured from the
// circle alone therefore lets decoration cross a border, so the rendered extent
// is padded instead.
const DECOR_Y = 10, DECOR_X = 18, LABEL_OVERHANG = 24;
// PAD_BASE: clearance from the innermost node extents to the AZ border.
// STEP: clearance per tier, i.e. the gap between one contour border and the
// next out. It must exceed LABEL_OVERHANG so a child label cannot reach its
// parent border.
export const PAD_BASE = 20, STEP = 28;
const CONTOUR_DEFS = [
  { depth: 3, cls: 'az',      prefix: 'AZ',      pad: PAD_BASE },
  { depth: 2, cls: 'vpc',     prefix: 'VPC',     pad: PAD_BASE + STEP },
  { depth: 1, cls: 'region',  prefix: 'Region',  pad: PAD_BASE + STEP * 2 },
  { depth: 0, cls: 'account', prefix: 'Account', pad: PAD_BASE + STEP * 3 },
];

// Path-scoped cells for a tier depth (0=account..3=az): key = full ancestor
// path so cells are per-parent. Each entry carries the leaf name + indices.
function cellsOf(fleet, depth) {
  const g = {};
  fleet.nodes.forEach((node, i) => {
    const leaf = node[HIER[depth]]; if (!leaf || leaf === 'unknown') return;
    const key = pathKeyOf(node, depth);
    (g[key] = g[key] || { leaf, idx: [] }).idx.push(i);
  });
  return g;
}

/**
 * Compute contour boxes from a fleet and positions. The box for each group at
 * each tier is the bounding box of its member nodes (including node radius)
 * expanded by the tier pad. Parent boxes are then expanded to strictly contain
 * all their children (the key fix for overlap issue).
 *
 * Returns: Array<{ tier, key, left, top, right, bottom, nodeIndices }>
 */
export function computeContourBoxes(fleet, positions) {
  const boxes = [];
  const boxByKey = {};
  // Compute raw boxes from innermost (az) to outermost (account).
  for (const def of CONTOUR_DEFS) {
    const cells = cellsOf(fleet, def.depth);
    for (const key of Object.keys(cells)) {
      const idx = cells[key].idx; if (idx.length === 0) continue;
      const xs = idx.map(i => positions[i].x), ys = idx.map(i => positions[i].y);
      const radii = idx.map(i => nodeRadius(fleet.nodes[i]));
      const left = minOf(xs.map((x, k) => x - radii[k] - DECOR_X)) - def.pad;
      const right = maxOf(xs.map((x, k) => x + radii[k] + DECOR_X)) + def.pad;
      const top = minOf(ys.map((y, k) => y - radii[k] - DECOR_Y)) - def.pad;
      const bottom = maxOf(ys.map((y, k) => y + radii[k] + DECOR_Y)) + def.pad;
      const box = { tier: def.cls, key, left, top, right, bottom, nodeIndices: idx };
      boxes.push(box);
      boxByKey[key] = box;
    }
  }
  // Expand parent boxes so every child is strictly contained. Walk from inner
  // tiers outward (az -> vpc -> region -> account). A parent must envelop all
  // its children with at least STEP margin (the gap between nested borders).
  for (let t = 0; t < CONTOUR_DEFS.length - 1; t++) {
    const childDef = CONTOUR_DEFS[t];
    const parentDef = CONTOUR_DEFS[t + 1];
    const childCells = cellsOf(fleet, childDef.depth);
    const parentCells = cellsOf(fleet, parentDef.depth);
    for (const pkey of Object.keys(parentCells)) {
      const pbox = boxByKey[pkey]; if (!pbox) continue;
      // Find all child boxes that belong to this parent.
      for (const ckey of Object.keys(childCells)) {
        if (!ckey.startsWith(pkey)) continue;
        const cbox = boxByKey[ckey]; if (!cbox) continue;
        // Parent must contain child with at least STEP margin.
        pbox.left = Math.min(pbox.left, cbox.left - STEP);
        pbox.right = Math.max(pbox.right, cbox.right + STEP);
        // The child's label sits above its own top edge, so clear that too.
        pbox.top = Math.min(pbox.top, cbox.top - Math.max(STEP, LABEL_OVERHANG));
        pbox.bottom = Math.max(pbox.bottom, cbox.bottom + STEP);
      }
    }
  }
  return boxes;
}

export function renderContours(ctx) {
  const { fleet, root, svg, positions } = ctx;
  const boxes = computeContourBoxes(fleet, positions);
  const vpcBoxes = [];
  for (const box of boxes) {
    const def = CONTOUR_DEFS.find(d => d.cls === box.tier);
    if (!def) continue;
    const cells = cellsOf(fleet, def.depth);
    const cell = cells[box.key];
    const left = box.left, top = box.top, right = box.right, bottom = box.bottom;
    const el = document.createElement('div'); el.className = 'contour ' + def.cls;
    el.style.left = left + 'px'; el.style.top = top + 'px'; el.style.width = (right - left) + 'px'; el.style.height = (bottom - top) + 'px';
    el.innerHTML = '<span class="label">' + esc(def.prefix + ': ' + (cell ? cell.leaf : '')) + '</span>';
    root.appendChild(el);
    if (def.cls === 'vpc') vpcBoxes.push({ cx: (left + right) / 2, cy: (top + bottom) / 2, hw: (right - left) / 2, hh: (bottom - top) / 2 });
  }
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
