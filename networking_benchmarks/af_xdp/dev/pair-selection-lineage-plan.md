# Implementation Plan — Targeted Runs & Measurement Lineage

Companion to `pair-selection-lineage-design.md`. That document states the intent;
this one is the build order, the interface contracts, and the UI decisions with
their rationale.

---

## What already works (do not rebuild)

Establishing this first, because it removes most of the "upsert" work:

| Capability | Where | Status |
|---|---|---|
| Per-edge upsert, last-write-wins | `backend/collector.go` `edgeKey(kind, variation, src, dst)` | works |
| Matrix rebuilt from all edges for a chosen kind/variation | `web/src/lib/live.js` `toFleet()` | works |
| Per-edge p50 history ring (60 samples) | `Edge.History []int64` | partial — p50 only |
| Per-edge measurement timestamp | `Edge.Unix` | works |
| Online-node filtering for a campaign | `orchestrator.go` `o.reg.Online()` | works |

A subset run therefore **already** refreshes only its own cells and leaves the
rest of the grid untouched. No new "event" or "report" concept is needed. What is
missing is the ability to *scope* a run, and durability + annotation of history.

---

## Terminology (fixed, to avoid collision)

`pin` is already taken: clicking a node pins a draggable latency table
(`nodes.js` `pinNode`, `ctx.selected`, the `Deselect all` button). Do not reuse it.

- **Target set** — the nodes chosen to scope the next run.
- **Select / deselect** — adding to or removing from the target set.
- **Scope** — how the target set expands into ordered pairs.

---

## UI design decisions

### D1. Selection affordance: a checkbox on the node, not a modifier key

**Decision.** Each node renders a small circular checkbox in its top-left corner
(mirroring the existing `pg-badge`/`role-badge` pattern). Clicking the checkbox
toggles target-set membership. Clicking the node body keeps its current meaning
(pin the latency table). `Shift+click` anywhere on the node is an accelerator for
the same toggle.

**Rationale.** Three alternatives were considered:

- *Modifier only (shift+click)* — undiscoverable. Nothing on screen suggests it
  exists, and it is the kind of feature a user finds once and forgets.
- *A "Selection mode" toggle button* — discoverable, but modal. The same click
  does two different things depending on hidden state, which produces
  "why did my table not open?" confusion.
- *Checkbox on the node* — discoverable, non-modal, zero conflict with the
  existing click, and a familiar idiom (photo-gallery multi-select).

The checkbox is always visible when the target set is non-empty, and appears on
hover when it is empty, so the resting map stays clean.

### D2. The run buttons carry the scope in their label

**Decision.** The existing `kernel` / `xdp` buttons do not change position or
count. Their labels become scope-aware:

- Target set empty → `kernel` (unchanged, runs full NxN)
- Target set active → `kernel · 6 pairs`

Disabled with a tooltip when the resolved pair count is 0.

**Rationale.** The single most likely failure of this feature is a user running a
full 462-pair campaign when they meant to run 6, or vice versa. Putting the
resolved pair count on the button that triggers the run makes the scope
impossible to miss at the moment of commitment. No separate "Run targeted"
button — a second button doubles the surface and invites the wrong one being
pressed.

### D3. Scope is explicit, not inferred from count

**Decision.** A `Scope` select in the panel with three values:

| Scope | Pairs | Use |
|---|---|---|
| `Among selected` (default) | k·(k−1) | "how do these k nodes see each other" |
| `Selected → all` | k·(N−1) | "how does this node reach the fleet" |
| `All → selected` | (N−1)·k | "how does the fleet reach this node" |

**Rationale.** The design doc proposed inferring the star pattern when exactly one
node is selected. That is non-monotonic and surprising — 1 node would yield 21
pairs while 2 nodes yield 2. With an explicit scope the count only ever grows as
you select more. When `Among selected` resolves to 0 pairs (k < 2) the UI
pre-selects `Selected → all` so a single selection is still immediately runnable.

Note `All → selected` is the expensive scope: the orchestrator groups by source,
so a fan-in costs two host-profile transitions per source node (O(N) systemctl
operations) for only k measurements each. Flag it in the UI with a cost hint.

### D4. Selected pairs are drawn on the map

**Decision.** While the target set is non-empty, the edges that the next run will
measure are overlaid in a distinct style (dashed, gold, above the normal edges).
Non-target edges dim.

**Rationale.** The topology view is the natural place to answer "what exactly is
about to run". Reading a count is verification; seeing the six links light up is
comprehension. It also catches selection mistakes (wrong AZ, wrong PG) that a
number cannot.

### D5. Presets act on the hovered node's groups

**Decision.** A row of preset chips in the selection block: `Same PG`, `Same AZ`,
`One per AZ`, `One per PG`, `All`, `Clear`. Each resolves against the current
fleet and replaces the target set.

**Rationale.** The interesting questions are almost always structural
("intra-PG vs cross-PG", "cross-AZ cost"), and hand-clicking 7 nodes in a PG is
tedious and error-prone. Presets encode the questions the topology already
models. They replace rather than extend the set so the result is predictable.

### D6. Variation stays a view selector; cross-mode comparison is its own view

**Decision.** The default matrix remains **one variation per view** (today's
`Show` dropdown). Upsert applies *within* a variation. A separate opt-in
`Compare modes` view renders per-cell `kernel → xdp` deltas with mode badges.

**Rationale.** Mixing a kernel p50 and an xdp p50 in one grid as "the latest
value" invites an invalid comparison — the two measure different datapaths, and a
green cell next to a red cell could be entirely explained by mode rather than by
network position. Keeping the default view single-mode preserves the property
that *colour is comparable across the whole grid*. The cross-mode question is
real, so it gets a first-class view where the delta is the value being shown and
the badges are meaningful.

Mode annotation still appears everywhere a single cell is inspected: edge
tooltips, pinned latency tables, and the report's per-edge table.

### D7. Staleness is shown, not enforced

**Decision.** Cells/edges whose newest sample is older than the run that filled
their neighbours fade progressively: > 5 min → 70% opacity, > 1 h → 40% + dashed.
Absolute times stay in tooltips.

**Rationale.** Once subset runs exist, a grid is routinely a mosaic of different
measurement ages. Without an age cue, a stale cell reads as current fact. Fading
is the least intrusive encoding that does not consume the colour channel
(already latency) or the badge slot (already mode).

---

## Phase 1 — Targeted runs

Independently shippable. No persistence, no schema. Highest value per unit work.

### 1.1 Backend: scope the campaign

`backend/orchestrator.go`

```go
type UcastMatrixParams struct {
    // ... existing fields ...

    // Nodes optionally restricts the campaign to these instance IDs. Empty =
    // every online node (full NxN, the default).
    Nodes []string `json:"nodes,omitempty"`
    // Scope expands Nodes into ordered pairs: "among" (default) | "fanout" | "fanin".
    Scope string `json:"scope,omitempty"`
}
```

Add a resolver next to the existing online-node lookup:

```go
// resolvePairs turns the target set + scope into the (sources, destsFor) the
// source-grouped loop needs. Unknown/offline IDs are dropped, and the dropped
// set is returned so the caller can surface it.
func resolvePairs(online []Node, ids []string, scope string) (
    sources []Node, destsFor map[string][]Node, skipped []string, err error)
```

- `among` — sources = targets, dests = targets minus self
- `fanout` — sources = targets, dests = all online minus self
- `fanin` — sources = all online, dests = targets minus self
- empty `ids` — full NxN (existing behaviour), regardless of scope

`RunUcastMatrix` then iterates `sources` and uses `destsFor[src.InstanceID]`
instead of `nodes`. The prepare phase (`EnsureHost` + `PurgeDests`) must cover
the **union** of sources and all dests — not just sources — or a destination that
was left in client profile by a previous run will not echo.

Emit the resolved scope in the opening job event so the UI log states what ran:

```
running ucast/kernel — 6 pairs (among 3 selected nodes)
```

Reject with a clear `status:"error"` when the resolution yields < 1 pair.

`handleRun` needs no change beyond the struct — it already unmarshals the body
into `UcastMatrixParams`.

**Tests** (`orchestrator_test.go`): table-driven over `resolvePairs` — each scope
against a 4-node fleet, plus offline-ID filtering, unknown IDs, empty target set,
and the k=1 `among` case (0 pairs → error).

### 1.2 Web: target-set state

`web/src/App.svelte`

- `let targetIds = new Set()` and `let scope = 'among'`
- `resolvePairCount(targetIds, scope, nodes)` — mirror of the Go resolver, used
  only for the button label. Keep it in `lib/pairs.js` so 2D, 3D, and the panel
  share one implementation.
- Pass `nodes: [...targetIds]` and `scope` through `runCampaign(body)`.
- Prune ids from `targetIds` when a node leaves the fleet, on every
  `liveRerender()`, so a terminated node cannot silently scope a run.

### 1.3 Web: selection block in the control panel

`web/src/lib/controls.js` — new block directly **above** `Test Latency`, since it
scopes what those buttons do:

```
┌─ Target set ───────────────────────────────┐
│ 3 selected · 6 pairs          [Clear]      │
│ Scope  [ Among selected      ▾ ]           │
│ [Same PG] [Same AZ] [1/AZ] [1/PG] [All]    │
└────────────────────────────────────────────┘
```

Empty state is one dim line — `No selection · full mesh (462 pairs)` — so the
default is stated rather than implied by absence.

New callbacks on `mountControls`: `onScopeChange`, `onPreset`, `onClearTargets`.
New setter: `setTargets({ count, pairs, scope })`.

### 1.4 2D: checkbox, selected styling, pair overlay

`web/src/lib/2d/nodes.js`

- Render `<span class="target-box">` per node; `click` toggles and calls
  `ctx.onToggleTarget(i)`; `stopPropagation` so the table does not also pin.
- `shift+click` on the node body routes to the same toggle instead of `pinNode`.
- `.node.targeted` class → gold ring, distinct from `.selected` (table-pinned).

`web/src/lib/2d/edges.js` + `selection.js`

- `ctx.targeted: Set<number>`, and `applyTargets(ctx)` adding `.target-edge` to
  edges whose both ends are in the run set for the current scope. Reuse the
  existing dim machinery rather than adding a second opacity system.

`web/src/lib/2d/styles.js` — `.target-box`, `.node.targeted`, `.edge.target-edge`.

### 1.5 3D: parity

`web/src/lib/topology3d.js` — `shift+click` on a node toggles the target set; a
gold wireframe outline marks membership. The 3D scene has no DOM node bodies, so
the checkbox idiom does not port; the panel remains the primary control there and
the count/preset UI is shared, so no capability is lost.

### Phase 1 acceptance

- Empty target set reproduces today's full-NxN behaviour byte-for-byte
- 3 nodes, `among` → exactly 6 measurements dispatched, 6 cells refreshed, all
  other cells unchanged (verify `Unix` on untouched edges is preserved)
- Terminating a targeted node mid-session removes it from the set
- Offline targeted node is skipped with a status-line warning, run still proceeds

---

## Phase 2 — SQLite persistence

### 2.1 Driver

`modernc.org/sqlite` — pure Go, no CGO. The backend is built on the
control-plane host by user-data and on fleet nodes by `sync.yaml`; requiring a C
toolchain would break both. `mattn/go-sqlite3` is the faster driver but needs
CGO, and this workload (a few hundred inserts per campaign) is nowhere near
needing it.

### 2.2 Schema

`backend/store.go`

```sql
PRAGMA journal_mode=WAL;      -- concurrent API reads during ingest
PRAGMA synchronous=NORMAL;    -- measurement data, not money

CREATE TABLE IF NOT EXISTS runs (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  started_at  INTEGER NOT NULL,
  ended_at    INTEGER,
  kind        TEXT NOT NULL,        -- ucast | mcast
  variation   TEXT NOT NULL,
  scope       TEXT,                 -- full | among | fanout | fanin
  target_ids  TEXT,                 -- JSON array, NULL for full mesh
  pairs_total INTEGER,
  pairs_ok    INTEGER,
  params      TEXT                  -- JSON: count, rate, warmup, max_loss_pct
);

CREATE TABLE IF NOT EXISTS measurements (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id    INTEGER REFERENCES runs(id),
  unix      INTEGER NOT NULL,
  kind      TEXT NOT NULL,
  variation TEXT NOT NULL,
  src_ip    TEXT NOT NULL,
  dst_ip    TEXT NOT NULL,
  tx_mode   TEXT,                   -- zero-copy | copy | '' (kernel TX)
  p50       INTEGER, p90 INTEGER, p99 INTEGER, p999 INTEGER, max INTEGER,
  min       INTEGER, mean INTEGER,
  messages  INTEGER, lost INTEGER, loss_pct REAL,
  cmd_id    TEXT
);

CREATE INDEX IF NOT EXISTS idx_m_edge ON measurements(kind, variation, src_ip, dst_ip, unix DESC);
CREATE INDEX IF NOT EXISTS idx_m_time ON measurements(unix);
CREATE INDEX IF NOT EXISTS idx_m_run  ON measurements(run_id);
```

The `runs` table is the lineage anchor the design doc's history ring cannot
provide: it makes "the full mesh at 01:30" a queryable object, so a later
comparison is `run A vs run B` rather than a timestamp range that may straddle
two campaigns. It is also what makes the MCP surface answerable.

`idx_m_edge` is ordered `unix DESC` so both the startup ring seed and the
per-cell history tooltip are index-only scans.

### 2.3 Write path — buffered, never blocking ingest

```go
type Store struct {
    db *sql.DB
    ch chan measurementRow   // buffered, cap 4096
}
```

`Collector.Apply` stays synchronous and in-memory, then does a non-blocking
send to `ch`. A single writer goroutine drains it, batching into one transaction
per 200 rows or 500 ms, whichever comes first.

On a full channel, **drop and count** (`store_dropped_total`, logged once per
minute). The in-memory matrix is the source of truth for the live UI; losing a
history row under extreme load is strictly better than stalling telemetry ingest
and skewing the very measurement being recorded.

### 2.4 Startup seed

```go
// SeedCollector repopulates the in-memory rings from the newest N samples per
// edge so a restart does not blank the map.
func (s *Store) SeedCollector(c *Collector, perEdge int) error
```

One window-function query rather than N+1:

```sql
SELECT kind, variation, src_ip, dst_ip, unix, p50, p99, /* ... */
FROM (
  SELECT *, ROW_NUMBER() OVER (
    PARTITION BY kind, variation, src_ip, dst_ip ORDER BY unix DESC) AS rn
  FROM measurements
) WHERE rn <= ?
ORDER BY unix ASC;
```

`ORDER BY unix ASC` at the outer level so replay appends to each ring in
chronological order and `Edge.Metrics`/`Unix` end on the newest sample.

### 2.5 Retention

Hourly `time.Ticker`: `DELETE FROM measurements WHERE unix < ?`, then
`DELETE FROM runs WHERE id NOT IN (SELECT DISTINCT run_id ...)`. Weekly
`VACUUM`. Flags: `--db-path` (default `/var/lib/af-xdp-cp/measurements.db`),
`--retention-days` (default 7). `--db-path=""` disables persistence entirely so
dev/CI runs stay in-memory.

### 2.6 CDK

`deploy/cdk/lib/control-plane.ts` — `mkdir -p /var/lib/af-xdp-cp` before the
unit starts, and add the two flags to `ExecStart`. The EBS volume is already
20 GB; a 7-day DB is single-digit MB.

### Phase 2 acceptance

- Restart the backend mid-session → map repopulates from disk, no blank cells
- `--db-path=""` → no file created, behaviour identical to today
- Retention deletes rows older than the window and leaves newer ones
- Ingest throughput unchanged with the writer artificially stalled (drop path)

---

## Phase 3 — Lineage UI

### 3.1 Richer history over the wire

`Edge.History []int64` → `[]Sample`:

```go
type Sample struct {
    Unix int64 `json:"u"`
    P50  int64 `json:"p50"`
    P99  int64 `json:"p99"`
}
```

Short JSON keys: this ships on every SSE edge event and the ring is 60 deep.
Bump `edgeHistoryLen` only if the sparkline needs more than 60 points.

### 3.2 Per-cell inspection

- Edge tooltip and pinned latency tables gain `variation · relative age`
  (`33 µs · kernel · 2 min ago`).
- Clicking a row in a pinned table draws an inline sparkline from `History`.
- Age fading per **D7**, driven by `Edge.Unix` against `Date.now()`.

### 3.3 Report

`web/src/lib/report.js`

- Per-cell `title` gains variation + absolute measurement time.
- New `Measurement ages` section: oldest/newest cell, and a count of cells older
  than the newest run — the report is a snapshot of a mosaic and should say so.

### 3.4 Compare-modes view (D6)

New `Show` entry `ucast/compare (kernel vs xdp)`:

- Cell value = `Δ p50` (xdp − kernel), diverging colour scale centred on 0
- Cells missing either mode render hatched, not green
- Requires no backend change — both variations are already in the collector

---

## Phase 4 — MCP server

`control-plane/mcp/` — read-only, opens the SQLite file with `mode=ro`. Read-only
by construction: the analysis surface must not be able to mutate measurements.

| Tool | Signature | Answers |
|---|---|---|
| `list_runs` | `(kind?, variation?, since?, limit?)` | "what campaigns exist" |
| `query_latency` | `(src?, dst?, kind?, variation?, since?, limit?)` | per-pair history |
| `compare_runs` | `(run_a, run_b)` | per-cell delta between two campaigns |
| `compare_modes` | `(kind, variation_a, variation_b, since?)` | kernel vs xdp across the fleet |
| `regressions` | `(threshold_us, window_hours, kind?, variation?)` | pairs whose p50 grew |
| `topology_summary` | `()` | fleet + newest sample per edge |

Each returns rows plus the SQL used, so a result can be reproduced and audited
by hand.

---

## Build order and rationale

| # | Phase | Depends on | Value |
|---|---|---|---|
| 1 | 1.1 backend scope + tests | — | full-mesh runs become optional |
| 2 | 1.2–1.4 web target set (2D) | 1.1 | the feature is usable |
| 3 | 1.5 3D parity | 1.2 | no dead end when switching views |
| 4 | 2.1–2.4 SQLite + seed | — | restarts stop losing data |
| 5 | 2.5–2.6 retention + CDK | 2.4 | unbounded growth prevented |
| 6 | 3.1–3.3 lineage UI | 2.4 | mosaic grids become legible |
| 7 | 3.4 compare view | 3.1 | cross-mode question answered |
| 8 | 4 MCP | 2.2 | programmatic analysis |

Phase 1 ships alone and delivers the wall-clock win. Phase 2 is independent of
Phase 1 — it can be built in parallel and is what makes Phases 3 and 4 possible.
Phase 3 is the phase that Phase 1 makes *necessary*: once subset runs are routine
the grid is a mosaic, and without age cues it silently misleads.

---

## Risks

| Risk | Mitigation |
|---|---|
| Checkbox click also pins the table | `stopPropagation` on the checkbox; explicit test |
| Stale target ids scope a run to terminated nodes | Prune on every `liveRerender`; backend re-filters against `Online()` |
| `fanin` scope costs O(N) profile transitions | Cost hint in the UI; document that `fanout` is the cheap direction |
| SQLite writer stalls telemetry ingest | Buffered channel + drop-and-count; never block `Apply` |
| CGO creeps in via the driver | Pin `modernc.org/sqlite`; assert `CGO_ENABLED=0` builds in CI |
| Ring seed floods SSE on startup | Seed the collector before the HTTP listener starts |
