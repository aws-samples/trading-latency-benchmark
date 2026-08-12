// report.js — build a self-contained HTML report (heatmap + full latency table)
// from the currently-shown fleet (afxdp.topology/v1 model), for the chosen
// kind/variation. Runs entirely client-side so it works against live data
// without a backend report endpoint (the offline gen/report.py remains for
// saved runs). Returns an HTML string; the caller downloads it as a Blob.

import { fmtLat, latencyColor, esc } from './2d/palette.js';

// Measurement ages section (3.3): reports oldest/newest cell and count of cells
// older than the newest run. A scoped-run grid is a mosaic of measurement ages.
function buildMeasurementAges(matrix, N) {
  let oldest = Infinity, newest = 0, total = 0;
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    const c = matrix[i] && matrix[i][j];
    if (!c || !c.unix) continue;
    total++;
    if (c.unix < oldest) oldest = c.unix;
    if (c.unix > newest) newest = c.unix;
  }
  if (total === 0) return '';
  const fmtTime = (u) => new Date(u * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC';
  let staleCount = 0;
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    const c = matrix[i] && matrix[i][j];
    if (c && c.unix && c.unix < newest) staleCount++;
  }
  return `<div class="coverage"><h3 style="margin:0 0 4px;font-size:13px;color:#58a6ff">Measurement ages</h3>`
    + `<b>newest</b>: ${fmtTime(newest)} · <b>oldest</b>: ${fmtTime(oldest)}`
    + (staleCount > 0 ? ` · <b>${staleCount}</b> of ${total} cells are older than the newest run - this grid is a mosaic of measurements taken at different times, not a single snapshot.` : ' · All cells from the same run.')
    + `</div>`;
}

// Compare-mode view (3.4/D6): cell = delta p50 (xdp - kernel), diverging colour,
// cells missing EITHER mode rendered hatched.
export function buildCompareHTML(nodes, kernelMatrix, xdpMatrix) {
  const N = nodes.length;
  const label = (n) => esc(n.private_ip || n.ec2_name || ('#' + n.index));
  // Compute max absolute delta for colour scale.
  let maxAbs = 1;
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    if (i === j) continue;
    const k = kernelMatrix[i] && kernelMatrix[i][j];
    const x = xdpMatrix[i] && xdpMatrix[i][j];
    if (k && x && k.p50 != null && x.p50 != null) {
      const d = Math.abs(x.p50 - k.p50);
      if (d > maxAbs) maxAbs = d;
    }
  }
  let html = '<table class="heat compare-heat"><tr><th>src \\ dst</th>'
    + nodes.map((n) => `<th>${label(n)}</th>`).join('') + '</tr>';
  for (let i = 0; i < N; i++) {
    html += `<tr><th>${label(nodes[i])}</th>`;
    for (let j = 0; j < N; j++) {
      if (i === j) { html += '<td class="diag">\u2014</td>'; continue; }
      const k = kernelMatrix[i] && kernelMatrix[i][j];
      const x = xdpMatrix[i] && xdpMatrix[i][j];
      if (!k && !x) { html += '<td class="na">\u00b7</td>'; continue; }
      if (!k || !x || k.p50 == null || x.p50 == null) {
        // Missing EITHER mode: hatched, not green.
        html += '<td class="hatched" title="missing ' + (!k ? 'kernel' : 'xdp') + ' data">\u2014</td>';
        continue;
      }
      const delta = x.p50 - k.p50;
      const t = Math.min(1, Math.abs(delta) / maxAbs);
      let bg;
      if (delta < 0) {
        const g = Math.round(60 + t * 150);
        bg = `rgba(46,${g},67,${(0.3 + t * 0.6).toFixed(2)})`;
      } else if (delta > 0) {
        const r = Math.round(100 + t * 148);
        bg = `rgba(${r},50,50,${(0.3 + t * 0.6).toFixed(2)})`;
      } else {
        bg = 'rgba(128,128,128,0.2)';
      }
      const sign = delta > 0 ? '+' : '';
      html += `<td style="background:${bg}" title="xdp p50 - kernel p50">${sign}${delta}</td>`;
    }
    html += '</tr>';
  }
  html += '</table>';
  return html;
}

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
  // Report name: spell out the transport and name the fwd/variation as a mode.
  const kindName = isMcast ? 'multicast' : kind === 'ucast' ? 'unicast' : esc(kind);
  const reportName = `Latency Report - ${kindName} ${esc(variation)} mode`;

  // ── Node inventory table (always shown) ─────────────────────────────────────
  let inventory = '<table class="inv sortable" id="inv-table"><tr><th>#</th><th>Private IP</th><th>Public IP</th><th>Role</th><th>VPC ID</th><th>AZ</th><th>PG</th><th>Type</th></tr>';
  nodes.forEach((n, i) => {
    inventory += `<tr data-ip="${esc(n.private_ip || '')}"><td>${i}</td><td>${label(n)}</td><td>${esc(n.public_ip || '—')}</td><td class="role-${esc(n.role || '')}">${esc(n.role || '—')}</td>`
      + `<td>${esc(n.vpc_id && n.vpc_id !== 'unknown' ? n.vpc_id : '—')}</td>`
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

    // Mcast path table: one row per source -> replicator -> destination path.
    heat = '<table class="heat sortable" id="fanout-table"><tr>'
      + '<th>Source</th><th>Replicator</th><th>Destination</th><th>Dst AZ</th>'
      + '<th>p50</th><th>p99</th><th>loss</th></tr>';
    dstIdxs.forEach(di => {
      // The live model renders mcast as two physical hops and attributes the
      // end-to-end one-way metric to the measured last leg (replicator -> dest),
      // so read that cell. Fall back to source -> dest for a saved fleet.json
      // that stored the direct edge instead.
      const c = (replIdx >= 0 && matrix[replIdx] && matrix[replIdx][di])
        || (srcIdx >= 0 && matrix[srcIdx] && matrix[srcIdx][di])
        || null;
      const n = nodes[di];
      const sIp = srcIdx >= 0 ? label(nodes[srcIdx]) : '\u2014';
      const rIp = replIdx >= 0 ? label(nodes[replIdx]) : '\u2014';
      const head = `<tr data-ip="${esc(n.private_ip || '')}">`
        + `<td>${sIp}</td><td>${rIp}</td><td>${label(n)}</td><td>${esc(n.az || '\u2014')}</td>`;
      if (c) {
        heat += head
          + `<td style="background:${latencyColor(c.p50, mn, mx)};color:#0d1117;font-weight:700">${fmtLat(c.p50)}</td>`
          + `<td>${fmtLat(c.p99)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
      } else {
        heat += head + '<td class="na">\u00b7</td><td class="na">\u00b7</td><td class="na">\u00b7</td></tr>';
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
      rows += `<tr data-src="${esc(sn.private_ip || '')}" data-dst="${esc(dn.private_ip || '')}"><td>${label(sn)}</td><td>${esc(sn.role || '—')}</td>`
        + `<td>${label(dn)}</td><td>${esc(dn.role || '—')}</td><td>${esc(dn.az || '—')}</td>`
        + `<td>${esc(sPg)}</td><td>${esc(dPg)}</td>`
        + `<td>${fmtLat(c.p50)}</td><td>${fmtLat(c.p90)}</td><td>${fmtLat(c.p99)}</td><td>${fmtLat(c.p999)}</td><td>${fmtLat(c.max)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
    }

  } else {
    // ── Ucast: NxN heatmap (rows = src, cols = dst), cell colour = p50 ─────────
    heat = '<table class="heat" id="heat-table"><tr><th>src \\ dst</th>'
      + nodes.map((n) => `<th data-col-ip="${esc(n.private_ip || '')}" title="${nodeInfo(n)}">${label(n)}</th>`).join('') + '</tr>';
    for (let i = 0; i < N; i++) {
      const rip = esc(nodes[i].private_ip || '');
      heat += `<tr data-ip="${rip}"><th data-row-ip="${rip}" title="${nodeInfo(nodes[i])}">${label(nodes[i])}</th>`;
      for (let j = 0; j < N; j++) {
        const c = matrix[i] && matrix[i][j];
        const dat = ` data-row-ip="${rip}" data-col-ip="${esc(nodes[j].private_ip || '')}"`;
        if (i === j) heat += `<td class="diag"${dat}>—</td>`;
        else if (!c) heat += `<td class="na"${dat}>·</td>`;
        else {
          const cellTime = c.unix ? new Date(c.unix * 1000).toISOString().replace('T', ' ').slice(0, 19) : '';
          const cellTitle = `${esc(variation)} · p99 ${fmtLat(c.p99)} · loss ${c.loss ?? 0}%${cellTime ? ' · ' + cellTime : ''}`;
          heat += `<td${dat} style="background:${latencyColor(c.p50, mn, mx)}" title="${cellTitle}">${fmtLat(c.p50)}</td>`;
        }
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
      rows += `<tr data-src="${esc(sn.private_ip || '')}" data-dst="${esc(dn.private_ip || '')}"><td>${label(sn)}</td><td>${esc(sn.role || '—')}</td>`
        + `<td>${label(dn)}</td><td>${esc(dn.role || '—')}</td><td>${esc(dn.az || '—')}</td>`
        + `<td>${esc(sPg)}</td><td>${esc(dPg)}</td>`
        + `<td>${fmtLat(c.p50)}</td><td>${fmtLat(c.p90)}</td><td>${fmtLat(c.p99)}</td><td>${fmtLat(c.p999)}</td><td>${fmtLat(c.max)}</td><td>${esc(c.loss ?? 0)}%</td></tr>`;
    }
  }

  // ── Measurement methodology ───────────────────────────────────────
  // Stated per mode: ucast and mcast measure different quantities on different
  // clock bases. A round trip inside ONE realtime domain needs no clock sync; a
  // one-way delay spans hosts and is only meaningful because chrony disciplines
  // every node to the ENA PHC hardware clock.
  const UCAST_VARIATION_NOTES = {
    kernel: 'TX <code>sendto()</code>; RX kernel busy-poll <code>recvmsg()</code>. No AF_XDP socket on the measuring node.',
    xdp: 'TX via AF_XDP zero-copy on a non-RSS TX queue; RX is still the kernel busy-poll socket, with the ingress time stamped at the XDP hook. <code>--xdp-rx</code> is instrumented kernel RX, NOT a bypass receive.',
  };
  const methodology = isMcast ? `
  <div class="metric-kind">Reported value is a <b>ONE-WAY</b> delay: source → replicator → destination. It is not a round trip, and is not comparable with the ucast RTT figures.</div>
  <details class="method">
    <summary>How this was measured</summary>
    <dl>
      <dt>Path</dt><dd>EC2 VPCs do not forward raw multicast, so an 8-byte <code>m2u</code> header rides inside a plain unicast UDP datagram. The source sends to the replicator, which emits one unicast copy per registered destination. Two hops, both measured.</dd>
      <dt>Datapath</dt><dd>Source: AF_XDP zero-copy TX. Replicator: the <code>mcast.o</code> XDP program redirects the matching frame to an AF_XDP socket and userspace re-emits per destination (fwd mode <code>${esc(variation)}</code>). Destination: its own <code>mcast.o</code> redirects to an XSK, so the kernel IP stack is not involved after the XDP redirect.</dd>
      <dt>Stamps</dt><dd><code>ts_ns</code> at the source immediately before TX ring submit, <code>replicator_ns</code> at replicator RX entry, <code>rx_ns</code> at destination RX. One-way = <code>rx_ns − ts_ns</code>, split as <code>replicator_ns − ts_ns</code> (source→replicator) and <code>rx_ns − replicator_ns</code> (replicator→destination).</dd>
      <dt>Clock</dt><dd><code>CLOCK_REALTIME</code> on all three nodes — necessarily, since a one-way delay spans hosts. chrony disciplines each node to the <b>ENA PHC hardware clock</b> (<code>refclock PHC /dev/ptp0</code>, <code>phc_enable=1</code>), reading the Nitro clock directly rather than over NTP-UDP; AWS Time Sync (<code>169.254.169.123</code>, <code>xleave</code>, <code>minpoll 2</code>) is the fallback until PHC is up. Observed RMS offset is tens of nanoseconds, well below the microsecond figures reported here.</dd>
      <dt>Gate</dt><dd>A run aborts when the inter-node offset exceeds the configured ceiling: a destination clock behind the source produces an invalid, possibly negative, one-way delay. Percentiles derive only from datagrams that arrived.</dd>
      <dt>On <code>kernel</code> fwd mode</dt><dd><code>XDP_TX</code> is a single-destination passthrough rather than a fan-out, so that mode measures one representative destination.</dd>
    </dl>
  </details>` : `
  <div class="metric-kind">Reported value is a <b>ROUND-TRIP TIME</b> (RTT) through the remote replicator's echo, at queue depth 1 — one datagram in flight at a time.</div>
  <details class="method">
    <summary>How this was measured</summary>
    <dl>
      <dt>Path</dt><dd>The measuring node sends to a peer whose replicator echoes the datagram straight back. Ordered pairs are measured one source at a time, and that source's own replicator is stopped for the duration so no AF_XDP socket owns the RX queue the echoes return on.</dd>
      <dt>Variation <code>${esc(variation)}</code></dt><dd>${UCAST_VARIATION_NOTES[variation] || 'See tools/rtt.cpp for this variation.'}</dd>
      <dt>Stamps</dt><dd>TX: <code>CLOCK_REALTIME</code> sampled immediately before the send. RX: kernel software timestamp (<code>SOF_TIMESTAMPING_RX_SOFTWARE</code>) recorded in the NAPI receive path just after the driver builds the skb — before the socket receive-queue enqueue, so socket-queue and scheduler jitter are excluded. Falls back to a userspace read if no cmsg timestamp is present.</dd>
      <dt>Clock</dt><dd>A single <code>CLOCK_REALTIME</code> domain on one host, so <b>no inter-node clock sync is required</b> and none of its error enters the result. (<code>--xdp-rx</code> uses <code>CLOCK_MONOTONIC</code> on both ends instead, to match the XDP <code>bpf_ktime_get_ns()</code> stamp.) No TSC and no PHC are used for RTT — ENA has no TX hardware timestamp.</dd>
      <dt>Statistic</dt><dd>Service-time RTT = <code>recv − actual_send</code>, which excludes coordinated omission. Warmup datagrams are discarded before percentiles are computed, and percentiles derive only from datagrams that returned: a run over the loss ceiling is rejected rather than published, since its surviving subset is not comparable with a clean run.</dd>
      <dt>Host tuning</dt><dd>The <code>kernel</code> baseline is not the generic stack: <code>SO_BUSY_POLL</code> + <code>SO_PREFER_BUSY_POLL</code>, <code>SCHED_FIFO</code>, isolated-core pinning, ENA IRQ affinity, <code>napi_defer_hard_irqs</code>, <code>gro_flush_timeout=10us</code>, coalescing off.</dd>
    </dl>
  </details>`;

  const tableHeader = '<tr><th>src IP</th><th>src role</th><th>dst IP</th><th>dst role</th><th>dst AZ</th><th>src PG</th><th>dst PG</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>';

  return `<!doctype html><html><head><meta charset="utf-8">
<title>${reportName}</title>
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
  /* The NxN heatmap tints every cell, so dark text is right there. The fan-out
     table has plain cells, where #0d1117 on the dark page is invisible. */
  #fanout-table td{color:#e6edf3;font-weight:400}
  #fanout-table td[style]{font-weight:700}
  /* Measurement methodology block */
  .method{font-size:12px;margin:8px 0 14px;padding:10px 12px;background:#161b22;
    border:1px solid #30363d;border-left:3px solid #58a6ff;border-radius:6px;
    color:#adbac7;line-height:1.65}
  .method summary{font-size:13px;color:#58a6ff;cursor:pointer;font-weight:600;
    list-style:none;user-select:none}
  .method summary::-webkit-details-marker{display:none}
  .method summary::before{content:'▶';display:inline-block;margin-right:6px;
    font-size:10px;transition:transform .12s}
  .method[open] summary::before{transform:rotate(90deg)}
  .method summary:hover{color:#79c0ff}
  .method[open] summary{margin-bottom:4px}
  .method dt{color:#e6edf3;font-weight:600;margin-top:6px}
  .method dd{margin:0 0 0 14px}
  .method code{background:#0d1117;padding:1px 4px;border-radius:3px;color:#79c0ff}
  .metric-kind{font-size:13px;color:#e6edf3;margin:2px 0 8px}
  .metric-kind b{color:#f0883e}
  .heat .diag,.heat .na{background:#161b22;color:#6e7681;font-weight:400}
  .inv td,.inv th{text-align:left;padding:3px 8px}
  td{font-family:'SF Mono',monospace}
  .role-source{color:#1f6feb} .role-replicator{color:#f0883e} .role-destination{color:#2ea043}
  .role{font-size:11px;padding:1px 5px;border-radius:8px;margin-left:4px}
  .coverage{font-size:12px;margin:6px 0 12px;padding:7px 10px;border-left:3px solid #d29922;
    background:#1c1810;color:#e3b341;line-height:1.5}
  .coverage b{color:#f0c674}
  /* Cross-table instance selection */
  .selbar{display:flex;align-items:center;gap:10px;font-size:12px;margin:10px 0;padding:7px 10px;
    border:1px solid #30363d;border-radius:6px;background:#161b22;color:#8b949e}
  .selbar.active{border-color:#d29922;color:#e3b341}
  .selbar button{background:#21262d;color:#adbac7;border:1px solid #30363d;border-radius:5px;
    padding:3px 9px;cursor:pointer;font:600 11px inherit;margin-left:auto;flex:0 0 auto}
  .selbar button:hover{background:#30363d;color:#fff}
  .inv tr.sel td, .heat tr.sel td{background:#243b53 !important;color:#e6edf3 !important}
  .inv tr.sel td:first-child{box-shadow:inset 3px 0 0 #d29922}
  /* Heatmap: a selected instance lights its whole row and column. */
  #heat-table td.sel-row, #heat-table td.sel-col{outline:2px solid #d29922;outline-offset:-2px}
  #heat-table th.sel-row, #heat-table th.sel-col{background:#243b53;color:#e6edf3}
  /* Latency rows: source match, destination match, or both. */
  #lat-table tr.sel-src td{background:#1f3350 !important;box-shadow:inset 3px 0 0 #58a6ff}
  #lat-table tr.sel-dst td{background:#1d3326 !important;box-shadow:inset 3px 0 0 #3fb950}
  #lat-table tr.sel-both td{background:#3a2f14 !important;box-shadow:inset 3px 0 0 #d29922}
</style></head><body>
  <h1>${reportName}</h1>
  <div class="meta">Region: ${esc(fleet.region || '?')} · Nodes: ${N} · Pairs: ${pairs} · Generated: ${gen}</div>
  ${methodology}
  ${isMcast ? '' : `<div class="coverage">Coverage: <b>${pairs}</b> of <b>${N * (N - 1)}</b> possible ordered pairs measured.${pairs < N * (N - 1) ? ` <b>${N * (N - 1) - pairs} missing.</b> A blank cell is either a pair that never ran, or one <b>rejected by the loss gate</b> — rtt derives percentiles only from datagrams that returned, so a lossy run describes its surviving subset and is not comparable to a clean run. Rejected pairs are recorded as failures rather than published as results; check the run log / error list for the reason.` : ''}</div>`}
  <div class="selbar" id="selbar-wrap"><span id="selbar"></span><button id="selclear">Clear</button></div>
  <h2>Fleet inventory</h2>
  ${inventory}
  <h2>${isMcast ? 'Multicast paths — one-way latency, source → replicator → destination' : 'Heatmap — round-trip p50 (green = fast, red = slow)'}</h2>
  ${heat}
  ${buildMeasurementAges(matrix, N)}
  <h2>All measured latencies — ${isMcast ? 'one-way' : 'round-trip (RTT)'}</h2>
  <table id="lat-table" class="sortable">${tableHeader}${rows}</table>
  <script>
  (function(){
    // ── Column sorting: every table marked .sortable. The NxN heatmap is
    // excluded because its rows and columns are the same node axis, so
    // reordering rows would break its correspondence with the header row.
    function parseLatUs(s) {
      // Backslashes are doubled: this lives inside a JS template literal, so a
      // single \\d would be eaten by escape processing and the match would
      // silently always fail, falling back to lexical order.
      //
      // A unit is REQUIRED for the numeric path. Without that, a dotted IP like
      // 10.0.0.10 parses as 10 (parseFloat stops at the second dot) and every
      // address in a subnet compares equal, so the IP columns never sorted.
      const u = s.match(/^([\\d.]+)\\s*(ms|s|\u00b5s|\u03bcs)$/);
      if (u) {
        const v = parseFloat(u[1]);
        return u[2] === 's' ? v * 1000000 : u[2] === 'ms' ? v * 1000 : v;
      }
      const pct = s.match(/^([\\d.]+)%$/);
      if (pct) return parseFloat(pct[1]);
      // Plain integer/decimal (row index, vCPU counts). Multi-dot strings such as
      // IPs fall through to NaN and are compared with numeric-aware collation.
      if (/^\\d+(\\.\\d+)?$/.test(s)) return parseFloat(s);
      return NaN;
    }
    document.querySelectorAll('table.sortable').forEach((table) => {
      const headers = table.querySelectorAll('tr:first-child th');
      let sortCol = -1, sortDir = 1;
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
            return av.localeCompare(bv, undefined, { numeric: true }) * sortDir;
          });
          rows.forEach(r => tbody.appendChild(r));
        });
      });
    });

    // ── Cross-table instance selection ────────────────────────────────────
    // Selection is a SET of instances, so several can be highlighted at once.
    // Clicking a row in the inventory, fan-out, or latency table toggles that
    // row's instance. A selected instance lights up: its inventory row, its
    // row AND column in the heatmap, and every latency row where it appears as
    // source or destination.
    const sel = new Set();

    function paint() {
      document.querySelectorAll('#inv-table tr[data-ip], #fanout-table tr[data-ip]').forEach((tr) => {
        tr.classList.toggle('sel', sel.has(tr.dataset.ip));
      });
      document.querySelectorAll('#heat-table [data-row-ip]').forEach((el) => {
        el.classList.toggle('sel-row', sel.has(el.dataset.rowIp));
      });
      document.querySelectorAll('#heat-table [data-col-ip]').forEach((el) => {
        el.classList.toggle('sel-col', sel.has(el.dataset.colIp));
      });
      document.querySelectorAll('#lat-table tr[data-src]').forEach((tr) => {
        const s1 = sel.has(tr.dataset.src), s2 = sel.has(tr.dataset.dst);
        tr.classList.toggle('sel-src', s1);
        tr.classList.toggle('sel-dst', s2 && !s1);
        tr.classList.toggle('sel-both', s1 && s2);
      });
      const n = sel.size;
      const bar = document.getElementById('selbar');
      bar.textContent = n
        ? n + ' instance' + (n > 1 ? 's' : '') + ' selected: ' + [...sel].join(', ') + '  (click a row to toggle)'
        : 'Click any row in Fleet inventory or All measured latencies to highlight that instance across the tables. Click again to clear.';
      bar.classList.toggle('active', n > 0);
    }

    function toggle(ip) {
      if (!ip) return;
      if (sel.has(ip)) sel.delete(ip); else sel.add(ip);
      paint();
    }

    // Inventory / fan-out: the row IS one instance.
    document.querySelectorAll('#inv-table tr[data-ip], #fanout-table tr[data-ip]').forEach((tr) => {
      tr.style.cursor = 'pointer';
      tr.addEventListener('click', () => toggle(tr.dataset.ip));
    });
    // Latency rows describe a PAIR. Clicking the src or dst cell selects that
    // endpoint; clicking anywhere else on the row selects the source.
    document.querySelectorAll('#lat-table tr[data-src]').forEach((tr) => {
      tr.style.cursor = 'pointer';
      tr.addEventListener('click', (ev) => {
        // Column order: 0 src IP, 1 src role, 2 dst IP, ... so index 2 is the
        // destination endpoint; anywhere else on the row selects the source.
        const ci = ev.target.cellIndex;
        toggle(ci === 2 ? tr.dataset.dst : tr.dataset.src);
      });
    });
    // The heatmap's own axis labels select too: it is the most natural place to
    // click when reading the grid. Row and column headers both toggle the node,
    // and paint() marks that node's row AND column.
    document.querySelectorAll('#heat-table th[data-row-ip], #heat-table th[data-col-ip]').forEach((th) => {
      th.style.cursor = 'pointer';
      th.addEventListener('click', () => toggle(th.dataset.rowIp || th.dataset.colIp));
    });
    document.getElementById('selclear').addEventListener('click', () => { sel.clear(); paint(); });
    paint();
  })();
  </script>
</body></html>`;
}
