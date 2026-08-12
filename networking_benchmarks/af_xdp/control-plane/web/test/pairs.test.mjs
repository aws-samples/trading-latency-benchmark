// Target-set resolution tests. The table mirrors backend/pairs_test.go: the UI
// label must agree with what the backend will actually run, so any divergence
// between the two resolvers is a bug in whichever one drifted.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolvePairs, countPairs, scopeDescription, prunedTargets } from '../src/lib/pairs.js';

// fleet(n) builds n online nodes i-1..i-n.
const fleet = (n) =>
  Array.from({ length: n }, (_, i) => ({
    instance_id: `i-${i + 1}`,
    private_ip: `10.0.0.${i + 1}`,
    online: true,
  }));

const pairsOf = (res) =>
  res.pairs.map(([s, d]) => `${s.instance_id}>${d.instance_id}`).sort();

test('empty target set is the full mesh, scope ignored', () => {
  const four = fleet(4);
  for (const scope of ['among', 'fanout', 'fanin', '']) {
    const r = resolvePairs(four, [], scope);
    assert.equal(r.error, null, `scope ${scope} should not error`);
    assert.deepEqual(pairsOf(r), [
      'i-1>i-2', 'i-1>i-3', 'i-1>i-4',
      'i-2>i-1', 'i-2>i-3', 'i-2>i-4',
      'i-3>i-1', 'i-3>i-2', 'i-3>i-4',
      'i-4>i-1', 'i-4>i-2', 'i-4>i-3',
    ]);
  }
});

test('among selects only between the chosen nodes', () => {
  const r = resolvePairs(fleet(4), ['i-1', 'i-2', 'i-3'], 'among');
  assert.deepEqual(pairsOf(r), [
    'i-1>i-2', 'i-1>i-3', 'i-2>i-1', 'i-2>i-3', 'i-3>i-1', 'i-3>i-2',
  ]);
});

test('fanout reaches every online node from each selected node', () => {
  const r = resolvePairs(fleet(4), ['i-1'], 'fanout');
  assert.deepEqual(pairsOf(r), ['i-1>i-2', 'i-1>i-3', 'i-1>i-4']);
});

test('fanin measures from every online node to the selected nodes', () => {
  const r = resolvePairs(fleet(4), ['i-3'], 'fanin');
  assert.deepEqual(pairsOf(r), ['i-1>i-3', 'i-2>i-3', 'i-4>i-3']);
});

test('among with one node resolves to nothing and reports it', () => {
  const r = resolvePairs(fleet(4), ['i-1'], 'among');
  assert.equal(r.pairs.length, 0);
  assert.ok(r.error, 'a zero-pair resolution must carry an error for the UI to show');
});

test('offline and unknown nodes are skipped, not measured', () => {
  const nodes = fleet(4);
  nodes[2].online = false; // i-3 offline
  const r = resolvePairs(nodes, ['i-1', 'i-2', 'i-3', 'i-ghost'], 'among');
  assert.deepEqual(pairsOf(r), ['i-1>i-2', 'i-2>i-1']);
  assert.deepEqual(r.skipped.sort(), ['i-3', 'i-ghost']);
});

test('a selection that resolves to nothing never widens to the full mesh', () => {
  // Guards the same trap the Go resolver had: asking for only unknown nodes
  // must fail, not silently run every pair in the fleet.
  const r = resolvePairs(fleet(4), ['i-ghost', 'i-phantom'], 'among');
  assert.equal(r.pairs.length, 0, 'must not fall back to the full mesh');
  assert.ok(r.error);
});

test('duplicate ids do not duplicate pairs', () => {
  const r = resolvePairs(fleet(4), ['i-1', 'i-2', 'i-1'], 'among');
  assert.deepEqual(pairsOf(r), ['i-1>i-2', 'i-2>i-1']);
});

test('no node ever measures to itself', () => {
  for (const scope of ['among', 'fanout', 'fanin']) {
    const r = resolvePairs(fleet(5), ['i-1', 'i-2'], scope);
    for (const [s, d] of r.pairs) {
      assert.notEqual(s.instance_id, d.instance_id, `self-pair in ${scope}`);
    }
  }
});

test('countPairs agrees with resolvePairs for every scope and k', () => {
  const six = fleet(6);
  for (const scope of ['among', 'fanout', 'fanin']) {
    for (let k = 0; k <= 4; k++) {
      const ids = Array.from({ length: k }, (_, i) => `i-${i + 1}`);
      const r = resolvePairs(six, ids, scope);
      assert.equal(
        countPairs(6, k, scope),
        r.pairs.length,
        `countPairs(6, ${k}, ${scope}) disagrees with the resolver`,
      );
    }
  }
});

test('scopeDescription matches the backend wording', () => {
  assert.equal(scopeDescription('', 0), 'full mesh');
  assert.equal(scopeDescription('among', 0), 'full mesh');
  assert.equal(scopeDescription('among', 3), 'among 3 selected nodes');
  assert.equal(scopeDescription('fanout', 1), '1 selected node to all');
  assert.equal(scopeDescription('fanout', 2), '2 selected nodes to all');
  assert.equal(scopeDescription('fanin', 1), 'all to 1 selected node');
  assert.equal(scopeDescription('fanin', 3), 'all to 3 selected nodes');
});

test('prunedTargets drops ids no longer in the fleet', () => {
  // A terminated node must not silently keep scoping runs.
  const nodes = fleet(3);
  const kept = prunedTargets(new Set(['i-1', 'i-2', 'i-gone']), nodes);
  assert.deepEqual([...kept].sort(), ['i-1', 'i-2']);
});

test('prunedTargets drops nodes that went offline', () => {
  const nodes = fleet(3);
  nodes[1].online = false;
  const kept = prunedTargets(new Set(['i-1', 'i-2']), nodes);
  assert.deepEqual([...kept].sort(), ['i-1']);
});

test('prunedTargets returns a new set and leaves the input alone', () => {
  const nodes = fleet(2);
  const input = new Set(['i-1', 'i-gone']);
  const kept = prunedTargets(input, nodes);
  assert.notEqual(kept, input);
  assert.equal(input.size, 2, 'input set must not be mutated');
});
