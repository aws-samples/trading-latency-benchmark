// 2d/layout.js — SMACOF MDS layout + AZ-row alignment for the 2D map.

import { nodeRadius } from './palette.js';

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

  // Arrange AZ clusters left→right in a horizontal row (all aligned on CY so the
  // boxes are level). Inter-AZ spacing is DYNAMIC: the centre-to-centre distance
  // is the representative cross-AZ latency mapped through the same latDist × scale
  // as intra-AZ edges, clamped only so boxes never overlap.
  const azMap = {};
  fleet.nodes.forEach((n, i) => { const a = n.az || 'z'; (azMap[a] = azMap[a] || []).push(i); });
  const azKeys = Object.keys(azMap).sort();
  if (azKeys.length > 1) {
    const blocks = azKeys.map(a => {
      const idx = azMap[a]; let mnx = Infinity, mxx = -Infinity, mny = Infinity, mxy = -Infinity;
      idx.forEach(i => { const rr = nodeRadius(fleet.nodes[i]), p = result[i];
        mnx = Math.min(mnx, p.x - rr); mxx = Math.max(mxx, p.x + rr); mny = Math.min(mny, p.y - rr); mxy = Math.max(mxy, p.y + rr); });
      return { idx, cx: (mnx + mxx) / 2, cy: (mny + mxy) / 2, halfW: (mxx - mnx) / 2 };
    });
    // mean cross-AZ p50 between two clusters (both directions)
    const interLat = (A, B) => {
      let s = 0, c = 0;
      A.idx.forEach(i => B.idx.forEach(j => {
        const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i];
        if (ab) { s += ab.p50; c++; } if (ba) { s += ba.p50; c++; }
      }));
      return c ? s / c : 100;
    };
    const MARGIN = 24;   // min px between adjacent boxes (overlap guard only)
    const centers = [0];
    for (let k = 1; k < blocks.length; k++) {
      const want = latDist(interLat(blocks[k - 1], blocks[k])) * scale;        // latency-proportional
      const minSep = blocks[k - 1].halfW + blocks[k].halfW + MARGIN;           // no-overlap floor
      centers.push(centers[k - 1] + Math.max(want, minSep));
    }
    const rowMid = (centers[0] + centers[centers.length - 1]) / 2;
    blocks.forEach((b, k) => { const dx = CX + (centers[k] - rowMid) - b.cx, dy = CY - b.cy; b.idx.forEach(i => { result[i].x += dx; result[i].y += dy; }); });
    // shrink toward center if the row overflows the viewport
    let mnx = Infinity, mxx = -Infinity, mny = Infinity, mxy = -Infinity;
    result.forEach((p, i) => { const rr = nodeRadius(fleet.nodes[i]); mnx = Math.min(mnx, p.x - rr); mxx = Math.max(mxx, p.x + rr); mny = Math.min(mny, p.y - rr); mxy = Math.max(mxy, p.y + rr); });
    const fitK = Math.min(1, (W - 160) / ((mxx - mnx) || 1), (H - 160) / ((mxy - mny) || 1));
    if (fitK < 1) result.forEach(p => { p.x = CX + (p.x - CX) * fitK; p.y = CY + (p.y - CY) * fitK; });
  }

  return { positions: result, stress };
}
