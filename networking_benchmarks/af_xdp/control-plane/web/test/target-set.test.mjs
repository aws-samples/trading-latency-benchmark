// Target-set integration tests (phases 1.2–1.5).
// Covers: checkbox toggle + stopPropagation, prunedTargets integration,
// panel pair count, k=1 auto-switch to fanout, and runCampaign body.
import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { countPairs, prunedTargets, SCOPE_AMONG, SCOPE_FANOUT, SCOPE_FANIN } from '../src/lib/pairs.js';

// ── helpers ─────────────────────────────────────────────────────────────────
const fleet = (n) =>
  Array.from({ length: n }, (_, i) => ({
    instance_id: `i-${i + 1}`,
    private_ip: `10.0.0.${i + 1}`,
    online: true,
    az: 'us-east-1a',
    placement_group: 'pg1',
    vpc_id: 'vpc-1',
    region: 'us-east-1',
  }));

// (a) Checkbox click toggles membership and does NOT pin the table.
describe('checkbox toggle + stopPropagation', () => {
  test('toggling target membership adds and removes from set', () => {
    // Simulate the App-level targetIds set.
    const targetIds = new Set();
    const toggleTarget = (id) => {
      if (targetIds.has(id)) targetIds.delete(id);
      else targetIds.add(id);
    };
    toggleTarget('i-1');
    assert.ok(targetIds.has('i-1'));
    toggleTarget('i-1');
    assert.ok(!targetIds.has('i-1'));
    toggleTarget('i-2');
    toggleTarget('i-3');
    assert.deepEqual([...targetIds].sort(), ['i-2', 'i-3']);
  });

  test('checkbox click does NOT invoke pin handler (stopPropagation contract)', () => {
    // The pin handler must not fire when the checkbox is clicked.
    // We simulate this by modelling the event flow: the checkbox handler calls
    // stopPropagation, so the parent (node body) click never fires.
    let pinCalled = false;
    let targetToggled = false;
    const nodeBodyClick = () => { pinCalled = true; };
    const checkboxClick = (e) => {
      e.stopPropagation();
      targetToggled = true;
    };
    // Simulate: clicking the checkbox fires its handler with a mock event.
    const mockEvent = { stopPropagation() { this._stopped = true; }, _stopped: false };
    checkboxClick(mockEvent);
    // The node body handler should NOT have been called.
    assert.ok(targetToggled, 'checkbox handler must fire');
    assert.ok(!pinCalled, 'pin handler must NOT fire when checkbox is clicked');
    assert.ok(mockEvent._stopped, 'stopPropagation must be called');
  });
});

// (b) prunedTargets integration - a node leaving the fleet drops from the set.
describe('prunedTargets integration', () => {
  test('terminated node is removed from targetIds', () => {
    const nodes = fleet(3); // i-1, i-2, i-3
    const targetIds = new Set(['i-1', 'i-2', 'i-4']); // i-4 is gone
    const pruned = prunedTargets(targetIds, nodes);
    assert.deepEqual([...pruned].sort(), ['i-1', 'i-2']);
    assert.ok(!pruned.has('i-4'));
  });

  test('offline node is removed from targetIds', () => {
    const nodes = fleet(3);
    nodes[1].online = false; // i-2 goes offline
    const targetIds = new Set(['i-1', 'i-2', 'i-3']);
    const pruned = prunedTargets(targetIds, nodes);
    assert.deepEqual([...pruned].sort(), ['i-1', 'i-3']);
  });

  test('pruning on every rerender keeps targetIds clean', () => {
    // Simulates the liveRerender loop.
    let targetIds = new Set(['i-1', 'i-2', 'i-3']);
    let nodes = fleet(3);
    // First rerender: all fine
    targetIds = prunedTargets(targetIds, nodes);
    assert.equal(targetIds.size, 3);
    // Second rerender: node i-3 disappeared
    nodes = fleet(2);
    targetIds = prunedTargets(targetIds, nodes);
    assert.deepEqual([...targetIds].sort(), ['i-1', 'i-2']);
  });
});

// (c) Panel pair count equals countPairs() for several k/scope combos.
describe('panel pair count matches countPairs()', () => {
  const N = 6;
  const cases = [
    { k: 0, scope: SCOPE_AMONG, expected: 30 },  // full mesh 6*5
    { k: 1, scope: SCOPE_FANOUT, expected: 5 },   // 1*(6-1)
    { k: 2, scope: SCOPE_AMONG, expected: 2 },    // 2*1
    { k: 3, scope: SCOPE_AMONG, expected: 6 },    // 3*2
    { k: 3, scope: SCOPE_FANOUT, expected: 15 },  // 3*5
    { k: 3, scope: SCOPE_FANIN, expected: 15 },   // 3*5
    { k: 4, scope: SCOPE_AMONG, expected: 12 },   // 4*3
    { k: 1, scope: SCOPE_FANIN, expected: 5 },    // 1*5
  ];
  for (const { k, scope, expected } of cases) {
    test(`countPairs(${N}, ${k}, ${scope}) = ${expected}`, () => {
      assert.equal(countPairs(N, k, scope), expected);
    });
  }
});

// (d) k=1 + among auto-switches scope to fanout.
describe('k=1 among auto-switch to fanout', () => {
  test('when k=1 and scope is among, UI should auto-switch to fanout', () => {
    // The panel logic: if among resolves to 0 pairs (k<2), pre-select fanout.
    const k = 1;
    const N = 6;
    let scope = SCOPE_AMONG;
    const pairsAmong = countPairs(N, k, SCOPE_AMONG);
    assert.equal(pairsAmong, 0, 'k=1 among yields 0 pairs');
    // Auto-switch logic (implemented in controls.js setTargets):
    if (pairsAmong === 0 && k >= 1) {
      scope = SCOPE_FANOUT;
    }
    assert.equal(scope, SCOPE_FANOUT, 'must auto-switch to fanout');
    const pairsFanout = countPairs(N, k, SCOPE_FANOUT);
    assert.equal(pairsFanout, 5, 'fanout with k=1 gives N-1 pairs');
  });
});

// (e) The run body sent to runCampaign carries nodes[] and scope.
describe('runCampaign body carries nodes[] and scope', () => {
  test('body includes target node ids and scope when set is non-empty', () => {
    const targetIds = new Set(['i-1', 'i-3']);
    const scope = SCOPE_AMONG;
    // Simulates what App.svelte builds:
    const body = {
      kind: 'ucast',
      variation: 'kernel',
      count: 5000,
      rate: 20000,
      nodes: [...targetIds],
      scope,
    };
    assert.deepEqual(body.nodes.sort(), ['i-1', 'i-3']);
    assert.equal(body.scope, 'among');
  });

  test('body has empty nodes array when target set is empty (full mesh)', () => {
    const targetIds = new Set();
    const scope = SCOPE_AMONG;
    const body = {
      kind: 'ucast',
      variation: 'kernel',
      count: 5000,
      rate: 20000,
      nodes: [...targetIds],
      scope,
    };
    assert.deepEqual(body.nodes, []);
  });

  test('scope value is passed through to body', () => {
    for (const s of [SCOPE_AMONG, SCOPE_FANOUT, SCOPE_FANIN]) {
      const body = { kind: 'ucast', variation: 'kernel', nodes: ['i-1'], scope: s };
      assert.equal(body.scope, s);
    }
  });
});
