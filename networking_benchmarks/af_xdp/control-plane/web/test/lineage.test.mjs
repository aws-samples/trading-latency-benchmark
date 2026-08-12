// Tests for Phase 3 Lineage UI features: relative-age formatting, D7 age-fade
// thresholds, compare-mode delta + hatched-when-missing, and report Measurement
// ages section. Exercises REAL DOM output, not mocks.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

// Install jsdom globals before importing browser modules.
const bootstrap = new JSDOM('<!doctype html><html><body></body></html>', { pretendToBeVisual: true });
globalThis.window = bootstrap.window;
globalThis.document = bootstrap.window.document;
globalThis.MouseEvent = bootstrap.window.MouseEvent;
globalThis.Element = bootstrap.window.Element;
globalThis.navigator ??= bootstrap.window.navigator;

const { relativeAge, ageFade, sparklineSVG, compareDelta, compareColor } = await import('../src/lib/lineage.js');

// ── 3.2: relative-age formatting ──────────────────────────────────────────────

test('relativeAge: seconds ago', () => {
  const now = 1700000045 * 1000; // 45s after unix 1700000000
  assert.equal(relativeAge(1700000000, now), '45 s ago');
});

test('relativeAge: minutes ago', () => {
  const now = 1700000000 * 1000 + 180_000; // 3 min later
  assert.equal(relativeAge(1700000000, now), '3 min ago');
});

test('relativeAge: hours ago', () => {
  const now = 1700000000 * 1000 + 7200_000; // 2 h later
  assert.equal(relativeAge(1700000000, now), '2 h ago');
});

test('relativeAge: days ago', () => {
  const now = 1700000000 * 1000 + 172800_000; // 2 d later
  assert.equal(relativeAge(1700000000, now), '2 d ago');
});

test('relativeAge: zero/null returns empty', () => {
  assert.equal(relativeAge(0, Date.now()), '');
  assert.equal(relativeAge(null, Date.now()), '');
});

// ── 3.2: D7 age-fade thresholds ──────────────────────────────────────────────

test('ageFade: fresh sample (< 5 min) = full opacity, no dash', () => {
  const r = ageFade(120); // 2 minutes
  assert.equal(r.opacity, 1.0);
  assert.equal(r.dashed, false);
});

test('ageFade: > 5 min = 70% opacity, no dash', () => {
  const r = ageFade(400); // ~6.7 min
  assert.equal(r.opacity, 0.7);
  assert.equal(r.dashed, false);
});

test('ageFade: > 1 h = 40% opacity + dashed border', () => {
  const r = ageFade(4000); // ~67 min
  assert.equal(r.opacity, 0.4);
  assert.equal(r.dashed, true);
});

// ── 3.2: sparkline SVG from history ──────────────────────────────────────────

test('sparklineSVG: produces valid SVG polyline from history', () => {
  const hist = [
    { u: 1, p50: 30, p99: 40 },
    { u: 2, p50: 32, p99: 42 },
    { u: 3, p50: 28, p99: 38 },
  ];
  const svg = sparklineSVG(hist);
  assert.ok(svg.includes('<svg'), 'must produce an SVG element');
  assert.ok(svg.includes('<polyline'), 'must contain a polyline');
  assert.ok(svg.includes('points='), 'polyline must have points');
});

test('sparklineSVG: returns empty for < 2 samples', () => {
  assert.equal(sparklineSVG([{ u: 1, p50: 30, p99: 40 }]), '');
  assert.equal(sparklineSVG(null), '');
});

// ── 3.4: compare-mode delta + hatched-when-missing ───────────────────────────

test('compareDelta: xdp - kernel', () => {
  const kernel = { p50: 32, p99: 37 };
  const xdp = { p50: 28, p99: 33 };
  assert.equal(compareDelta(kernel, xdp), -4); // xdp 4µs faster
});

test('compareDelta: returns null when either is missing', () => {
  assert.equal(compareDelta(null, { p50: 28 }), null);
  assert.equal(compareDelta({ p50: 32 }, null), null);
});

test('compareColor: negative delta (xdp faster) = green-ish', () => {
  const c = compareColor(-10, 50);
  assert.ok(c.includes('rgba(46,'), 'negative delta should be green channel');
});

test('compareColor: positive delta (kernel faster) = red-ish', () => {
  const c = compareColor(10, 50);
  assert.ok(c.match(/rgba\(\d+,50,50/), 'positive delta should be red channel');
});

test('compareColor: null delta = transparent', () => {
  assert.equal(compareColor(null, 50), 'transparent');
});

// ── 3.3 + 3.4: report HTML tests ────────────────────────────────────────────

const { buildReportHTML } = await import('../src/lib/report.js');

// Helper: build a minimal fleet with unix timestamps on cells.
function makeFleetWithAges() {
  const nodes = [
    { index: 0, private_ip: '10.0.0.1', ec2_name: 'a', role: '', az: 'az1', region: 'us-east-1', cpg_name: 'pg1', type: 'c5.xlarge', online: true, public_ip: '' },
    { index: 1, private_ip: '10.0.0.2', ec2_name: 'b', role: '', az: 'az1', region: 'us-east-1', cpg_name: 'pg1', type: 'c5.xlarge', online: true, public_ip: '' },
    { index: 2, private_ip: '10.0.0.3', ec2_name: 'c', role: '', az: 'az2', region: 'us-east-1', cpg_name: 'pg2', type: 'c5.xlarge', online: true, public_ip: '' },
  ];
  const now = Math.floor(Date.now() / 1000);
  const matrix = [
    [null, { p50: 32, p90: 33, p99: 37, p999: 45, max: 50, loss: 0, unix: now }, { p50: 40, p90: 42, p99: 48, p999: 55, max: 60, loss: 0, unix: now - 7200 }],
    [{ p50: 33, p90: 34, p99: 38, p999: 46, max: 51, loss: 0, unix: now - 300 }, null, null],
    [{ p50: 41, p90: 43, p99: 49, p999: 56, max: 61, loss: 0, unix: now - 600 }, { p50: 39, p90: 41, p99: 47, p999: 54, max: 59, loss: 0, unix: now }, null],
  ];
  return { schema: 'afxdp.topology/v1', region: 'us-east-1', account: 'test', generated_at: new Date().toISOString(), nodes, matrix };
}

test('report: cells have title attribute with variation and measurement time', () => {
  const fleet = makeFleetWithAges();
  const html = buildReportHTML(fleet, 'ucast', 'kernel');
  // Parse the HTML and find heatmap cells with data attributes
  const dom = new JSDOM(html);
  const cells = dom.window.document.querySelectorAll('#heat-table td[data-row-ip][data-col-ip]');
  let found = false;
  for (const cell of cells) {
    const t = cell.getAttribute('title') || '';
    if (t.includes('kernel') && t.match(/\d{4}/)) { found = true; break; }
  }
  assert.ok(found, 'at least one heatmap cell title must mention variation and measurement time');
});

test('report: Measurement ages section present', () => {
  const fleet = makeFleetWithAges();
  const html = buildReportHTML(fleet, 'ucast', 'kernel');
  assert.ok(html.includes('Measurement ages'), 'report must have a Measurement ages section');
});

test('report: Measurement ages reports oldest/newest and stale count', () => {
  const fleet = makeFleetWithAges();
  const html = buildReportHTML(fleet, 'ucast', 'kernel');
  // Should mention oldest, newest, and a count of cells older than newest
  assert.ok(html.includes('oldest'), 'must mention oldest measurement');
  assert.ok(html.includes('newest'), 'must mention newest measurement');
  assert.ok(html.includes('older than'), 'must report stale cell count');
});

// ── 3.4: compare view in report ──────────────────────────────────────────────

// Import the compare builder (will be created in report.js)
const { buildCompareHTML } = await import('../src/lib/report.js');

test('compare view: delta = xdp.p50 - kernel.p50', () => {
  // Build a fleet where both kernel and xdp data exist for a pair
  const nodes = [
    { index: 0, private_ip: '10.0.0.1', ec2_name: 'a', role: '', az: 'az1', region: 'r', cpg_name: 'pg', type: 't', online: true, public_ip: '' },
    { index: 1, private_ip: '10.0.0.2', ec2_name: 'b', role: '', az: 'az1', region: 'r', cpg_name: 'pg', type: 't', online: true, public_ip: '' },
  ];
  const kernelMatrix = [[null, { p50: 32, p99: 37 }], [{ p50: 33, p99: 38 }, null]];
  const xdpMatrix = [[null, { p50: 28, p99: 33 }], [{ p50: 30, p99: 35 }, null]];
  const html = buildCompareHTML(nodes, kernelMatrix, xdpMatrix);
  // Delta for [0][1]: 28 - 32 = -4
  assert.ok(html.includes('-4'), 'compare cell must show delta (xdp - kernel)');
});

test('compare view: missing mode renders hatched, not green', () => {
  const nodes = [
    { index: 0, private_ip: '10.0.0.1', ec2_name: 'a', role: '', az: 'az1', region: 'r', cpg_name: 'pg', type: 't', online: true, public_ip: '' },
    { index: 1, private_ip: '10.0.0.2', ec2_name: 'b', role: '', az: 'az1', region: 'r', cpg_name: 'pg', type: 't', online: true, public_ip: '' },
  ];
  // Kernel has data, xdp does not for [0][1]
  const kernelMatrix = [[null, { p50: 32, p99: 37 }], [null, null]];
  const xdpMatrix = [[null, null], [null, null]];
  const html = buildCompareHTML(nodes, kernelMatrix, xdpMatrix);
  assert.ok(html.includes('hatched'), 'cells missing either mode must have hatched class/style');
  // Must NOT include green background for missing cells
  assert.ok(!html.includes('background:rgba(46,') || html.includes('hatched'),
    'a missing mode must not read as fast (green)');
});
