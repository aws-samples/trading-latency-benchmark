# report/web — topology frontend (Vite + Svelte + three.js)

Renders the shared `fleet.json` topology model (schema `afxdp.topology/v1`, see
`dev/roadmap.md`). This is **Phase 0** of the real-time roadmap: data is decoupled
from view — the Python generators (`report/gen/`) emit `fleet.json`, and this app
renders it. Later the same app will consume a live WebSocket stream unchanged.

## Run (dev)

```bash
cd report/web
npm install            # svelte, vite, three
npm run dev            # http://localhost:5173  (loads public/fleet.json)
```

Point it at any results dir's data:

```bash
# from a results dir, emit fleet.json into public/, then:
python3 ../gen/fleet_json.py <results_dir> public/fleet.json
npm run dev
# or load an arbitrary URL:  http://localhost:5173/?data=/some/fleet.json
```

## Build (static)

```bash
npm run build          # -> dist/ (relative base; open dist/index.html via a static server)
npm run preview
```

## Layout

```
web/
├── index.html            entry
├── src/
│   ├── main.js           mounts App
│   ├── App.svelte        fetches fleet.json (?data= override) and mounts the renderer
│   ├── app.css           global styles for the 3D renderer
│   └── lib/
│       ├── 2d/            2D DOM/SVG map — concern-modules (index, layout, palette,
│       │                  contours, edges, nodes, panels, selection, tables, styles) sharing a ctx
│       └── topology3d.js  3D three.js renderer (uses app.css)
└── public/fleet.json     sample data (regenerate with gen/fleet_json.py)
```

## Notes
- three.js is a real dependency (bundled by Vite) — no CDN, no WASM.
- Two **independent** renderers toggled from the toolbar on the same `fleet.json`:
  `topology3d.js` (three.js, orbit) and `2d/` (DOM/SVG map ported from the
  original `topology_map.html`; injects its own scoped styles). No shared module — a
  small amount of helper duplication is intentional to keep them decoupled.
- The NxN **heatmap** (`matrix_report.html`) remains a Python batch artifact for now
  (`gen/report.py`); it could be added to the web app as a table view later.
- **Next:** add a live (WebSocket) data source alongside the static `fleet.json`
  fetch — the renderers consume the same model (see `dev/roadmap.md`).
