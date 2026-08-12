// Exercises the REAL renderNodes() DOM, not a hand-rolled mock of it.
//
// The plan's risk table calls out "checkbox click also pins the table" as
// needing an explicit test. A test that builds its own listener and asserts that
// listener calls stopPropagation proves nothing about nodes.js -- it only proves
// the mock does what the mock was written to do. These drive the actual module.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

// nodes.js is a browser module and reads the global `document`. Install a DOM
// before importing it so the real module runs unmodified under node:test.
const bootstrap = new JSDOM('<!doctype html><html><body></body></html>', { pretendToBeVisual: true });
globalThis.window = bootstrap.window;
globalThis.document = bootstrap.window.document;
globalThis.MouseEvent = bootstrap.window.MouseEvent;
globalThis.Element = bootstrap.window.Element;
globalThis.navigator ??= bootstrap.window.navigator;

const { renderNodes } = await import('../src/lib/2d/nodes.js');

// A minimal ctx matching what renderNodes reads. Anything it touches for
// tooltips/edges is stubbed; we only care about click routing and classes.
function makeCtx(dom, { targetIds = new Set(), nodes } = {}) {
  const doc = dom.window.document;
  const root = doc.createElement('div');
  const tooltip = doc.createElement('div');
  doc.body.append(root, tooltip);

  const fleetNodes = nodes || [
    { index: 0, instance_id: 'i-1', private_ip: '10.0.0.1', public_ip: '1.1.1.1', role: 'source', az: 'a', region: 'r', cpg_name: 'pg', type: 't', online: true },
    { index: 1, instance_id: 'i-2', private_ip: '10.0.0.2', public_ip: '1.1.1.2', role: 'destination', az: 'a', region: 'r', cpg_name: 'pg', type: 't', online: true },
  ];

  const calls = { toggled: [], pinned: [] };
  const ctx = {
    root, tooltip,
    W: 1000, H: 800,
    fleet: { nodes: fleetNodes, matrix: fleetNodes.map(() => fleetNodes.map(() => null)) },
    nodes: fleetNodes,
    positions: fleetNodes.map((_, i) => ({ x: 100 + i * 200, y: 200 })),
    nodeEls: [],
    edgeEls: [],
    selected: new Set(),
    // renderNodes registers teardown and drives the Deselect-all button.
    disposers: [],
    deselectBtn: doc.createElement('button'),
    matrix: fleetNodes.map(() => fleetNodes.map(() => null)),
    targetIds,
    onToggleTarget: (id) => calls.toggled.push(id),
    applySel: () => {},
    tipHTML: () => '',
  };
  // Pinning is internal to renderNodes; a pinned panel in the DOM is the only
  // externally observable evidence that it happened.
  const pinnedCount = () => doc.querySelectorAll('.node-tooltip.pinned').length;
  return { ctx, calls, doc, pinnedCount };
}

// Reuse the bootstrap window: nodes.js creates elements via the global document,
// so a per-test JSDOM would produce elements from a different realm.
function fresh() {
  bootstrap.window.document.body.innerHTML = '';
  return bootstrap;
}

test('clicking the checkbox toggles the target and does NOT pin the table', () => {
  const dom = fresh();
  const { ctx, calls, doc, pinnedCount } = makeCtx(dom);
  renderNodes(ctx);

  const box = doc.querySelector('[data-target-box]');
  assert.ok(box, 'renderNodes must render a target checkbox');
  assert.equal(pinnedCount(), 0, 'nothing pinned before the click');

  // A real bubbling click on the checkbox, exactly as a user produces.
  box.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true, cancelable: true }));

  assert.deepEqual(calls.toggled, ['i-1'], 'checkbox click must toggle that node');
  assert.equal(pinnedCount(), 0,
    'checkbox click must NOT also pin the latency table (stopPropagation contract)');
});

test('clicking the node body still pins and does not toggle', () => {
  const dom = fresh();
  const { ctx, calls, pinnedCount } = makeCtx(dom);
  renderNodes(ctx);

  // The node element itself, not the checkbox.
  ctx.nodeEls[0].dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true, cancelable: true }));

  assert.deepEqual(calls.toggled, [], 'plain body click must not toggle the target set');
  assert.equal(pinnedCount(), 1, 'plain body click must still pin the latency table');
});

test('shift+click on the node body toggles instead of pinning', () => {
  const dom = fresh();
  const { ctx, calls, pinnedCount } = makeCtx(dom);
  renderNodes(ctx);

  ctx.nodeEls[1].dispatchEvent(new dom.window.MouseEvent('click', {
    bubbles: true, cancelable: true, shiftKey: true,
  }));

  assert.deepEqual(calls.toggled, ['i-2'], 'shift+click is the accelerator for the same toggle');
  assert.equal(pinnedCount(), 0, 'shift+click must not pin');
});

test('a targeted node is marked, and distinctly from a table-pinned one', () => {
  const dom = fresh();
  const { ctx, doc } = makeCtx(dom, { targetIds: new Set(['i-2']) });
  renderNodes(ctx);

  const els = ctx.nodeEls;
  assert.ok(!els[0].classList.contains('targeted'), 'unselected node must not be marked');
  assert.ok(els[1].classList.contains('targeted'), 'selected node must carry .targeted');
  // .targeted and .selected are different states (target set vs pinned table).
  assert.ok(!els[1].classList.contains('selected'),
    '.targeted must not imply .selected -- they are different concepts');

  const boxes = doc.querySelectorAll('[data-target-box]');
  assert.ok(boxes[1].classList.contains('checked'), 'targeted node checkbox must read as checked');
  assert.ok(!boxes[0].classList.contains('checked'));
});

test('checkboxes are visible once anything is selected, per D1', () => {
  // Empty set: hover-only (no .visible). Non-empty: always visible, so the
  // resting map stays clean but an active selection is never hidden.
  const a = fresh();
  const ctxA = makeCtx(a, { targetIds: new Set() });
  renderNodes(ctxA.ctx);
  for (const b of ctxA.doc.querySelectorAll('[data-target-box]')) {
    assert.ok(!b.classList.contains('visible'), 'empty selection: checkbox is hover-only');
  }

  const b2 = fresh();
  const ctxB = makeCtx(b2, { targetIds: new Set(['i-1']) });
  renderNodes(ctxB.ctx);
  for (const b of ctxB.doc.querySelectorAll('[data-target-box]')) {
    assert.ok(b.classList.contains('visible'), 'non-empty selection: all checkboxes visible');
  }
});
