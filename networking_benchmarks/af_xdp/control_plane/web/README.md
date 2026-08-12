# control_plane/web - live topology frontend (Vite + Svelte + three.js)

Real-time visualization and control UI for the AF_XDP benchmark fleet. Renders
the `afxdp.topology/v1` fleet model - consumed **live** from the backend SSE
stream or loaded from a static `fleet.json` file.

## Quick start

```bash
cd control_plane/web
npm install            # svelte, vite, three
npm run dev            # http://localhost:5173 - proxies to backend at :8080
```

The dev server proxies `/api/*` to the backend, so you get live data out of the
box if the backend is running locally. Otherwise it falls back to the bundled
`public/fleet.json`.

### Running without a real backend (mock server)

`mock/server.mjs` is a zero-dependency, drop-in replacement for the Go backend
that speaks the same `/api/*` wire contract - useful for UI work with no AWS
fleet, NATS, or Go backend running at all.

**Step by step:**

1. **Start the mock backend** (terminal 1):
   ```bash
   cd control_plane/web
   node mock/server.mjs          # http://localhost:8080  (PORT env overrides)
   ```
   Confirms it's up:
   ```
   mock control-plane on http://localhost:8080
     17 nodes across 2 regions, 4 AZs, 5 VPCs
     1110 seeded edges (ucast x4 variations + mcast x2 modes)
   ```
2. **Start the Vite dev server** (terminal 2, same directory):
   ```bash
   npm install     # first time only
   npm run dev     # http://localhost:5173
   ```
   Vite's dev proxy (`vite.config.js`) forwards `/api/*` to `localhost:8080` -
   step 1 must already be running, or every `/api/*` request 404s/ECONNREFUSEs
   and the app looks broken even though Vite itself started fine. If you see
   `vite] http proxy error: /api/events ... ECONNREFUSED`, this is the cause:
   nothing is listening on the port Vite is forwarding to. Either start the
   mock (step 1) or the real Go backend, whichever you meant to test against.
3. **Open the dashboard**: `http://localhost:5173/` in a browser. You should
   see the 17-node mock fleet rendered immediately (no campaign run needed -
   the mock seeds ucast + mcast edges at startup).
4. **Open a report**: click the **ucast** or **mcast** link in the control
   panel's "Show" row (opens `?report=ucast` / `?report=mcast` in a new tab),
   or navigate directly to `http://localhost:5173/?report=mcast`. The report
   is **store-backed** (fetches `GET /api/measurements` + `GET
   /api/mcast-replicators` from the mock, not the live SSE state - see
   [Downloadable HTML report](#downloadable-html-report-reportjs--report-combinedjs)
   below), so it has data immediately, before any campaign runs in this
   session. It auto-refreshes every 180s and has an explicit "↻ Refresh"
   button.
5. **(Optional) run a live campaign**: click any Test Latency button (e.g.
   "kernel" under ucast, or "copy" under mcast). This streams synthetic
   job/edge progress over SSE and also grows the mock's `/api/measurements` /
   `/api/mcast-replicators` data, so a re-render (or the next 180s
   auto-refresh) of an already-open report reflects it.

Sanity-check the mock directly with curl if the UI looks wrong and you want to
rule out the backend before debugging the frontend:
```bash
curl -s localhost:8080/api/fleet | head -c 200          # live snapshot
curl -s localhost:8080/api/measurements?kind=mcast       # report data (mcast only)
curl -s localhost:8080/api/mcast-replicators              # per-replicator mcast history
```

It serves a deterministic 17-node fleet (`mock/topology.mjs`): 2 regions, 2
accounts, 5 VPCs, cluster + spread placement groups, and **two replicators per
region** (different PG/AZ each) so multi-replicator mcast campaigns and the
report's "Per-replicator paths" section have real data to render without
touching a live fleet. `/api/run` streams believable job/edge deltas over SSE
just like the real backend - Test Latency buttons, Live mode, the Show combo
dropdown, and the report all work end to end. See [`mock/`](#mock-server-mockserverjs-topologymjs)
below for what it does and does not reproduce.

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

The ⤓ button generates a self-contained HTML file. **`report-combined.js`
is the path actually used by the app** (`App.svelte`'s report overlay and PDF
export); `report.js`'s `buildReportHTML`/`buildCompareHTML` remain live-model
helpers - `buildCompareHTML` is still used internally by the combined report's
delta view, and `buildReportHTML` is kept for any future single-mode,
in-memory-only use but is not wired into the app today.

**This is a different report from `afxdpctl report`.** The CLI's `report`
command (`control_plane/cmd/afxdpctl/main_report.go`) is a separate,
self-contained Go implementation - it fetches the live `/api/fleet` snapshot
(not the SQLite store) and renders a much smaller HTML file: a fleet
inventory table (private/public IP, role, region, AZ, type, tenancy), a p50
heatmap, and a flat per-edge latency table - no hop1/hop2 split, no
per-replicator paths section, no PDF/XLS export. Useful for a quick CLI-only
snapshot without opening a browser; use the web UI's ⤓ button (this section)
for the full report with mcast hop splits and multi-replicator comparison.

**Data source (report-combined.js): the backend's SQLite store, not the live
in-memory model.** Every table - Latest measurements, per-mode heatmaps, All
measurements, and Per-replicator paths - is built from flat rows fetched via
`GET /api/measurements` (general: every kind/mode, one row per
`(kind,variation,src,dst,replicator)` edge) and `GET /api/mcast-replicators`
(mcast-only, run-scoped). This is why the report can show **every**
src/replicator/dst path variation, not just the newest one per pair: the live
`/api/fleet`/SSE snapshot is keyed only by `(kind,variation,src,dst)` and can
therefore only ever hold ONE value per pair - a second replicator measuring
the same destination silently overwrites the first's number there. See the
[control-plane README](../README.md#multi-replicator-mcast-campaigns) for the
backend side of this.

**Refresh model**: the live overlay fetches on open, auto-refetches every
180s while open, and has an explicit "↻ Refresh" button
(`buildCombinedReportBody`'s `opts.showRefresh`). The report is otherwise
fully decoupled from the live SSE stream - it does **not** re-render on every
live update tick the way it used to before this data source change. The
downloaded standalone HTML omits the Refresh button (no backend to refresh
against once downloaded) but keeps Save as PDF / Save as XLS.

Contents:

- A **Latest measurements** overview grid (freshest value per cell, mode-badged).
- Per-mode heatmaps and tables (colour stays comparable within a mode). When
  more than one replicator measured the same destination for a mode, the
  heatmap cell still shows only the freshest value (an NxN grid has one cell
  per (src,dst), not per (src,dst,replicator)) and says so in its tooltip -
  every replicator's number is still in "All measurements" and
  "Per-replicator paths" below.
- A combined **All measurements** table with a "replicator" column
  (blank for ucast), so every mode AND every replicator path appears as its
  own row.
- **Per-replicator paths** (mcast only, shown when more than one replicator
  was swept): one row per `(replicator, mode, destination)`, with the
  replicator's PG/AZ - a focused, pre-sorted, mcast-only view for scanning
  specifically for placement differences without the ucast rows/extra columns
  from "All measurements" in the way. See
  `dev/roadmap/mcast-replicator-selection.md` for the planned follow-up
  (choosing a subset of replicators to run, instead of always sweeping all of
  them).
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
└── public/
    └── fleet.json          sample/fallback data (regenerate with gen/fleet_json.py)
```

Not shown above (sits alongside `src/`, not part of the bundle):

```
web/
└── mock/
    ├── server.mjs           zero-dependency mock backend (see below)
    └── topology.mjs         deterministic fleet + edge generator it serves
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
| Report data (general) | `GET /api/measurements?kind=&since_unix=&limit=` | Report open / 180s auto-refresh / Refresh button |
| Report data (mcast replicators) | `GET /api/mcast-replicators?since_run_id=&limit=` | Same as above |

---

## Cohesion checks (controls.js)

Before launching, the UI runs sanity checks on the configured parameters:

- Warns if packet count < 1000 (noisy p99.9).
- Warns if ucast run duration < 50 ms or rate > 200k pps.
- For mcast: auto-raises `timeout_sec` if `count * interval_us` exceeds it.
- Warnings appear as a ⚠ prefix in the status line.

---

---

## Mock server (mock/server.mjs, topology.mjs)

A zero-dependency Node HTTP server (`node:http` only, no npm deps) that
implements the backend's `/api/*` wire contract closely enough to drive the
whole UI - Test Latency buttons, Live mode, the Show dropdown, cancel, and both
report variants - without a Go backend, NATS, or any real EC2 fleet. See
[Quick start](#running-without-a-real-backend-mock-server) for how to run it.

### Endpoint parity

| Endpoint | Real backend | Mock | Notes |
|---|---|---|---|
| `GET /api/fleet` | ✅ | ✅ | Full `{nodes, edges}` snapshot |
| `GET /api/events` (SSE) | ✅ | ✅ | `snapshot` then `node`/`edge`/`job` deltas + keepalive |
| `POST /api/run` | ✅ | ✅ | ucast + mcast; mcast sweeps every mock replicator per region, matching the backend's automatic multi-replicator sweep |
| `POST /api/cancel` | ✅ | ✅ | Checked at the same granularity (between mcast destinations / ucast pairs) |
| `POST /api/cmd` | ✅ | ✅ (stub) | Always returns `ok:true` - nothing in the web UI calls this |
| `GET /api/measurements` | ✅ | ✅ | The report's general, store-backed data source. Seeded from the mock fleet's ucast edges + mcast replicator sweep on startup, updated as ucast/mcast campaigns run. `kind`/`since_unix` filters supported; `limit` accepted but not enforced (mock fleet is small) |
| `GET /api/mcast-replicators` | ✅ | ✅ | Seeded on startup (44 rows: 2 regions × 2 replicators × 2 modes × destinations) and grows as mock campaigns run, mirroring the backend's SQLite-backed per-replicator history |
| `GET /api/errors` | ✅ | ✅ (stub) | Always empty - the mock never fails a command, so there's nothing to report; present for API-surface completeness, not called by the UI today |
| `GET /healthz` | ✅ | ✅ | |

### Deliberate differences (the mock isn't meant to be the backend)

- **No agents, no NATS, no real network.** Every "measurement" is a
  deterministic formula (`topology.mjs`'s `baseLatency`) plus jitter, not a
  real RTT. Good enough to exercise every UI code path; not a latency
  simulator.
- **No SQLite / persistence.** `measurementRows`, `mcastReplicatorResults`, and
  `edgeStore` are plain in-memory arrays/maps that reset on restart - they
  mirror the SHAPE of the real store's query results, not an actual database.
  The real store survives backend restarts and is queried by the MCP server
  too - the mock has no equivalent to `mcp/`.
- **No retries, no partial failure.** `dispatchRetry`'s at-most-once/retry
  semantics and the loss-gate rejection path (`MaxLossPct`) don't exist here -
  every mock pair "succeeds". A UI change that specifically needs to see a
  failed/rejected pair (e.g. testing the coverage-gap messaging in the report)
  needs the real backend or a manual edit to the mock's `bumpEdge`.
- **No round-based ucast scheduler.** The mock runs ucast pairs one at a time
  in registration order; the real orchestrator packs them into node-disjoint
  concurrent rounds (`scheduleRounds`). Total pair count and final data are
  the same; the *pacing* of progress events during a run is not representative
  of real wall-clock behavior.
- **Fixed fleet shape.** `topology.mjs`'s `DEF` table is the only topology the
  mock can serve - no scenario JSON, no CDK, no dynamic node count. Editing
  `DEF` and restarting the mock is the only way to test a different shape.
- **`replicator_id` values are synthetic** (`i-us<az><host><k>`-style, from
  `buildNodes()`), not real EC2 instance IDs - fine for display, not for
  anything that expects an `i-0123456789abcdef0`-shaped string.

If a UI change depends on backend behavior not listed as mocked above, treat
that as a real gap and extend `mock/server.mjs`/`topology.mjs` rather than
leaving the mock silently non-representative - this is exactly how the
multi-replicator mcast sweep and `/api/mcast-replicators` were added here.

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
