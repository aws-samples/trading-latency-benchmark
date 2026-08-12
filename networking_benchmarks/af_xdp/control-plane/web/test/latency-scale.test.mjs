// Cross-region pairs are millisecond-scale and must not participate in the
// latency colour scale at all - they render grey. The scale is computed over
// intra-region cells only, so microsecond differences stay visible.
import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { latencyColor, cellColor, CROSS_REGION_COLOR, latencyRange, isCrossRegion }
  from '../src/lib/2d/palette.js';

function rgb(css) {
  const m = /rgb\((\d+),\s*(\d+),\s*(\d+)\)/.exec(css);
  assert.ok(m, `not an rgb() colour: ${css}`);
  return [Number(m[1]), Number(m[2]), Number(m[3])];
}
// The green stop is [57,211,83], the red stop [248,81,73]: r - g orders the ramp.
const heat = (css) => { const [r, g] = rgb(css); return r - g; };

// Shape of the real 26-node fleet: eu-central-1 plus three eu-west-2 nodes.
const NODES = [
  { instance_id: 'a', region: 'eu-central-1' },
  { instance_id: 'b', region: 'eu-central-1' },
  { instance_id: 'c', region: 'eu-west-2' },
];
// Intra-region cells are 23-179us; the cross-region ones are ~12ms.
const CELLS = [
  { a: NODES[0], b: NODES[1], p50: 23 },
  { a: NODES[0], b: NODES[1], p50: 72 },
  { a: NODES[1], b: NODES[0], p50: 179 },
  { a: NODES[0], b: NODES[2], p50: 11355 },
  { a: NODES[2], b: NODES[1], p50: 11990 },
];

describe('isCrossRegion', () => {
  test('compares the region of both endpoints', () => {
    assert.equal(isCrossRegion(NODES[0], NODES[1]), false);
    assert.equal(isCrossRegion(NODES[0], NODES[2]), true);
    assert.equal(isCrossRegion(NODES[2], NODES[0]), true);
  });

  test('an unknown region is not treated as cross-region', () => {
    // Missing metadata must not silently grey out a real intra-region pair.
    assert.equal(isCrossRegion({ region: 'eu-central-1' }, {}), false);
    assert.equal(isCrossRegion({}, {}), false);
  });
});

describe('latencyRange excludes cross-region cells', () => {
  test('min and max come from intra-region cells only', () => {
    const { mn, mx } = latencyRange(CELLS);
    assert.equal(mn, 23, 'min should be the fastest intra-region cell');
    assert.equal(mx, 179, 'max must ignore the 12ms cross-region cells');
  });

  test('all-cross-region input yields a degenerate but safe range', () => {
    const { mn, mx } = latencyRange(CELLS.filter((c) => isCrossRegion(c.a, c.b)));
    assert.ok(Number.isFinite(mn) && Number.isFinite(mx), `got ${mn}..${mx}`);
  });

  test('null and zero p50 are ignored', () => {
    const { mn, mx } = latencyRange([
      { a: NODES[0], b: NODES[1], p50: null },
      { a: NODES[0], b: NODES[1], p50: 0 },
      { a: NODES[0], b: NODES[1], p50: 40 },
      { a: NODES[0], b: NODES[1], p50: 90 },
    ]);
    assert.equal(mn, 40);
    assert.equal(mx, 90);
  });
});

describe('cellColor', () => {
  const { mn, mx } = latencyRange(CELLS);

  test('a cross-region cell is grey regardless of its value', () => {
    assert.equal(cellColor(11990, mn, mx, true), CROSS_REGION_COLOR);
    assert.equal(cellColor(23, mn, mx, true), CROSS_REGION_COLOR);
  });

  test('an intra-region cell keeps the latency ramp', () => {
    assert.equal(cellColor(23, mn, mx, false), latencyColor(23, mn, mx));
  });

  test('intra-region values now spread across the ramp', () => {
    // With the cross-region outliers excluded the scale is 23-179us, so these
    // must be clearly distinct rather than all sitting on the green stop.
    const h = [23, 72, 179].map((v) => heat(cellColor(v, mn, mx, false)));
    const full = heat(latencyColor(mx, mn, mx)) - heat(latencyColor(mn, mn, mx));
    assert.ok(h[1] - h[0] > 0.2 * full, `23 vs 72 too close (${h[0]} vs ${h[1]})`);
    assert.ok(h[2] - h[1] > 0.2 * full, `72 vs 179 too close (${h[1]} vs ${h[2]})`);
  });

  test('an out-of-scale intra-region value clamps to valid channels', () => {
    for (const v of [1, 99999]) {
      for (const c of rgb(cellColor(v, mn, mx, false))) {
        assert.ok(Number.isFinite(c) && c >= 0 && c <= 255, `bad channel ${c} for ${v}`);
      }
    }
  });
});
