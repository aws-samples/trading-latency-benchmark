// 2d/tables.js — SHARED latency-panel builder (used by BOTH the 2D map and the
// 3D scene, so the per-node latency table is identical in either view). Pure:
// takes (fleet, matrix, i) and returns HTML — no ctx, no DOM, no renderer state.

import { fmtLat, esc } from './palette.js';

// Peers of node i (outbound i→peer, or inbound peer→i), grouped by placement
// group and sorted by p50. `data-peer` carries the peer's index so the 2D map
// can cross-highlight on row hover; the 3D view simply ignores it.
export function buildPeerTable(fleet, matrix, i, inbound) {
  const N = fleet.nodes.length;
  const rows = [];
  for (let j = 0; j < N; j++) { if (i === j) continue; const data = inbound ? (matrix[j] && matrix[j][i]) : (matrix[i] && matrix[i][j]); if (!data) continue; rows.push({ peer: fleet.nodes[j], j, data }); }
  const groups = {};
  rows.forEach(r => { const pg = (r.peer.cpg_name && r.peer.cpg_name !== 'unknown') ? r.peer.cpg_name : 'no PG'; (groups[pg] = groups[pg] || []).push(r); });
  const keys = Object.keys(groups).sort((a, b) => Math.min(...groups[a].map(r => r.data.p50)) - Math.min(...groups[b].map(r => r.data.p50)));
  let h = '<table><tr><th>Peer</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>';
  keys.forEach(pg => {
    h += '<tr class="pg-group"><td colspan="7">' + esc(pg) + '</td></tr>';
    groups[pg].sort((a, b) => a.data.p50 - b.data.p50).forEach(r => { const d = r.data;
      h += '<tr data-peer="' + r.j + '"><td class="peer-name">' + esc(r.peer.public_ip || r.peer.private_ip || r.peer.ec2_name) + '</td><td class="highlight">' + fmtLat(d.p50) + '</td><td>' + (d.p90 ? fmtLat(d.p90) : '\u2014') + '</td><td>' + fmtLat(d.p99) + '</td><td>' + (d.p999 ? fmtLat(d.p999) : '\u2014') + '</td><td>' + (d.max ? fmtLat(d.max) : '\u2014') + '</td><td>' + (d.loss !== undefined ? esc(d.loss) + '%' : '\u2014') + '</td></tr>'; });
  });
  return h + '</table>';
}

// Inline-styled role badge (renderer-agnostic — no dependence on scoped CSS so
// it looks identical in the 2D map and the 3D CSS2D tooltip).
const ROLE_TIP = { source: ['src', '#1f6feb', '#fff'], replicator: ['relay', '#f0883e', '#0d1117'], destination: ['dst', '#2ea043', '#fff'] };
function roleBadge(role) {
  const r = ROLE_TIP[role]; if (!r) return '';
  return ' <span class="role-badge" style="background:' + r[1] + ';color:' + r[2] + ';font-size:9px;font-weight:700;padding:1px 6px;border-radius:8px;text-transform:uppercase;letter-spacing:.3px">' + r[0] + '</span>';
}

// Full latency-panel inner HTML: title (+role badge) + Outbound + Inbound
// tables. Shared verbatim by the 2D hover/pinned tooltip and the 3D node
// tooltip so the panel's content and structure are one implementation.
export function nodeTipHTML(fleet, matrix, i) {
  const node = fleet.nodes[i];
  return '<h3>' + esc(node.public_ip || node.private_ip || node.ec2_name) + roleBadge(node.role) + '</h3>'
    + '<div class="direction">Outbound</div>' + buildPeerTable(fleet, matrix, i, false)
    + '<div class="direction" style="margin-top:6px">Inbound</div>' + buildPeerTable(fleet, matrix, i, true);
}
