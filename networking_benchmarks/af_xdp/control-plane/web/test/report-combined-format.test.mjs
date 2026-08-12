// Tests for the format changes: title, headings, column order,
// click-target index, and best/worst colouring.
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

function view(kind, variation, cells, unix = 1000) {
  const nodes = nodes3();
  const N = 3;
  const m = Array.from({ length: N }, () => Array(N).fill(null));
  for (const [i, j, p] of cells) {
    m[i][j] = { p50: p, p90: p + 2, p99: p + 5, p999: p + 9, max: p + 20, loss: 0, unix };
  }
  return { kind, variation, unix, fleet: { nodes, matrix: m, region: 'eu-central-1' } };
}

const ucastViews = () => [
  view('ucast', 'kernel', [[0, 1, 31], [1, 0, 32], [0, 2, 33], [2, 0, 30]], 2000),
  view('ucast', 'xdp', [[0, 1, 28], [1, 0, 29]], 3000),
];
const mcastViews = () => [
  view('mcast', 'copy', [[1, 2, 50]], 4000),
  view('mcast', 'inplace', [[1, 2, 54]], 4100),
];

const parse = (html) => new JSDOM(html, { runScripts: 'dangerously' }).window.document;

// --- (1) Title and <h1> per kind ---

test('ucast report title and h1 say "Latency Report - unicast"', () => {
  const html = buildCombinedReportHTML(ucastViews());
  const doc = parse(html);
  assert.equal(doc.title, 'Latency Report - unicast');
  assert.equal(doc.querySelector('h1').textContent.trim(), 'Latency Report - unicast');
});

test('mcast report title and h1 say "Latency Report - multicast"', () => {
  const html = buildCombinedReportHTML(mcastViews());
  const doc = parse(html);
  assert.equal(doc.title, 'Latency Report - multicast');
  assert.equal(doc.querySelector('h1').textContent.trim(), 'Latency Report - multicast');
});

// --- (2) Combined latency heading ---

test('combined latency heading is exactly "All measurements"', () => {
  const doc = parse(buildCombinedReportHTML(ucastViews()));
  const headings = [...doc.querySelectorAll('h2')].map((h) => h.textContent.trim());
  assert.ok(headings.includes('All measurements'),
    `expected "All measurements" in: ${headings}`);
  assert.ok(!headings.some((h) => /every mode/i.test(h)),
    'must not contain the old "every mode" text');
});

// --- (3) Per-mode section headings drop metric type, but methodology keeps it ---

test('per-mode section heading is just the mode key, no metric type', () => {
  const doc = parse(buildCombinedReportHTML(ucastViews()));
  const headings = [...doc.querySelectorAll('h2')].map((h) => h.textContent.trim());
  // Must have "ucast/kernel" and "ucast/xdp" as plain headings
  assert.ok(headings.includes('ucast/kernel'), `missing "ucast/kernel": ${headings}`);
  assert.ok(headings.includes('ucast/xdp'), `missing "ucast/xdp": ${headings}`);
  // Must NOT have the trailing metric type
  assert.ok(!headings.some((h) => /ucast\/kernel.*round-trip/i.test(h)),
    'heading must not include round-trip');
  assert.ok(!headings.some((h) => /ucast\/xdp.*round-trip/i.test(h)),
    'heading must not include round-trip');
});

test('mcast section heading drops "one-way" but methodology still says ONE-WAY', () => {
  const html = buildCombinedReportHTML(mcastViews());
  const doc = parse(html);
  const headings = [...doc.querySelectorAll('h2')].map((h) => h.textContent.trim());
  assert.ok(headings.includes('mcast/copy'), `missing "mcast/copy": ${headings}`);
  assert.ok(!headings.some((h) => /mcast\/copy.*one-way/i.test(h)),
    'heading must not include one-way');
  // But the methodology still states ONE-WAY
  assert.match(html, /ONE-WAY/, 'methodology must still state ONE-WAY');
});

test('ucast methodology still states ROUND-TRIP TIME', () => {
  const html = buildCombinedReportHTML(ucastViews());
  assert.match(html, /ROUND-TRIP TIME/, 'methodology must state ROUND-TRIP TIME');
});

// --- (4) Column order ---

test('latency table columns are in the exact required order', () => {
  const doc = parse(buildCombinedReportHTML(ucastViews()));
  const cols = [...doc.querySelectorAll('#lat-table tr:first-child th')].map((t) => t.textContent.trim());
  const expected = [
    'mode', 'src IP', 'dst IP', 'src VPC', 'dst VPC',
    'src role', 'dst role', 'src AZ', 'dst AZ', 'src PG', 'dst PG',
    'p50', 'p90', 'p99', 'p99.9', 'max', 'loss', 'measured',
  ];
  assert.deepEqual(cols, expected);
});

// --- (4b) dst-IP click selects destination after reorder ---

test('clicking dst IP column selects destination (sel-dst), not source', () => {
  const dom = new JSDOM(buildCombinedReportHTML(ucastViews()), { runScripts: 'dangerously' });
  const doc = dom.window.document;
  // dst IP is now column index 2
  const row = doc.querySelector('#lat-table tr[data-src]');
  const dstCell = row.cells[2]; // dst IP column
  dstCell.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  // The row whose dst matches should get sel-dst
  const dstIp = row.dataset.dst;
  const markedDst = doc.querySelectorAll(`#lat-table tr.sel-dst`);
  assert.ok(markedDst.length > 0, 'clicking dst IP must select the destination');
  // And it must NOT be sel-src on this row (since we clicked dst column)
  assert.ok(!row.classList.contains('sel-src'),
    'clicking dst column must not select as source');
});

test('clicking src IP column selects source (sel-src), not destination', () => {
  const dom = new JSDOM(buildCombinedReportHTML(ucastViews()), { runScripts: 'dangerously' });
  const doc = dom.window.document;
  // src IP is column index 1
  const row = doc.querySelector('#lat-table tr[data-src]');
  const srcCell = row.cells[1]; // src IP column
  srcCell.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  const markedSrc = doc.querySelectorAll('#lat-table tr.sel-src');
  assert.ok(markedSrc.length > 0, 'clicking src IP must select the source');
});

// --- (5) Best/worst colouring ---

test('best (min) value in measurement columns is gold, worst (max) is red', () => {
  // Use views with varying latencies so min != max
  const vs = [
    view('ucast', 'kernel', [[0, 1, 20], [1, 0, 50], [0, 2, 35]], 2000),
  ];
  const doc = parse(buildCombinedReportHTML(vs));
  const lat = doc.getElementById('lat-table');
  const rows = [...lat.querySelectorAll('tr[data-src]')];
  // p50 column: min=20, max=50
  // Find the header index for p50
  const cols = [...lat.querySelectorAll('tr:first-child th')].map((t) => t.textContent.trim());
  const p50idx = cols.indexOf('p50');
  assert.ok(p50idx >= 0);
  // Gather styles
  const p50cells = rows.map((r) => r.cells[p50idx]);
  const goldCell = p50cells.find((c) => /color:\s*rgb\(57,\s*211,\s*83\)/i.test(c.getAttribute('style') || ''));
  const redCell = p50cells.find((c) => /color:\s*red/i.test(c.getAttribute('style') || ''));
  assert.ok(goldCell, 'the minimum p50 cell must be gold');
  assert.ok(redCell, 'the maximum p50 cell must be red');
  // Verify the gold is actually the min and red is the max
  assert.match(goldCell.textContent, /20/, 'gold cell should be the min value (20)');
  assert.match(redCell.textContent, /50/, 'red cell should be the max value (50)');
});

test('best/worst colouring skipped when all values in column are equal', () => {
  // All p50 values are 31
  const vs = [
    view('ucast', 'kernel', [[0, 1, 31], [1, 0, 31], [0, 2, 31]], 2000),
  ];
  const doc = parse(buildCombinedReportHTML(vs));
  const lat = doc.getElementById('lat-table');
  const cols = [...lat.querySelectorAll('tr:first-child th')].map((t) => t.textContent.trim());
  const p50idx = cols.indexOf('p50');
  const rows = [...lat.querySelectorAll('tr[data-src]')];
  const p50cells = rows.map((r) => r.cells[p50idx]);
  // None should be green or red
  for (const c of p50cells) {
    const style = c.getAttribute('style') || '';
    assert.ok(!/color:\s*(green|red)/i.test(style),
      `no cell should be coloured when all values equal: ${style}`);
  }
});

test('best/worst colouring skipped when only one row', () => {
  const vs = [view('ucast', 'kernel', [[0, 1, 40]], 2000)];
  const doc = parse(buildCombinedReportHTML(vs));
  const lat = doc.getElementById('lat-table');
  const cols = [...lat.querySelectorAll('tr:first-child th')].map((t) => t.textContent.trim());
  const p50idx = cols.indexOf('p50');
  const rows = [...lat.querySelectorAll('tr[data-src]')];
  assert.equal(rows.length, 1);
  const style = rows[0].cells[p50idx].getAttribute('style') || '';
  assert.ok(!/color:\s*(green|red)/i.test(style),
    'single row must not be coloured');
});
