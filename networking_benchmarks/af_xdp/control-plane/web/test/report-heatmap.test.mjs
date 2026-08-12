// Report heatmap selection: clicking an IP on the heatmap itself must select
// that node and highlight its row AND column.
//
// The cross-table highlighting already painted .sel-row/.sel-col, but only
// something else (inventory, fan-out, latency rows) could start a selection -
// the heatmap's own axis labels were inert, which is the most obvious place to
// click when you are looking at the grid.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';
import { buildReportHTML } from '../src/lib/report.js';

function ucastReport() {
  const nodes = [0, 1, 2].map((i) => ({
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
  const N = 3;
  const m = Array.from({ length: N }, () => Array(N).fill(null));
  for (const [i, j, p] of [[0, 1, 31], [1, 0, 32], [0, 2, 33], [2, 0, 30], [1, 2, 34], [2, 1, 35]]) {
    m[i][j] = { p50: p, p90: p + 2, p99: p + 5, p999: p + 9, max: p + 20, loss: 0 };
  }
  const html = buildReportHTML({ nodes, matrix: m, region: 'eu-central-1' }, 'ucast', 'kernel');
  const dom = new JSDOM(html, { runScripts: 'dangerously' });
  return { dom, doc: dom.window.document };
}

const click = (dom, el) =>
  el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true, cancelable: true }));

test('clicking a heatmap ROW header selects that node and marks its row and column', () => {
  const { dom, doc } = ucastReport();
  const rowTh = doc.querySelector('#heat-table th[data-row-ip="10.0.0.2"]');
  assert.ok(rowTh, 'heatmap must have a row header for the node');

  click(dom, rowTh);

  // Its own row and column in the grid.
  assert.ok(doc.querySelectorAll('#heat-table td.sel-row').length > 0, 'row cells must be marked');
  assert.ok(doc.querySelectorAll('#heat-table td.sel-col').length > 0, 'column cells must be marked');
  // And the selection propagates to the other tables.
  const invSel = [...doc.querySelectorAll('#inv-table tr.sel')].map((t) => t.dataset.ip);
  assert.deepEqual(invSel, ['10.0.0.2'], 'inventory row must highlight too');
});

test('clicking a heatmap COLUMN header selects that node', () => {
  const { dom, doc } = ucastReport();
  const colTh = doc.querySelector('#heat-table th[data-col-ip="10.0.0.3"]');
  assert.ok(colTh, 'heatmap must have a column header for the node');

  click(dom, colTh);

  const invSel = [...doc.querySelectorAll('#inv-table tr.sel')].map((t) => t.dataset.ip);
  assert.deepEqual(invSel, ['10.0.0.3']);
  assert.ok(doc.querySelectorAll('#heat-table td.sel-row').length > 0);
  assert.ok(doc.querySelectorAll('#heat-table td.sel-col').length > 0);
});

test('clicking a heatmap header twice toggles the selection off', () => {
  const { dom, doc } = ucastReport();
  const rowTh = doc.querySelector('#heat-table th[data-row-ip="10.0.0.1"]');
  click(dom, rowTh);
  assert.equal([...doc.querySelectorAll('#inv-table tr.sel')].length, 1);
  click(dom, rowTh);
  assert.equal([...doc.querySelectorAll('#inv-table tr.sel')].length, 0, 'second click must deselect');
  assert.equal(doc.querySelectorAll('#heat-table td.sel-row').length, 0);
});

test('the marked row and column are the ones belonging to the clicked IP', () => {
  const { dom, doc } = ucastReport();
  click(dom, doc.querySelector('#heat-table th[data-row-ip="10.0.0.2"]'));

  for (const td of doc.querySelectorAll('#heat-table td.sel-row')) {
    assert.equal(td.dataset.rowIp, '10.0.0.2', 'sel-row must only mark that node row');
  }
  for (const td of doc.querySelectorAll('#heat-table td.sel-col')) {
    assert.equal(td.dataset.colIp, '10.0.0.2', 'sel-col must only mark that node column');
  }
});

test('selecting two nodes from the heatmap marks both axes for each', () => {
  const { dom, doc } = ucastReport();
  click(dom, doc.querySelector('#heat-table th[data-row-ip="10.0.0.1"]'));
  click(dom, doc.querySelector('#heat-table th[data-col-ip="10.0.0.3"]'));

  const invSel = [...doc.querySelectorAll('#inv-table tr.sel')].map((t) => t.dataset.ip).sort();
  assert.deepEqual(invSel, ['10.0.0.1', '10.0.0.3']);
  // The pair where both ends are selected shows as both in the latency table.
  assert.ok(doc.querySelectorAll('#lat-table tr.sel-both').length > 0,
    'a pair with both endpoints selected must be marked as both');
});

test('the heatmap stays unsorted when its headers become clickable', () => {
  // Rows and columns are the same node axis, so reordering rows would break
  // their correspondence with the header row. Clicking must select, not sort.
  const { dom, doc } = ucastReport();
  const before = [...doc.querySelectorAll('#heat-table tr[data-ip]')].map((tr) => tr.dataset.ip);
  click(dom, doc.querySelector('#heat-table th[data-row-ip="10.0.0.3"]'));
  const after = [...doc.querySelectorAll('#heat-table tr[data-ip]')].map((tr) => tr.dataset.ip);
  assert.deepEqual(after, before, 'heatmap row order must not change');
  assert.ok(!doc.getElementById('heat-table').classList.contains('sortable'));
});
