import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
// af_xdp/report/web  ->  af_xdp/results
const RESULTS_ROOT = path.resolve(__dirname, '../../results');

/**
 * Dev-only API so the app's "Browse" menu can load any run's fleet.json
 * straight from the results dir — no copying into public/, no ?data= URLs.
 *   GET /api/results        -> [{ path, name, mtime }, ...] (newest first)
 *   GET /api/fleet?path=REL  -> the fleet.json at results/REL/fleet.json
 * (Present under `vite dev` only; a static `build`/`preview` has no API, and the
 *  Browse menu simply hides itself — ?data= and the bundled fleet.json still work.)
 */
function resultsBrowser() {
  // Recursively collect every subdirectory (any depth) that contains a
  // fleet.json — not just the timestamped date/run two-level layout.
  const findFleets = (root) => {
    const out = [];
    const walk = (dir, rel) => {
      let entries;
      try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
      if (rel && entries.some((e) => e.isFile() && e.name === 'fleet.json')) {
        const fleet = path.join(dir, 'fleet.json');
        out.push({ path: rel, name: rel, mtime: fs.statSync(fleet).mtimeMs });
      }
      for (const e of entries) {
        if (e.isDirectory()) walk(path.join(dir, e.name), rel ? path.posix.join(rel, e.name) : e.name);
      }
    };
    walk(root, '');
    return out;
  };

  return {
    name: 'afxdp-results-browser',
    configureServer(server) {
      server.middlewares.use('/api/results', (_req, res) => {
        let runs = [];
        try { runs = findFleets(RESULTS_ROOT); } catch { /* results/ may not exist yet */ }
        runs.sort((a, b) => b.mtime - a.mtime);
        res.setHeader('Content-Type', 'application/json');
        res.end(JSON.stringify(runs));
      });

      server.middlewares.use('/api/fleet', (req, res) => {
        const rel = new URL(req.url, 'http://x').searchParams.get('path') || '';
        const fail = () => { res.statusCode = 404; res.setHeader('Content-Type', 'application/json'); res.end('{"error":"fleet.json not found"}'); };
        // Reject any '..' segment outright before resolving (defence in depth).
        if (rel.split(/[\\/]/).includes('..')) return fail();
        const abs = path.resolve(RESULTS_ROOT, rel, 'fleet.json');
        // Contain the read within RESULTS_ROOT (block traversal + symlink escape).
        let real;
        try { real = fs.realpathSync(abs); } catch { return fail(); }
        const realRoot = fs.realpathSync(RESULTS_ROOT);
        if (!real.startsWith(realRoot + path.sep) || path.basename(real) !== 'fleet.json') return fail();
        res.setHeader('Content-Type', 'application/json');
        res.end(fs.readFileSync(real));
      });
    },
  };
}

// base: './' so a built bundle works when opened from any path.
// Dev proxy: forward the LIVE control-plane endpoints to a running backend so
// `npm run dev` gets live data/streaming without rebuilding + copying the
// bundle to the host. Point it with CP_URL (default localhost:8080). The
// results-browser middleware keeps /api/results and /api/fleet?path= local.
const CP = process.env.CP_URL || 'http://localhost:8080';

export default defineConfig({
  base: './',
  plugins: [svelte(), resultsBrowser()],
  server: {
    proxy: {
      '/api/events': { target: CP, changeOrigin: true },
      '/api/run':    { target: CP, changeOrigin: true },
      '/api/cancel': { target: CP, changeOrigin: true },
      '/api/cmd':    { target: CP, changeOrigin: true },
      '/api/mcast-replicators': { target: CP, changeOrigin: true },
      '/api/measurements': { target: CP, changeOrigin: true },
      '/api/errors': { target: CP, changeOrigin: true },
    },
  },
});
