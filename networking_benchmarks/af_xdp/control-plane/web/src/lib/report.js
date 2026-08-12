// report.js — build a self-contained HTML report (heatmap + full latency table)
// from the currently-shown fleet (afxdp.topology/v1 model), for the chosen
// kind/variation. Runs entirely client-side so it works against live data
// without a backend report endpoint (the offline gen/report.py remains for
// saved runs). Returns an HTML string; the caller downloads it as a Blob.

import { fmtLat, latencyColor, esc } from './2d/palette.js';

export function buildReportHTML(fleet, kind, variation) {
  const nodes = (fleet && fleet.nodes) || [];
  const matrix = (fleet && fleet.matrix) || [];
  const N = nodes.length;
  const label = (n) => esc(n.public_ip || n.private_ip || n.ec2_name || ('#' + n.index));

  // Global p50 range for the heatmap colour scale.
  let mn = Infinity, mx = -Infinity, pairs = 0;
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    const c = matrix[i] && matrix[i][j];
    if (c) { pairs++; if (c.p50 < mn) mn = c.p50; if (c.p50 > mx) mx = c.p50; }
  }
  if (!isFinite(mn)) { mn = 0; mx = 1; }

  // NxN heatmap (rows = src, cols = dst), cell colour = p50.
  let heat = '<table class="heat"><tr><th>src \\ dst</th>' + nodes.map((n) => `<th>${label(n)}</th>`).join('') + '</tr>';
  for (let i = 0; i < N; i++) {
    heat += `<tr><th>${label(nodes[i])}</th>`;
    for (let j = 0; j < N; j++) {
      const c = matrix[i] && matrix[i][j];
      if (i === j) heat += '<td class="diag">—</td>';
      else if (!c) heat += '<td class="na">·</td>';
      else heat += `<td style="background:${latencyColor(c.p50, mn, mx)}" title="p99 ${fmtLat(c.p99)} · loss ${c.loss ?? 0}%">${fmtLat(c.p50)}</td>`;
    }
    heat += '</tr>';
  }
  heat += '</table>';

  // Full per-edge latency table.
  let rows = '';
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    const c = matrix[i] && matrix[i][j];
    if (!c || i === j) continue;
    rows += `<tr><td>${label(nodes[i])}</td><td>${label(nodes[j])}</td><td>${fmtLat(c.p50)}</td><td>${fmtLat(c.p90)}</td><td>${fmtLat(c.p99)}</td><td>${fmtLat(c.p999)}</td><td>${fmtLat(c.max)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
  }

  const gen = new Date().toISOString();
  return `<!doctype html><html><head><meta charset="utf-8">
<title>AF_XDP report — ${esc(kind)}/${esc(variation)}</title>
<style>
  body{font:13px -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;padding:24px;margin:0}
  h1{font-size:18px;margin:0 0 4px} h2{font-size:15px;margin:24px 0 4px;color:#58a6ff}
  .meta{color:#8b949e}
  table{border-collapse:collapse;margin-top:8px}
  td,th{border:1px solid #30363d;padding:4px 9px;text-align:center;font-size:12px}
  th{background:#161b22;color:#8b949e}
  .heat td{font-family:'SF Mono',monospace;color:#0d1117;font-weight:700}
  .heat .diag,.heat .na{background:#161b22;color:#6e7681;font-weight:400}
  td{font-family:'SF Mono',monospace}
</style></head><body>
  <h1>AF_XDP latency report — ${esc(kind)} / ${esc(variation)}</h1>
  <div class="meta">Region: ${esc(fleet.region || '?')} · Nodes: ${N} · Pairs: ${pairs} · Generated: ${gen}</div>
  <h2>Heatmap — p50 (green = fast, red = slow)</h2>
  ${heat}
  <h2>All measured latencies</h2>
  <table><tr><th>src</th><th>dst</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>${rows}</table>
</body></html>`;
}
