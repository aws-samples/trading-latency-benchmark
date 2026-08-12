// Preset resolution: a preset NAME must become the set of node ids to select.
//
// The chip handler passed the preset name straight into `new Set(...)`, so
// pressing "PG" produced Set{'p','g'} - a set of characters matching no
// node. Nothing ticked and the pair count was nonsense.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolvePreset, PRESETS } from '../src/lib/pairs.js';

// Mirrors the live view model from live.js toFleet(): note there is NO
// instance_id, so ids are private IPs. That absence is what broke both the
// preset chips and the backend target resolution.
const fleet = [
  { private_ip: '10.0.0.1', role: 'source', az: 'eu-central-1a', cpg_name: 'cpg-a', vpc_id: 'vpc-1', region: 'eu-central-1', online: true },
  { private_ip: '10.0.0.2', role: 'replicator', az: 'eu-central-1a', cpg_name: 'cpg-a', vpc_id: 'vpc-1', region: 'eu-central-1', online: true },
  { private_ip: '10.0.0.3', role: 'destination', az: 'eu-central-1b', cpg_name: 'cpg-b', vpc_id: 'vpc-1', region: 'eu-central-1', online: true },
  { private_ip: '10.0.0.4', role: 'destination', az: 'eu-central-1b', cpg_name: 'cpg-b', vpc_id: 'vpc-2', region: 'eu-west-2', online: true },
  { private_ip: '10.0.0.5', role: 'destination', az: 'eu-central-1b', cpg_name: 'cpg-b', vpc_id: 'vpc-2', region: 'eu-west-2', online: false },
];

const ids = (r) => [...r].sort();

test('every advertised preset resolves to something usable', () => {
  for (const p of PRESETS) {
    const r = resolvePreset(p.id, fleet, null);
    assert.ok(Array.isArray(r), `${p.id} must return an array, got ${typeof r}`);
    // Every returned id must actually identify a node in the fleet.
    for (const id of r) {
      assert.ok(fleet.some((n) => n.private_ip === id), `${p.id} returned unknown id ${id}`);
    }
  }
});

test('a preset never returns single characters', () => {
  // The exact original bug: new Set("pg") -> {'p','g'}.
  const r = resolvePreset('pg', fleet, null);
  for (const id of r) {
    assert.ok(id.length > 2, `preset returned a character fragment: ${JSON.stringify(id)}`);
  }
});

test('all selects every ONLINE node only', () => {
  const r = resolvePreset('all', fleet, null);
  assert.deepEqual(ids(r), ['10.0.0.1', '10.0.0.2', '10.0.0.3', '10.0.0.4']);
  assert.ok(!r.includes('10.0.0.5'), 'an offline node cannot be a run target');
});

test('clear selects nothing', () => {
  assert.deepEqual(resolvePreset('clear', fleet, null), []);
});

test('PG uses the anchor node group', () => {
  assert.deepEqual(ids(resolvePreset('pg', fleet, '10.0.0.3')), ['10.0.0.3', '10.0.0.4']);
  assert.deepEqual(ids(resolvePreset('pg', fleet, '10.0.0.1')), ['10.0.0.1', '10.0.0.2']);
});

test('same AZ, VPC and region group on their own attribute', () => {
  assert.deepEqual(ids(resolvePreset('az', fleet, '10.0.0.1')), ['10.0.0.1', '10.0.0.2']);
  assert.deepEqual(ids(resolvePreset('vpc', fleet, '10.0.0.1')), ['10.0.0.1', '10.0.0.2', '10.0.0.3']);
  assert.deepEqual(ids(resolvePreset('region', fleet, '10.0.0.4')), ['10.0.0.4']);
});

test('with no anchor, tied groups resolve in fleet order', () => {
  // In this fleet cpg-a and cpg-b both have two ONLINE members (10.0.0.5 is
  // offline), so this pins the tie-break rather than the largest-group rule.
  assert.deepEqual(ids(resolvePreset('pg', fleet, null)), ['10.0.0.1', '10.0.0.2']);
  assert.deepEqual(ids(resolvePreset('pg', fleet, null)), ids(resolvePreset('pg', fleet, null)),
    'repeated presses must not wander');
});

test('with no anchor a preset picks the genuinely largest group', () => {
  const skewed = [
    { private_ip: '10.0.0.1', cpg_name: 'cpg-a', online: true },
    { private_ip: '10.0.0.2', cpg_name: 'cpg-b', online: true },
    { private_ip: '10.0.0.3', cpg_name: 'cpg-b', online: true },
    { private_ip: '10.0.0.4', cpg_name: 'cpg-b', online: true },
  ];
  assert.deepEqual(ids(resolvePreset('pg', skewed, null)), ['10.0.0.2', '10.0.0.3', '10.0.0.4']);
});

test('an offline anchor still resolves via its group', () => {
  // The node itself stays excluded because it cannot be measured.
  const r = resolvePreset('pg', fleet, '10.0.0.5');
  assert.ok(!r.includes('10.0.0.5'));
  assert.deepEqual(ids(r), ['10.0.0.3', '10.0.0.4']);
});

test('an unknown preset returns an empty selection rather than throwing', () => {
  assert.deepEqual(resolvePreset('nonsense', fleet, null), []);
});

test('a preset ignores nodes missing the attribute', () => {
  const partial = [
    { private_ip: '10.0.0.1', cpg_name: 'cpg-a', online: true },
    { private_ip: '10.0.0.2', online: true },              // no cpg_name
    { private_ip: '10.0.0.3', cpg_name: 'unknown', online: true }, // sentinel
  ];
  const r = resolvePreset('pg', partial, '10.0.0.1');
  assert.deepEqual(ids(r), ['10.0.0.1'], 'missing/unknown attributes must not group together');
});

test('preset order matches the control panel', () => {
  // The panel markup and this list must agree, or the chips resolve to the
  // wrong grouping after a reorder.
  assert.deepEqual(PRESETS.map((p) => p.id), ['pg', 'vpc', 'az', 'region', 'all']);
});
