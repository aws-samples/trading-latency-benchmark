// 2d/tables.js — per-node latency table (peers grouped by PG, sorted by p50).

import { fmtLat } from './palette.js';

export function buildPeerTable(ctx, i, inbound) {
  const { fleet, matrix, N } = ctx;
  const rows = [];
  for (let j = 0; j < N; j++) { if (i === j) continue; const data = inbound ? (matrix[j] && matrix[j][i]) : (matrix[i] && matrix[i][j]); if (!data) continue; rows.push({ peer: fleet.nodes[j], j, data }); }
  const groups = {};
  rows.forEach(r => { const pg = (r.peer.cpg_name && r.peer.cpg_name !== 'unknown') ? r.peer.cpg_name : 'no PG'; (groups[pg] = groups[pg] || []).push(r); });
  const keys = Object.keys(groups).sort((a, b) => Math.min(...groups[a].map(r => r.data.p50)) - Math.min(...groups[b].map(r => r.data.p50)));
  let h = '<table><tr><th>Peer</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>';
  keys.forEach(pg => {
    h += '<tr class="pg-group"><td colspan="7">' + pg + '</td></tr>';
    groups[pg].sort((a, b) => a.data.p50 - b.data.p50).forEach(r => { const d = r.data;
      h += '<tr data-peer="' + r.j + '"><td class="peer-name">' + r.peer.ec2_name + '</td><td class="highlight">' + fmtLat(d.p50) + '</td><td>' + (d.p90 ? fmtLat(d.p90) : '\u2014') + '</td><td>' + fmtLat(d.p99) + '</td><td>' + (d.p999 ? fmtLat(d.p999) : '\u2014') + '</td><td>' + (d.max ? fmtLat(d.max) : '\u2014') + '</td><td>' + (d.loss !== undefined ? d.loss + '%' : '\u2014') + '</td></tr>'; });
  });
  return h + '</table>';
}
