# control-plane/web - live topology frontend (Vite + Svelte + three.js)

Real-time visualization and control UI for the AF_XDP benchmark fleet. Renders
the `afxdp.topology/v1` fleet model - consumed **live** from the backend SSE
stream or loaded from a static `fleet.json` file.

## Quick start

```bash
cd control-plane/web
npm install            # svelte, vite, three
npm run dev            # http://localhost:5173 - proxies to backend at :8080
```

The dev server proxies `/api/*` to the backend, so you get live data out of the
box if the backend is running locally. Otherwise it falls back to the bundled
`public/fleet.json`.

## Build (static, served by the backend)

```bash
npm run build          # -> dist/  (the backend auto-detects and serves this)
npm run preview        # preview the production build locally
```

---

## How the web app works

### Data flow

1. **On mount**, the app opens an `EventSource` to `GET /api/events` (the SSE
   stream). This is the **always-on data connection** - independent of whether
   the user is running tests or just observing.
2. The backend sends a `snapshot` event with the full `{nodes, edges}` state.
3. Incremental `node`, `edge`, and `job` deltas flow as they occur.
4. The `live.js` module maintains an in-memory `nodes[]` array and `edges` Map
   (keyed `kind|variation|src|dst`).
5. On any update, a debounced (500 ms) `liveRerender()` converts the live state
   into the `fleet.json` schema for the chosen kind+variation, then remounts
   the 2D or 3D renderer.

### Control panel (controls.js)

A shared, framework-agnostic DOM overlay mounted once above whichever topology
view is active. It survives viz dispose/remount cycles. Sections:

| Section | Contents |
|---|---|
| **Header** | 2D/3D toggle, Live button, fold-all. Stats line: `<online>/<nodes> online - <n> links` (fed from the rendered fleet). |
| **Timezone** | Display TZ for the Show dropdown timestamps |
| **Show** | Dropdown of `kind|variation` combos (sorted newest-first by measurement time) |
| **Report** | Download button (⤓) - generates a self-contained HTML report client-side |
| **Targets** | Preset buttons (PG / VPC / AZ / Region / All), scope select (among / between selected / fan out / fan in - shown only when something is selected, defaults to among), Deselect button. The All preset needs no anchor and is always enabled. |
| **Test Latency** | Packet count, rate, interval inputs + one-shot run buttons |
| **Live section** | (visible only when Live is toggled) shows only the Targets section; heartbeat runs carry nodes/scope |
| **Log** | Monospace status line showing campaign progress, with a separate Cancel Run button below it |

### One-shot test buttons

Each button POSTs to `/api/run` and waits for the campaign's terminal SSE event
(`done`/`cancelled`). While active:

- The pressed button turns **orange** and all others are **disabled** (gray).
- A dedicated **Cancel Run** button (below the log) becomes enabled and sends
  `POST /api/cancel` when clicked.
- On completion, `endRunUI()` clears the active state and re-enables all buttons.
- The **"all"** button (`ucast/all`) sequences all 4 variations by awaiting each
  `done` event before launching the next.

### Live (heartbeat) mode

Toggling **Live** swaps the control panel body to the heartbeat section:

- The user picks an interval (min 10 s, default 30 s) and a mode button.
- Clicking a heartbeat mode button starts a `setInterval` loop that calls
  `doRun(body)` on each tick (with smaller 1000-packet bursts).
- Clicking the same button again stops the loop.
- Toggling Live off clears the heartbeat and returns to the one-shot panel.
- The SSE connection is **always open** - it is independent of heartbeat mode.

### View switching (kind + variation)

The **Show** dropdown lists every distinct `kind|variation` combo present in the
live `edges` map, sorted by most-recent measurement time (newest first),
labelled with the local-timezone time of last measurement. Selecting one calls
`liveRerender()` which filters the edge map to that combo and rebuilds the
topology model.

### Panel folding (fold.js)

A single folding path for every panel in the app. Each fold is keyed by a panel
identifier (element id preferred, then class list), persisted in localStorage
under `afxdp-fold-state`. Toggling a fold hides/shows the panel content, reflects
the state via a `collapsed` class on the chevron, and exposes `apply()` to
re-assert the fold after a repaint. A registry (`makeFoldable`) tracks all panels
so the Fold-all button drives them all in one step. Handlers bind at construction.

Consumed by `controls.js` (control panel fold) and `2d/panels.js` (info panels).
`panelKey()` in `panels.js` prefers the element id, then the class list - the 3D
panels all share className `panel` and differ by id.

### 2D viewport zoom (2d/zoom.js)

Wheel-to-zoom about the cursor, double-click to reset. Scale range 0.3x - 5x.
Composed with the existing drag-to-pan as a single CSS transform
(`translate + scale`) on a viewport element inside the root.

`mountTopology2D` accepts `{ view }` (saved zoom/pan state) and returns
`getView()`, so zoom and pan survive the full remount that every live update
performs (App.svelte keeps `view2d` alongside `view3d`).

### Node latency tables (2d/nodes.js)

- **Hover** shows a transient tooltip (content from `tables.js`).
- **Click** pins a latency table at the mouse position (converted to viewport
  coordinates and divided by the zoom scale). Multiple nodes can be pinned.
- **Un-pin**: click the node body again, or press the Deselect all button.
- Pins live in a module-level `Map` keyed by instance id and are restored at the
  end of each mount via `ctx.restorePins` - surviving the live-update remount.
- Pinned tables live inside the transformed viewport so they hold position
  relative to their node across zoom/pan changes.
- `enhancePinned` in `panels.js` adds drag-only behaviour (header drag, delta
  divided by zoom scale, resting position reported back via `onMove`).

### Colour scale (2d/palette.js)

`latencyRange()` computes the p50 min/max over **intra-region** cells only and
returns a gold threshold (1st percentile of those values).

`cellColor(p50, mn, mx, crossRegion, gold)`:

- Cross-region pairs → neutral grey (`CROSS_REGION_COLOR rgb(110,118,129)`) -
  excluded from the scale because a ~12 ms WAN hop compresses intra-AZ values.
- Fastest 1% → vivid green `rgb(57,211,83)` (`GOLD_COLOR`).
- Rest → ramp from mild pre-green `rgb(154,190,90)` through orange to red.

Used by `edges.js`, `topology3d.js`, `report.js`, and `report-combined.js`.

### Contour geometry (2d/contours.js + layout.js)

Contour boxes are padded from each node's **rendered extent** (nodes draw badges
outside their circle, and every contour draws its label above its own border).
`PAD_BASE = 20`, `STEP = 28`; parents expand to contain children. Group
separation is re-enforced AFTER the viewport fit (the fit scales positions while
pads are absolute pixels). Node collision resolution also runs after the fit.

Guarded by `test/contour-no-overlap.test.mjs` which runs the real layout over
five fleet shapes.

### Legend instructions (2d/panels.js)

Both the 2D and 3D legend panels put their usage hints under a collapsible
**Instructions** header at the top of the panel. Built by the shared
`instructionsHTML(rowsHtml)` / `wireInstructions(el)` helpers in `panels.js`.

### Multicast rendering

When `kind === "mcast"`, the viewer identifies the replicator node and **splits
each end-to-end edge** (source→destination) into two visual hops:

- `source → replicator` (shared first leg, same metric)
- `replicator → destination` (measured last leg)

This reflects the physical datapath: packets travel source → replicator
(multicast-over-unicast) → AF_XDP fan-out → each destination.

### Downloadable HTML report (report.js + report-combined.js)

The ⤓ button generates a self-contained HTML file from the in-memory model:

- **NxN heatmap** - cells coloured by p50 (green→red scale), hover shows p99 + loss.
- **Full latency table** - every edge with p50/p90/p99/p99.9/max/loss%.
- Assembled entirely in the browser (no server call), downloaded as a Blob.
- Filename: `afxdp-report-<kind>-<variation>-<timestamp>.html`.

**Combined report** (`report-combined.js`): when multiple modes have been
measured, the ⤓ builds a multi-mode document with:

- A **Latest measurements** overview grid (freshest value per cell, mode-badged).
- An export bar at the top: **Save as PDF** and **Save as XLS** buttons.
- Per-mode heatmaps and tables (colour stays comparable within a mode).
- `reportInteractions()` is serialised via `Function.prototype.toString` so the
  saved HTML is self-contained (no external imports).
- **XLS export**: SpreadsheetML 2003 workbook, one worksheet per table, sheet
  names sanitised to Excel limits, numeric cells typed. Filename:
  `latency-report-<modes>-<timestamp>.xls`.
- **PDF export**: prints a full standalone copy via a hidden page-sized iframe
  (`window.__afxdpPrintReport` hook); export controls are hidden in print media.
- `REPORT_CSS` is injected into the live app scoped under `.report-view` to
  avoid leaking table styles into the 2D node tooltip.

---

## Layout

```
web/
├── index.html              entry point
├── vite.config.js          dev server + /api proxy
├── src/
│   ├── main.js             mounts App.svelte
│   ├── App.svelte          top-level: SSE connection, panel wiring, viz lifecycle
│   ├── app.css             global styles (3D renderer)
│   └── lib/
│       ├── live.js         SSE client, in-memory state, fleet-schema adapter, POST helpers
│       ├── controls.js     shared control panel (pure DOM, framework-agnostic)
│       ├── fold.js         unified panel folding: persist, toggle, apply, Fold-all registry
│       ├── pairs.js        scope/preset logic (among, fanout, fanin) + pair counting
│       ├── report.js       single-mode client-side HTML report builder
│       ├── report-combined.js  multi-mode combined report + PDF/XLS export
│       ├── topology3d.js   3D three.js renderer (orbit, CSS2DRenderer labels)
│       └── 2d/             2D DOM/SVG map modules:
│           ├── index.js       entry: mount, dispose, getView (zoom/pan state)
│           ├── zoom.js        viewport zoom (wheel + double-click reset, MIN 0.3 / MAX 5)
│           ├── layout.js      force-directed / geographic node positioning
│           ├── palette.js     colour scales, formatters, HTML escaping
│           ├── contours.js    AZ/PG/VPC/Region/Account contour rendering
│           ├── edges.js       directed edge arcs with latency labels
│           ├── nodes.js       node circles, hover tooltip, click-to-pin latency tables
│           ├── panels.js      draggable/foldable info panels + instructions helpers
│           ├── selection.js   click/hover node selection
│           ├── tables.js      tabular data overlays
│           └── styles.js      scoped CSS injection
├── test/
│   ├── contour-no-overlap.test.mjs   layout overlap guard (five fleet shapes)
│   └── contour-spacing.test.mjs      contour spacing regression
└── public/
    └── fleet.json          sample/fallback data (regenerate with gen/fleet_json.py)
```

---

## How the SSE stream is consumed (lib/live.js)

```
EventSource("/api/events")
    │
    ├─ "snapshot" → reset nodes[], clear edges Map, onUpdate()
    ├─ "node"     → upsert in nodes[], onUpdate()
    ├─ "edge"     → upsert in edges Map (key: kind|variation|src|dst), onUpdate()
    └─ "job"      → onJob() callback (status line + done-waiter resolution)
```

The `toFleet(kind, variation)` method filters the edge map and builds the matrix
array the renderers expect: `fleet.nodes[]` + `fleet.matrix[i][j]` (null = no
data, object = `{p50,p90,p99,p999,max,loss}`).

---

## Interacting with the backend

| Action | API call | Triggered by |
|---|---|---|
| Start ucast test | `POST /api/run {kind:"ucast", variation, count, rate, warmup, nodes?, scope?}` | Test button click |
| Start mcast test | `POST /api/run {kind:"mcast", modes[], count, interval_us, timeout_sec}` | Test button click |
| Cancel campaign | `POST /api/cancel` | Cancel Run button |
| Ad-hoc command | `POST /api/cmd {instance_id, command}` | (programmatic only) |
| Full snapshot | `GET /api/fleet` | `?data=` param / browse results |

---

## Cohesion checks (controls.js)

Before launching, the UI runs sanity checks on the configured parameters:

- Warns if packet count < 1000 (noisy p99.9).
- Warns if ucast run duration < 50 ms or rate > 200k pps.
- For mcast: auto-raises `timeout_sec` if `count * interval_us` exceeds it.
- Warnings appear as a ⚠ prefix in the status line.

---

## Dev-mode browse results

Under `npm run dev`, the Vite dev server exposes:

- `GET /api/results` - lists `fleet.json` files under `af_xdp/results/` (newest first).
- `GET /api/fleet?path=<subdir>` - serves a specific saved `fleet.json`.

The Show dropdown can host these as "Browse results…" entries. In a production
`build`/`preview`, these endpoints don't exist - the dropdown shows only live
combos.

---

## Notes

- three.js is a real bundled dependency (no CDN, no WASM).
- Two **independent** renderers toggled from the panel on the same model:
  `topology3d.js` (three.js orbit + CSS2DRenderer) and `2d/` (DOM/SVG map). No
  shared state between them - a small amount of helper duplication is intentional
  for decoupling.
- The 3D camera view is persisted across live-update remounts (stored in a
  `view3d` variable) so zoom/pan isn't reset on every tick.
- The 2D zoom/pan state is persisted the same way (`view2d` in App.svelte,
  passed to `mountTopology2D({ view })` and returned via `getView()`).
- The control panel uses `enhancePanel` from `2d/panels.js` for drag/fold/resize.
- The web app fully remounts the topology on each debounced update (fine for tens
  of nodes; see scaling notes in the main README).

---

## License

This project is licensed under the MIT-0 License. See the LICENSE file.
