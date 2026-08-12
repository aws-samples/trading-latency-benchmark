// 3D target-sphere contract.
//
// The full 3D scene needs a WebGL context, so this asserts the two things that
// actually break silently: the sphere must be wired to onToggleTarget and it
// must re-enable pointer events. Its parent .node-label sets
// pointer-events:none, so without an explicit auto the sphere renders perfectly
// and is simply unclickable.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const css = readFileSync(new URL('../src/app.css', import.meta.url), 'utf8');
const src = readFileSync(new URL('../src/lib/topology3d.js', import.meta.url), 'utf8');

// Extract a rule body by selector.
function rule(sel) {
  const i = css.indexOf(sel);
  assert.ok(i >= 0, `missing CSS rule: ${sel}`);
  return css.slice(i, css.indexOf('}', i));
}

test('the label suppresses pointer events but the sphere re-enables them', () => {
  assert.match(rule('.node-label {'), /pointer-events:\s*none/,
    'label must not intercept clicks meant for the scene');
  assert.match(rule('.node-label .target-sphere {'), /pointer-events:\s*auto/,
    'without auto the sphere is rendered but unclickable');
});

test('the sphere is round, small, and positioned top-left of the node', () => {
  const r = rule('.node-label .target-sphere {');
  assert.match(r, /border-radius:\s*50%/, 'must be a circle, not a square');
  assert.match(r, /position:\s*absolute/);
  assert.match(r, /top:\s*-?\d+px/, 'offset vertically out of the label');
  assert.match(r, /left:\s*-?\d+px/, 'offset horizontally out of the label');
  // Negative offsets put it above-left of the centred label.
  const top = Number(/top:\s*(-?\d+)px/.exec(r)[1]);
  const left = Number(/left:\s*(-?\d+)px/.exec(r)[1]);
  assert.ok(top < 0, `top must be negative (above), got ${top}`);
  assert.ok(left < 0, `left must be negative (to the left), got ${left}`);
});

test('it reads as a sphere, not a flat dot', () => {
  assert.match(rule('.node-label .target-sphere {'), /radial-gradient/,
    'a radial gradient is what makes it look spherical');
});

test('the contour is permanently visible, not hover-gated', () => {
  const r = rule('.node-label .target-sphere {');
  assert.match(r, /opacity:\s*1/, 'the sphere must not start invisible');
  assert.match(r, /border:\s*2px solid/, 'it needs a contour to be visible at rest');
  assert.ok(!css.includes('.target-sphere.visible'),
    'a .visible gate would mean the contour is conditional again');
});

test('unselected is grey and selected is the same gold as 2D', () => {
  const base = rule('.node-label .target-sphere {');
  assert.match(base, /#8b949e|#6e7681|#484f58/, 'unselected must be grey');
  const on = rule('.node-label .target-sphere.checked');
  assert.match(on, /#ffd700/, 'selected must be the 2D gold (#ffd700)');
});

test('the sphere toggles the target set and stops propagation', () => {
  // stopPropagation matters for the same reason as the 2D checkbox: the click
  // must not also select/pin the node underneath.
  const i = src.indexOf("box.addEventListener('click'");
  assert.ok(i > 0, 'sphere must have a click handler');
  const handler = src.slice(i, i + 400);
  assert.match(handler, /stopPropagation/, 'must not also trigger the node body');
  assert.match(handler, /onToggleTarget/, 'must toggle the target set');
});

test('every node gets a sphere keyed the same way the target set is', () => {
  // targetIds holds whatever idOf()/nodes.js uses, which is instance_id when
  // present and private_ip otherwise. A mismatch here is exactly the bug that
  // made the 2D presets tick nothing.
  assert.match(src, /dataset\.targetSphere\s*=\s*n\.instance_id\s*\|\|\s*n\.private_ip/);
  assert.match(src, /targetSpheres\[i\]\s*=\s*box/, 'spheres must be indexed per node');
  assert.match(src, /classList\.toggle\('checked',\s*targeted\)/,
    'render must reflect membership on the sphere');
});

test('shift really pans, so the legend is not lying', () => {
  // A legend that claims shift+drag pans while OrbitControls still has PAN on
  // the right button only is worse than an unclear one.
  assert.match(src, /THREE\.MOUSE\.PAN/, 'shift must remap the left button to PAN');
  assert.match(src, /addEventListener\('keydown', onKeyShift\)/);
  assert.match(src, /addEventListener\('keyup', onKeyShift\)/);
  assert.match(src, /shift\+drag<\/b> = pan/, 'legend must say shift+drag');
  assert.ok(!/right-drag<\/b> = pan/.test(src), 'stale right-drag legend must be gone');
});

test('the shift listeners are removed on dispose', () => {
  // The 3D view remounts on every live update, so window listeners that are
  // never removed accumulate and mutate a disposed controls object.
  const i = src.indexOf('dispose() {');
  const body = src.slice(i, i + 500);
  assert.match(body, /removeEventListener\('keydown', onKeyShift\)/);
  assert.match(body, /removeEventListener\('keyup', onKeyShift\)/);
});
