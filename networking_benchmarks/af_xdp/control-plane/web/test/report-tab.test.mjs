// Regressions for the report tab.
//
// The report rendered as unstyled plain text because the stylesheet injection
// was nested inside a "no .report-content yet" branch, while liveRerender()
// (driven by the SSE init) created that content first - so the branch never ran.
// The injection must not depend on render order.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const app = readFileSync(new URL('../src/App.svelte', import.meta.url), 'utf8');
const controls = readFileSync(new URL('../src/lib/controls.js', import.meta.url), 'utf8');

test('report CSS injection is not gated on content already existing', () => {
  // The old shape was:
  //   if (open && el && !el.querySelector('.report-content')) { ...inject CSS... }
  // which loses the race against the first data-driven render.
  const guard = /!\s*reportOverlayEl\.querySelector\('\.report-content'\)[\s\S]{0,400}?REPORT_CSS/;
  assert.ok(!guard.test(app),
    'REPORT_CSS must not be injected inside the "no content yet" branch');
});

test('the stylesheet is injected idempotently and lands in <head>', () => {
  assert.match(app, /function ensureReportCss\(\)/, 'a dedicated injector must exist');
  const fn = app.slice(app.indexOf('function ensureReportCss()'),
    app.indexOf('function rerenderReportOverlay'));
  assert.match(fn, /getElementById\('afxdp-report-css'\)/, 'must be idempotent by id');
  assert.match(fn, /document\.head\.appendChild/, 'global styles belong in <head>');
  assert.match(fn, /REPORT_CSS/);
});

test('the render function ensures the CSS before it writes any body', () => {
  // Positional windows are brittle; assert the ORDER inside the function body.
  const start = app.indexOf('function rerenderReportOverlay');
  const end = app.indexOf('\n  function ', start + 10);
  const body = app.slice(start, end > start ? end : start + 2000);
  const css = body.indexOf('ensureReportCss()');
  const write = body.indexOf('buildCombinedReportBody');
  assert.ok(css > 0, 'the render function must guarantee the stylesheet itself');
  assert.ok(write > 0, 'and it is the thing that writes the body');
  assert.ok(css < write, 'the stylesheet must be in place before the body is written');
});

test('a render before the element or data is ready retries instead of dropping', () => {
  // This is why the tab needed a manual refresh: the element is bound after the
  // {#if} flushes, and the first SSE event can beat it.
  const start = app.indexOf('function rerenderReportOverlay');
  const cond = app.indexOf('!reportOverlayEl || !getReportViews().length', start);
  assert.ok(cond > 0, 'both not-ready cases must be handled together');
  // Take the branch body only, so the assertion cannot be satisfied by a retry
  // that sits somewhere else in the function.
  const braceOpen = app.indexOf('{', cond);
  const branch = app.slice(braceOpen, app.indexOf('\n    }', braceOpen));
  const schedule = branch.indexOf('setTimeout');
  const bail = branch.indexOf('return');
  assert.ok(schedule > 0, 'the not-ready branch itself must schedule another attempt');
  assert.ok(bail > schedule,
    'it must schedule BEFORE returning, otherwise the render is dropped for good');
});

test('view controls carry no selected state', () => {
  const block = controls.slice(controls.indexOf('const renderViewButtons'),
    controls.indexOf('tzSel.addEventListener'));
  assert.ok(!/classList\.toggle\('on'/.test(block),
    'a control that only opens a tab must not paint itself as active');
  assert.ok(!/activeViewKind\s*=/.test(block),
    'no state to track: the panel does not "have" a chosen kind');
  // Statefulness is limited to enabled/disabled, which reflects DATA not selection.
  assert.match(block, /disabled/, 'a kind without data must be disabled');
  assert.match(block, /target="_blank"/, 'and an enabled one opens a tab');
});

test('Targets folds like Test Latency', () => {
  assert.match(controls, /data-fold-targets/, 'Targets needs a fold caret');
  assert.match(controls, /data-targets-content/, 'Targets needs a foldable body');
  // The attribute is valueless, so there is no closing quote after it.
  assert.match(controls, /data-targets-content style="display:none"/,
    'Targets starts collapsed, like Test Latency');
  // Folding lives in one shared module now, so assert both sections route
  // through it rather than re-checking inlined handler source.
  assert.match(controls, /makeFoldable\(/, 'sections must use the shared fold module');
  assert.match(controls, /'cp-targets'/, 'Targets needs a persistent fold key');
  assert.match(controls, /'cp-latency'/, 'Test Latency needs a persistent fold key');
});

test('the foldable Targets body encloses the whole block', () => {
  // The preset chips, cancel and scope select all live inside the fold - folding
  // a header that leaves its controls visible would be worse than not folding.
  const open = controls.indexOf('data-targets-content');
  const scope = controls.indexOf('data-scope');
  const tip = controls.indexOf('data-target-tip');
  const preset = controls.indexOf('data-preset=');
  for (const [name, i] of [['tip', tip], ['presets', preset], ['scope', scope]]) {
    assert.ok(i > open, `${name} must sit inside the foldable body`);
  }
});
