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
//
// Data source: every table here is built from FLAT measurement rows fetched
// from the backend's SQLite store (GET /api/measurements, GET
// /api/mcast-replicators) — not from the live in-memory fleet.matrix. The
// live matrix is keyed only by (kind,variation,src,dst) and can therefore
// only ever hold ONE value per pair: with a multi-replicator mcast sweep, the
// last replicator to measure a destination silently overwrites every earlier
// replicator's result in that model. The store keeps every replicator's
// numbers (each swept (replicator,mode) combination is its own `runs` row),
// so reading from there is what makes every table - not just the dedicated
// "Per-replicator paths" section - correctly show all src/replicator/dst path
// variations. See dev/roadmap/mcast-replicator-selection.md for background,
// and App.svelte for the fetch/cache/refresh wiring (auto-refetch every 180s
// while the report is open, plus an explicit Refresh button).

import { fmtLat, cellColor, isCrossRegion, esc, LATENCY_BEST_COLOR } from './2d/palette.js';
import { buildCompareHTML } from './report.js';

/** Short per-mode badge: K/X for ucast kernel/xdp, C/I/BT/MK for mcast fwd modes. */
export const MODE_BADGE = {
  'ucast/kernel': 'K',
  'ucast/xdp': 'X',
  'mcast/copy': 'C',
  'mcast/inplace': 'I',
  'mcast/bpf_tx': 'BT',
  // 'MK' (not 'K') so it's visually distinct from ucast/kernel's badge in any
  // mixed ucast+mcast view — both are plain-kernel-socket baselines but for
  // different kinds.
  'mcast/kernel': 'MK',
};

const modeKey = (kind, variation) => `${kind}/${variation}`;
const badgeOf = (key) => MODE_BADGE[key] || key.split('/')[1].slice(0, 2).toUpperCase();
/** Node/endpoint label from a flat measurement row's IP - no node-index model needed. */
const ipLabel = (ip) => esc(ip || '?');
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
 * Normalize the two backend endpoints' rows into one flat, uniform shape:
 * { key, kind, variation, replicatorIp, replicatorPg, replicatorAz, src, dst,
 *   cell:{p50,p90,p99,p999,max,loss}, unix }.
 *
 * measurementRows: GET /api/measurements results (ucast + mcast; replicator_ip
 *   is "" for ucast). mcastReplicatorResults: GET /api/mcast-replicators
 *   results (mcast only; always has replicator identity, and independently
 *   covers cases where /api/measurements' time/limit window excluded a row
 *   /api/mcast-replicators still has via its run-id scoping) - merged in,
 *   de-duplicated by the same edge key so a row present in both is not
 *   double-counted.
 */
function collate(measurementRows, mcastReplicatorResults) {
  const rows = [];
  const best = new Map(); // "src|dst" -> { unix, key, cell } (mixes modes AND replicators - freshest wins)
  const seenKeys = new Set();

  const pushRow = (kind, variation, replicatorIp, replicatorPg, replicatorAz, replicatorVpc, srcIp, dstIp, cell, unix,
    srcRegion, dstRegion) => {
    const key = modeKey(kind, variation);
    const edgeKey = `${key}|${srcIp}|${dstIp}|${replicatorIp || ''}`;
    if (seenKeys.has(edgeKey)) return;
    seenKeys.add(edgeKey);
    const src = { private_ip: srcIp, region: srcRegion }, dst = { private_ip: dstIp, region: dstRegion };
    rows.push({ key, kind, variation, replicatorIp, replicatorPg, replicatorAz, replicatorVpc, src, dst, cell, unix });
    const ck = `${srcIp}|${dstIp}`;
    const prev = best.get(ck);
    if (!prev || unix >= prev.unix) best.set(ck, { unix, key, cell, src, dst });

    // A mcast row's real path is src -> replicator -> dst, not a direct
    // src -> dst line. When hop1/hop2 are present, also register the two
    // physical hops as their OWN cells (src|replicator using hop1,
    // replicator|dst using hop2) so the replicator's row/column in the grid
    // is populated instead of showing "no data" for every cell involving it -
    // the grid axis already includes the replicator (distinctNodeIPs), it
    // just never had anything to put in those cells before hop1/hop2 existed.
    if (replicatorIp && (cell.hop1_p50 != null || cell.hop2_p50 != null)) {
      const replRegion = undefined; // not tracked on the replicator; isCrossRegion degrades to false, matching prior behavior for these cells
      if (cell.hop1_p50 != null) {
        // p50/p99/p999 are on the wire (mcast_receive.cpp's hop1_us/hop2_us
        // shape) - p90/max are left null rather than fabricated, which
        // fmtLat renders as em-dash rather than a misleadingly precise number.
        const hop1Cell = { p50: cell.hop1_p50, p90: null, p99: cell.hop1_p99, p999: cell.hop1_p999, max: null, loss: cell.loss, isHop: true };
        const k1 = `${srcIp}|${replicatorIp}`;
        const prev1 = best.get(k1);
        if (!prev1 || unix >= prev1.unix) {
          best.set(k1, { unix, key, cell: hop1Cell, src, dst: { private_ip: replicatorIp, region: replRegion } });
        }
      }
      if (cell.hop2_p50 != null) {
        const hop2Cell = { p50: cell.hop2_p50, p90: null, p99: cell.hop2_p99, p999: cell.hop2_p999, max: null, loss: cell.loss, isHop: true };
        const k2 = `${replicatorIp}|${dstIp}`;
        const prev2 = best.get(k2);
        if (!prev2 || unix >= prev2.unix) {
          best.set(k2, { unix, key, cell: hop2Cell, src: { private_ip: replicatorIp, region: replRegion }, dst });
        }
      }
    }
  };

  for (const r of measurementRows || []) {
    const cell = { p50: r.p50, p90: r.p90, p99: r.p99, p999: r.p999, max: r.max, min: r.min, loss: r.loss_pct ?? 0,
      src_role: r.src_role, dst_role: r.dst_role, src_az: r.src_az, dst_az: r.dst_az,
      src_vpc: r.src_vpc, dst_vpc: r.dst_vpc, src_pg: r.src_pg, dst_pg: r.dst_pg,
      src_tenancy: r.src_tenancy, dst_tenancy: r.dst_tenancy,
      hop1_p50: r.hop1_p50, hop1_p99: r.hop1_p99, hop1_p999: r.hop1_p999,
      hop2_p50: r.hop2_p50, hop2_p99: r.hop2_p99, hop2_p999: r.hop2_p999 };
    pushRow(r.kind, r.variation, r.replicator_ip, r.replicator_pg, r.replicator_az, r.replicator_vpc,
      r.src_ip, r.dst_ip, cell, r.unix, r.src_region, r.dst_region);
  }
  for (const r of mcastReplicatorResults || []) {
    const cell = { p50: r.p50, p90: r.p90, p99: r.p99, p999: r.p999, max: r.max, loss: r.loss_pct ?? 0,
      hop1_p50: r.hop1_p50, hop1_p99: r.hop1_p99, hop1_p999: r.hop1_p999,
      hop2_p50: r.hop2_p50, hop2_p99: r.hop2_p99, hop2_p999: r.hop2_p999 };
    pushRow('mcast', r.mode, r.replicator_ip, r.replicator_pg, r.replicator_az, r.replicator_vpc,
      r.src_ip, r.dst_ip, cell, r.unix);
  }
  return { rows, best };
}

/** Distinct node IPs across every row, for the inventory table + heatmap axes. */
function distinctNodeIPs(rows) {
  const ips = new Set();
  for (const r of rows) {
    ips.add(r.src.private_ip);
    ips.add(r.dst.private_ip);
    if (r.replicatorIp) ips.add(r.replicatorIp);
  }
  return [...ips].sort();
}

/** Region per IP, best-effort from whichever row mentions it as src/dst - for
 * isCrossRegion() calls that need a {region} object keyed by IP rather than
 * by row. */
function regionByIP(rows) {
  const out = new Map();
  for (const r of rows) {
    if (r.src.private_ip && r.src.region && !out.has(r.src.private_ip)) out.set(r.src.private_ip, r.src.region);
    if (r.dst.private_ip && r.dst.region && !out.has(r.dst.private_ip)) out.set(r.dst.private_ip, r.dst.region);
  }
  return out;
}

/** Mode-annotated overview: freshest value per cell, badged with its mode. */
function overviewGrid(nodeIPs, best, scale, tz) {
  const { mn, mx, gold } = scale;
  let h = '<table class="heat" id="overview-table"><tr><th>src \\ dst</th>'
    + nodeIPs.map((ip) => `<th data-col-ip="${esc(ip)}">${ipLabel(ip)}</th>`).join('')
    + '</tr>';
  nodeIPs.forEach((rip) => {
    h += `<tr data-ip="${esc(rip)}"><th data-row-ip="${esc(rip)}">${ipLabel(rip)}</th>`;
    nodeIPs.forEach((cip) => {
      const dat = ` data-row-ip="${esc(rip)}" data-col-ip="${esc(cip)}"`;
      if (rip === cip) { h += `<td class="na"${dat}>\u00b7</td>`; return; }
      const b = best.get(`${rip}|${cip}`);
      if (!b) { h += `<td class="na"${dat}>\u00b7</td>`; return; }
      const hopNote = b.cell.isHop ? ' \u00b7 hop leg' : '';
      const tip = `${fmtLat(b.cell.p50)} \u00b7 ${b.key}${hopNote} \u00b7 ${stampTz(b.unix, tz)}`;
      h += `<td${dat} style="color:${cellColor(b.cell.p50, mn, mx, isCrossRegion(b.src, b.dst), gold)};font-weight:700"`
        + ` title="${esc(tip)}">${fmtLat(b.cell.p50)}`
        + `<span class="mode-badge">${badgeOf(b.key)}</span></td>`;
    });
    h += '</tr>';
  });
  return h + '</table>';
}

/**
 * Per-mode heatmap, so colour remains comparable inside that mode.
 * modeRows: rows already filtered to this (kind,variation). When more than
 * one replicator measured the same destination for this mode, the direct
 * (src,dst) cell still shows only one value - that is a real display limit
 * of an NxN grid, not a data-loss bug: every replicator's number is still
 * present in "All measurements" and (for mcast) "Per-replicator paths" below.
 * Picks the freshest of the candidates for the grid cell and says so in the
 * tooltip when more than one replicator contributed.
 *
 * For mcast rows with hop1/hop2 present, ALSO populates the physical-path
 * cells (src,replicator) from hop1 and (replicator,dst) from hop2 - the
 * replicator is a real node on the grid's axis (distinctNodeIPs includes it)
 * but previously had nothing in its row/column, since only the combined
 * end-to-end (src,dst) number was ever recorded there.
 */
function modeHeatmap(kind, variation, modeRows, nodeIPs, scale) {
  const key = modeKey(kind, variation);
  const { mn, mx, gold } = scale;
  // freshest cell per (src,dst), counting how many distinct replicators fed it
  const cellOf = new Map();
  const setIfFresher = (ck, entry) => {
    const prev = cellOf.get(ck);
    if (!prev || entry.unix >= prev.unix) cellOf.set(ck, entry);
  };
  for (const r of modeRows) {
    const ck = `${r.src.private_ip}|${r.dst.private_ip}`;
    const prev = cellOf.get(ck);
    if (!prev) cellOf.set(ck, { cell: r.cell, unix: r.unix, src: r.src, dst: r.dst, replicators: new Set([r.replicatorIp || '']) });
    else {
      prev.replicators.add(r.replicatorIp || '');
      if (r.unix >= prev.unix) { prev.cell = r.cell; prev.unix = r.unix; }
    }

    if (r.replicatorIp && (r.cell.hop1_p50 != null || r.cell.hop2_p50 != null)) {
      const replEndpoint = { private_ip: r.replicatorIp };
      if (r.cell.hop1_p50 != null) {
        setIfFresher(`${r.src.private_ip}|${r.replicatorIp}`, {
          cell: { p50: r.cell.hop1_p50, p90: null, p99: r.cell.hop1_p99, p999: null, max: null, loss: r.cell.loss, isHop: true },
          unix: r.unix, src: r.src, dst: replEndpoint, replicators: new Set([r.replicatorIp]),
        });
      }
      if (r.cell.hop2_p50 != null) {
        setIfFresher(`${r.replicatorIp}|${r.dst.private_ip}`, {
          cell: { p50: r.cell.hop2_p50, p90: null, p99: r.cell.hop2_p99, p999: null, max: null, loss: r.cell.loss, isHop: true },
          unix: r.unix, src: replEndpoint, dst: r.dst, replicators: new Set([r.replicatorIp]),
        });
      }
    }
  }
  let h = `<table class="heat" data-mode="${esc(key)}"><tr><th>src \\ dst</th>`
    + nodeIPs.map((ip) => `<th data-col-ip="${esc(ip)}">${ipLabel(ip)}</th>`).join('')
    + '</tr>';
  nodeIPs.forEach((rip) => {
    h += `<tr data-ip="${esc(rip)}"><th data-row-ip="${esc(rip)}">${ipLabel(rip)}</th>`;
    nodeIPs.forEach((cip) => {
      const dat = ` data-row-ip="${esc(rip)}" data-col-ip="${esc(cip)}"`;
      const c = cellOf.get(`${rip}|${cip}`);
      if (!c) { h += `<td class="na"${dat}>\u00b7</td>`; return; }
      const multi = c.replicators.size > 1 ? ` (${c.replicators.size} replicator paths - see All measurements / Per-replicator paths)` : '';
      const hopNote = c.cell.isHop ? ' \u00b7 hop leg (see Per-replicator paths for the full one-way path)' : '';
      h += `<td${dat} style="color:${cellColor(c.cell.p50, mn, mx, isCrossRegion(c.src, c.dst), gold)};font-weight:700"`
        + ` title="${esc(fmtLat(c.cell.p50) + ' \u00b7 ' + key + hopNote + multi)}">${fmtLat(c.cell.p50)}</td>`;
    });
    h += '</tr>';
  });
  return h + '</table>';
}

/** Per-mode methodology. With both kinds in one document this cannot be global. */
function methodology(kind, variation) {
  const isMcast = kind === 'mcast';
  const metric = isMcast
    ? 'Reported value is a <b>ONE-WAY</b> delay: source \u2192 replicator \u2192 destination. It is not a round trip, and is not comparable with the ucast RTT figures.'
    : 'Reported value is a <b>ROUND-TRIP TIME</b> (RTT) through the remote replicator\u2019s echo, at queue depth 1.';
  const detail = isMcast
    ? `<dt>Clock</dt><dd><code>CLOCK_REALTIME</code> on all three nodes \u2014 necessarily, since a one-way delay spans hosts. chrony disciplines each node to the <b>ENA PHC hardware clock</b> (<code>refclock PHC /dev/ptp0</code>); AWS Time Sync is the fallback. Observed RMS offset is tens of nanoseconds.</dd>
       <dt>Stamps</dt><dd><code>ts_ns</code> at the source before TX ring submit, <code>replicator_ns</code> at replicator RX, <code>rx_ns</code> at destination RX. One-way = <code>rx_ns \u2212 ts_ns</code>.</dd>
       <dt>Fwd mode</dt><dd><code>${esc(variation)}</code>. <code>XDP_TX</code> (<code>xdp_tx</code>) is a single-destination passthrough, not a fan-out.</dd>`
    : `<dt>Clock</dt><dd>A single <code>CLOCK_REALTIME</code> domain on one host, so <b>no inter-node clock sync is required</b> and none of its error enters the result. No TSC and no PHC are used for RTT.</dd>
       <dt>Stamps</dt><dd>TX <code>CLOCK_REALTIME</code> immediately before the send; RX a kernel software timestamp recorded in the NAPI receive path, before the socket queue.</dd>
       <dt>Variation</dt><dd><code>${esc(variation)}</code>. <code>--xdp-rx</code> is instrumented kernel RX, NOT a bypass receive.</dd>`;
  return `<div class="metric-kind">${metric}</div>
  <details class="method"><summary>How this was measured</summary><dl>${detail}
    <dt>Statistic</dt><dd>Service-time RTT excludes coordinated omission; warmup datagrams are discarded and percentiles derive only from datagrams that arrived. A run over the loss ceiling is rejected rather than published.</dd>
  </dl></details>`;
}

/** Combined latency table: every mode AND every replicator path, one table.
 * Column set adapts to what's actually in `rows`: replicator/hop1/hop2 are
 * mcast-only data (always empty for ucast) and are dropped entirely when the
 * report has no mcast rows, rather than shown as a wall of em-dashes. */
function latencyTable(rows, tz) {
  const hasMcast = rows.some((r) => r.kind === 'mcast');
  const head = '<tr><th>mode</th>'
    + (hasMcast ? '<th>replicator</th>' : '')
    + '<th>src IP</th><th>dst IP</th><th>src VPC</th><th>dst VPC</th>'
    + '<th>src role</th><th>dst role</th><th>src AZ</th><th>dst AZ</th><th>src PG</th><th>dst PG</th>'
    + '<th>min</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th>'
    + (hasMcast ? '<th>hop1 p50</th><th>hop1 p99</th><th>hop1 p99.9</th><th>hop2 p50</th><th>hop2 p99</th><th>hop2 p99.9</th>' : '')
    + '<th>measured</th></tr>';
  const val = (v) => esc(v && v !== 'unknown' ? v : '\u2014');
  const sorted = rows
    .slice()
    .sort((a, b) => a.key.localeCompare(b.key)
      || (a.replicatorIp || '').localeCompare(b.replicatorIp || '')
      || (a.src.private_ip || '').localeCompare(b.src.private_ip || ''));

  // Compute best/worst per measurement column for colouring
  const measCols = ['min', 'p50', 'p90', 'p99', 'p999', 'max', 'loss'];
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

  // hop1/hop2 are mcast-only (source->replicator / replicator->destination
  // legs of the one-way path) and only present when the underlying telemetry
  // carried a replicator timestamp - '\u2014' otherwise (including every
  // ucast row, which has no replicator hop at all).
  const hop = (v) => (v == null ? '\u2014' : fmtLat(v));

  const body = sorted
    .map((r) => {
      const c = r.cell;
      // No PG badge here - src PG / dst PG are their own dedicated columns
      // already; repeating the replicator's PG inline is redundant.
      const replCell = r.replicatorIp ? esc(r.replicatorIp) : '\u2014';
      return `<tr data-src="${esc(r.src.private_ip || '')}" data-dst="${esc(r.dst.private_ip || '')}"`
        + ` data-mode="${esc(r.key)}"${r.replicatorIp ? ` data-replicator="${esc(r.replicatorIp)}"` : ''}>`
        + `<td>${esc(r.key)}</td>`
        + (hasMcast ? `<td>${replCell}</td>` : '')
        + `<td>${ipLabel(r.src.private_ip)}</td><td>${ipLabel(r.dst.private_ip)}</td>`
        + `<td>${val(c.src_vpc)}</td><td>${val(c.dst_vpc)}</td>`
        + `<td>${val(c.src_role)}</td><td>${val(c.dst_role)}</td>`
        + `<td>${val(c.src_az)}</td><td>${val(c.dst_az)}</td>`
        + `<td>${val(c.src_pg)}</td><td>${val(c.dst_pg)}</td>`
        + `<td${colourCell('min', c.min)}>${fmtLat(c.min)}</td>`
        + `<td${colourCell('p50', c.p50)}>${fmtLat(c.p50)}</td>`
        + `<td${colourCell('p90', c.p90)}>${fmtLat(c.p90)}</td>`
        + `<td${colourCell('p99', c.p99)}>${fmtLat(c.p99)}</td>`
        + `<td${colourCell('p999', c.p999)}>${fmtLat(c.p999)}</td>`
        + `<td${colourCell('max', c.max)}>${fmtLat(c.max)}</td>`
        + `<td${colourCell('loss', c.loss ?? 0)}>${esc(c.loss ?? 0)}%</td>`
        + (hasMcast ? `<td>${hop(c.hop1_p50)}</td><td>${hop(c.hop1_p99)}</td><td>${hop(c.hop1_p999)}</td>`
            + `<td>${hop(c.hop2_p50)}</td><td>${hop(c.hop2_p99)}</td><td>${hop(c.hop2_p999)}</td>` : '')
        + `<td>${esc(stampTz(r.unix, tz))}</td></tr>`;
    })
    .join('');
  return `<table class="sortable" id="lat-table">${head}${body}</table>`;
}

/** Per-replicator mcast paths: one row per (replicator, mode, destination),
 * so a fleet with multiple replicator placements (different PG/AZ) shows every
 * path's latency side by side. Redundant with the "replicator" column now in
 * "All measurements" above, but kept as a focused, mcast-only, pre-sorted view
 * - useful when scanning specifically for placement differences without the
 * ucast rows/extra columns in the way. */
function replicatorPathsSection(results, tz) {
  if (!results || !results.length) return '';
  const byReplicator = new Set(results.map((r) => r.replicator_ip || r.replicator_id || 'unknown'));
  if (byReplicator.size <= 1) return ''; // nothing to compare with a single replicator
  const head = '<tr><th>replicator</th><th>replicator PG</th><th>replicator AZ</th><th>mode</th>'
    + '<th>dst IP</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th>'
    + '<th>hop1 p50</th><th>hop1 p99</th><th>hop1 p99.9</th><th>hop2 p50</th><th>hop2 p99</th><th>hop2 p99.9</th><th>measured</th></tr>';
  const sorted = results.slice().sort((a, b) =>
    (a.replicator_ip || '').localeCompare(b.replicator_ip || '')
    || (a.mode || '').localeCompare(b.mode || '')
    || (a.dst_ip || '').localeCompare(b.dst_ip || ''));
  // Present only when the underlying telemetry carried a replicator
  // timestamp (mcast_receive.cpp's has_replicator_ts) - '\u2014' otherwise.
  const hop = (v) => (v == null ? '\u2014' : fmtLat(v));
  const body = sorted.map((r) => {
    const repl = esc(r.replicator_ip || r.replicator_id || '\u2014');
    return `<tr data-replicator="${repl}">`
      + `<td>${repl}</td><td>${esc(r.replicator_pg || '\u2014')}</td><td>${esc(r.replicator_az || '\u2014')}</td>`
      + `<td>mcast/${esc(r.mode || '\u2014')}</td><td>${esc(r.dst_ip || '\u2014')}</td>`
      + `<td>${fmtLat(r.p50)}</td><td>${fmtLat(r.p90)}</td><td>${fmtLat(r.p99)}</td>`
      + `<td>${fmtLat(r.p999)}</td><td>${fmtLat(r.max)}</td><td>${esc(r.loss_pct ?? 0)}%</td>`
      + `<td>${hop(r.hop1_p50)}</td><td>${hop(r.hop1_p99)}</td><td>${hop(r.hop1_p999)}</td>`
      + `<td>${hop(r.hop2_p50)}</td><td>${hop(r.hop2_p99)}</td><td>${hop(r.hop2_p999)}</td>`
      + `<td>${esc(stampTz(r.unix, tz))}</td></tr>`;
  }).join('');
  return `<h2>Per-replicator paths (${byReplicator.size} replicators)</h2>
  <div class="warn">One-way latency measured through each replicator online during the mcast campaign(s) below,
  so placement (PG/AZ) differences between replicator paths are directly comparable. This campaign automatically
  swept every online replicator; choosing a specific subset is a planned follow-up. <b>hop1</b> is the
  source&rarr;replicator leg and <b>hop2</b> is the replicator&rarr;destination leg of the same one-way path -
  present only when the underlying telemetry carried a replicator timestamp.</div>
  <table class="sortable" id="replicator-paths-table">${head}${body}</table>`;
}

/** Fleet inventory, built from the DISTINCT node IPs appearing in the
 * measurement rows plus (best-effort) topology from the same rows' src_ and dst_
 * fields. A node that never appears as a src or dst in this window (e.g. an
 * idle destination outside the report's time floor) will not appear here -
 * this is a report of measured nodes, not the full live fleet roster. */
function inventory(rows) {
  const topo = new Map(); // ip -> { role, az, vpc, pg, region, tenancy }
  for (const r of rows) {
    if (r.src.private_ip && !topo.has(r.src.private_ip)) {
      topo.set(r.src.private_ip, { role: r.cell.src_role, az: r.cell.src_az, vpc: r.cell.src_vpc, pg: r.cell.src_pg, region: r.src.region, tenancy: r.cell.src_tenancy });
    }
    if (r.dst.private_ip && !topo.has(r.dst.private_ip)) {
      topo.set(r.dst.private_ip, { role: r.cell.dst_role, az: r.cell.dst_az, vpc: r.cell.dst_vpc, pg: r.cell.dst_pg, region: r.dst.region, tenancy: r.cell.dst_tenancy });
    }
  }
  // A replicator is a third node in a mcast row (never itself the src/dst of
  // that row), so its VPC/AZ/PG were previously only recoverable if it
  // happened to also appear as src/dst in some other row in this report -
  // which fails for a mcast-only report (?report=mcast), where no such row
  // exists. The backend now threads the replicator's VPC through
  // runs.params (replicator_vpc, alongside replicator_pg/az already there),
  // so cell.replicatorVpc is authoritative and should be preferred; the
  // cross-row lookup below remains only as a fallback for older data.
  for (const r of rows) {
    if (r.replicatorIp && (!topo.has(r.replicatorIp) || (r.replicatorVpc && !topo.get(r.replicatorIp).vpc))) {
      // Region isn't tracked on the replicator identity (see collate()'s
      // pushRow) - fall back to whatever this row's src/dst region already
      // resolved, on the reasonable assumption a replicator sits in the same
      // region as the pair it serves. Tenancy has no such fallback (not
      // meaningfully inferable from a peer's tenancy) so it's left blank
      // unless the replicator itself appears as a src/dst elsewhere.
      const region = topo.get(r.replicatorIp)?.region || r.src.region || r.dst.region;
      const tenancy = topo.get(r.replicatorIp)?.tenancy;
      topo.set(r.replicatorIp, { role: 'replicator', az: r.replicatorAz, vpc: r.replicatorVpc || '', pg: r.replicatorPg, region, tenancy });
    }
  }
  const ips = [...topo.keys()].sort();
  const u = (v) => esc(v && v !== 'unknown' ? v : '\u2014');
  let t = '<table class="inv sortable" id="inv-table"><tr><th>#</th><th>Private IP</th>'
    + '<th>Role</th><th>Region</th><th>VPC ID</th><th>AZ</th><th>PG</th><th>Tenancy</th></tr>';
  ips.forEach((ip, i) => {
    const t2 = topo.get(ip) || {};
    t += `<tr data-ip="${esc(ip)}"><td>${i}</td><td>${ipLabel(ip)}</td>`
      + `<td class="role-${esc(t2.role || '')}">${u(t2.role)}</td>`
      + `<td>${u(t2.region)}</td><td>${u(t2.vpc)}</td><td>${u(t2.az)}</td><td>${u(t2.pg)}</td><td>${u(t2.tenancy)}</td></tr>`;
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
 *
 * @param {Array<object>} measurementRows - GET /api/measurements results
 *   (flat rows: kind, variation, src_ip, dst_ip, replicator_ip/pg/az, p50..,
 *   src_role/az/vpc/pg, dst_role/az/vpc/pg, unix). Covers ucast + mcast.
 * @param {Array<object>} mcastReplicatorResults - GET /api/mcast-replicators
 *   results, for the "Per-replicator paths" section AND merged into every
 *   other table via collate() (see its docstring for why both are needed).
 * @param {string} tz
 * @param {{showRefresh?: boolean}} [opts] - showRefresh (default false) adds a
 *   "Refresh" button that dispatches a `afxdp-report-refresh` CustomEvent on
 *   `root` when clicked; only meaningful in the live app overlay (App.svelte
 *   listens for it and re-fetches). The downloaded standalone HTML has no
 *   backend to refresh against, so it omits the button by default.
 */
export function buildCombinedReportBody(measurementRows, mcastReplicatorResults, tz, opts) {
  const { showRefresh = false } = opts || {};
  const { rows, best } = collate(measurementRows, mcastReplicatorResults);
  const gen = new Date().toISOString();
  if (!rows.length) {
    return `<h1>Latency Report</h1><p>No measurements yet \u2014 run a campaign first.</p>`;
  }
  const nodeIPs = distinctNodeIPs(rows);
  const modeList = [...new Set(rows.map((r) => r.key))].sort();
  const allP50 = rows.map((r) => r.cell.p50).filter((v) => v != null && v > 0);
  allP50.sort((a, b) => a - b);
  const scale = {
    mn: allP50.length ? allP50[0] : 0,
    mx: allP50.length ? allP50[allP50.length - 1] : 1,
    gold: allP50.length ? allP50[Math.floor(allP50.length * 0.01)] : undefined,
  };

  const sections = modeList.map((key) => {
    const [kind, variation] = key.split('/');
    const modeRows = rows.filter((r) => r.key === key);
    return `
  <h2>${esc(key)}</h2>
  ${methodology(kind, variation)}
  ${modeHeatmap(kind, variation, modeRows, nodeIPs, scale)}`;
  }).join('\n');

  // Delta needs two node-indexed matrices (buildCompareHTML's shape); built
  // locally from the flat rows rather than touching report.js, which still
  // serves the separate single-mode report unchanged.
  const delta = modeList.length >= 2 ? (() => {
    const aKey = modeList[modeList.length - 1], bKey = modeList[0];
    const regions = regionByIP(rows);
    const idxNodes = nodeIPs.map((ip, i) => ({ index: i, private_ip: ip, region: regions.get(ip) }));
    const idx = new Map(idxNodes.map((n) => [n.private_ip, n.index]));
    const toMatrix = (key) => {
      const N = nodeIPs.length;
      const m = Array.from({ length: N }, () => Array(N).fill(null));
      for (const r of rows) {
        if (r.key !== key) continue;
        const i = idx.get(r.src.private_ip), j = idx.get(r.dst.private_ip);
        if (i == null || j == null) continue;
        // Freshest wins if more than one replicator fed this (src,dst) for this mode.
        if (!m[i][j] || (r.unix || 0) >= (m[i][j].unix || 0)) m[i][j] = { ...r.cell, unix: r.unix };
      }
      return m;
    };
    return `
  <h2>Delta \u2014 ${esc(bKey)} minus ${esc(aKey)}</h2>
  <div class="warn">Per-cell <b>p50 difference</b> on a diverging scale centred on zero. Cells
  missing either mode are hatched rather than coloured, so an unmeasured pair cannot read as
  "no change". When more than one replicator measured a pair, the freshest value is used.</div>
  ${buildCompareHTML(idxNodes, toMatrix(aKey), toMatrix(bKey))}`;
  })() : '';

  const firstKind = rows[0].kind;
  const kindLabel = firstKind === 'mcast' ? 'multicast' : 'unicast';
  const title = `Latency Report - ${kindLabel}`;
  const kindsAttr = [...new Set(rows.map((r) => (r.kind === 'mcast' ? 'multicast' : 'unicast')))].join('-');
  const replicatorPaths = replicatorPathsSection(mcastReplicatorResults, tz);
  const refreshBtn = showRefresh ? '<button data-refresh-btn class="report-toolbar-btn">\u21bb Refresh</button>' : '';
  const autoRefreshNote = showRefresh ? ' \u00b7 Auto-refreshes every 180s while open' : '';
  return `<div class="report-export-bar" data-report-kinds="${esc(kindsAttr)}"><button data-print-btn class="report-toolbar-btn">Save as PDF</button><button data-xls-btn class="report-toolbar-btn">Save as XLS</button>${refreshBtn}</div>
  <h1>${esc(title)}</h1>
  <div class="meta">Nodes: ${nodeIPs.length} \u00b7 Modes: ${esc(modeList.join(', '))} \u00b7 Measurements: ${rows.length} \u00b7 Timezone: ${esc(tzLabel(tz))} \u00b7 Generated: ${esc(gen)}${autoRefreshNote}</div>
  ${ages(rows, tz)}

  <div class="selbar"><span id="selinfo">Click an IP anywhere to highlight that instance everywhere.</span><button id="selclear">Clear</button></div>
  <h2>Latest measurements</h2>
  ${overviewGrid(nodeIPs, best, scale, tz)}

  <h2>Fleet inventory</h2>
  ${inventory(rows)}
  ${sections}
  ${delta}
  ${replicatorPaths}

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

  // ── Refresh: only present when showRefresh was passed to buildCombinedReportBody
  // (the live app overlay). Dispatches a CustomEvent instead of calling a
  // module-scope function directly, since this whole function is serialised
  // via Function.prototype.toString() for the standalone document and must not
  // close over anything outside its own body. App.svelte listens for this
  // event on the overlay root and re-fetches + re-renders.
  var refreshBtn = root.querySelector('[data-refresh-btn]');
  if (refreshBtn) refreshBtn.addEventListener('click', function() {
    root.dispatchEvent(new CustomEvent('afxdp-report-refresh', { bubbles: true }));
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
 * Build the combined report as a self-contained HTML document (standalone
 * download - no live backend to refresh against, so the Refresh button is
 * omitted; see buildCombinedReportBody's opts.showRefresh for the live overlay).
 * @param {Array<object>} measurementRows - GET /api/measurements results
 * @param {Array<object>} mcastReplicatorResults - GET /api/mcast-replicators results
 * @param {string} tz
 */
export function buildCombinedReportHTML(measurementRows, mcastReplicatorResults, tz) {
  if (!measurementRows || !measurementRows.length) {
    return `<!doctype html><html><head><meta charset="utf-8"><title>Latency Report</title></head>`
      + `<body style="background:#0d1117;color:#e6edf3;font-family:system-ui;padding:24px">`
      + `<h1>Latency Report</h1><p>No measurements yet \u2014 run a campaign first.</p></body></html>`;
  }

  const body = buildCombinedReportBody(measurementRows, mcastReplicatorResults, tz);
  const kindLabel = measurementRows[0].kind === 'mcast' ? 'multicast' : 'unicast';
  const docTitle = `Latency Report - ${kindLabel}`;

  return `<!doctype html><html><head><meta charset="utf-8">
  <title>${docTitle}</title>
  <style>${REPORT_CSS}</style></head><body class="report-view">
  ${body}
  <script>(${reportInteractions.toString()})(document);</script></body></html>`;
}
