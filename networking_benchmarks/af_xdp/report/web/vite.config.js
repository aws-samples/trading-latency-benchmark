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
 *   GET /api/results        -> [{ path, date, run, kind, mtime }, ...] (newest first)
 *   GET /api/fleet?path=REL  -> the fleet.json at results/REL/fleet.json
 * (Present under `vite dev` only; a static `build`/`preview` has no API, and the
 *  Browse menu simply hides itself — ?data= and the bundled fleet.json still work.)
 */
function resultsBrowser() {
  return {
    name: 'afxdp-results-browser',
    configureServer(server) {
      server.middlewares.use('/api/results', (_req, res) => {
        const runs = [];
        try {
          for (const date of fs.readdirSync(RESULTS_ROOT)) {
            const dateDir = path.join(RESULTS_ROOT, date);
            if (!fs.statSync(dateDir).isDirectory()) continue;
            for (const run of fs.readdirSync(dateDir)) {
              const fleet = path.join(dateDir, run, 'fleet.json');
              if (fs.existsSync(fleet)) {
                const m = /-(mcast|ucast)$/.exec(run);
                runs.push({
                  path: path.posix.join(date, run),
                  date, run,
                  kind: m ? m[1] : 'run',
                  mtime: fs.statSync(fleet).mtimeMs,
                });
              }
            }
          }
        } catch { /* results/ may not exist yet */ }
        runs.sort((a, b) => b.mtime - a.mtime);
        res.setHeader('Content-Type', 'application/json');
        res.end(JSON.stringify(runs));
      });

      server.middlewares.use('/api/fleet', (req, res) => {
        const rel = new URL(req.url, 'http://x').searchParams.get('path') || '';
        const abs = path.resolve(RESULTS_ROOT, rel, 'fleet.json');
        // Contain the read within RESULTS_ROOT (block ../ traversal).
        if (!abs.startsWith(RESULTS_ROOT + path.sep) || !fs.existsSync(abs)) {
          res.statusCode = 404;
          res.setHeader('Content-Type', 'application/json');
          res.end('{"error":"fleet.json not found"}');
          return;
        }
        res.setHeader('Content-Type', 'application/json');
        res.end(fs.readFileSync(abs));
      });
    },
  };
}

// base: './' so a built bundle works when opened from any path.
export default defineConfig({
  base: './',
  plugins: [svelte(), resultsBrowser()],
});
