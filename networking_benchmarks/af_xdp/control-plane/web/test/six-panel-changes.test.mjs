// Tests for the six UI changes: scope dropdown visibility, default scope,
// zoom, hover table borders, fold icons, cancel-run button position.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

const dom = new JSDOM('<!doctype html><html><body></body></html>', { pretendToBeVisual: true });
globalThis.window = dom.window;
globalThis.document = dom.window.document;
globalThis.MouseEvent = dom.window.MouseEvent;
globalThis.WheelEvent = dom.window.WheelEvent;
globalThis.Element = dom.window.Element;
globalThis.HTMLElement = dom.window.HTMLElement;
globalThis.navigator ??= dom.window.navigator;
globalThis.localStorage = (() => {
  const s = {};
  return { getItem: (k) => s[k] ?? null, setItem: (k, v) => { s[k] = v; }, removeItem: (k) => { delete s[k]; } };
})();
globalThis.ResizeObserver = class { observe() {} unobserve() {} disconnect() {} };
globalThis.getComputedStyle = dom.window.getComputedStyle;

const { mountControls } = await import('../src/lib/controls.js');

function freshPanel(opts = {}) {
  dom.window.document.body.innerHTML = '';
  const host = dom.window.document.createElement('div');
  dom.window.document.body.appendChild(host);
  return mountControls(host, opts);
}

// ════════════════════════════════════════════════════════════════════════════════
// (1) SCOPE DROPDOWN VISIBILITY
// ════════════════════════════════════════════════════════════════════════════════

test('(1) scope dropdown is hidden when no instances selected', () => {
  freshPanel();
  const scopeRow = dom.window.document.querySelector('[data-scope]');
  assert.ok(scopeRow, 'scope select element must exist');
  // The scope row or its parent must be hidden when count=0
  const ctrl = freshPanel();
  ctrl.setTargets({ count: 0, pairs: 0, scope: 'among', totalNodes: 5 });
  const scope = dom.window.document.querySelector('[data-scope]');
  const scopeParent = scope.closest('[data-targets-content]') || scope.parentElement;
  // Either the scope itself or the containing row should be hidden
  const scopeVisible = scope.offsetParent !== null ||
    getComputedStyle(scope).display !== 'none';
  const rowEl = scope.closest('.row');
  const rowHidden = rowEl && rowEl.style.display === 'none';
  assert.ok(rowHidden || scope.style.display === 'none',
    'scope select row must be hidden when 0 instances selected');
});

test('(1) scope dropdown appears when 1+ instances selected', () => {
  const ctrl = freshPanel();
  ctrl.setTargets({ count: 3, pairs: 6, scope: 'among', totalNodes: 5 });
  const scope = dom.window.document.querySelector('[data-scope]');
  const rowEl = scope.closest('.row');
  const hidden = (rowEl && rowEl.style.display === 'none') || scope.style.display === 'none';
  assert.ok(!hidden, 'scope select row must be visible when instances are selected');
});

test('(1) scope dropdown hides again when selection cleared to 0', () => {
  const ctrl = freshPanel();
  ctrl.setTargets({ count: 2, pairs: 2, scope: 'among', totalNodes: 5 });
  ctrl.setTargets({ count: 0, pairs: 0, scope: 'among', totalNodes: 5 });
  const scope = dom.window.document.querySelector('[data-scope]');
  const rowEl = scope.closest('.row');
  const hidden = (rowEl && rowEl.style.display === 'none') || scope.style.display === 'none';
  assert.ok(hidden, 'scope must hide when selection returns to 0');
});

// ════════════════════════════════════════════════════════════════════════════════
// (2) DEFAULT SCOPE IS 'among'
// ════════════════════════════════════════════════════════════════════════════════

test('(2) default scope value is "among"', () => {
  freshPanel();
  const scope = dom.window.document.querySelector('[data-scope]');
  assert.equal(scope.value, 'among', 'initial scope must default to among');
});

test('(2) scope resets to "among" after Cancel clears targets', () => {
  const ctrl = freshPanel();
  ctrl.setTargets({ count: 3, pairs: 6, scope: 'fanout', totalNodes: 5 });
  // Verify it was set to fanout
  const scope = dom.window.document.querySelector('[data-scope]');
  assert.equal(scope.value, 'fanout');
  // Now simulate clear (what the cancel button triggers)
  ctrl.setTargets({ count: 0, pairs: 0, scope: 'among', totalNodes: 5 });
  assert.equal(scope.value, 'among', 'scope must reset to among when targets cleared');
});

// ════════════════════════════════════════════════════════════════════════════════
// (3) 2D ZOOM - transform math
// ════════════════════════════════════════════════════════════════════════════════

test('(3) wheel zoom produces correct scale and translation', async () => {
  // Import the zoom module to test math directly
  const { applyZoom, resetZoom, getZoomState } = await import('../src/lib/2d/zoom.js');
  const state = { scale: 1, tx: 0, ty: 0 };

  // Zoom in at point (200, 150) by a factor
  applyZoom(state, -100, 200, 150); // negative deltaY = zoom in
  assert.ok(state.scale > 1, 'negative deltaY should zoom in (scale > 1)');
  // The viewport should translate so the cursor stays fixed
  const expectedTx = 200 - 200 * state.scale;
  const expectedTy = 150 - 150 * state.scale;
  // After zoom, origin shifts toward the cursor
  assert.ok(Math.abs(state.tx - (200 * (1 - state.scale))) < 0.01,
    `tx should shift toward cursor: got ${state.tx}`);
  assert.ok(Math.abs(state.ty - (150 * (1 - state.scale))) < 0.01,
    `ty should shift toward cursor: got ${state.ty}`);
});

test('(3) zoom respects min/max limits', async () => {
  const { applyZoom, MIN_SCALE, MAX_SCALE } = await import('../src/lib/2d/zoom.js');
  const state = { scale: 1, tx: 0, ty: 0 };
  // Zoom out many times
  for (let i = 0; i < 50; i++) applyZoom(state, 200, 0, 0);
  assert.ok(state.scale >= MIN_SCALE, `scale must not go below MIN_SCALE(${MIN_SCALE}), got ${state.scale}`);
  // Zoom in many times
  for (let i = 0; i < 100; i++) applyZoom(state, -200, 0, 0);
  assert.ok(state.scale <= MAX_SCALE, `scale must not exceed MAX_SCALE(${MAX_SCALE}), got ${state.scale}`);
});

test('(3) resetZoom returns to identity', async () => {
  const { applyZoom, resetZoom } = await import('../src/lib/2d/zoom.js');
  const state = { scale: 2.5, tx: -100, ty: -50 };
  resetZoom(state);
  assert.equal(state.scale, 1);
  assert.equal(state.tx, 0);
  assert.equal(state.ty, 0);
});

// ════════════════════════════════════════════════════════════════════════════════
// (4) HOVER TABLE BORDERS - leaking CSS
// ════════════════════════════════════════════════════════════════════════════════

test('(4) REPORT_CSS does not leak border into tooltip td elements', async () => {
  const { REPORT_CSS } = await import('../src/lib/report-combined.js');
  // The unscoped `th,td{border:...}` rule would hit .node-tooltip td.
  // After the fix, the rule should be scoped to .report-view or similar.
  // Parse: does the CSS contain an unscoped td rule?
  const lines = REPORT_CSS.split('\n');
  const unleakyTd = lines.filter(l => {
    const trimmed = l.trim();
    // Match unscoped `td,th` or `th,td` that sets border
    return /^(th|td)\s*,\s*(th|td)\s*\{/.test(trimmed) && /border/.test(trimmed);
  });
  assert.equal(unleakyTd.length, 0,
    'REPORT_CSS must not contain unscoped td,th{border:...} - it leaks into hover tooltips. ' +
    `Found: ${unleakyTd.join(' | ')}`);
});

test('(4) REPORT_CSS still applies border to .heat td', async () => {
  const { REPORT_CSS } = await import('../src/lib/report-combined.js');
  // Verify the heatmap cells still get their styling
  const hasHeatTd = REPORT_CSS.includes('.heat td');
  assert.ok(hasHeatTd, 'REPORT_CSS must still style .heat td for heatmap tables');
  // The scoped rule for report tables must exist
  const hasReportScope = /\.report-view\s+th|\.report-view\s+td/.test(REPORT_CSS) ||
    /\.report-view\s+(th|td)\s*,/.test(REPORT_CSS);
  assert.ok(hasReportScope, 'report table td/th border rule must be scoped under .report-view');
});

// ════════════════════════════════════════════════════════════════════════════════
// (5) FOLD ICONS - chevron that differs between states
// ════════════════════════════════════════════════════════════════════════════════

test('(5) fold indicator differs between folded and unfolded for Targets', () => {
  freshPanel();
  const btn = dom.window.document.querySelector('[data-fold-targets]');
  assert.ok(btn, 'fold-targets button must exist');
  const closedCollapsed = btn.classList.contains('collapsed');
  btn.click();
  const openCollapsed = btn.classList.contains('collapsed');
  // A chevron shows state by rotation, so the collapsed class must flip.
  assert.notEqual(closedCollapsed, openCollapsed,
    'the collapsed class must flip between folded and unfolded');
  assert.equal(btn.textContent, '\u2304', 'the indicator is a chevron');
});

test('(5) fold indicator differs between folded and unfolded for Latency', () => {
  freshPanel();
  const btn = dom.window.document.querySelector('[data-fold-latency]');
  assert.ok(btn, 'fold-latency button must exist');
  const closedCollapsed = btn.classList.contains('collapsed');
  btn.click();
  const openCollapsed = btn.classList.contains('collapsed');
  // A chevron shows state by rotation, so the collapsed class must flip.
  assert.notEqual(closedCollapsed, openCollapsed,
    'the collapsed class must flip between folded and unfolded');
  assert.equal(btn.textContent, '\u2304', 'the indicator is a chevron');
});

// ════════════════════════════════════════════════════════════════════════════════
// (6) CANCEL RUN BUTTON - positioned after log element
// ════════════════════════════════════════════════════════════════════════════════

test('(6) Cancel Run button is after the log/status element in DOM order', () => {
  freshPanel();
  const cancelBtn = dom.window.document.querySelector('[data-cancel-run]');
  const statusEl = dom.window.document.querySelector('[data-status]');
  assert.ok(cancelBtn, 'cancel-run button must exist');
  assert.ok(statusEl, 'status/log element must exist');
  // compareDocumentPosition: DOCUMENT_POSITION_FOLLOWING means cancelBtn comes after statusEl
  const pos = statusEl.compareDocumentPosition(cancelBtn);
  assert.ok(pos & dom.window.Node.DOCUMENT_POSITION_FOLLOWING,
    'cancel-run button must be positioned AFTER the log element in DOM');
});

test('(6) Cancel Run button is still wired to cancel (disabled when idle)', () => {
  freshPanel();
  const cancelBtn = dom.window.document.querySelector('[data-cancel-run]');
  assert.ok(cancelBtn.disabled, 'cancel-run must be disabled when no run is active');
});

test('(6) Cancel Run is not in the LOG header row', () => {
  freshPanel();
  const cancelBtn = dom.window.document.querySelector('[data-cancel-run]');
  const logRow = dom.window.document.querySelector('.cp-log-row');
  assert.ok(logRow, 'log row must exist');
  assert.ok(!logRow.contains(cancelBtn),
    'cancel-run button must NOT be inside the .cp-log-row (it should be below the log)');
});
