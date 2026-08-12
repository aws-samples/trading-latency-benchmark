// 2d/layout.js — SMACOF MDS layout + AZ-row alignment for the 2D map.

import { nodeRadius } from './palette.js';
import { separateHierarchy } from '../grouplayout.js';

// Uniform latency→distance curve (replaces the old per-pair intra/inter split).
// At/below KNEE µs the mapping is 1:1 linear, so small differences in the
// low range (e.g. 40–120µs) are directly comparable by eye. Above KNEE the
// extra distance is log-compressed: larger latencies are still placed visibly
// farther (monotonic), but progressively less so, keeping the plot compact
// while preserving "bigger = farther" for comparison.
const KNEE = 50, COMP = 0.1;
export function latDist(us) {
  if (us <= KNEE) return us;
  return KNEE + Math.log10(1 + (us - KNEE) / KNEE) * KNEE * COMP;
}

export function computePositions(ctx) {
  const { fleet, matrix, N, W, H, CX, CY } = ctx;
  if (N < 2) return { positions: [{ x: CX, y: CY }], stress: 0 };

  const D = Array.from({ length: N }, () => Array(N).fill(0));
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) if (i !== j) {
    const ab = (matrix[i][j] && matrix[i][j].p50) || 35;
    const ba = (matrix[j] && matrix[j][i] && matrix[j][i].p50) || 35;
    D[i][j] = latDist((ab + ba) / 2);
  }

  let pos = fleet.nodes.map((_, i) => ({
    x: CX + 120 * Math.cos(2 * Math.PI * i / N - Math.PI / 2),
    y: CY + 120 * Math.sin(2 * Math.PI * i / N - Math.PI / 2),
  }));
  for (let iter = 0; iter < 800; iter++) {
    const newPos = pos.map(() => ({ x: 0, y: 0 }));
    for (let i = 0; i < N; i++) {
      let wx = 0, wy = 0, wsum = 0;
      for (let j = 0; j < N; j++) {
        if (i === j) continue;
        const dx = pos[i].x - pos[j].x, dy = pos[i].y - pos[j].y;
        const dist = Math.sqrt(dx * dx + dy * dy) || 0.001, target = D[i][j], w = 1.0 / (target * target);
        wx += w * (pos[j].x + target * (dx / dist)); wy += w * (pos[j].y + target * (dy / dist)); wsum += w;
      }
      newPos[i].x = wx / wsum; newPos[i].y = wy / wsum;
    }
    pos = newPos;
  }

  const PAD = 180;
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (const p of pos) { minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x); minY = Math.min(minY, p.y); maxY = Math.max(maxY, p.y); }
  const rangeX = maxX - minX || 1, rangeY = maxY - minY || 1;
  const scale = Math.min((W - 2 * PAD) / rangeX, (H - 2 * PAD) / rangeY);
  const result = pos.map(p => ({ x: CX + (p.x - (minX + rangeX / 2)) * scale, y: CY + (p.y - (minY + rangeY / 2)) * scale }));

  let stressNum = 0, stressDen = 0;
  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) {
    const dx = result[i].x - result[j].x, dy = result[i].y - result[j].y;
    const dij = Math.sqrt(dx * dx + dy * dy), target = D[i][j] * scale;
    stressNum += (dij - target) ** 2; stressDen += target ** 2;
  }
  const stress = stressDen > 0 ? Math.sqrt(stressNum / stressDen) : 0;

  // Group-aware separation (B+E): after the latency layout, rigidly push apart
  // sibling groups per tier (Account ⊃ Region ⊃ VPC ⊃ AZ) so the nested contour
  // boxes never intersect. Clusters move as rigid blocks — intra-group latency
  // shape is preserved; only inter-group distance is traded for cleanliness.
  // Per-tier "mandatory distance" between sibling containers DIMINISHES by tier —
  // account gets the full separation, region 80%, vpc 50%, az 35% — so the outer
  // structure reads clearly without flinging inner groups apart (composition
  // cohesion). Floored at 2·pad + margin so contour boxes never overlap.
  // Contour pads: account 104, region 76, vpc 48, az 20 (PAD_BASE 20, STEP 28).
  // DECOR covers the badges a node draws outside its circle and the label a
  // contour draws above its border, so separated groups clear both.
  // PG (5th tier) has no 2D contour — its gap just spaces PG groups within an AZ.
  const pads = [104, 76, 48, 20, 14];           // [account, region, vpc, az, pg] — sync with contours.js
  const DECOR = 18, LABEL_OVERHANG = 24;        // sync with contours.js
  const ratio = [1.0, 0.8, 0.5, 0.35, 0.22];    // diminishing distance by tier
  const SEP = 215;                              // base (account) separation
  const R = nodeRadius();
  const gaps = pads.map((p, d) => Math.max(SEP * ratio[d], 2 * p + 2 * R + 2 * DECOR + LABEL_OVERHANG));
  const pts = result.map((p) => [p.x, p.y]);
  separateHierarchy(fleet.nodes, pts, 2, R, gaps);
  result.forEach((p, i) => { p.x = pts[i][0]; p.y = pts[i][1]; });
  // ── Node collision resolution: push apart any individual nodes whose bodies
  // overlap after the group separation. The hierarchy pass guarantees GROUP
  // boxes don't intersect, but within a group (PG) nodes can still land on
  // top of each other when the MDS distances collapse (e.g. p50 ≈ equal for all
  // pairs in a cluster PG). This is a simple O(N²) iterative push — cheap for
  // fleet sizes (<100 nodes, <5 iterations).
  // Push apart nodes whose bodies overlap. Within a group nodes can land on
  // each other when the MDS distances collapse, and the viewport fit scales
  // positions while radii stay fixed, so this runs again in final coordinates.
  const resolveNodeCollisions = () => {
    const R2 = nodeRadius() * 2 + 6; // min centre-to-centre distance (2 radii + gap)
    for (let iter = 0; iter < 10; iter++) {
      let nudged = false;
      for (let i = 0; i < result.length; i++) {
        for (let j = i + 1; j < result.length; j++) {
          const dx = result[j].x - result[i].x, dy = result[j].y - result[i].y;
          const dist = Math.sqrt(dx * dx + dy * dy);
          if (dist < R2) {
            const push = (R2 - dist) / 2 + 1;
            const nx = dist > 0.01 ? dx / dist : 1, ny = dist > 0.01 ? dy / dist : 0;
            result[i].x -= nx * push; result[i].y -= ny * push;
            result[j].x += nx * push; result[j].y += ny * push;
            nudged = true;
          }
        }
      }
      if (!nudged) break;
    }
  };
  resolveNodeCollisions();
  // Fit to the viewport, then re-enforce separation. Contour pads are absolute
  // pixels, so scaling positions shrinks the gaps the pads still need: the
  // constraint only holds if it is applied in final coordinates. The view
  // supports zoom and pan, so overflowing the viewport is preferable to
  // boundaries that intersect.
  const extent = () => {
    let mnx = Infinity, mxx = -Infinity, mny = Infinity, mxy = -Infinity;
    result.forEach((p, i) => {
      const rr = nodeRadius(fleet.nodes[i]);
      mnx = Math.min(mnx, p.x - rr); mxx = Math.max(mxx, p.x + rr);
      mny = Math.min(mny, p.y - rr); mxy = Math.max(mxy, p.y + rr);
    });
    return { mnx, mxx, mny, mxy };
  };
  let e = extent();
  const fitK = Math.min(1, (W - 200) / ((e.mxx - e.mnx) || 1), (H - 200) / ((e.mxy - e.mny) || 1));
  const gcx = (e.mnx + e.mxx) / 2, gcy = (e.mny + e.mxy) / 2;
  result.forEach((p) => { p.x = CX + (p.x - gcx) * fitK; p.y = CY + (p.y - gcy) * fitK; });
  if (fitK < 1) {
    const scaled = result.map((p) => [p.x, p.y]);
    separateHierarchy(fleet.nodes, scaled, 2, R, gaps);
    result.forEach((p, i) => { p.x = scaled[i][0]; p.y = scaled[i][1]; });
  }
  resolveNodeCollisions();
  // Translate-only recentre so the restored gaps survive.
  e = extent();
  const dx = CX - (e.mnx + e.mxx) / 2, dy = CY - (e.mny + e.mxy) / 2;
  result.forEach((p) => { p.x += dx; p.y += dy; });

  return { positions: result, stress };
}
