// Column resize + reorder on the All measurements table, driven through the real
// reportInteractions() so the standalone file and the in-app view are both covered.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';
import { buildCombinedReportBody, reportInteractions, REPORT_CSS } from '../src/lib/report-combined.js';

function view(kind, variation, cells, unix = 1000) {
  const nodes = [0, 1, 2].map((i) => ({
    index: i, private_ip: `10.0.0.${i + 1}`, public_ip: `1.1.1.${i + 1}`,
    role: ['source', 'replicator', 'destination'][i], az: 'eu-central-1a',
    region: 'eu-central-1', cpg_name: 'cpg-a', vpc_id: 'vpc-1',
    type: 'c7i.4xlarge', online: true,
  }));
  const m = Array.from({ length: 3 }, () => Array(3).fill(null));
  for (const [i, j, p] of cells) {
    m[i][j] = { p50: p, p90: p + 2, p99: p + 5, p999: p + 9, max: p + 20, loss: 0, unix };
  }
  return { kind, variation, unix, fleet: { nodes, matrix: m, region: 'eu-central-1' } };
}

function mount() {
  const dom = new JSDOM('<!doctype html><html><body><div id="r"></div></body></html>');
  const doc = dom.window.document;
  globalThis.document = doc;   // reportInteractions creates elements via document
  const root = doc.getElementById('r');
  root.innerHTML = buildCombinedReportBody(
    [view('ucast', 'kernel', [[0, 1, 31], [1, 0, 32], [0, 2, 44]])], '');
  reportInteractions(root);
  return { dom, doc, root, lat: root.querySelector('#lat-table') };
}

const headers = (lat) => [...lat.querySelectorAll('tr:first-child th')].map((t) =>
  t.textContent.replace(/\s+$/, '').trim());

test('every header gets a resize grip', () => {
  const { lat } = mount();
  const ths = [...lat.querySelectorAll('tr:first-child th')];
  assert.ok(ths.length > 5);
  for (const th of ths) {
    assert.ok(th.querySelector('.rsz'), `missing grip on "${th.textContent.trim()}"`);
  }
});

test('dragging the grip sets an explicit width', () => {
  const { dom, lat } = mount();
  const th = lat.querySelector('tr:first-child th');
  const grip = th.querySelector('.rsz');
  // jsdom reports zero-size boxes, so assert the mechanism, not the pixel value.
  grip.dispatchEvent(new dom.window.MouseEvent('mousedown', { bubbles: true, clientX: 100 }));
  dom.window.document.dispatchEvent(new dom.window.MouseEvent('mousemove', { bubbles: true, clientX: 160 }));
  dom.window.document.dispatchEvent(new dom.window.MouseEvent('mouseup', { bubbles: true }));
  assert.ok(th.style.width, 'a width must be applied to the header');
  assert.equal(th.style.width, th.style.minWidth, 'width must be pinned, not just hinted');
});

test('the resize grip does not also trigger a sort', () => {
  // The grip sits inside the header, whose click sorts. Without stopPropagation a
  // resize would reorder the rows underneath the user.
  const { dom, lat } = mount();
  const before = [...lat.querySelectorAll('tr[data-src]')].map((t) => t.dataset.dst);
  lat.querySelector('tr:first-child th .rsz')
    .dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  const after = [...lat.querySelectorAll('tr[data-src]')].map((t) => t.dataset.dst);
  assert.deepEqual(after, before, 'clicking the grip must not sort');
});

test('headers are draggable and a drop reorders that column everywhere', () => {
  const { dom, lat } = mount();
  const ths = [...lat.querySelectorAll('tr:first-child th')];
  assert.ok(ths.every((t) => t.draggable), 'every header must be draggable');

  const before = headers(lat);
  const rowBefore = [...lat.querySelector('tr[data-src]').children].map((c) => c.textContent.trim());

  // Move column 0 (mode) to position 2 (dst IP).
  const data = new Map();
  const dt = { setData: (k, v) => data.set(k, v), getData: (k) => data.get(k), effectAllowed: '' };
  const fire = (el, type) => {
    const ev = new dom.window.Event(type, { bubbles: true, cancelable: true });
    ev.dataTransfer = dt;
    el.dispatchEvent(ev);
  };
  fire(ths[0], 'dragstart');
  fire(ths[2], 'drop');

  const after = headers(lat);
  assert.notDeepEqual(after, before, 'the header order must change');
  assert.equal(after.indexOf('mode'), 2, 'mode must land where it was dropped');
  // Body cells must move with their header, or the table starts lying.
  const rowAfter = [...lat.querySelector('tr[data-src]').children].map((c) => c.textContent.trim());
  assert.equal(rowAfter[2], rowBefore[0], 'the mode value must follow its column');
  assert.equal(rowAfter.length, rowBefore.length, 'no cell may be lost in the move');
});

test('dropping a column on itself changes nothing', () => {
  const { dom, lat } = mount();
  const ths = [...lat.querySelectorAll('tr:first-child th')];
  const before = headers(lat);
  const data = new Map();
  const dt = { setData: (k, v) => data.set(k, v), getData: (k) => data.get(k), effectAllowed: '' };
  const fire = (el, type) => {
    const ev = new dom.window.Event(type, { bubbles: true, cancelable: true });
    ev.dataTransfer = dt;
    el.dispatchEvent(ev);
  };
  fire(ths[1], 'dragstart');
  fire(ths[1], 'drop');
  assert.deepEqual(headers(lat), before);
});

test('the grip and drag styles ship in REPORT_CSS', () => {
  // The standalone download must look and behave the same as the in-app view.
  assert.match(REPORT_CSS, /col-resize/);
  assert.match(REPORT_CSS, /#lat-table th\.drop-target/);
});
