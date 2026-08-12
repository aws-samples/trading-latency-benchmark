// 2d/edges.js — SVG edges, edge-hover tooltip, and rotated edge labels.

import { fmtLat, edgeSigma, jitterColor } from './palette.js';

const EDGE_WIDTH = 2.5;

export function renderEdges(ctx) {
  const { fleet, matrix, N, root, svg, positions, edgeElements, edgeLabelEls, W, H } = ctx;
  const { minSigma, maxSigma } = ctx.ranges;
  const jc = (sig) => jitterColor(sig, minSigma, maxSigma);

  // edges
  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) {
    const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i];
    if (!ab && !ba) continue;
    const avgP50 = Math.round((((ab && ab.p50) || 0) + ((ba && ba.p50) || 0)) / 2);
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', positions[i].x); line.setAttribute('y1', positions[i].y);
    line.setAttribute('x2', positions[j].x); line.setAttribute('y2', positions[j].y);
    line.setAttribute('stroke', jc(edgeSigma(ab, ba))); line.setAttribute('stroke-width', EDGE_WIDTH); line.setAttribute('opacity', '0.55');
    line.classList.add('edge-line'); svg.appendChild(line);
    edgeElements.push({ line, i, j, avgP50, ab, ba });
  }

  // edge tooltip
  const edgeTooltip = document.createElement('div'); edgeTooltip.className = 'edge-tooltip'; root.appendChild(edgeTooltip);
  const metricRow = (d) =>
    '<div class="dir-values"><span class="metric-label">p50</span><span class="metric-label">p90</span><span class="metric-label">p99</span><span class="metric-label">p99.9</span><span class="metric-label">max</span><span class="metric-label">loss</span>'
    + '<span class="metric-val highlight">' + fmtLat(d.p50) + '</span><span class="metric-val">' + (d.p90 ? fmtLat(d.p90) : '\u2014') + '</span><span class="metric-val">' + fmtLat(d.p99) + '</span><span class="metric-val">' + (d.p999 ? fmtLat(d.p999) : '\u2014') + '</span><span class="metric-val">' + (d.max ? fmtLat(d.max) : '\u2014') + '</span><span class="metric-val">' + (d.loss !== undefined ? d.loss + '%' : '\u2014') + '</span></div>';
  function showEdgeTooltip(i, j) {
    const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i], nodeA = fleet.nodes[i], nodeB = fleet.nodes[j];
    const diff = ab && ba ? Math.abs(ab.p50 - ba.p50) : 0;
    const diffPct = ab && ba && Math.min(ab.p50, ba.p50) > 0 ? ((diff / Math.min(ab.p50, ba.p50)) * 100).toFixed(1) : '0';
    let html = '<h4>' + nodeA.ec2_name + ' \u2194 ' + nodeB.ec2_name + '</h4>';
    if (ab) html += '<div class="dir-block"><div class="dir-label">\u2192 ' + nodeA.ec2_name + ' \u2192 ' + nodeB.ec2_name + '</div>' + metricRow(ab) + '</div>';
    if (ba) html += '<div class="dir-block"><div class="dir-label">\u2190 ' + nodeB.ec2_name + ' \u2192 ' + nodeA.ec2_name + '</div>' + metricRow(ba) + '</div>';
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

  // edge labels (hidden until node hover / pin)
  for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) {
    const ab = matrix[i] && matrix[i][j], ba = matrix[j] && matrix[j][i];
    if (!ab && !ba) continue;
    const avgP50 = Math.round((((ab && ab.p50) || 0) + ((ba && ba.p50) || 0)) / 2);
    const x1 = positions[i].x, y1 = positions[i].y, x2 = positions[j].x, y2 = positions[j].y;
    const mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
    let ang = Math.atan2(y2 - y1, x2 - x1) * 180 / Math.PI;
    if (ang > 90) ang -= 180; else if (ang < -90) ang += 180;
    const el = document.createElement('div'); el.className = 'edge-label';
    el.style.left = mx + 'px'; el.style.top = my + 'px'; el.style.transform = 'translate(-50%,-50%) rotate(' + ang + 'deg)';
    el.style.color = jc(edgeSigma(ab, ba)); el.textContent = fmtLat(avgP50) + ' \u00b1' + fmtLat(edgeSigma(ab, ba)); el.style.display = 'none';
    const ci = i, cj = j; edgeLabelEls.push({ el, i: ci, j: cj });
    el.addEventListener('mouseenter', () => showEdgeTooltip(ci, cj));
    el.addEventListener('mousemove', positionEdgeTooltip);
    el.addEventListener('mouseleave', hideEdgeTooltip);
    root.appendChild(el);
  }
}
