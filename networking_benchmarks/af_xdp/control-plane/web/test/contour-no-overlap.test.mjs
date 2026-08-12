// Contour boxes must never overlap on a real fleet. Synthetic fixtures missed
// this: boxes are padded from the rendered extent, which includes the badges a
// node draws outside its circle and the label a contour draws above its border.
// The layout positions nodes by latency, so groups are only kept apart by the
// separation pass - these run the real layout end to end and check the result.
import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { computePositions } from '../src/lib/2d/layout.js';
import { computeContourBoxes } from '../src/lib/2d/contours.js';
import { nodeRadius } from '../src/lib/2d/palette.js';

// Decoration overhang, mirroring the CSS: .pg-badge top:-9px, .role-badge
// bottom:-9px (both centred, so they overhang sideways too) and
// .contour .label top:-11px.
const DECOR_Y = 10, DECOR_X = 18;

const W = 1600, H = 1000;

function layout(nodes, p50 = 60) {
  const N = nodes.length;
  const matrix = Array.from({ length: N }, () => Array(N).fill(null));
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    if (i !== j) matrix[i][j] = { p50, p99: p50 + 10 };
  }
  const ctx = { fleet: { nodes }, matrix, N, W, H, CX: W / 2, CY: H / 2 };
  const { positions } = computePositions(ctx);
  return { positions, boxes: computeContourBoxes({ nodes }, positions) };
}

const intersect = (a, b) =>
  !(a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top);
const contains = (p, c) =>
  p.left <= c.left && p.top <= c.top && p.right >= c.right && p.bottom >= c.bottom;
const short = (k) => String(k).split('\u0001').pop();

// Assert the three invariants the 2D view depends on.
function assertNoOverlap(nodes, boxes, positions, label) {
  const byTier = {};
  for (const b of boxes) (byTier[b.tier] = byTier[b.tier] || []).push(b);

  // (a) No two boxes at the same tier may intersect.
  for (const [tier, bs] of Object.entries(byTier)) {
    for (let i = 0; i < bs.length; i++) for (let j = i + 1; j < bs.length; j++) {
      assert.ok(!intersect(bs[i], bs[j]),
        `${label}: ${tier} boxes "${short(bs[i].key)}" and "${short(bs[j].key)}" intersect`);
    }
  }

  // (b) An inner box overlapping an outer box must be fully inside it - so a
  // child never escapes its parent and never crosses a foreign parent.
  const order = ['az', 'vpc', 'region', 'account'];
  for (let t = 0; t < order.length - 1; t++) {
    for (const c of byTier[order[t]] || []) {
      for (const p of byTier[order[t + 1]] || []) {
        if (!intersect(p, c)) continue;
        assert.ok(contains(p, c),
          `${label}: ${order[t]} "${short(c.key)}" is not contained by ${order[t + 1]} "${short(p.key)}"`);
      }
    }
  }

  // (d) No two node bodies may overlap. The viewport fit scales positions while
  // radii stay fixed, so this only holds if collisions are resolved last.
  for (let i = 0; i < nodes.length; i++) {
    for (let j = i + 1; j < nodes.length; j++) {
      const need = nodeRadius(nodes[i]) + nodeRadius(nodes[j]);
      const dx = positions[i].x - positions[j].x, dy = positions[i].y - positions[j].y;
      const dist = Math.hypot(dx, dy);
      assert.ok(dist >= need,
        `${label}: nodes ${nodes[i].private_ip} and ${nodes[j].private_ip} overlap `
        + `(${dist.toFixed(1)}px apart, need ${need})`);
    }
  }

  // (c) A node's rendered footprint must sit wholly inside every box it belongs
  // to and wholly outside every box it does not - so no border cuts a node.
  for (const b of boxes) {
    const members = new Set(b.nodeIndices || []);
    nodes.forEach((n, i) => {
      const r = nodeRadius(n), pos = positions[i];
      const rx = r + DECOR_X, ry = r + DECOR_Y;
      const l = pos.x - rx, rr = pos.x + rx, t = pos.y - ry, bo = pos.y + ry;
      const inside = l >= b.left && rr <= b.right && t >= b.top && bo <= b.bottom;
      const outside = rr <= b.left || l >= b.right || bo <= b.top || t >= b.bottom;
      if (members.has(i)) {
        assert.ok(inside,
          `${label}: ${b.tier} "${short(b.key)}" border cuts its own node ${n.private_ip}`);
      } else {
        assert.ok(outside,
          `${label}: node ${n.private_ip} intrudes into ${b.tier} "${short(b.key)}"`);
      }
    });
  }
}

// Build n nodes in one az of one vpc of one region.
function group(region, vpc, az, n, from = 0) {
  return Array.from({ length: n }, (_, k) => ({
    instance_id: `i-${region}-${az}-${from + k}`,
    private_ip: `10.${from + k}.0.${k + 1}`,
    region, vpc_id: vpc, az, role: k === 0 ? 'source' : 'destination',
    cpg_name: 'cpg-' + az, online: true,
  }));
}

describe('contour boxes never overlap', () => {
  // The shape that shipped broken: 23 nodes across two AZs in eu-central-1 plus
  // 3 in eu-west-2, which is the deployed 26-node cross-region fleet.
  test('the real 26-node cross-region fleet', () => {
    const nodes = [
      ...group('eu-central-1', 'vpc-a', 'eu-central-1c', 16, 0),
      ...group('eu-central-1', 'vpc-a', 'eu-central-1b', 7, 16),
      ...group('eu-west-2', 'vpc-b', 'eu-west-2a', 3, 23),
    ];
    const { positions, boxes } = layout(nodes);
    assert.ok(boxes.length >= 5, `expected several boxes, got ${boxes.length}`);
    assertNoOverlap(nodes, boxes, positions, '26-node');
  });

  test('two regions of one node each', () => {
    const nodes = [
      ...group('eu-central-1', 'vpc-a', 'eu-central-1a', 1, 0),
      ...group('eu-west-2', 'vpc-b', 'eu-west-2a', 1, 1),
    ];
    const { positions, boxes } = layout(nodes);
    assertNoOverlap(nodes, boxes, positions, 'two-single');
  });

  test('lopsided groups: 1 node beside 9', () => {
    const nodes = [
      ...group('eu-central-1', 'vpc-a', 'eu-central-1a', 1, 0),
      ...group('eu-central-1', 'vpc-a', 'eu-central-1b', 9, 1),
    ];
    const { positions, boxes } = layout(nodes);
    assertNoOverlap(nodes, boxes, positions, 'lopsided');
  });

  test('three AZs across two VPCs in two regions', () => {
    const nodes = [
      ...group('eu-central-1', 'vpc-a', 'eu-central-1a', 4, 0),
      ...group('eu-central-1', 'vpc-a', 'eu-central-1b', 4, 4),
      ...group('eu-central-1', 'vpc-c', 'eu-central-1c', 4, 8),
      ...group('eu-west-2', 'vpc-b', 'eu-west-2a', 4, 12),
    ];
    const { positions, boxes } = layout(nodes);
    assertNoOverlap(nodes, boxes, positions, 'multi');
  });

  // Cross-region latency is ~12ms against ~30us intra-region, and the layout
  // spaces nodes by latency, so a wide spread must not defeat the separation.
  test('a 500x latency spread still separates cleanly', () => {
    const nodes = [
      ...group('eu-central-1', 'vpc-a', 'eu-central-1c', 8, 0),
      ...group('eu-west-2', 'vpc-b', 'eu-west-2a', 3, 8),
    ];
    const N = nodes.length;
    const matrix = Array.from({ length: N }, () => Array(N).fill(null));
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
      if (i === j) continue;
      const cross = nodes[i].region !== nodes[j].region;
      matrix[i][j] = { p50: cross ? 11500 : 30, p99: cross ? 12000 : 40 };
    }
    const ctx = { fleet: { nodes }, matrix, N, W, H, CX: W / 2, CY: H / 2 };
    const { positions } = computePositions(ctx);
    assertNoOverlap(nodes, computeContourBoxes({ nodes }, positions), positions, 'wide-spread');
  });
});
