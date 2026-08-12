// Tests for the panel UX changes:
// 1. View buttons open new tab via window.open (not overlay)
// 2. [data-report] download icon is removed
// 3. Scope select disabled at count=0, enabled above
// 4. Run body carries entered parameters (count, rate, warmup, max_loss_pct)
// 5. Log ring holds full history, only one line displayed
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

const dom = new JSDOM('<!doctype html><html><body></body></html>', { pretendToBeVisual: true });
globalThis.window = dom.window;
globalThis.document = dom.window.document;
globalThis.MouseEvent = dom.window.MouseEvent;
globalThis.Element = dom.window.Element;
globalThis.navigator ??= dom.window.navigator;
globalThis.URL = dom.window.URL;
globalThis.Blob = dom.window.Blob;
globalThis.Intl = dom.window.Intl;
globalThis.HTMLElement = dom.window.HTMLElement;
// Stub ResizeObserver (not provided by jsdom)
globalThis.ResizeObserver = class { observe() {} unobserve() {} disconnect() {} };
dom.window.ResizeObserver = globalThis.ResizeObserver;

// Stub window.open so we can assert calls
const openCalls = [];
dom.window.open = (...args) => { openCalls.push(args); return null; };
globalThis.window.open = dom.window.open;

const { mountControls } = await import('../src/lib/controls.js');

function freshHost() {
  dom.window.document.body.innerHTML = '';
  openCalls.length = 0;
  const host = dom.window.document.createElement('div');
  dom.window.document.body.appendChild(host);
  return host;
}

// ─── Test 1: View buttons call window.open with correct URL and target ─────
test('view controls are real links that open a tab, never navigating this page', () => {
  // window.open from a button can be redirected into the current tab by the
  // browser's popup settings, which rewrote this page's URL. A real anchor with
  // target=_blank cannot do that.
  const host = freshHost();
  const panel = mountControls(host, {});
  panel.setCombos([{ kind: 'ucast', unix: 1 }], { kind: 'ucast' });
  const a = host.querySelector('[data-view-btn="ucast"]');
  assert.ok(a, 'a ucast control must exist');
  assert.equal(a.tagName, 'A', 'must be an anchor, not a button');
  assert.equal(a.getAttribute('href'), '?report=ucast');
  assert.equal(a.getAttribute('target'), '_blank');
  assert.match(a.getAttribute('rel') || '', /noopener/);
});

test('a kind with no data is present but disabled', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  panel.setCombos([{ kind: 'ucast', unix: 1 }], { kind: 'ucast' });
  const mcast = host.querySelector('[data-view-btn="mcast"]');
  assert.ok(mcast, 'the other kind stays visible so the panel states what exists');
  assert.ok(mcast.classList.contains('disabled'));
  assert.equal(mcast.getAttribute('href'), null, 'a disabled control must not be followable');
  assert.equal(mcast.getAttribute('aria-disabled'), 'true');
});

test('both kinds are disabled before any test has run', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  panel.setCombos([], null);
  for (const k of ['ucast', 'mcast']) {
    const a = host.querySelector(`[data-view-btn="${k}"]`);
    assert.ok(a, `${k} control must still render`);
    assert.ok(a.classList.contains('disabled'), `${k} must be disabled with no data`);
  }
});


// ─── Test 2: [data-report] download icon is gone ────────────────────────────
test('[data-report] download icon button does not exist', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  const reportBtn = host.querySelector('[data-report]');
  assert.equal(reportBtn, null, '[data-report] must not exist in the panel');
  panel.dispose();
});

// ─── Test 3: Scope select disabled at count=0, enabled above ────────────────
test('scope select disabled when count=0, enabled when count>0', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  const scopeSel = host.querySelector('[data-scope]');
  assert.ok(scopeSel, 'scope select must exist');

  // Initially (before setTargets called), should be disabled
  assert.equal(scopeSel.disabled, true, 'scope select should be disabled by default');

  // After setTargets with count=0
  panel.setTargets({ count: 0, pairs: 0, scope: 'among', totalNodes: 5, preset: null });
  assert.equal(scopeSel.disabled, true, 'scope select should be disabled at count=0');

  // After setTargets with count=2
  panel.setTargets({ count: 2, pairs: 1, scope: 'among', totalNodes: 5, preset: null });
  assert.equal(scopeSel.disabled, false, 'scope select should be enabled when count>0');

  // Back to 0
  panel.setTargets({ count: 0, pairs: 0, scope: 'among', totalNodes: 5, preset: null });
  assert.equal(scopeSel.disabled, true, 'scope select should disable again at count=0');

  panel.dispose();
});

// ─── Test 4: Run body carries entered parameters ────────────────────────────
test('run body includes count, rate, warmup, max_loss_pct from inputs', async () => {
  const host = freshHost();
  let capturedBody = null;
  const panel = mountControls(host, {
    onRun: (body) => { capturedBody = body; },
  });

  // Set input values
  const countInput = host.querySelector('[data-count]');
  const rateInput = host.querySelector('[data-rate]');
  const maxLossInput = host.querySelector('[data-max-loss]');
  const warmupInput = host.querySelector('[data-warmup]');
  assert.ok(countInput, 'count input must exist');
  assert.ok(rateInput, 'rate input must exist');
  assert.ok(maxLossInput, 'max-loss input must exist');
  assert.ok(warmupInput, 'warmup input must exist');

  countInput.value = '10000';
  rateInput.value = '10000';
  maxLossInput.value = '5';
  warmupInput.value = '2000';

  // Click the kernel ucast run button
  const kernelBtn = host.querySelector('[data-run-ucast="kernel"]');
  assert.ok(kernelBtn, 'kernel run button must exist');
  kernelBtn.click();

  assert.ok(capturedBody, 'onRun should have been called');
  assert.equal(capturedBody.count, 10000, 'count should be 10000');
  assert.equal(capturedBody.rate, 10000, 'rate should be 10000');
  assert.equal(capturedBody.warmup, 2000, 'warmup should be 2000 (from input)');
  assert.equal(capturedBody.max_loss_pct, 5, 'max_loss_pct should be 5');

  panel.dispose();
});

// ─── Test 5: Log ring holds full history, display is single line ────────────
test('log ring retains full history but display shows only one line', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  const statusEl = host.querySelector('[data-status]');
  assert.ok(statusEl, 'status element must exist');

  // Push many messages
  panel.setStatus('line 1');
  panel.setStatus('line 2');
  panel.setStatus('line 3');
  panel.setStatus('line 4');
  panel.setStatus('line 5');

  // The underlying ring should have all 5
  const log = panel.opsLog();
  assert.ok(log.length >= 5, `ring should have at least 5 entries, got ${log.length}`);

  // The DISPLAYED text should be only the most recent line (single row)
  const displayed = statusEl.textContent.trim();
  const lines = displayed.split('\n').filter(Boolean);
  assert.equal(lines.length, 1, `display should show 1 line, got ${lines.length}: ${JSON.stringify(displayed)}`);
  assert.ok(displayed.includes('line 5'), 'displayed line should be the most recent');

  panel.dispose();
});

// ─── Test 6: Section label says 'Targets' and is centred ────────────────────
test('target section label says Targets and is centred', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  const targetBlock = host.querySelector('[data-target-block]');
  assert.ok(targetBlock, 'target block must exist');
  const sectionLabel = targetBlock.querySelector('.cp-section');
  assert.ok(sectionLabel, 'section label must exist');
  assert.equal(sectionLabel.textContent.trim(), 'Targets', 'label should say Targets');
  // The row containing the label should have center class
  const row = sectionLabel.closest('.row');
  assert.ok(row, 'label should be in a row');
  assert.ok(row.classList.contains('center'), 'row should have center class for alignment');
  panel.dispose();
});

// ─── Test 7: Test Latency section is foldable ───────────────────────────────
test('Test Latency section is foldable and defaults to collapsed', () => {
  const host = freshHost();
  const panel = mountControls(host, {});
  const foldBtn = host.querySelector('[data-fold-latency]');
  assert.ok(foldBtn, 'Test Latency fold toggle must exist');
  const latencyContent = host.querySelector('[data-latency-content]');
  assert.ok(latencyContent, 'latency content section must exist');

  // Should default to collapsed (hidden)
  assert.equal(latencyContent.style.display, 'none', 'latency params should be hidden by default');

  // Click to expand
  foldBtn.click();
  assert.notEqual(latencyContent.style.display, 'none', 'latency params should show after click');

  // Click to collapse again
  foldBtn.click();
  assert.equal(latencyContent.style.display, 'none', 'latency params should hide on second click');

  panel.dispose();
});
