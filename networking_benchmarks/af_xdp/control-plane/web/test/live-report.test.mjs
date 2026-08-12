// Tests for the live report view refactor:
// - REPORT_CSS / buildCombinedReportBody / reportInteractions exports
// - standalone document still embeds serialised interactions and works
// - reportInteractions scopes to its root rather than document
// - print stylesheet exists with required properties
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

import {
  buildCombinedReportHTML,
  buildCombinedReportBody,
  REPORT_CSS,
  reportInteractions,
  MODE_BADGE,
} from '../src/lib/report-combined.js';

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

const views = () => [
  view('ucast', 'kernel', [[0, 1, 31], [1, 0, 32], [0, 2, 33], [2, 0, 30]], 2000),
  view('ucast', 'xdp', [[0, 1, 28], [1, 0, 29]], 3000),
];

// ─── Export existence tests ─────────────────────────────────────────────────

test('REPORT_CSS is exported as a non-empty string', () => {
  assert.equal(typeof REPORT_CSS, 'string');
  assert.ok(REPORT_CSS.length > 100, 'REPORT_CSS should be a substantial stylesheet');
  assert.ok(REPORT_CSS.includes('.heat'), 'REPORT_CSS must contain heat table styles');
  assert.ok(REPORT_CSS.includes('.mode-badge'), 'REPORT_CSS must contain mode-badge styles');
});

test('buildCombinedReportBody is exported and returns HTML body content', () => {
  assert.equal(typeof buildCombinedReportBody, 'function');
  const body = buildCombinedReportBody(views());
  assert.equal(typeof body, 'string');
  // Must NOT be a full document - no <html>, <head>, <style>, <script>
  assert.ok(!body.includes('<!doctype'), 'body must not be a full document');
  assert.ok(!body.includes('<html'), 'body must not contain <html>');
  assert.ok(!body.includes('<style'), 'body must not contain <style>');
  assert.ok(!body.includes('<script'), 'body must not contain <script>');
  // Must contain the report content
  assert.ok(body.includes('overview-table'), 'body must contain overview table');
  assert.ok(body.includes('inv-table'), 'body must contain inventory table');
  assert.ok(body.includes('lat-table'), 'body must contain latency table');
});

test('reportInteractions is exported as a function', () => {
  assert.equal(typeof reportInteractions, 'function');
});

// ─── Standalone document serialises reportInteractions ──────────────────────

test('standalone document embeds the serialised reportInteractions function', () => {
  const html = buildCombinedReportHTML(views());
  // The function must be serialised into the script tag
  assert.ok(html.includes('reportInteractions'), 'HTML must embed reportInteractions by name');
  // It should be invoked with document as argument
  assert.ok(html.includes('(document)'), 'serialised function must be called with document');
});

test('standalone document interactions still work (sorting)', () => {
  const dom = new JSDOM(buildCombinedReportHTML(views()), { runScripts: 'dangerously' });
  const doc = dom.window.document;
  const lat = doc.getElementById('lat-table');
  // Click the p50 column header to sort
  const headers = [...lat.querySelectorAll('tr:first-child th')];
  const p50h = headers.find((th) => th.textContent.trim() === 'p50');
  assert.ok(p50h, 'lat table must have a p50 header');
  p50h.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  // After first click, rows are sorted (descending is the default direction).
  const p50col = headers.indexOf(p50h);
  const vals = [...lat.querySelectorAll('tr[data-src]')].map(
    (tr) => tr.cells[p50col].textContent.trim()
  );
  const nums = vals.map((v) => parseFloat(v));
  // Second click flips to ascending.
  p50h.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  const vals2 = [...lat.querySelectorAll('tr[data-src]')].map(
    (tr) => tr.cells[p50col].textContent.trim()
  );
  const nums2 = vals2.map((v) => parseFloat(v));
  for (let i = 1; i < nums2.length; i++) {
    assert.ok(nums2[i] >= nums2[i - 1], `p50 should be ascending after 2nd click: ${nums2}`);
  }
  // And first click should have been descending
  for (let i = 1; i < nums.length; i++) {
    assert.ok(nums[i] <= nums[i - 1], `p50 should be descending after 1st click: ${nums}`);
  }
});

test('standalone document interactions still work (cross-table selection)', () => {
  const dom = new JSDOM(buildCombinedReportHTML(views()), { runScripts: 'dangerously' });
  const doc = dom.window.document;
  const row = doc.querySelector('#inv-table tr[data-ip="10.0.0.1"]');
  row.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  assert.equal(doc.querySelectorAll('#inv-table tr.sel').length, 1);
  assert.ok(doc.querySelectorAll('table.heat td.sel-row').length > 0);
  assert.ok(doc.querySelectorAll('#lat-table tr.sel-src').length > 0);
});

// ─── reportInteractions scopes to root, not document ────────────────────────

test('reportInteractions scopes to its root parameter rather than document', () => {
  const body = buildCombinedReportBody(views());
  // Create a DOM with the body content wrapped in a scoped div
  const dom = new JSDOM(`<!doctype html><html><body>
    <div id="outside"><table class="sortable"><tr><th>x</th></tr><tr><td>1</td></tr></table></div>
    <div id="scoped">${body}</div>
  </body></html>`, { runScripts: 'dangerously' });
  const doc = dom.window.document;
  const scoped = doc.getElementById('scoped');

  // Run reportInteractions scoped to the inner div
  const fn = new dom.window.Function('root', `(${reportInteractions.toString()})(root);`);
  fn(scoped);

  // Click an IP in the scoped area - selection should work
  const row = scoped.querySelector('#inv-table tr[data-ip="10.0.0.1"]');
  row.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  assert.equal(scoped.querySelectorAll('#inv-table tr.sel').length, 1);

  // The outside area should be unaffected
  const outsideTable = doc.getElementById('outside').querySelector('table');
  assert.equal(outsideTable.querySelectorAll('tr.sel').length, 0,
    'selection must not leak outside the scoped root');
});

// ─── Print stylesheet ───────────────────────────────────────────────────────

test('App.svelte contains a @media print stylesheet with required properties', async () => {
  const { readFileSync } = await import('node:fs');
  const { join } = await import('node:path');
  const appContent = readFileSync(
    join(import.meta.dirname, '..', 'src', 'App.svelte'), 'utf8'
  );
  // Must have @media print
  assert.ok(appContent.includes('@media print'), 'App.svelte must have @media print styles');
  // White background with dark text
  assert.ok(/background\s*:\s*#?(?:fff|white)/i.test(appContent) ||
    /background-color\s*:\s*#?(?:fff|white)/i.test(appContent),
    'print style must set white background');
  assert.ok(/color\s*:\s*#?(?:000|black|1[0-9a-f]{5}|2[0-9a-f]{5})/i.test(appContent),
    'print style must set dark text color');
  // Avoid page-breaking inside tables
  assert.ok(/break-inside\s*:\s*avoid/i.test(appContent) ||
    /page-break-inside\s*:\s*avoid/i.test(appContent),
    'print style must avoid page breaks inside tables');
  // Expand collapsed details
  assert.ok(/details\[open\]|details/.test(appContent),
    'print style should address details elements');
  // Hide app chrome
  assert.ok(/\.cp-panel|\.controls-host|\.root/.test(appContent),
    'print style must hide app chrome');
});
