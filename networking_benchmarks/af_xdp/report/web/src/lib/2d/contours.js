// 2d/contours.js — nested topology contours (AZ ⊂ VPC ⊂ Region ⊂ Account) plus
// cross-region VPC peering lines. CPG is shown as a per-node badge, not a contour.

import { nodeRadius } from './palette.js';

export function renderContours(ctx) {
  const { fleet, root, svg, positions } = ctx;
  const groupBy = (key) => { const g = {}; fleet.nodes.forEach((node, i) => { const v = node[key] || 'unknown'; (g[v] = g[v] || []).push(i); }); return g; };
  const PAD_BASE = 12, STEP = 18;
  const contourDefs = [
    { groups: groupBy('az'),      cls: 'az',      prefix: 'AZ',      pad: PAD_BASE },
    { groups: groupBy('vpc_id'),  cls: 'vpc',     prefix: 'VPC',     pad: PAD_BASE + STEP },
    { groups: groupBy('region'),  cls: 'region',  prefix: 'Region',  pad: PAD_BASE + STEP * 2 },
    { groups: groupBy('account'), cls: 'account', prefix: 'Account', pad: PAD_BASE + STEP * 3 },
  ];
  const vpcBoxes = [];
  contourDefs.forEach(def => {
    const keys = Object.keys(def.groups); if (keys.length === 0) return;
    keys.forEach(key => {
      if (key === 'unknown') return;
      const idx = def.groups[key]; if (idx.length === 0) return;
      const xs = idx.map(i => positions[i].x), ys = idx.map(i => positions[i].y), radii = idx.map(i => nodeRadius(fleet.nodes[i]));
      const left = Math.min(...xs.map((x, k) => x - radii[k])) - def.pad;
      const right = Math.max(...xs.map((x, k) => x + radii[k])) + def.pad;
      const top = Math.min(...ys.map((y, k) => y - radii[k])) - def.pad;
      const bottom = Math.max(...ys.map((y, k) => y + radii[k])) + def.pad;
      const el = document.createElement('div'); el.className = 'contour ' + def.cls;
      el.style.left = left + 'px'; el.style.top = top + 'px'; el.style.width = (right - left) + 'px'; el.style.height = (bottom - top) + 'px';
      const label = keys.length === 1 ? def.prefix + ': ' + key : key;
      el.innerHTML = '<span class="label">' + label + '</span>';
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
