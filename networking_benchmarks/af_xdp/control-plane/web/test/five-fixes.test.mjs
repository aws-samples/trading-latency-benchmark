// Tests for the five UI fixes (contour overlap, fold persistence, All preset,
// heartbeat targets, cancel-run button).
import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

// ─── DOM bootstrap (browser modules read global `document`) ──────────────────
const dom = new JSDOM('<!doctype html><html><body></body></html>', {
  pretendToBeVisual: true, url: 'http://localhost/',
});
globalThis.window = dom.window;
globalThis.document = dom.window.document;
globalThis.MouseEvent = dom.window.MouseEvent;
globalThis.Element = dom.window.Element;
globalThis.HTMLElement = dom.window.HTMLElement;
globalThis.navigator ??= dom.window.navigator;
globalThis.localStorage = dom.window.localStorage;
globalThis.EventSource = class { onmessage = null; close() {} };
globalThis.ResizeObserver = class { observe() {} unobserve() {} disconnect() {} };
globalThis.Blob = class { constructor(p, o) { this.parts = p; this.options = o; } };
if (!globalThis.URL.createObjectURL) globalThis.URL.createObjectURL = () => 'blob:fake';
if (!globalThis.URL.revokeObjectURL) globalThis.URL.revokeObjectURL = () => {};

function resetDOM() { dom.window.document.body.innerHTML = ''; dom.window.localStorage.clear(); }

// ═══════════════════════════════════════════════════════════════════════════════
// (1) CONTOUR OVERLAP — geometry invariants
// ═══════════════════════════════════════════════════════════════════════════════
describe('(1) contour overlap geometry', () => {
  let computeContourBoxes, NODE_RADIUS_2D;

  test('imports', async () => {
    const c = await import('../src/lib/2d/contours.js');
    computeContourBoxes = c.computeContourBoxes;
    assert.ok(typeof computeContourBoxes === 'function',
      'contours.js must export computeContourBoxes');
    const p = await import('../src/lib/2d/palette.js');
    NODE_RADIUS_2D = p.NODE_RADIUS_2D;
  });

  function makeFleet(groups) {
    const nodes = []; const positions = [];
    let idx = 0;
    for (const g of groups) {
      for (let n = 0; n < g.count; n++) {
        nodes.push({
          index: idx, instance_id: `i-${idx}`, private_ip: `10.${idx}.0.1`,
          account: g.account || 'acct-1', region: g.region, vpc_id: g.vpc,
          az: g.az, cpg_name: g.pg || 'unknown', online: true,
        });
        positions.push({ x: g.baseX + n * 90, y: g.baseY + (n % 2) * 40 });
        idx++;
      }
    }
    return { fleet: { nodes, matrix: nodes.map(() => nodes.map(() => null)) }, positions };
  }

  function boxesIntersect(a, b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
  }
  function boxContains(outer, inner) {
    return inner.left >= outer.left && inner.right <= outer.right &&
           inner.top >= outer.top && inner.bottom <= outer.bottom;
  }

  function assertContourProperties(label, groups) {
    const { fleet, positions } = makeFleet(groups);
    const boxes = computeContourBoxes(fleet, positions);
    assert.ok(boxes.length > 0, `${label}: should produce boxes`);

    // (a) No two boxes at the SAME tier intersect.
    const byTier = {};
    for (const b of boxes) { (byTier[b.tier] = byTier[b.tier] || []).push(b); }
    for (const [tier, bs] of Object.entries(byTier)) {
      for (let i = 0; i < bs.length; i++) for (let j = i + 1; j < bs.length; j++) {
        assert.ok(!boxesIntersect(bs[i], bs[j]),
          `${label}: same-tier (${tier}) boxes "${bs[i].key}" and "${bs[j].key}" intersect`);
      }
    }
    // (b) Every child box is strictly contained in its parent box.
    const tierOrder = ['az', 'vpc', 'region', 'account'];
    for (let t = 0; t < tierOrder.length - 1; t++) {
      const children = byTier[tierOrder[t]] || [];
      const parents = byTier[tierOrder[t + 1]] || [];
      for (const child of children) {
        const parent = parents.find(p => child.key.startsWith(p.key));
        if (parent) {
          assert.ok(boxContains(parent, child),
            `${label}: child "${child.key}" (${tierOrder[t]}) not in parent "${parent.key}" (${tierOrder[t+1]})`);
        }
      }
    }
    // (c) No contour edge crosses a node body.
    for (const b of boxes) {
      for (const i of b.nodeIndices) {
        const r = NODE_RADIUS_2D;
        assert.ok(positions[i].x - r >= b.left,
          `${label}: node ${i} left crosses contour ${b.key}`);
        assert.ok(positions[i].x + r <= b.right,
          `${label}: node ${i} right crosses contour ${b.key}`);
        assert.ok(positions[i].y - r >= b.top,
          `${label}: node ${i} top crosses contour ${b.key}`);
        assert.ok(positions[i].y + r <= b.bottom,
          `${label}: node ${i} bottom crosses contour ${b.key}`);
      }
    }
  }

  test('single AZ in one VPC', () => {
    assertContourProperties('single-az', [
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-a', count: 4, baseX: 100, baseY: 100 },
    ]);
  });

  test('two AZs in one VPC', () => {
    assertContourProperties('two-azs', [
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-a', count: 3, baseX: 100, baseY: 100 },
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-b', count: 3, baseX: 500, baseY: 100 },
    ]);
  });

  test('two VPCs in one region', () => {
    assertContourProperties('two-vpcs', [
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-a', count: 3, baseX: 100, baseY: 100 },
      { region: 'us-east-1', vpc: 'vpc-2', az: 'az-b', count: 3, baseX: 700, baseY: 100 },
    ]);
  });

  test('two regions', () => {
    assertContourProperties('two-regions', [
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-a', count: 3, baseX: 100, baseY: 100 },
      { region: 'us-west-2', vpc: 'vpc-2', az: 'az-c', count: 3, baseX: 900, baseY: 100 },
    ]);
  });

  test('lopsided: 1 node vs 9 nodes', () => {
    assertContourProperties('lopsided', [
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-a', count: 1, baseX: 100, baseY: 100 },
      { region: 'us-east-1', vpc: 'vpc-1', az: 'az-b', count: 9, baseX: 500, baseY: 100 },
    ]);
  });
});

// ═══════════════════════════════════════════════════════════════════════════════
// (2) PANEL FOLD STATE persisted across re-renders
// ═══════════════════════════════════════════════════════════════════════════════
describe('(2) panel fold state persistence', () => {
  test('fold state stored in localStorage and survives remount', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host);

    const foldBtn = host.querySelector('[data-fold-targets]');
    assert.ok(foldBtn, 'fold-targets caret exists');
    foldBtn.click();
    const content = host.querySelector('[data-targets-content]');
    assert.equal(content.style.display, '', 'targets expanded after click');

    const stored = localStorage.getItem('afxdp-fold-state');
    assert.ok(stored, 'fold state stored in localStorage');
    const state = JSON.parse(stored);
    assert.equal(state['cp-targets'], false, 'targets recorded as unfolded');

    // Destroy and remount
    panel.dispose();
    const host2 = document.createElement('div'); document.body.appendChild(host2);
    const panel2 = mountControls(host2);
    const content2 = host2.querySelector('[data-targets-content]');
    assert.equal(content2.style.display, '', 'targets still expanded after remount');
    panel2.dispose();
  });

  test('latency fold state also persisted', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host);

    const foldBtn = host.querySelector('[data-fold-latency]');
    foldBtn.click();
    const content = host.querySelector('[data-latency-content]');
    assert.equal(content.style.display, '', 'latency expanded after click');

    const state = JSON.parse(localStorage.getItem('afxdp-fold-state'));
    assert.equal(state['cp-latency'], false, 'latency recorded as unfolded');
    panel.dispose();
  });
});

// ═══════════════════════════════════════════════════════════════════════════════
// (3) 'All' preset always enabled
// ═══════════════════════════════════════════════════════════════════════════════
describe('(3) All preset always enabled', () => {
  test('All enabled when count===0; others disabled', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host);
    panel.setTargets({ count: 0, pairs: 0, scope: 'among', totalNodes: 5 });

    const allBtn = host.querySelector('[data-preset="all"]');
    const pgBtn = host.querySelector('[data-preset="pg"]');
    const vpcBtn = host.querySelector('[data-preset="vpc"]');
    const azBtn = host.querySelector('[data-preset="az"]');
    const regionBtn = host.querySelector('[data-preset="region"]');

    assert.equal(allBtn.disabled, false, 'All must be enabled');
    assert.equal(pgBtn.disabled, true, 'PG disabled');
    assert.equal(vpcBtn.disabled, true, 'VPC disabled');
    assert.equal(azBtn.disabled, true, 'AZ disabled');
    assert.equal(regionBtn.disabled, true, 'Region disabled');
    panel.dispose();
  });

  test('All still enabled when count > 0', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host);
    panel.setTargets({ count: 3, pairs: 6, scope: 'among', totalNodes: 5 });

    const allBtn = host.querySelector('[data-preset="all"]');
    assert.equal(allBtn.disabled, false, 'All enabled with selection too');
    panel.dispose();
  });
});

// ═══════════════════════════════════════════════════════════════════════════════
// (4) LIVE MODE heartbeat includes target set
// ═══════════════════════════════════════════════════════════════════════════════
describe('(4) heartbeat carries targetIds', () => {
  test('heartbeat callback body carries nodes and scope', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    let hbPayload = null;
    const panel = mountControls(host, {
      onToggleLive: () => {},
      onHeartbeat: (body) => { hbPayload = body; },
      onScopeChange: () => {},
      onPreset: () => {},
      onClearTargets: () => {},
    });

    // Set targets
    panel.setTargets({ count: 3, pairs: 6, scope: 'fanout', totalNodes: 5 });
    panel.setTargetIds(new Set(['i-1', 'i-2', 'i-3']));

    // Enable live mode
    host.querySelector('[data-live]').click();
    // Click a heartbeat button
    const hbBtn = host.querySelector('[data-hb-ucast="kernel"]');
    assert.ok(hbBtn, 'heartbeat kernel button exists in live section');
    hbBtn.click();

    assert.ok(hbPayload, 'heartbeat callback was called');
    assert.deepEqual([...hbPayload.nodes].sort(), ['i-1', 'i-2', 'i-3'],
      'heartbeat body carries target nodes');
    assert.equal(hbPayload.scope, 'fanout', 'heartbeat body carries scope');
    panel.dispose();
  });

  test('target block is visible in live mode', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host, { onToggleLive: () => {} });

    // Enable live mode
    host.querySelector('[data-live]').click();
    const targetBlock = host.querySelector('[data-target-block]');
    assert.ok(targetBlock, 'target block exists');
    // It must not be inside [data-normal] (which is hidden in live mode)
    const normalSection = host.querySelector('[data-normal]');
    assert.ok(!normalSection.contains(targetBlock),
      'target block must NOT be inside data-normal (hidden in live mode)');
    panel.dispose();
  });
});

// ═══════════════════════════════════════════════════════════════════════════════
// (5) Cancel RUN button
// ═══════════════════════════════════════════════════════════════════════════════
describe('(5) cancel run button', () => {
  test('exists and is disabled when nothing running', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host, { onRun: () => {} });

    const cancelRun = host.querySelector('[data-cancel-run]');
    assert.ok(cancelRun, 'cancel-run button exists');
    assert.equal(cancelRun.disabled, true, 'disabled when nothing running');
    // Distinct from target-cancel
    const cancelTargets = host.querySelector('[data-cancel-targets]');
    assert.ok(cancelTargets, 'cancel-targets still exists');
    assert.notEqual(cancelRun, cancelTargets, 'different elements');
    assert.ok(cancelRun.textContent.toLowerCase().includes('cancel'),
      'button label contains "cancel"');
    panel.dispose();
  });

  test('enabled during a run, disabled after endRun()', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    const panel = mountControls(host, { onRun: () => {} });

    const cancelRun = host.querySelector('[data-cancel-run]');
    assert.equal(cancelRun.disabled, true, 'disabled before');
    panel.startRunUI();
    assert.equal(cancelRun.disabled, false, 'enabled during run');
    panel.endRun();
    assert.equal(cancelRun.disabled, true, 'disabled after endRun');
    panel.dispose();
  });

  test('clicking cancel-run fires onRun(null)', async () => {
    resetDOM();
    const { mountControls } = await import('../src/lib/controls.js');
    const host = document.createElement('div'); document.body.appendChild(host);
    let runPayloads = [];
    const panel = mountControls(host, { onRun: (b) => { runPayloads.push(b); } });

    // Simulate a run active state
    panel.startRunUI();
    const cancelRun = host.querySelector('[data-cancel-run]');
    cancelRun.click();
    assert.ok(runPayloads.includes(null), 'onRun(null) was called as cancel signal');
    panel.dispose();
  });
});
