// The contour padding is declared TWICE - the geometry in contours.js and a copy
// in layout.js used to floor the group separation. They are commented "sync with
// contours.js", which is exactly the kind of pairing that silently drifts, so
// this pins them together.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const contours = readFileSync(new URL('../src/lib/2d/contours.js', import.meta.url), 'utf8');
const layout = readFileSync(new URL('../src/lib/2d/layout.js', import.meta.url), 'utf8');

function padsFromContours() {
  const m = /PAD_BASE = (\d+), STEP = (\d+)/.exec(contours);
  assert.ok(m, 'PAD_BASE/STEP must be declared in contours.js');
  const base = Number(m[1]), step = Number(m[2]);
  // contourDefs order is az, vpc, region, account -> pad = base + step*tier
  return { base, step, az: base, vpc: base + step, region: base + 2 * step, account: base + 3 * step };
}

test('layout pads match the contour geometry', () => {
  const p = padsFromContours();
  const m = /const pads = \[(\d+), (\d+), (\d+), (\d+), (\d+)\]/.exec(layout);
  assert.ok(m, 'layout.js must declare the pads array');
  const [, account, region, vpc, az] = m.map(Number);
  assert.equal(account, p.account, 'account pad drifted from contours.js');
  assert.equal(region, p.region, 'region pad drifted');
  assert.equal(vpc, p.vpc, 'vpc pad drifted');
  assert.equal(az, p.az, 'az pad drifted');
});

test('nested borders are separated and the inner margin is non-trivial', () => {
  const p = padsFromContours();
  assert.ok(p.step >= 20, `distance between nested borders is ${p.step}, want >= 20`);
  assert.ok(p.base >= 20, `inner AZ margin is ${p.base}, want >= 20`);
  // Strictly increasing outward, or contours would touch or invert.
  assert.ok(p.az < p.vpc && p.vpc < p.region && p.region < p.account);
});

test('the separation floor keeps sibling contours from overlapping', () => {
  // gaps = max(SEP*ratio, 2*pad + 2*R + 2*DECOR + LABEL_OVERHANG). Beyond the pad
  // and node radius the floor must also clear the badges a node draws outside its
  // circle and the label a contour draws above its border.
  assert.match(layout, /Math\.max\(SEP \* ratio\[d\], 2 \* p \+ 2 \* R \+ 2 \* DECOR \+ LABEL_OVERHANG\)/,
    'the decoration-aware floor must remain, otherwise contours overlap');
  assert.match(layout, /const DECOR = \d+, LABEL_OVERHANG = \d+;/,
    'DECOR and LABEL_OVERHANG must stay declared in sync with contours.js');
});
