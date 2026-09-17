// 2d/edges.js — SVG edges, edge-hover tooltip, and rotated edge labels.
//
// Clutter reduction: edges whose p50 sits in the top 20% of the p50 distribution
// render at low base opacity (0.15) so the dominant fast-path edges stay legible.
// Any edge touching a hovered or selected node always lifts to full opacity.

import { fmtLat, edgeSigma, jitterColor, cellColor, isCrossRegion, esc } from './palette.js';
import { relativeAge, ageFade } from '../lineage.js';

const EDGE_WIDTH = 2.5;

function meanP50(ab, ba) {
  const v = [];
  if (ab && ab.p50 != null) v.push(ab.p50);
  if (ba && ba.p50 != null) v.push(ba.p50);
  return v.length ? Math.round(v.reduce((a, b) => a + b, 0) / v.length) : 0;
}

export function renderEdges(ctx) {
  const { fleet, matrix, N, root, svg, positions, edgeElements, edgeLabelEls, W, H } = ctx;
  const { minSigma, maxSigma, minP50, maxP50 } = ctx.ranges;
  // Gold threshold: 1st percentile of intra-region p50 values.
  const sortedP50 = ctx.ranges.allP50;
  const gold = sortedP50.length ? sortedP50[Math.floor(sortedP50.length * 0.01)] : undefined;
  // Edge colour = p50 latency (green = fast, red = slow) out of the global range.
  const nodesArr = fleet.nodes || [];
  const lc = (avg, i, j) => cellColor(avg, minP50, maxP50, isCrossRegion(nodesArr[i], nodesArr[j]), gold);
  const jc = (sig) => jitterColor(sig, minSigma, maxSigma);  // kept for label colour

  // Reuse the pre-sorted allP50 from ctx.ranges (computed once in index.js)
  // to derive the opacity p60 threshold.
  const p60 = sortedP50.length ? sortedP50[Math.floor(sortedP50.length * 0.6)] : Infinity;
  const pMin = sortedP50[0] || 0;
  const baseOpacity = (avg) => {
    if (avg > p60) return 0.07;
    const t = p60 > pMin ? (avg - pMin) / (p60 - pMin) : 0;
    return +(0.18 - t * 0.06).toFixed(3);   // fast/green de-emphasised further
  };

  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) {
    const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i];
    if (!ab && !ba) continue;
    const avgP50 = meanP50(ab, ba);
    const op = baseOpacity(avgP50);
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', positions[i].x); line.setAttribute('y1', positions[i].y);
    line.setAttribute('x2', positions[j].x); line.setAttribute('y2', positions[j].y);
    line.setAttribute('stroke', lc(avgP50, i, j));
    line.setAttribute('stroke-width', EDGE_WIDTH);
    line.setAttribute('opacity', op);
    line.dataset.baseOp = op;
    line.classList.add('edge-line');
    svg.appendChild(line);
    edgeElements.push({ line, i, j, avgP50, ab, ba });
  }

  // edge tooltip
  const edgeTooltip = document.createElement('div'); edgeTooltip.className = 'edge-tooltip'; root.appendChild(edgeTooltip);
  const metricRow = (d) =>
    '<div class="dir-values"><span class="metric-label">p50</span><span class="metric-label">p90</span><span class="metric-label">p99</span><span class="metric-label">p99.9</span><span class="metric-label">max</span><span class="metric-label">loss</span>'
    + '<span class="metric-val highlight">' + fmtLat(d.p50) + '</span><span class="metric-val">' + (d.p90 ? fmtLat(d.p90) : '\u2014') + '</span><span class="metric-val">' + fmtLat(d.p99) + '</span><span class="metric-val">' + (d.p999 ? fmtLat(d.p999) : '\u2014') + '</span><span class="metric-val">' + (d.max ? fmtLat(d.max) : '\u2014') + '</span><span class="metric-val">' + (d.loss !== undefined ? esc(d.loss) + '%' : '\u2014') + '</span></div>';
  function showEdgeTooltip(i, j) {
    const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i], nodeA = fleet.nodes[i], nodeB = fleet.nodes[j];
    const diff = ab && ba ? Math.abs(ab.p50 - ba.p50) : 0;
    const diffPct = ab && ba && Math.min(ab.p50, ba.p50) > 0 ? ((diff / Math.min(ab.p50, ba.p50)) * 100).toFixed(1) : '0';
    const nameA = esc(nodeA.ec2_name), nameB = esc(nodeB.ec2_name);
    const now = Date.now();
    // Variation + relative age annotation (3.2).
    const variation = ctx.variation || '';
    const ageAB = ab && ab.unix ? relativeAge(ab.unix, now) : '';
    const ageBA = ba && ba.unix ? relativeAge(ba.unix, now) : '';
    let html = '<h4>' + nameA + ' \u2194 ' + nameB + '</h4>';
    if (ab) html += '<div class="dir-block"><div class="dir-label">\u2192 ' + nameA + ' \u2192 ' + nameB + (variation || ageAB ? ' <span style="color:#6e7681;font-size:9px">' + esc(variation) + (ageAB ? ' \u00b7 ' + ageAB : '') + '</span>' : '') + '</div>' + metricRow(ab) + '</div>';
    if (ba) html += '<div class="dir-block"><div class="dir-label">\u2190 ' + nameB + ' \u2192 ' + nameA + (variation || ageBA ? ' <span style="color:#6e7681;font-size:9px">' + esc(variation) + (ageBA ? ' \u00b7 ' + ageBA : '') + '</span>' : '') + '</div>' + metricRow(ba) + '</div>';
    if (diff > 0) html += '<div class="asymmetry">Asymmetry: \u0394' + diff + '\u03bcs (' + diffPct + '%)</div>';
    edgeTooltip.innerHTML = html; edgeTooltip.classList.add('visible');
  }
  function positionEdgeTooltip(e) {
    let tx = e.clientX + 16, ty = e.clientY - 10;
    const tw = edgeTooltip.offsetWidth || 260, th = edgeTooltip.offsetHeight || 150;
    if (tx + tw > W - 20) tx = e.clientX - tw - 16;
    if (ty + th > H - 20) ty = H - th - 20;
    if (ty < 10) ty = 10;
    edgeTooltip.style.left = tx + 'px'; edgeTooltip.style.top = ty + 'px';
  }
  const hideEdgeTooltip = () => edgeTooltip.classList.remove('visible');

  // edge labels — initially hidden; appear on hover/click via selection.js
  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) {
    const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i];
    if (!ab && !ba) continue;
    const avgP50 = meanP50(ab, ba);
    const x1 = positions[i].x, y1 = positions[i].y, x2 = positions[j].x, y2 = positions[j].y;
    const mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
    let ang = Math.atan2(y2 - y1, x2 - x1) * 180 / Math.PI;
    if (ang > 90) ang -= 180; else if (ang < -90) ang += 180;
    const el = document.createElement('div'); el.className = 'edge-label';
    el.style.left = mx + 'px'; el.style.top = my + 'px';
    el.style.transform = 'translate(-50%,-50%) rotate(' + ang + 'deg)';
    el.style.color = lc(avgP50, i, j);
    el.textContent = fmtLat(avgP50) + ' \u00b1' + fmtLat(edgeSigma(ab, ba));
    el.style.opacity = '0'; el.style.pointerEvents = 'none';
    const ci = i, cj = j; edgeLabelEls.push({ el, i: ci, j: cj });
    el.addEventListener('mouseenter', () => showEdgeTooltip(ci, cj));
    el.addEventListener('mousemove', positionEdgeTooltip);
    el.addEventListener('mouseleave', hideEdgeTooltip);
    root.appendChild(el);
  }
}

