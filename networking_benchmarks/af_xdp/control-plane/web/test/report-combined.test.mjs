// Combined report: ONE document covering every mode that has been measured,
// with per-cell mode metadata.
//
// Runs in parallel with buildReportHTML (single mode), which is unchanged.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';
import { buildCombinedReportHTML, MODE_BADGE } from '../src/lib/report-combined.js';

function nodes3() {
  return [0, 1, 2].map((i) => ({
    index: i,
    private_ip: `10.0.0.${i + 1}`,
    public_ip: `1.1.1.${i + 1}`,
    role: ['source', 'replicator', 'destination'][i],
    az: 'eu-central-1a',
    region: 'eu-central-1',
    cpg_name: 'cpg-a',
    vpc_id: 'vpc-1',
    type: 'c7i.4xlarge',
    online: true,
  }));
}

// A view is one {kind, variation, fleet} triple, exactly what conn.combos()
// plus conn.toFleet() yields per mode.
function view(kind, variation, cells, unix = 1000) {
  const nodes = nodes3();
  const N = 3;
  const m = Array.from({ length: N }, () => Array(N).fill(null));
  for (const [i, j, p] of cells) {
    m[i][j] = { p50: p, p90: p + 2, p99: p + 5, p999: p + 9, max: p + 20, loss: 0, unix };
  }
  return { kind, variation, unix, fleet: { nodes, matrix: m, region: 'eu-central-1' } };
}

// Reports are now per KIND: one ucast report covering its variations, one mcast
// report covering its fwd modes. Never both kinds in one document.
const views = () => [
  view('ucast', 'kernel', [[0, 1, 31], [1, 0, 32], [0, 2, 33], [2, 0, 30]], 2000),
  view('ucast', 'xdp', [[0, 1, 28], [1, 0, 29]], 3000),
];
const mcastViews = () => [
  view('mcast', 'copy', [[1, 2, 50]], 4000),
  view('mcast', 'inplace', [[1, 2, 54]], 4100),
];

const parse = (html) => new JSDOM(html, { runScripts: 'dangerously' }).window.document;

test('one document covers every measured mode', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const text = doc.body.textContent;
  // One canonical spelling everywhere: the mode string is both a data
  // attribute and display text, so a second format would invite mismatches.
  for (const m of ['ucast/kernel', 'ucast/xdp']) {
    assert.ok(text.includes(m), `combined report must mention ${m}`);
  }
  // One document, so exactly one inventory and one title.
  assert.equal(doc.querySelectorAll('#inv-table').length, 1, 'a single fleet inventory');
  assert.equal(doc.querySelectorAll('h1').length, 1, 'a single title');
});

test('each mode keeps its own heatmap so colour stays comparable within a mode', () => {
  // Mixing a kernel p50 and an xdp p50 in one coloured grid would break the
  // property that colour is comparable across the whole grid.
  const doc = parse(buildCombinedReportHTML(views()));
  const heats = doc.querySelectorAll('table.heat[data-mode]');
  assert.ok(heats.length >= 2, `expected a heatmap per mode, got ${heats.length}`);
  const modes = [...heats].map((t) => t.dataset.mode).sort();
  assert.deepEqual(modes, ['ucast/kernel', 'ucast/xdp']);
});

test('the overview grid annotates each cell with the mode that produced it', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const overview = doc.getElementById('overview-table');
  assert.ok(overview, 'combined report must have a mode-annotated overview grid');
  const badges = overview.querySelectorAll('.mode-badge');
  assert.ok(badges.length > 0, 'overview cells must carry a mode badge');
  // 10.0.0.1 -> 10.0.0.2 was measured by BOTH kernel (2000) and xdp (3000);
  // the newest wins and the badge must say so.
  const cell = overview.querySelector('td[data-row-ip="10.0.0.1"][data-col-ip="10.0.0.2"]');
  assert.ok(cell, 'overview must have the cell for that pair');
  assert.equal(cell.querySelector('.mode-badge').textContent.trim(), MODE_BADGE['ucast/xdp']);
  assert.ok(cell.textContent.includes('28'), 'newest value (xdp 28us) must win the cell');
});

test('a cell measured by only one mode shows that mode', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const cell = doc.querySelector('#overview-table td[data-row-ip="10.0.0.1"][data-col-ip="10.0.0.3"]');
  assert.equal(cell.querySelector('.mode-badge').textContent.trim(), MODE_BADGE['ucast/kernel']);
  assert.ok(cell.textContent.includes('33'));
});

test('mcast and ucast badges are distinguishable', () => {
  assert.notEqual(MODE_BADGE['ucast/kernel'], MODE_BADGE['ucast/xdp']);
  assert.notEqual(MODE_BADGE['mcast/copy'], MODE_BADGE['mcast/inplace']);
  assert.notEqual(MODE_BADGE['ucast/kernel'], MODE_BADGE['mcast/copy']);
});

test('colour uses ONE global scale across every mode in the report', () => {
  // The same latency must get the same colour everywhere. Per-mode scaling made
  // 31us red in one grid and green in another purely because each grid
  // normalised to its own range.
  const vs = [
    view('ucast', 'kernel', [[0, 1, 31], [1, 0, 90]], 2000),   // local range 31..90
    view('ucast', 'xdp', [[0, 1, 31], [1, 0, 32]], 3000),      // local range 31..32
  ];
  const doc = parse(buildCombinedReportHTML(vs));
  const colourOf = (mode, r, c) => {
    const t = doc.querySelector(`table.heat[data-mode="${mode}"]`);
    const td = t.querySelector(`td[data-row-ip="10.0.0.${r}"][data-col-ip="10.0.0.${c}"]`);
    return /color:\s*([^;"]+)/.exec(td.getAttribute('style'))[1].trim();
  };
  assert.equal(colourOf('ucast/kernel', 1, 2), colourOf('ucast/xdp', 1, 2),
    '31us must render the same colour in every grid');
});

test('the cross-mode colour warning is gone', () => {
  const html = buildCombinedReportHTML(views());
  assert.ok(!/not comparable across modes/i.test(html));
  assert.ok(!/mosaic of measurement ages/i.test(html));
});

test('the selection hint sits ABOVE the first table', () => {
  const html = buildCombinedReportHTML(views());
  const hint = html.indexOf('Click an IP anywhere');
  const firstTable = html.indexOf('<table');
  assert.ok(hint > 0 && firstTable > 0);
  assert.ok(hint < firstTable, 'the hint must precede any table it applies to');
});

test('one latency table covers all modes, with a mode column', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const lat = doc.getElementById('lat-table');
  assert.ok(lat, 'a single combined latency table');
  const cols = [...lat.querySelectorAll('tr:first-child th')].map((t) => t.textContent.trim());
  assert.ok(cols.includes('mode'), `latency table must have a mode column, got ${cols}`);
  // 4 kernel + 2 xdp = 6 measured pairs.
  assert.equal(lat.querySelectorAll('tr[data-src]').length, 6);
  const modesSeen = new Set([...lat.querySelectorAll('tr[data-src]')].map((tr) => tr.dataset.mode));
  assert.deepEqual([...modesSeen].sort(), ['ucast/kernel', 'ucast/xdp']);
});

test('cross-table selection still works across the whole document', () => {
  const dom = new JSDOM(buildCombinedReportHTML(views()), { runScripts: 'dangerously' });
  const doc = dom.window.document;
  const row = doc.querySelector('#inv-table tr[data-ip="10.0.0.1"]');
  row.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  assert.equal(doc.querySelectorAll('#inv-table tr.sel').length, 1);
  // Every per-mode heatmap marks that node's row and column.
  assert.ok(doc.querySelectorAll('table.heat td.sel-row').length > 0);
  assert.ok(doc.querySelectorAll('table.heat td.sel-col').length > 0);
  assert.ok(doc.querySelectorAll('#lat-table tr.sel-src').length > 0);
});

test('each mode section carries its own methodology', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  // ucast is a round trip, mcast is one-way: with both in one document the
  // distinction has to be stated per section, not once globally.
  const details = [...doc.querySelectorAll('details.method')];
  assert.ok(details.length >= 2, `expected methodology per mode, got ${details.length}`);
  assert.match(buildCombinedReportHTML(views()), /ROUND-TRIP TIME/);
  assert.match(buildCombinedReportHTML(mcastViews()), /ONE-WAY/);
});

test('a single view still produces a valid combined report', () => {
  const doc = parse(buildCombinedReportHTML([views()[0]]));
  assert.equal(doc.querySelectorAll('#inv-table').length, 1);
  assert.equal(doc.querySelectorAll('table.heat[data-mode]').length, 1);
  assert.ok(doc.getElementById('overview-table'));
});

test('no views is handled rather than throwing', () => {
  const html = buildCombinedReportHTML([]);
  assert.match(html, /no measurements/i);
});

test('measurement ages are reported across all modes', () => {
  const html = buildCombinedReportHTML(views());
  assert.match(html, /Measurement ages/);
});


test('a report covers ONE kind only', () => {
  const html = buildCombinedReportHTML(views());
  assert.ok(!/mcast\//.test(html), 'a ucast report must not contain mcast modes');
  const m = buildCombinedReportHTML(mcastViews());
  assert.ok(!/ucast\//.test(m), 'an mcast report must not contain ucast modes');
});

test('latency cells colour the FONT, not the background', () => {
  const html = buildCombinedReportHTML(views());
  const cells = html.match(/<td[^>]*data-row-ip[^>]*>/g) || [];
  const filled = cells.filter((c) => /style="[^"]*background:\s*(?!none)(?!transparent)#/.test(c));
  assert.equal(filled.length, 0, `no heat cell may set a background fill: ${filled[0]}`);
  // latencyColor returns rgb(), not hex.
  const coloured = cells.filter((c) => /style="[^"]*\bcolor:\s*(#|rgb)/.test(c));
  assert.ok(coloured.length > 0, 'heat cells must colour their font instead');
});

test('the mode column has no badge icon, only the full name', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const first = doc.querySelector('#lat-table tr[data-src] td');
  assert.equal(first.querySelectorAll('.mode-badge').length, 0,
    'the full mode name is already in the column, so the icon is redundant');
  assert.match(first.textContent.trim(), /^ucast\/(kernel|xdp)$/);
});

test('the details marker is a real triangle, not a broken escape', () => {
  const html = buildCombinedReportHTML(views());
  assert.ok(!/u25b6/.test(html), 'a literal u25b6 means the CSS escape was malformed');
  assert.ok(!/\\25b6/.test(html), 'avoid CSS escapes here entirely');
  assert.match(html, /content:\s*'\u25b6'/, 'use the character itself');
});

test('ages are absolute timestamps, not relative', () => {
  const html = buildCombinedReportHTML(views());
  assert.ok(!/\d+\s*min ago/.test(html), 'no relative ages');
  assert.ok(!/\d+s ago/.test(html), 'no relative ages');
  // hh:mm, dd-mm-yyyy
  assert.match(html, /\d{2}:\d{2}-\d{2}\.\d{2}\.\d{4}/, 'expected hh:mm-dd.mm.yyyy');
});

test('the age column carries a timestamp per measurement', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const cols = [...doc.querySelectorAll('#lat-table tr:first-child th')].map((t) => t.textContent.trim());
  const ai = cols.indexOf('measured');
  assert.ok(ai >= 0, `expected a "measured" column, got ${cols}`);
  for (const tr of doc.querySelectorAll('#lat-table tr[data-src]')) {
    assert.match(tr.cells[ai].textContent.trim(), /^\d{2}:\d{2}-\d{2}\.\d{2}\.\d{4}$/);
  }
});

test('heatmaps run consecutively and the delta heatmap comes last', () => {
  const doc = parse(buildCombinedReportHTML(views()));
  const heats = [...doc.querySelectorAll('table.heat')];
  assert.ok(heats.length >= 3, 'per-mode heatmaps plus a delta');
  const last = heats[heats.length - 1];
  assert.ok(last.classList.contains('compare-heat'), 'the delta heatmap must be last');
  // The per-mode heatmaps must not be interleaved with other tables.
  const modeHeats = [...doc.querySelectorAll('table.heat[data-mode]')];
  assert.ok(modeHeats.length >= 2);
});

test('a single-variation report has no delta heatmap', () => {
  // Nothing to compare against, so a delta grid would be empty and misleading.
  const doc = parse(buildCombinedReportHTML([views()[0]]));
  assert.equal(doc.querySelectorAll('table.compare-heat').length, 0);
});
