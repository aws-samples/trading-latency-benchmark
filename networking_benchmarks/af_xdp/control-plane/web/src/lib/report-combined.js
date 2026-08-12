// Combined report: ONE self-contained HTML report covering every mode that
// has been measured, with per-cell mode metadata.
//
// Runs in parallel with report.js buildReportHTML(), which still produces a
// single-mode document and is unchanged. This module composes the same shape
// per mode and adds a mode-annotated overview.
//
// Two properties are deliberately preserved:
//   - Each mode keeps its OWN heatmap, so colour stays comparable within a mode.
//     A grid mixing a kernel p50 with an mcast one-way would make colour a
//     function of mode rather than of network position.
//   - The overview grid, which does mix modes to show the freshest value per
//     cell, says so, and badges every cell with the mode that produced it.

import { fmtLat, cellColor, isCrossRegion, esc, LATENCY_BEST_COLOR, latencyRange } from './2d/palette.js';
import { buildCompareHTML } from './report.js';

/** Short per-mode badge: K/X for ucast kernel/xdp, C/I/K for mcast fwd modes. */
export const MODE_BADGE = {
  'ucast/kernel': 'K',
  'ucast/xdp': 'X',
  'mcast/copy': 'C',
  'mcast/inplace': 'I',
  'mcast/kernel': 'MK',
};

const modeKey = (v) => `${v.kind}/${v.variation}`;
const badgeOf = (key) => MODE_BADGE[key] || key.split('/')[1].slice(0, 2).toUpperCase();
const label = (n) => esc(n.private_ip || n.ec2_name || '#' + n.index);
const p2 = (v) => String(v).padStart(2, '0');
/** hh:mm, dd-mm-yyyy. A saved report outlives any relative age. */
const stampTz = (unix, tz) => {
  if (!unix) return 'unknown';
  const d = new Date(unix * 1000);
  if (!tz) {
    return `${p2(d.getHours())}:${p2(d.getMinutes())}-`
      + `${p2(d.getDate())}.${p2(d.getMonth() + 1)}.${d.getFullYear()}`;
  }
  // Same hh:mm-dd.mm.yyyy shape, rendered in the chosen zone.
  const f = new Intl.DateTimeFormat('en-GB', {
    timeZone: tz, hour: '2-digit', minute: '2-digit',
    day: '2-digit', month: '2-digit', year: 'numeric', hour12: false,
  }).formatToParts(d).reduce((a, x) => (a[x.type] = x.value, a), {});
  return `${f.hour}:${f.minute}-${f.day}.${f.month}.${f.year}`;
};

/** Zone label for the report header: the chosen zone, or the browser's. */
const tzLabel = (tz) => tz || Intl.DateTimeFormat().resolvedOptions().timeZone || 'local';

/**
 * Flatten every view into one measurement list plus a per-cell freshest index.
 * A cell can be measured by several modes; the overview shows the newest.
 */
function collate(views) {
  const rows = [];   // one per measured pair, per mode
  const best = new Map(); // "src|dst" -> { unix, key, cell }
  for (const v of views) {
    const key = modeKey(v);
    const { nodes, matrix } = v.fleet;
    for (let i = 0; i < nodes.length; i++) {
      for (let j = 0; j < nodes.length; j++) {
        const c = matrix[i] && matrix[i][j];
        if (!c) continue;
        const src = nodes[i], dst = nodes[j];
        const unix = c.unix || v.unix || 0;
        rows.push({ key, kind: v.kind, variation: v.variation, src, dst, cell: c, unix });
        const ck = `${src.private_ip}|${dst.private_ip}`;
        const prev = best.get(ck);
        if (!prev || unix >= prev.unix) best.set(ck, { unix, key, cell: c });
      }
    }
  }
  return { rows, best };
}

/** Mode-annotated overview: freshest value per cell, badged with its mode. */
function overviewGrid(nodes, best, scale, tz) {
  const { mn, mx, gold } = scale;
  let h = '<table class="heat" id="overview-table"><tr><th>src \\ dst</th>'
    + nodes.map((n) => `<th data-col-ip="${esc(n.private_ip || '')}">${label(n)}</th>`).join('')
    + '</tr>';
  nodes.forEach((rn) => {
    const rip = esc(rn.private_ip || '');
    h += `<tr data-ip="${rip}"><th data-row-ip="${rip}">${label(rn)}</th>`;
    nodes.forEach((cn) => {
      const cip = esc(cn.private_ip || '');
      const dat = ` data-row-ip="${rip}" data-col-ip="${cip}"`;
      if (rn === cn) { h += `<td class="na"${dat}>\u00b7</td>`; return; }
      const b = best.get(`${rn.private_ip}|${cn.private_ip}`);
      if (!b) { h += `<td class="na"${dat}>\u00b7</td>`; return; }
      const tip = `${fmtLat(b.cell.p50)} \u00b7 ${b.key} \u00b7 ${stampTz(b.unix, tz)}`;
      h += `<td${dat} style="color:${cellColor(b.cell.p50, mn, mx, isCrossRegion(rn, cn), gold)};font-weight:700"`
        + ` title="${esc(tip)}">${fmtLat(b.cell.p50)}`
        + `<span class="mode-badge">${badgeOf(b.key)}</span></td>`;
    });
    h += '</tr>';
  });
  return h + '</table>';
}

/** Per-mode heatmap, so colour remains comparable inside that mode. */
function modeHeatmap(v, scale) {
  const { nodes, matrix } = v.fleet;
  const key = modeKey(v);
  const { mn, mx, gold } = scale;
  let h = `<table class="heat" data-mode="${esc(key)}"><tr><th>src \\ dst</th>`
    + nodes.map((n) => `<th data-col-ip="${esc(n.private_ip || '')}">${label(n)}</th>`).join('')
    + '</tr>';
  nodes.forEach((rn, i) => {
    const rip = esc(rn.private_ip || '');
    h += `<tr data-ip="${rip}"><th data-row-ip="${rip}">${label(rn)}</th>`;
    nodes.forEach((cn, j) => {
      const cip = esc(cn.private_ip || '');
      const dat = ` data-row-ip="${rip}" data-col-ip="${cip}"`;
      const c = matrix[i] && matrix[i][j];
      if (!c) { h += `<td class="na"${dat}>\u00b7</td>`; return; }
      h += `<td${dat} style="color:${cellColor(c.p50, mn, mx, isCrossRegion(rn, cn), gold)};font-weight:700"`
        + ` title="${esc(fmtLat(c.p50) + ' \u00b7 ' + key)}">${fmtLat(c.p50)}</td>`;
    });
    h += '</tr>';
  });
  return h + '</table>';
}

/** Per-mode methodology. With both kinds in one document this cannot be global. */
function methodology(v) {
  const isMcast = v.kind === 'mcast';
  const metric = isMcast
    ? 'Reported value is a <b>ONE-WAY</b> delay: source \u2192 replicator \u2192 destination. It is not a round trip, and is not comparable with the ucast RTT figures.'
    : 'Reported value is a <b>ROUND-TRIP TIME</b> (RTT) through the remote replicator\u2019s echo, at queue depth 1.';
  const detail = isMcast
    ? `<dt>Clock</dt><dd><code>CLOCK_REALTIME</code> on all three nodes \u2014 necessarily, since a one-way delay spans hosts. chrony disciplines each node to the <b>ENA PHC hardware clock</b> (<code>refclock PHC /dev/ptp0</code>); AWS Time Sync is the fallback. Observed RMS offset is tens of nanoseconds.</dd>
       <dt>Stamps</dt><dd><code>ts_ns</code> at the source before TX ring submit, <code>replicator_ns</code> at replicator RX, <code>rx_ns</code> at destination RX. One-way = <code>rx_ns \u2212 ts_ns</code>.</dd>
       <dt>Fwd mode</dt><dd><code>${esc(v.variation)}</code>. <code>XDP_TX</code> (kernel) is a single-destination passthrough, not a fan-out.</dd>`
    : `<dt>Clock</dt><dd>A single <code>CLOCK_REALTIME</code> domain on one host, so <b>no inter-node clock sync is required</b> and none of its error enters the result. No TSC and no PHC are used for RTT.</dd>
       <dt>Stamps</dt><dd>TX <code>CLOCK_REALTIME</code> immediately before the send; RX a kernel software timestamp recorded in the NAPI receive path, before the socket queue.</dd>
       <dt>Variation</dt><dd><code>${esc(v.variation)}</code>. <code>--xdp-rx</code> is instrumented kernel RX, NOT a bypass receive.</dd>`;
  return `<div class="metric-kind">${metric}</div>
  <details class="method"><summary>How this was measured</summary><dl>${detail}
    <dt>Statistic</dt><dd>Service-time RTT excludes coordinated omission; warmup datagrams are discarded and percentiles derive only from datagrams that arrived. A run over the loss ceiling is rejected rather than published.</dd>
  </dl></details>`;
}

/** Combined latency table: every mode, one table, with a mode column. */
function latencyTable(rows, tz) {
  const head = '<tr><th>mode</th><th>src IP</th><th>dst IP</th><th>src VPC</th><th>dst VPC</th>'
    + '<th>src role</th><th>dst role</th><th>src AZ</th><th>dst AZ</th><th>src PG</th><th>dst PG</th>'
    + '<th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th><th>measured</th></tr>';
  const pg = (n) => esc(n.cpg_name && n.cpg_name !== 'unknown' ? n.cpg_name : '\u2014');
  const vpc = (n) => esc(n.vpc_id && n.vpc_id !== 'unknown' ? n.vpc_id : '\u2014');
  const sorted = rows
    .slice()
    .sort((a, b) => a.key.localeCompare(b.key)
      || (a.src.private_ip || '').localeCompare(b.src.private_ip || ''));

  // Compute best/worst per measurement column for colouring
  const measCols = ['p50', 'p90', 'p99', 'p999', 'max', 'loss'];
  const extremes = {};
  if (sorted.length > 1) {
    for (const col of measCols) {
      const vals = sorted.map((r) => col === 'loss' ? (r.cell.loss ?? 0) : r.cell[col]);
      const mn = Math.min(...vals);
      const mx = Math.max(...vals);
      if (mn !== mx) extremes[col] = { mn, mx };
    }
  }

  const colourCell = (col, val) => {
    const e = extremes[col];
    if (!e) return '';
    if (val === e.mn) return ` style="color:${LATENCY_BEST_COLOR}"`;
    if (val === e.mx) return ' style="color:red"';
    return '';
  };

  const body = sorted
    .map((r) => {
      const c = r.cell;
      return `<tr data-src="${esc(r.src.private_ip || '')}" data-dst="${esc(r.dst.private_ip || '')}"`
        + ` data-mode="${esc(r.key)}">`
        + `<td>${esc(r.key)}</td>`
        + `<td>${label(r.src)}</td><td>${label(r.dst)}</td>`
        + `<td>${vpc(r.src)}</td><td>${vpc(r.dst)}</td>`
        + `<td>${esc(r.src.role || '\u2014')}</td><td>${esc(r.dst.role || '\u2014')}</td>`
        + `<td>${esc(r.src.az || '\u2014')}</td><td>${esc(r.dst.az || '\u2014')}</td>`
        + `<td>${pg(r.src)}</td><td>${pg(r.dst)}</td>`
        + `<td${colourCell('p50', c.p50)}>${fmtLat(c.p50)}</td>`
        + `<td${colourCell('p90', c.p90)}>${fmtLat(c.p90)}</td>`
        + `<td${colourCell('p99', c.p99)}>${fmtLat(c.p99)}</td>`
        + `<td${colourCell('p999', c.p999)}>${fmtLat(c.p999)}</td>`
        + `<td${colourCell('max', c.max)}>${fmtLat(c.max)}</td>`
        + `<td${colourCell('loss', c.loss ?? 0)}>${esc(c.loss ?? 0)}%</td>`
        + `<td>${esc(stampTz(r.unix, tz))}</td></tr>`;
    })
    .join('');
  return `<table class="sortable" id="lat-table">${head}${body}</table>`;
}

function inventory(nodes) {
  let t = '<table class="inv sortable" id="inv-table"><tr><th>#</th><th>Private IP</th>'
    + '<th>Public IP</th><th>Role</th><th>VPC ID</th><th>AZ</th><th>PG</th><th>Type</th></tr>';
  nodes.forEach((n, i) => {
    const u = (v) => esc(v && v !== 'unknown' ? v : '\u2014');
    t += `<tr data-ip="${esc(n.private_ip || '')}"><td>${i}</td><td>${label(n)}</td>`
      + `<td>${esc(n.public_ip || '\u2014')}</td>`
      + `<td class="role-${esc(n.role || '')}">${esc(n.role || '\u2014')}</td>`
      + `<td>${u(n.vpc_id)}</td><td>${esc(n.az || '\u2014')}</td><td>${u(n.cpg_name)}</td>`
      + `<td>${esc(n.type || '\u2014')}</td></tr>`;
  });
  return t + '</table>';
}

function ages(rows, tz) {
  if (!rows.length) return '';
  const us = rows.map((r) => r.unix).filter(Boolean);
  if (!us.length) return '';
  const newest = Math.max(...us), oldest = Math.min(...us);
  const stale = rows.filter((r) => r.unix && newest - r.unix > 300).length;
  return '<div class="coverage"><h3 style="margin:0 0 4px;font-size:13px;color:#58a6ff">Measurement ages</h3>'
    + `newest ${esc(stampTz(newest, tz))} \u00b7 oldest ${esc(stampTz(oldest, tz))}`
    + (stale ? ` \u00b7 <b>${stale}</b> measurement(s) more than 5 min older than the newest` : '')
    + '</div>';
}

/** The report stylesheet - extracted for reuse in the in-app view. */
export const REPORT_CSS = `
  body{background:#0d1117;color:#e6edf3;font-family:system-ui,-apple-system,sans-serif;padding:22px;margin:0}
  h1{font-size:19px;margin:0 0 4px}h2{font-size:15px;margin:22px 0 6px;color:#e6edf3}
  h3{font-size:13px}
  .meta{color:#8b949e;font-size:12px;margin-bottom:10px}
  .report-view table{border-collapse:collapse;margin:8px 0 4px;font-size:12px}
  .report-view th,.report-view td{border:1px solid #30363d;padding:3px 7px;text-align:right;white-space:nowrap}
  .report-view th{background:#161b22;color:#8b949e;font-weight:600;cursor:pointer;user-select:none}
  .inv td,.inv th{text-align:left}
  .heat td{font-family:'SF Mono',monospace;font-weight:700;background:#0d1117}
  .heat th{font-family:'SF Mono',monospace}
  td.na{background:#161b22;color:#484f58;font-weight:400}
  #lat-table td{color:#e6edf3}
  #lat-table td:first-child,.inv td{text-align:left}
  .mode-badge{display:inline-block;margin-left:4px;padding:0 3px;border-radius:3px;
    background:#0d1117;color:#79c0ff;font:9px 'SF Mono',monospace;font-weight:700;vertical-align:top}
  #lat-table .mode-badge{background:#161b22}
  .coverage{font-size:12px;margin:10px 0;padding:9px 11px;background:#1c1810;color:#e3b341;
    border:1px solid #30363d;border-radius:6px}
  .warn{font-size:12px;margin:6px 0 2px;padding:8px 10px;background:#161b22;color:#adbac7;
    border:1px solid #30363d;border-left:3px solid #d29922;border-radius:6px}
  .method{font-size:12px;margin:6px 0 12px;padding:9px 11px;background:#161b22;
    border:1px solid #30363d;border-left:3px solid #58a6ff;border-radius:6px;color:#adbac7;line-height:1.6}
  .method summary{font-size:13px;color:#58a6ff;cursor:pointer;font-weight:600;list-style:none}
  .method summary::-webkit-details-marker{display:none}
  .method summary::before{content:'\u25b6';display:inline-block;margin-right:6px;font-size:10px}
  .method[open] summary::before{transform:rotate(90deg)}
  .method dt{color:#e6edf3;font-weight:600;margin-top:6px}.method dd{margin:0 0 0 14px}
  .method code{background:#0d1117;padding:1px 4px;border-radius:3px;color:#79c0ff}
  .metric-kind{font-size:13px;color:#e6edf3;margin:2px 0 6px}.metric-kind b{color:#f0883e}
  #lat-table th{position:relative;user-select:none}
  #lat-table th .rsz{position:absolute;top:0;right:0;width:6px;height:100%;cursor:col-resize}
  #lat-table th .rsz:hover{background:#58a6ff}
  #lat-table th.dragging{opacity:.55}
  #lat-table th.drop-target{box-shadow:inset 3px 0 0 #58a6ff}
  .selbar{font-size:12px;color:#8b949e;margin:10px 0 2px}
  .selbar button{background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:5px;
    padding:1px 7px;font-size:11px;cursor:pointer;margin-left:6px}
  .report-export-bar{display:flex;gap:8px;margin-bottom:12px}
  @media print{.report-export-bar{display:none !important}}
  .report-toolbar-btn{background:#21262d;color:#e6edf3;border:1px solid #30363d;
    border-radius:6px;padding:6px 14px;cursor:pointer;font:600 13px system-ui,-apple-system,sans-serif}
  .report-toolbar-btn:hover{background:#30363d;color:#fff}
  .inv tr.sel{background:#1f2937;outline:2px solid #d29922;outline-offset:-2px}
  .heat td.sel-row,.heat td.sel-col{outline:2px solid #d29922;outline-offset:-2px}
  .heat th.sel-row,.heat th.sel-col{background:#243b53;color:#e6edf3}
  #lat-table tr.sel-src{background:#132a3f}#lat-table tr.sel-dst{background:#12301c}
  #lat-table tr.sel-both{background:#3a2d10}
`;

/**
 * Build the report body HTML - everything between <body> and </body> except scripts.
 * Reusable both in the standalone document and the in-app overlay.
 * @param {Array<{kind:string,variation:string,unix?:number,fleet:object}>} views
 */
export function buildCombinedReportBody(views, tz) {
  const vs = (views || []).filter((v) => v && v.fleet && (v.fleet.nodes || []).length);
  const gen = new Date().toISOString();
  if (!vs.length) {
    return `<h1>Latency Report</h1><p>No measurements yet \u2014 run a campaign first.</p>`;
  }
  const nodes = vs[0].fleet.nodes;
  const region = vs[0].fleet.region || '?';
  const { rows, best } = collate(vs);
  const modeList = vs.map((v) => modeKey(v));
  const allP50 = rows.filter((r) => !isCrossRegion(r.src, r.dst))
    .map((r) => r.cell.p50).filter((v) => v != null && v > 0);
  allP50.sort((a, b) => a - b);
  const scale = {
    mn: allP50.length ? allP50[0] : 0,
    mx: allP50.length ? allP50[allP50.length - 1] : 1,
    gold: allP50.length ? allP50[Math.floor(allP50.length * 0.01)] : undefined,
  };

  const sections = vs.map((v) => `
  <h2>${esc(modeKey(v))}</h2>
  ${methodology(v)}
  ${modeHeatmap(v, scale)}`).join('\n');

  const delta = vs.length >= 2 ? (() => {
    const a = vs[vs.length - 1], b = vs[0];
    return `
  <h2>Delta \u2014 ${esc(modeKey(b))} minus ${esc(modeKey(a))}</h2>
  <div class="warn">Per-cell <b>p50 difference</b> on a diverging scale centred on zero. Cells
  missing either mode are hatched rather than coloured, so an unmeasured pair cannot read as
  "no change".</div>
  ${buildCompareHTML(nodes, a.fleet.matrix, b.fleet.matrix)}`;
  })() : '';

  const kindLabel = vs[0].kind === 'mcast' ? 'multicast' : 'unicast';
  const title = `Latency Report - ${kindLabel}`;
  const kindsAttr = [...new Set(vs.map((v) => (v.kind === 'mcast' ? 'multicast' : 'unicast')))].join('-');
  return `<div class="report-export-bar" data-report-kinds="${esc(kindsAttr)}"><button data-print-btn class="report-toolbar-btn">Save as PDF</button><button data-xls-btn class="report-toolbar-btn">Save as XLS</button></div>
  <h1>${esc(title)}</h1>
  <div class="meta">Region: ${esc(region)} \u00b7 Nodes: ${nodes.length} \u00b7 Modes: ${esc(modeList.join(', '))} \u00b7 Measurements: ${rows.length} \u00b7 Timezone: ${esc(tzLabel(tz))} \u00b7 Generated: ${esc(gen)}</div>
  ${ages(rows, tz)}

  <div class="selbar"><span id="selinfo">Click an IP anywhere to highlight that instance everywhere.</span><button id="selclear">Clear</button></div>
  <h2>Latest measurements</h2>
  ${overviewGrid(nodes, best, scale, tz)}

  <h2>Fleet inventory</h2>
  ${inventory(nodes)}
  ${sections}
  ${delta}

  <h2>All measurements</h2>
  ${latencyTable(rows, tz)}`;
}

/**
 * Self-contained interaction handler for sorting and cross-table IP selection.
 * Scoped to `root` - uses root.querySelectorAll, not document.querySelectorAll.
 * No closure over module scope, no imports - safe to serialise with .toString().
 */
export function reportInteractions(root) {
  // Sorting: unit-aware.
  function sortKey(s) {
    var u = s.match(/^([\d.]+)\s*(ms|s|\u00b5s|\u03bcs)$/);
    if (u) { var v = parseFloat(u[1]); return u[2] === 's' ? v*1e6 : u[2] === 'ms' ? v*1e3 : v; }
    var p = s.match(/^([\d.]+)%$/); if (p) return parseFloat(p[1]);
    if (/^\d+(\.\d+)?$/.test(s)) return parseFloat(s);
    return NaN;
  }
  root.querySelectorAll('table.sortable').forEach(function(table) {
    var dir = {};
    table.querySelectorAll('tr:first-child th').forEach(function(th, col) {
      th.addEventListener('click', function() {
        var rows = Array.prototype.slice.call(table.querySelectorAll('tr'), 1);
        var asc = !(dir[col] = !dir[col]);
        rows.sort(function(a, b) {
          var x = (a.cells[col] || {}).textContent ? a.cells[col].textContent.trim() : '';
          var y = (b.cells[col] || {}).textContent ? b.cells[col].textContent.trim() : '';
          var nx = sortKey(x), ny = sortKey(y);
          var c = (!isNaN(nx) && !isNaN(ny)) ? nx - ny
            : x.localeCompare(y, undefined, { numeric: true });
          return asc ? c : -c;
        });
        rows.forEach(function(r) { table.appendChild(r); });
      });
    });
  });

  // Cross-table selection by instance IP, spanning every mode section.
  // All measurements: resizable + reorderable columns, both driven from the
  // header cells. Resize uses a grip on the right edge so it never competes with
  // the sort click; reorder is a drag of the header itself.
  var lat = root.querySelector('#lat-table');
  if (lat) {
    var headRow = lat.querySelector('tr');
    var moveColumn = function (from, to) {
      if (from === to) return;
      var rows = lat.querySelectorAll('tr');
      for (var r = 0; r < rows.length; r++) {
        var cells = [].slice.call(rows[r].children);
        if (!cells[from] || !cells[to]) continue;
        rows[r].insertBefore(cells[from], to > from ? cells[to].nextSibling : cells[to]);
      }
    };
    [].slice.call(headRow.querySelectorAll('th')).forEach(function (th) {
      var grip = document.createElement('span');
      grip.className = 'rsz';
      grip.title = 'Drag to resize';
      th.appendChild(grip);
      grip.addEventListener('click', function (e) { e.stopPropagation(); });
      grip.addEventListener('mousedown', function (e) {
        e.preventDefault(); e.stopPropagation();
        var x0 = e.clientX, w0 = th.getBoundingClientRect().width;
        var move = function (ev) {
          var w = Math.max(28, w0 + ev.clientX - x0);
          th.style.width = w + 'px'; th.style.minWidth = w + 'px'; th.style.maxWidth = w + 'px';
        };
        var up = function () {
          document.removeEventListener('mousemove', move);
          document.removeEventListener('mouseup', up);
        };
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', up);
      });

      th.draggable = true;
      th.addEventListener('dragstart', function (e) {
        e.dataTransfer.setData('text/plain', String([].slice.call(headRow.children).indexOf(th)));
        e.dataTransfer.effectAllowed = 'move';
        th.classList.add('dragging');
      });
      th.addEventListener('dragend', function () {
        th.classList.remove('dragging');
        [].slice.call(headRow.children).forEach(function (h) { h.classList.remove('drop-target'); });
      });
      th.addEventListener('dragover', function (e) { e.preventDefault(); th.classList.add('drop-target'); });
      th.addEventListener('dragleave', function () { th.classList.remove('drop-target'); });
      th.addEventListener('drop', function (e) {
        e.preventDefault();
        th.classList.remove('drop-target');
        var from = parseInt(e.dataTransfer.getData('text/plain'), 10);
        var to = [].slice.call(headRow.children).indexOf(th);
        if (!isNaN(from)) moveColumn(from, to);
      });
    });
  }

  var sel = new Set();
  function paint() {
    root.querySelectorAll('#inv-table tr[data-ip]').forEach(function(tr) {
      tr.classList.toggle('sel', sel.has(tr.dataset.ip));
    });
    root.querySelectorAll('table.heat td, table.heat th').forEach(function(el) {
      el.classList.toggle('sel-row', !!el.dataset.rowIp && sel.has(el.dataset.rowIp));
      el.classList.toggle('sel-col', !!el.dataset.colIp && sel.has(el.dataset.colIp));
    });
    root.querySelectorAll('#lat-table tr[data-src]').forEach(function(tr) {
      var s = sel.has(tr.dataset.src), d = sel.has(tr.dataset.dst);
      tr.classList.toggle('sel-both', s && d);
      tr.classList.toggle('sel-src', s && !d);
      tr.classList.toggle('sel-dst', d && !s);
    });
    var info = root.querySelector('#selinfo');
    if (info) {
      info.textContent = sel.size
        ? sel.size + ' instance' + (sel.size > 1 ? 's' : '') + ' selected: ' + Array.from(sel).join(', ')
        : 'Click an IP anywhere to highlight that instance everywhere.';
    }
  }
  var toggle = function(ip) { if (!ip) return; sel.has(ip) ? sel.delete(ip) : sel.add(ip); paint(); };
  root.querySelectorAll('#inv-table tr[data-ip]').forEach(function(tr) {
    tr.style.cursor = 'pointer';
    tr.addEventListener('click', function() { toggle(tr.dataset.ip); });
  });
  root.querySelectorAll('table.heat th[data-row-ip], table.heat th[data-col-ip]').forEach(function(th) {
    th.style.cursor = 'pointer';
    th.addEventListener('click', function() { toggle(th.dataset.rowIp || th.dataset.colIp); });
  });
  root.querySelectorAll('#lat-table tr[data-src]').forEach(function(tr) {
    tr.style.cursor = 'pointer';
    tr.addEventListener('click', function(ev) {
      toggle(ev.target.cellIndex === 2 ? tr.dataset.dst : tr.dataset.src);
    });
  });
  var clearBtn = root.querySelector('#selclear');
  if (clearBtn) clearBtn.addEventListener('click', function() { sel.clear(); paint(); });
  paint();

  // ── Save as PDF: triggers the browser print dialog ──────────────────────────
  var printBtn = root.querySelector('[data-print-btn]');
  if (printBtn) printBtn.addEventListener('click', function() {
    var hook = (typeof window !== 'undefined') && window.__afxdpPrintReport;
    if (typeof hook === 'function') hook();
    else window.print();
  });

  // ── Save as XLS: single SpreadsheetML 2003 workbook, one sheet per table ───
  function xmlEsc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  function sanitizeSheetName(s) {
    return s.replace(/[:\\/?\*\[\]]/g, '').slice(0, 31) || 'Sheet';
  }
  function findHeadingForTable(table) {
    var prev = table.previousElementSibling;
    while (prev) {
      if (/^H[1-6]$/.test(prev.tagName)) return prev.textContent.trim();
      prev = prev.previousElementSibling;
    }
    return '';
  }
  function tableToWorksheet(table, name) {
    var rows = [].slice.call(table.querySelectorAll('tr'));
    var xml = '<Worksheet ss:Name="' + xmlEsc(sanitizeSheetName(name)) + '"><Table>';
    for (var r = 0; r < rows.length; r++) {
      xml += '<Row>';
      var cells = [].slice.call(rows[r].querySelectorAll('th, td'));
      for (var c = 0; c < cells.length; c++) {
        var cc = cells[c].cloneNode(true);
        var badges = cc.querySelectorAll('.mode-badge');
        for (var bi = 0; bi < badges.length; bi++) badges[bi].remove();
        var txt = cc.textContent.trim();
        var isNum = /^-?\d+(\.\d+)?$/.test(txt);
        var type = isNum ? 'Number' : 'String';
        var val = isNum ? txt : xmlEsc(txt);
        xml += '<Cell><Data ss:Type="' + type + '">' + val + '</Data></Cell>';
      }
      xml += '</Row>';
    }
    xml += '</Table></Worksheet>';
    return xml;
  }

  var xlsBtn = root.querySelector('[data-xls-btn]');
  if (xlsBtn) xlsBtn.addEventListener('click', function() {
    var tables = [].slice.call(root.querySelectorAll('table'));
    var usedNames = {};
    var xml = '<?xml version="1.0"?><?mso-application progid="Excel.Sheet"?>'
      + '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet"'
      + ' xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">';
    for (var i = 0; i < tables.length; i++) {
      var heading = findHeadingForTable(tables[i]);
      var name = sanitizeSheetName(heading || ('Sheet' + (i + 1)));
      if (usedNames[name]) { name = name.slice(0, 28) + (i + 1); }
      usedNames[name] = true;
      xml += tableToWorksheet(tables[i], name);
    }
    xml += '</Workbook>';
    var blob = new Blob([xml], { type: 'application/vnd.ms-excel' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    var bar = root.querySelector('[data-report-kinds]');
    var kinds = (bar && bar.getAttribute('data-report-kinds')) || 'report';
    var d = new Date(), pad = function (n) { return (n < 10 ? '0' : '') + n; };
    var stamp = d.getFullYear() + pad(d.getMonth() + 1) + pad(d.getDate())
      + '-' + pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds());
    a.download = 'latency-report-' + kinds + '-' + stamp + '.xls';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  });
}

/**
 * Build the combined report as a self-contained HTML document.
 * @param {Array<{kind:string,variation:string,unix?:number,fleet:object}>} views
 */
export function buildCombinedReportHTML(views, tz) {
  const vs = (views || []).filter((v) => v && v.fleet && (v.fleet.nodes || []).length);
  if (!vs.length) {
    return `<!doctype html><html><head><meta charset="utf-8"><title>Latency Report</title></head>`
      + `<body style="background:#0d1117;color:#e6edf3;font-family:system-ui;padding:24px">`
      + `<h1>Latency Report</h1><p>No measurements yet \u2014 run a campaign first.</p></body></html>`;
  }

  const body = buildCombinedReportBody(views, tz);
  const kindLabel = vs[0].kind === 'mcast' ? 'multicast' : 'unicast';
  const docTitle = `Latency Report - ${kindLabel}`;

  return `<!doctype html><html><head><meta charset="utf-8">
  <title>${docTitle}</title>
  <style>${REPORT_CSS}</style></head><body class="report-view">
  ${body}
  <script>(${reportInteractions.toString()})(document);</script></body></html>`;
}
