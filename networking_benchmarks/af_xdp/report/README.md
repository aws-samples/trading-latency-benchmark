# report/ — latency reporting + topology viewer

Turns a run's per-pair JSON into a heatmap, a shared topology model, and a 2D/3D
web view. Same pipeline for unicast and multicast; multicast additionally carries
the **two-hop split** (source→replicator, replicator→dest).

```
report/
├── gen/
│   ├── report.py       # per-pair JSON → matrix_report.html (heatmap) + matrix_summary.json
│   └── fleet_json.py   # → fleet.json (schema afxdp.topology/v1) for the web viewer
└── web/                # Vite + Svelte + three.js — renders fleet.json (2D map + 3D)
```

## What a run produces

`run_mcast.yaml` (and `run_ucast.yaml`) call both generators, so a results dir ends up with:

| File | Produced by | Contents |
|------|-------------|----------|
| `<src_ip>-<dst_ip>.json` | `mcast_receive -j` | per-pair `service_rtt_us` (one-way) **+ `hop1_us` / `hop2_us`** (mcast only) |
| `matrix_report.html` | `report.py` | heatmap; hover a cell → p50/p99/loss, and **hop1(src→repl)/hop2(repl→dst)** for mcast |
| `matrix_summary.json` | `report.py` | full NxN matrix incl. `hop1_p50_us`/`hop1_p99_us`/`hop2_p50_us`/`hop2_p99_us` |
| `fleet.json` | `fleet_json.py` | topology model for the web viewer; each edge cell carries `hop1`/`hop2` (mcast) |

Regenerate for any results dir:

```bash
python3 report/gen/report.py      results/<date>/<hh-mm-ss>-mcast
python3 report/gen/fleet_json.py  results/<date>/<hh-mm-ss>-mcast
```

## View it

**1. Heatmap (no tooling):** open `results/<date>/<...>-mcast/matrix_report.html` in a
browser. For mcast, the cell tooltip shows the one-way p50/p99 **and** the hop1/hop2 split.

**2. Web 2D/3D viewer** (interactive topology, toggles 2D map ↔ 3D):

```bash
cd report/web
npm install                                        # first time (svelte, vite, three)
npm run dev                                         # http://localhost:5173
```

Under `npm run dev` the toolbar has a **Browse results…** dropdown that lists every
`fleet.json` under `af_xdp/results/` (newest first) — pick any run and it loads live.
No copying, no URLs needed. (Alternatives below still work — e.g. for a static build.)

Point the viewer at a specific run's fleet.json without the menu:

```bash
python3 ../gen/fleet_json.py ../../results/<date>/<hh-mm-ss>-mcast public/fleet.json
npm run dev
```

Or load any fleet.json without copying it, via the `?data=` query param:

```bash
npm run dev
# then browse to:  http://localhost:5173/?data=/abs/or/served/path/fleet.json
```

Static build (open via any static server): `npm run build && npm run preview`.

Toolbar toggles the two **independent** renderers over the same `fleet.json`:
- **2D** — DOM/SVG map (`src/lib/2d/`): nodes by AZ/PG, edges shaded by p50.
- **3D** — three.js (`src/lib/topology3d.js`).

See [`web/README.md`](web/README.md) for the frontend internals and the
`afxdp.topology/v1` data contract (also in `dev/roadmap.md`).
