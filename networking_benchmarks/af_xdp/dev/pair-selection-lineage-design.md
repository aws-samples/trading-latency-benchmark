# Pair Selection & Data Lineage Design

## Problem

With 22 nodes, an NxN matrix is 462 pairs — minutes of wall-clock. Often you only
want to test specific pairs. Additionally, measurement history is lost on restart
and there's no way to compare kernel vs xdp results in a single view.

## Pair Selection: Pin Nodes → Run Subset

### Mental model

The heatmap is a **single living NxN grid**. Measurements are **upserted** — a
pinned-pair run fills/refreshes selected cells without creating a new event. The
"Show" dropdown stays as one entry per kind/variation, showing the latest value
per cell regardless of when it was taken.

### UX

- **Shift+click** a node to pin it (gold outline, pin badge)
- Panel shows "Pinned: N nodes (M pairs)"
- Existing kernel/xdp buttons **respect the pin selection**:
  - 0 pinned → full NxN (today's behaviour)
  - 1 pinned → star pattern (that node as source → all others)
  - 2+ pinned → only ordered pairs among pinned nodes
- **Clear pins** button resets to full NxN mode
- Offline pinned nodes are silently skipped (warning in status line)

### API

```json
POST /api/run
{ "kind": "ucast", "variation": "kernel", "nodes": ["i-abc", "i-def"] }
```

`nodes` is optional. When present, the orchestrator filters `reg.Online()` to only
the listed instance IDs. Omit for full NxN (backward-compatible).

### Quick presets (future)

- "Same PG only" — auto-pins all nodes in the clicked node's PG
- "Cross-AZ" — pins one node per AZ
- Right-click context menu: "Test this node → all" / "Test within this PG"

---

## Data Lineage: Per-Cell History with Mode Annotation

### Data model

Each matrix cell (src→dst) stores a **bounded history ring** of measurements:

```
cell[src][dst] = [
  { unix: 1722820000, variation: "kernel", p50: 33, p99: 38, loss: 0 },
  { unix: 1722819500, variation: "xdp",    p50: 28, p99: 32, loss: 0 },
  { unix: 1722818000, variation: "kernel", p50: 34, p99: 40, loss: 0 },
]
```

Display always shows the latest entry (or latest for a chosen variation filter).

### Single matrix, mode-annotated cells

- Cell background = latency colour (green→red)
- Tiny badge in corner: **K** (kernel) or **X** (xdp)
- Hover: "34 µs · kernel · 2 min ago"
- Filter mode: show only kernel / only xdp cells (dim missing)
- Compare mode: split cell (left kernel, right xdp) for delta analysis

### Storage: SQLite

**Choice rationale:** SQLite enables a future MCP server to query measurement
history programmatically (cross-cell analytics, regression detection, SLA
reporting). A JSONL log would require ad-hoc parsing; SQLite gives indexed
queries with zero external dependencies.

**Schema:**

```sql
CREATE TABLE measurements (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  unix       INTEGER NOT NULL,
  kind       TEXT NOT NULL,           -- ucast | mcast
  variation  TEXT NOT NULL,           -- kernel | xdp | copy | inplace
  src_ip     TEXT NOT NULL,
  dst_ip     TEXT NOT NULL,
  p50        INTEGER,
  p90        INTEGER,
  p99        INTEGER,
  p999       INTEGER,
  max        INTEGER,
  loss_pct   REAL,
  count      INTEGER,
  cmd_id     TEXT,                    -- correlation with the campaign
  created_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX idx_measurements_edge ON measurements(kind, variation, src_ip, dst_ip);
CREATE INDEX idx_measurements_time ON measurements(unix);
```

**Retention:** periodic `DELETE FROM measurements WHERE unix < ?` based on
configurable `--retention-days` (default 7).

**Hot path:** The in-memory collector ring stays for live rendering (zero-latency
SSE). SQLite is the **write-behind** durable store — every `collector.Apply()`
also inserts a row. On startup, the ring is seeded from `SELECT ... ORDER BY unix
DESC LIMIT <ring_size>` per edge.

**File location:** `/var/lib/af-xdp-cp/measurements.db` (configurable via
`--db-path`).

### UI rendering with lineage

1. **Heatmap cell** — latest value + mode badge (K/X)
2. **Cell hover** — history: "kernel 33µs (2m ago) · xdp 28µs (5m ago)"
3. **Cell click** — sparkline of p50 over time, colour-coded by mode
4. **Stale indicator** — cells >5min fade; >1h dashed border
5. **Report** — latest-per-cell + optional full history table

### MCP Server (future)

An MCP tool server exposes the SQLite DB for AI-assisted analysis:

- `query_latency(src, dst, kind, variation, since)` — per-pair history
- `compare_modes(src, dst)` — kernel vs xdp delta for a pair
- `regressions(threshold_us, window_hours)` — pairs whose p50 increased
- `topology_summary()` — current fleet state + latest measurements

---

## Implementation Order

1. Backend: add `Nodes []string` to `UcastMatrixParams`, filter in orchestrator
2. Web: shift+click pin tracking, pass pinned IDs to `runCampaign`
3. Backend: SQLite write-behind in collector (+ startup seed)
4. Backend: retention GC (hourly timer)
5. Web: mode badge on cells, stale indicator, history tooltip
6. MCP server: read-only query tools over the SQLite DB
