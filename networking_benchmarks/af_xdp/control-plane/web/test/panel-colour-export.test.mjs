// Tests for the four changes: (1) live-mode Test Latency removal,
// (2) Cancel->Deselect rename, (3) green-red ramp + gold 1% highlight,
// (4) single-XLS export replacing multi-CSV.
import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from '/tmp/reptest/node_modules/jsdom/lib/api.js';

// ── DOM bootstrap for browser modules ──────────────────────────────────────
const bootstrap = new JSDOM('<!doctype html><html><body></body></html>', { pretendToBeVisual: true });
globalThis.window = bootstrap.window;
globalThis.document = bootstrap.window.document;
globalThis.MouseEvent = bootstrap.window.MouseEvent;
globalThis.Element = bootstrap.window.Element;
globalThis.HTMLElement = bootstrap.window.HTMLElement;
globalThis.navigator ??= bootstrap.window.navigator;
globalThis.localStorage = (() => {
  const s = {};
  return { getItem: (k) => s[k] ?? null, setItem: (k, v) => { s[k] = v; }, removeItem: (k) => { delete s[k]; } };
})();
globalThis.ResizeObserver = class { observe() {} unobserve() {} disconnect() {} };
globalThis.getComputedStyle = bootstrap.window.getComputedStyle;

const { mountControls } = await import('../src/lib/controls.js');

function freshPanel(opts = {}) {
  bootstrap.window.document.body.innerHTML = '';
  const host = bootstrap.window.document.createElement('div');
  bootstrap.window.document.body.appendChild(host);
  return mountControls(host, opts);
}

// ════════════════════════════════════════════════════════════════════════════════
// (1) LIVE MODE: 'Test Latency' section must NOT render
// ════════════════════════════════════════════════════════════════════════════════

describe('(1) live mode hides Test Latency section', () => {
  test('normal mode shows both Targets and Test Latency sections', () => {
    freshPanel();
    const doc = bootstrap.window.document;
    const normalBlocks = doc.querySelectorAll('[data-normal]');
    // data-normal blocks are visible (display != none)
    for (const block of normalBlocks) {
      assert.notEqual(block.style.display, 'none', 'normal blocks must be visible');
    }
    // Confirm "Test Latency" text is present in normal mode
    assert.ok(doc.querySelector('[data-fold-latency]'), 'latency fold btn exists in normal');
  });

  test('live mode hides all [data-normal] sections (including Test Latency)', () => {
    const ctrl = freshPanel();
    ctrl.setLive(true);
    const doc = bootstrap.window.document;
    const normalBlocks = doc.querySelectorAll('[data-normal]');
    for (const block of normalBlocks) {
      assert.equal(block.style.display, 'none', '[data-normal] must be hidden in live mode');
    }
  });

  test('live mode still shows the Targets section', () => {
    const ctrl = freshPanel();
    ctrl.setLive(true);
    const doc = bootstrap.window.document;
    const targetBlock = doc.querySelector('[data-target-block]');
    assert.ok(targetBlock, 'target block exists');
    assert.notEqual(targetBlock.style.display, 'none', 'target block must be visible in live');
  });

  test('live mode shows the heartbeat section', () => {
    const ctrl = freshPanel();
    ctrl.setLive(true);
    const doc = bootstrap.window.document;
    const liveSection = doc.querySelector('[data-live-section]');
    assert.ok(liveSection, 'live section must exist');
    assert.notEqual(liveSection.style.display, 'none', 'live section visible in live mode');
  });

  test('returning to normal mode restores Test Latency', () => {
    const ctrl = freshPanel();
    ctrl.setLive(true);
    ctrl.setLive(false);
    const doc = bootstrap.window.document;
    const normalBlocks = doc.querySelectorAll('[data-normal]');
    for (const block of normalBlocks) {
      assert.notEqual(block.style.display, 'none', 'normal blocks visible again');
    }
  });
});

// ════════════════════════════════════════════════════════════════════════════════
// (2) RENAME Cancel -> Deselect on [data-cancel-targets]
// ════════════════════════════════════════════════════════════════════════════════

describe('(2) target clear button labelled Deselect', () => {
  test('[data-cancel-targets] button says "Deselect", not "Cancel"', () => {
    freshPanel();
    const btn = bootstrap.window.document.querySelector('[data-cancel-targets]');
    assert.ok(btn, 'button must exist');
    assert.equal(btn.textContent.trim(), 'Deselect');
  });

  test('[data-cancel-run] button still says "Cancel Run"', () => {
    freshPanel();
    const btn = bootstrap.window.document.querySelector('[data-cancel-run]');
    assert.ok(btn, 'cancel-run button must exist');
    assert.equal(btn.textContent.trim(), 'Cancel Run');
  });
});

// ════════════════════════════════════════════════════════════════════════════════
// (3) GREEN-RED RAMP + GOLD 1st PERCENTILE
// ════════════════════════════════════════════════════════════════════════════════

describe('(3) latency ramp: green-orange-red with gold 1% highlight', () => {
  function rgb(css) {
    const m = /rgb\((\d+),\s*(\d+),\s*(\d+)\)/.exec(css);
    assert.ok(m, `not an rgb() colour: ${css}`);
    return [Number(m[1]), Number(m[2]), Number(m[3])];
  }

  test('lowest latency is GREEN (G > R), not gold or red', async () => {
    const { latencyColor } = await import('../src/lib/2d/palette.js');
    const css = latencyColor(10, 10, 100);
    const [r, g] = rgb(css);
    assert.ok(g > r, `Expected green (G>R) but got R=${r} G=${g}: ${css}`);
  });

  test('highest latency is RED (R > G)', async () => {
    const { latencyColor } = await import('../src/lib/2d/palette.js');
    const css = latencyColor(100, 10, 100);
    const [r, g] = rgb(css);
    assert.ok(r > g, `Expected red (R>G) but got R=${r} G=${g}: ${css}`);
  });

  test('latencyRange returns a gold threshold at the 1st percentile of intra-region values', async () => {
    const { latencyRange } = await import('../src/lib/2d/palette.js');
    const NODES = [
      { region: 'us-east-1' }, { region: 'us-east-1' },
      { region: 'eu-west-1' },
    ];
    // 100 intra-region cells with p50 from 20 to 119
    const cells = [];
    for (let i = 0; i < 100; i++) {
      cells.push({ a: NODES[0], b: NODES[1], p50: 20 + i });
    }
    // One cross-region cell (should NOT affect gold)
    cells.push({ a: NODES[0], b: NODES[2], p50: 5 });

    const result = latencyRange(cells);
    assert.ok('gold' in result, 'must return a gold field');
    // 1st percentile of 100 values (20..119): floor(100*0.01)=1, so vals[1]=21
    assert.equal(result.gold, 21, 'gold = 1st percentile of intra-region p50');
    assert.equal(result.mn, 20);
    assert.equal(result.mx, 119);
  });

  test('gold threshold excludes cross-region cells', async () => {
    const { latencyRange } = await import('../src/lib/2d/palette.js');
    const NODES = [
      { region: 'us-east-1' }, { region: 'us-east-1' },
      { region: 'eu-west-1' },
    ];
    const cells = [];
    for (let i = 0; i < 50; i++) {
      cells.push({ a: NODES[0], b: NODES[1], p50: 30 + i });
    }
    // Cross-region cell with very low p50 - must NOT lower the gold threshold
    cells.push({ a: NODES[0], b: NODES[2], p50: 1 });
    const { gold } = latencyRange(cells);
    assert.ok(gold >= 30, `gold=${gold} must not be pulled down by cross-region cell`);
  });

  test('cellColor returns gold when value <= gold threshold', async () => {
    const { cellColor, GOLD_COLOR } = await import('../src/lib/2d/palette.js');
    const color = cellColor(20, 20, 100, false, 21);
    assert.equal(color, GOLD_COLOR, 'value at/below threshold must be gold');
  });

  test('cellColor returns non-gold green for value just above gold threshold', async () => {
    const { cellColor, GOLD_COLOR } = await import('../src/lib/2d/palette.js');
    const color = cellColor(22, 20, 100, false, 21);
    assert.notEqual(color, GOLD_COLOR, 'value above threshold must NOT be gold');
    // It should be greenish
    const [r, g] = rgb(color);
    assert.ok(g > r, `just above gold should be green end: R=${r} G=${g}`);
  });

  test('cellColor returns grey for cross-region regardless of gold', async () => {
    const { cellColor, CROSS_REGION_COLOR } = await import('../src/lib/2d/palette.js');
    const color = cellColor(20, 20, 100, true, 21);
    assert.equal(color, CROSS_REGION_COLOR);
  });

  test('cellColor works without gold argument (backward compat)', async () => {
    const { cellColor, latencyColor } = await import('../src/lib/2d/palette.js');
    const color = cellColor(50, 20, 100, false);
    assert.equal(color, latencyColor(50, 20, 100));
  });

  test('tiny data set (1-2 cells): gold threshold stays finite', async () => {
    const { latencyRange } = await import('../src/lib/2d/palette.js');
    const NODES = [{ region: 'r' }, { region: 'r' }];
    // Single cell
    const r1 = latencyRange([{ a: NODES[0], b: NODES[1], p50: 42 }]);
    assert.ok(Number.isFinite(r1.gold), `gold must be finite, got ${r1.gold}`);
    assert.equal(r1.gold, 42);
    // Two cells
    const r2 = latencyRange([
      { a: NODES[0], b: NODES[1], p50: 30 },
      { a: NODES[1], b: NODES[0], p50: 40 },
    ]);
    assert.ok(Number.isFinite(r2.gold), `gold must be finite with 2 cells`);
    assert.equal(r2.gold, 30);
  });

  test('empty input: gold is 0 (safe degenerate)', async () => {
    const { latencyRange } = await import('../src/lib/2d/palette.js');
    const r = latencyRange([]);
    assert.ok(Number.isFinite(r.gold), 'gold must be finite for empty input');
    assert.equal(r.gold, 0);
  });
});

// ════════════════════════════════════════════════════════════════════════════════
// (4) SINGLE XLS EXPORT (SpreadsheetML) REPLACING MULTI-CSV
// ════════════════════════════════════════════════════════════════════════════════

describe('(4) Save as XLS - single SpreadsheetML file', () => {
  function makeViews() {
    const nodes = [0, 1].map((i) => ({
      index: i, private_ip: `10.0.0.${i + 1}`, public_ip: `1.1.1.${i + 1}`,
      role: 'source', az: 'a', region: 'r', cpg_name: 'cpg', vpc_id: 'vpc',
      type: 't', online: true,
    }));
    const m = [[null, { p50: 30, p90: 32, p99: 35, p999: 40, max: 50, loss: 0, unix: 1 }],
               [{ p50: 31, p90: 33, p99: 36, p999: 41, max: 51, loss: 0, unix: 1 }, null]];
    return [{ kind: 'ucast', variation: 'kernel', unix: 1, fleet: { nodes, matrix: m, region: 'r' } }];
  }

  test('report header has a "Save as XLS" button with same class as print', async () => {
    const { buildCombinedReportHTML } = await import('../src/lib/report-combined.js');
    const html = buildCombinedReportHTML(makeViews());
    const dom = new JSDOM(html, { url: 'http://localhost' });
    const doc = dom.window.document;
    const btn = doc.querySelector('[data-xls-btn]');
    assert.ok(btn, 'XLS button must exist in the report');
    assert.ok(/save\s+as\s+xls/i.test(btn.textContent), `label must say Save as XLS, got "${btn.textContent}"`);
  });

  test('clicking XLS button produces a single .xls download (SpreadsheetML)', async () => {
    const { buildCombinedReportHTML, reportInteractions } = await import('../src/lib/report-combined.js');
    const html = buildCombinedReportHTML(makeViews());
    const dom = new JSDOM(html, { url: 'http://localhost' });
    const doc = dom.window.document;

    // reportInteractions uses bare `Blob`, `URL`, `document` - they resolve
    // against the Node global scope when called from tests.
    const blobs = [];
    const anchors = [];
    const origBlob = globalThis.Blob;
    const origURL = globalThis.URL;
    globalThis.Blob = function(parts, opts) { this.parts = parts; this.type = opts?.type; blobs.push(this); };
    globalThis.URL = { createObjectURL: () => 'blob:test', revokeObjectURL: () => {} };
    const origDoc = globalThis.document;
    const origCreate = doc.createElement.bind(doc);
    globalThis.document = doc;
    doc.createElement = function(tag) {
      const el = origCreate(tag);
      if (tag === 'a') {
        el.click = function() { anchors.push({ href: el.href, download: el.download }); };
      }
      return el;
    };

    try {
      reportInteractions(doc);
      const btn = doc.querySelector('[data-xls-btn]');
      assert.ok(btn, 'XLS button must exist');
      btn.click();

      assert.equal(blobs.length, 1, 'exactly one Blob created (one file)');
      assert.equal(anchors.length, 1, 'exactly one download triggered');
      assert.ok(anchors[0].download.endsWith('.xls'), `filename must end .xls, got ${anchors[0].download}`);

      const xml = blobs[0].parts[0];
      assert.ok(xml.includes('<Workbook'), 'must contain <Workbook> element');
      assert.ok(xml.includes('<Worksheet'), 'must contain <Worksheet> element');
      const tables = doc.querySelectorAll('table');
      const worksheetCount = (xml.match(/<Worksheet/g) || []).length;
      assert.equal(worksheetCount, tables.length, 'one Worksheet per table');
    } finally {
      globalThis.Blob = origBlob;
      globalThis.URL = origURL;
      globalThis.document = origDoc;
    }
  });

  test('sheet names are sanitised (max 31 chars, no forbidden chars)', async () => {
    const { buildCombinedReportHTML, reportInteractions } = await import('../src/lib/report-combined.js');
    const html = buildCombinedReportHTML(makeViews());
    const dom = new JSDOM(html, { url: 'http://localhost' });
    const doc = dom.window.document;

    const blobs = [];
    const origBlob = globalThis.Blob;
    const origURL = globalThis.URL;
    const origDoc = globalThis.document;
    globalThis.Blob = function(parts, opts) { this.parts = parts; blobs.push(this); };
    globalThis.URL = { createObjectURL: () => 'blob:test', revokeObjectURL: () => {} };
    globalThis.document = doc;
    const origCreate = doc.createElement.bind(doc);
    doc.createElement = function(tag) {
      const el = origCreate(tag);
      if (tag === 'a') el.click = function() {};
      return el;
    };

    try {
      reportInteractions(doc);
      doc.querySelector('[data-xls-btn]').click();
      const xml = blobs[0].parts[0];
      const names = [...xml.matchAll(/ss:Name="([^"]+)"/g)].map(m => m[1]);
      for (const name of names) {
        assert.ok(name.length <= 31, `sheet name too long: "${name}" (${name.length})`);
        assert.ok(!/[:\\/?\*\[\]]/.test(name), `forbidden char in sheet name: "${name}"`);
      }
    } finally {
      globalThis.Blob = origBlob;
      globalThis.URL = origURL;
      globalThis.document = origDoc;
    }
  });

  test('numeric cells are typed as Number, text as String', async () => {
    const { reportInteractions } = await import('../src/lib/report-combined.js');
    const dom = new JSDOM(`<!doctype html><html><body>
      <h2>TestSheet</h2>
      <table><tr><th>Name</th><th>Value</th></tr>
      <tr><td>hello</td><td>42</td></tr>
      <tr><td>world</td><td>3.14</td></tr></table>
      <button data-xls-btn>Save as XLS</button>
    </body></html>`, { url: 'http://localhost' });
    const doc = dom.window.document;

    const blobs = [];
    const origBlob = globalThis.Blob;
    const origURL = globalThis.URL;
    const origDoc = globalThis.document;
    globalThis.Blob = function(parts, opts) { this.parts = parts; blobs.push(this); };
    globalThis.URL = { createObjectURL: () => 'blob:test', revokeObjectURL: () => {} };
    globalThis.document = doc;
    const origCreate = doc.createElement.bind(doc);
    doc.createElement = function(tag) {
      const el = origCreate(tag);
      if (tag === 'a') el.click = function() {};
      return el;
    };

    try {
      reportInteractions(doc);
      doc.querySelector('[data-xls-btn]').click();
      const xml = blobs[0].parts[0];
      assert.ok(xml.includes('ss:Type="Number"'), 'must have Number typed cells');
      assert.ok(xml.includes('<Data ss:Type="Number">42</Data>'), '42 must be Number');
      assert.ok(xml.includes('<Data ss:Type="Number">3.14</Data>'), '3.14 must be Number');
      assert.ok(xml.includes('<Data ss:Type="String">hello</Data>'), 'hello must be String');
    } finally {
      globalThis.Blob = origBlob;
      globalThis.URL = origURL;
      globalThis.document = origDoc;
    }
  });

  test('XML-escapes special characters in cell text', async () => {
    const { reportInteractions } = await import('../src/lib/report-combined.js');
    const dom = new JSDOM(`<!doctype html><html><body>
      <h2>Escape Test</h2>
      <table><tr><th>A</th></tr><tr><td>x &lt; y &amp; "z"</td></tr></table>
      <button data-xls-btn>Save as XLS</button>
    </body></html>`, { url: 'http://localhost' });
    const doc = dom.window.document;

    const blobs = [];
    const origBlob = globalThis.Blob;
    const origURL = globalThis.URL;
    const origDoc = globalThis.document;
    globalThis.Blob = function(parts, opts) { this.parts = parts; blobs.push(this); };
    globalThis.URL = { createObjectURL: () => 'blob:test', revokeObjectURL: () => {} };
    globalThis.document = doc;
    const origCreate = doc.createElement.bind(doc);
    doc.createElement = function(tag) {
      const el = origCreate(tag);
      if (tag === 'a') el.click = function() {};
      return el;
    };

    try {
      reportInteractions(doc);
      doc.querySelector('[data-xls-btn]').click();
      const xml = blobs[0].parts[0];
      assert.ok(xml.includes('&lt;') || xml.includes('&amp;'), 'must XML-escape special chars');
      const dataContents = [...xml.matchAll(/<Data[^>]*>(.*?)<\/Data>/g)].map(m => m[1]);
      for (const d of dataContents) {
        if (d.includes('x')) {
          assert.ok(!d.includes(' < '), 'raw < must be escaped');
        }
      }
    } finally {
      globalThis.Blob = origBlob;
      globalThis.URL = origURL;
      globalThis.document = origDoc;
    }
  });
});
