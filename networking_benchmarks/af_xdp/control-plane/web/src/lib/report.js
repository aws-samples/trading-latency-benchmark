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

  // Node label: prefer private IP, fallback to ec2_name/index.
  const label = (n) => esc(n.private_ip || n.ec2_name || ('#' + n.index));
  // Short node descriptor: IP + role + PG (for table context).
  const nodeDesc = (n) => {
    let s = label(n);
    if (n.role) s += ` <span class="role role-${esc(n.role)}">${esc(n.role)}</span>`;
    return s;
  };
  const nodeInfo = (n) => {
    const parts = [label(n)];
    if (n.role) parts.push(n.role);
    if (n.az) parts.push(n.az);
    if (n.cpg_name && n.cpg_name !== 'unknown') parts.push(n.cpg_name);
    return parts.map(esc).join(' · ');
  };

  // Global p50 range for the heatmap colour scale.
  let mn = Infinity, mx = -Infinity, pairs = 0;
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    const c = matrix[i] && matrix[i][j];
    if (c) { pairs++; if (c.p50 < mn) mn = c.p50; if (c.p50 > mx) mx = c.p50; }
  }
  if (!isFinite(mn)) { mn = 0; mx = 1; }

  const gen = new Date().toISOString();
  const isMcast = kind === 'mcast';

  // ── Node inventory table (always shown) ─────────────────────────────────────
  let inventory = '<table class="inv"><tr><th>#</th><th>Private IP</th><th>Public IP</th><th>Role</th><th>AZ</th><th>PG</th><th>Type</th></tr>';
  nodes.forEach((n, i) => {
    inventory += `<tr><td>${i}</td><td>${label(n)}</td><td>${esc(n.public_ip || '—')}</td><td class="role-${esc(n.role || '')}">${esc(n.role || '—')}</td>`
      + `<td>${esc(n.az || '—')}</td><td>${esc(n.cpg_name && n.cpg_name !== 'unknown' ? n.cpg_name : '—')}</td>`
      + `<td>${esc(n.type || '—')}</td></tr>`;
  });
  inventory += '</table>';

  let heat, rows;

  if (isMcast) {
    // ── Mcast: fan-out view (source → replicator → destinations) ──────────────
    // Find the source and replicator by role, then show latency to each destination.
    const srcIdx = nodes.findIndex(n => n.role === 'source');
    const replIdx = nodes.findIndex(n => n.role === 'replicator');
    const dstIdxs = nodes.map((n, i) => n.role === 'destination' ? i : -1).filter(i => i >= 0);

    // Mcast heatmap: single-column (source → each destination via replicator)
    heat = '<table class="heat"><tr><th>Destination</th><th>Role</th><th>AZ</th><th>PG</th><th>p50</th><th>p99</th><th>loss</th></tr>';
    dstIdxs.forEach(di => {
      // In mcast matrix, the edge is typically src→dst (one-way through replicator)
      const c = (srcIdx >= 0 && matrix[srcIdx] && matrix[srcIdx][di]) || null;
      const n = nodes[di];
      const pg = n.cpg_name && n.cpg_name !== 'unknown' ? n.cpg_name : '—';
      if (c) {
        heat += `<tr><td>${label(n)}</td><td>${esc(n.role || '')}</td><td>${esc(n.az || '')}</td><td>${esc(pg)}</td>`
          + `<td style="background:${latencyColor(c.p50, mn, mx)};color:#0d1117;font-weight:700">${fmtLat(c.p50)}</td>`
          + `<td>${fmtLat(c.p99)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
      } else {
        heat += `<tr><td>${label(n)}</td><td>${esc(n.role || '')}</td><td>${esc(n.az || '')}</td><td>${esc(pg)}</td><td class="na">·</td><td class="na">·</td><td class="na">·</td></tr>`;
      }
    });
    heat += '</table>';

    // Path summary
    const srcLabel = srcIdx >= 0 ? nodeInfo(nodes[srcIdx]) : '?';
    const replLabel = replIdx >= 0 ? nodeInfo(nodes[replIdx]) : '?';
    heat = `<div class="path-info"><b>Path:</b> ${srcLabel} → ${replLabel} → ${dstIdxs.length} destination(s)</div>` + heat;

    // Full per-edge table (all edges that exist in the matrix, with roles)
    rows = '';
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
      const c = matrix[i] && matrix[i][j];
      if (!c || i === j) continue;
      const sn = nodes[i], dn = nodes[j];
      const sPg = sn.cpg_name && sn.cpg_name !== 'unknown' ? sn.cpg_name : '—';
      const dPg = dn.cpg_name && dn.cpg_name !== 'unknown' ? dn.cpg_name : '—';
      rows += `<tr><td>${label(sn)}</td><td>${esc(sn.role || '—')}</td><td>${esc(sPg)}</td>`
        + `<td>${label(dn)}</td><td>${esc(dn.role || '—')}</td>`
        + `<td>${esc(dn.az || '—')}</td><td>${esc(dPg)}</td>`
        + `<td>${fmtLat(c.p50)}</td><td>${fmtLat(c.p90)}</td><td>${fmtLat(c.p99)}</td><td>${fmtLat(c.p999)}</td><td>${fmtLat(c.max)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
    }

  } else {
    // ── Ucast: NxN heatmap (rows = src, cols = dst), cell colour = p50 ─────────
    heat = '<table class="heat"><tr><th>src \\ dst</th>' + nodes.map((n) => `<th title="${nodeInfo(n)}">${label(n)}</th>`).join('') + '</tr>';
    for (let i = 0; i < N; i++) {
      heat += `<tr><th title="${nodeInfo(nodes[i])}">${label(nodes[i])}</th>`;
      for (let j = 0; j < N; j++) {
        const c = matrix[i] && matrix[i][j];
        if (i === j) heat += '<td class="diag">—</td>';
        else if (!c) heat += '<td class="na">·</td>';
        else heat += `<td style="background:${latencyColor(c.p50, mn, mx)}" title="p99 ${fmtLat(c.p99)} · loss ${c.loss ?? 0}%">${fmtLat(c.p50)}</td>`;
      }
      heat += '</tr>';
    }
    heat += '</table>';

    // Full per-edge table with roles
    rows = '';
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
      const c = matrix[i] && matrix[i][j];
      if (!c || i === j) continue;
      const sn = nodes[i], dn = nodes[j];
      const sPg = sn.cpg_name && sn.cpg_name !== 'unknown' ? sn.cpg_name : '—';
      const dPg = dn.cpg_name && dn.cpg_name !== 'unknown' ? dn.cpg_name : '—';
      rows += `<tr><td>${label(sn)}</td><td>${esc(sn.role || '—')}</td><td>${esc(sPg)}</td>`
        + `<td>${label(dn)}</td><td>${esc(dn.role || '—')}</td>`
        + `<td>${esc(dn.az || '—')}</td><td>${esc(dPg)}</td>`
        + `<td>${fmtLat(c.p50)}</td><td>${fmtLat(c.p90)}</td><td>${fmtLat(c.p99)}</td><td>${fmtLat(c.p999)}</td><td>${fmtLat(c.max)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
    }
  }

  const tableHeader = '<tr><th>src</th><th>src role</th><th>src PG</th><th>dst</th><th>dst role</th><th>dst AZ</th><th>dst PG</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>';

  return `<!doctype html><html><head><meta charset="utf-8">
<title>AF_XDP report — ${esc(kind)}/${esc(variation)}</title>
<style>
  body{font:13px -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0d1117;color:#e6edf3;padding:24px;margin:0}
  h1{font-size:18px;margin:0 0 4px} h2{font-size:15px;margin:24px 0 4px;color:#58a6ff}
  .meta{color:#8b949e;margin-bottom:12px}
  .path-info{margin:8px 0;padding:8px 12px;background:#161b22;border:1px solid #30363d;border-radius:6px;font-size:12px}
  table{border-collapse:collapse;margin-top:8px}
  td,th{border:1px solid #30363d;padding:4px 9px;text-align:center;font-size:12px}
  th{background:#161b22;color:#8b949e;cursor:pointer;user-select:none}
  th:hover{background:#21262d}
  th.sorted-asc::after{content:' ▲';font-size:10px}
  th.sorted-desc::after{content:' ▼';font-size:10px}
  .heat td{font-family:'SF Mono',monospace;color:#0d1117;font-weight:700}
  .heat .diag,.heat .na{background:#161b22;color:#6e7681;font-weight:400}
  .inv td,.inv th{text-align:left;padding:3px 8px}
  td{font-family:'SF Mono',monospace}
  .role-source{color:#1f6feb} .role-replicator{color:#f0883e} .role-destination{color:#2ea043}
  .role{font-size:11px;padding:1px 5px;border-radius:8px;margin-left:4px}
  .coverage{font-size:12px;margin:6px 0 12px;padding:7px 10px;border-left:3px solid #d29922;background:#fff8e1;line-height:1.5}
</style></head><body>
  <h1>AF_XDP latency report — ${esc(kind)} / ${esc(variation)}</h1>
  <div class="meta">Region: ${esc(fleet.region || '?')} · Nodes: ${N} · Pairs: ${pairs} · Generated: ${gen}</div>
  ${isMcast ? '' : `<div class="coverage">Coverage: <b>${pairs}</b> of <b>${N * (N - 1)}</b> possible ordered pairs measured.${pairs < N * (N - 1) ? ` <b>${N * (N - 1) - pairs} missing.</b> A blank cell is either a pair that never ran, or one <b>rejected by the loss gate</b> — rtt derives percentiles only from datagrams that returned, so a lossy run describes its surviving subset and is not comparable to a clean run. Rejected pairs are recorded as failures rather than published as results; check the run log / error list for the reason.` : ''}</div>`}
  <h2>Fleet inventory</h2>
  ${inventory}
  <h2>${isMcast ? 'Fan-out latency — source → replicator → destinations' : 'Heatmap — p50 (green = fast, red = slow)'}</h2>
  ${heat}
  <h2>All measured latencies</h2>
  <table id="lat-table">${tableHeader}${rows}</table>
  <script>
  // Column sorting for the latency table
  (function(){
    const table = document.getElementById('lat-table');
    if (!table) return;
    const headers = table.querySelectorAll('th');
    let sortCol = -1, sortDir = 1;
    // Parse a formatted latency cell ("34 µs", "0.2 ms", "1.5 s") into µs for
    // numeric comparison. Returns NaN for non-latency cells (sorted lexically).
    function parseLatUs(s) {
      const m = s.match(/^([\\d.]+)\\s*(s|ms|µs|%?)$/);
      if (!m) return NaN;
      const v = parseFloat(m[1]);
      if (m[2] === 's') return v * 1000000;
      if (m[2] === 'ms') return v * 1000;
      return v; // µs or bare number (loss %)
    }
    headers.forEach((th, col) => {
      th.addEventListener('click', () => {
        if (sortCol === col) sortDir *= -1;
        else { sortCol = col; sortDir = 1; }
        headers.forEach(h => h.classList.remove('sorted-asc','sorted-desc'));
        th.classList.add(sortDir === 1 ? 'sorted-asc' : 'sorted-desc');
        const tbody = table.querySelector('tbody') || table;
        const rows = Array.from(tbody.querySelectorAll('tr')).slice(1);
        rows.sort((a, b) => {
          const av = a.cells[col]?.textContent?.trim() || '';
          const bv = b.cells[col]?.textContent?.trim() || '';
          const an = parseLatUs(av), bn = parseLatUs(bv);
          if (!isNaN(an) && !isNaN(bn)) return (an - bn) * sortDir;
          return av.localeCompare(bv) * sortDir;
        });
        rows.forEach(r => tbody.appendChild(r));
      });
    });
  })();
  </script>
</body></html>`;
}
