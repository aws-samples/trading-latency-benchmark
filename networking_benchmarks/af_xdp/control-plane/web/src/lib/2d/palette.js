// 2d/palette.js — pure visual helpers (no DOM, no ctx). Shared by the 2D modules.

// Escape a value for safe interpolation into innerHTML. Fleet data (ec2_name,
// IPs, PG/region/account names, instance types) originates from arbitrary
// sources — a ?data= URL or the live stream — so every field concatenated into
// markup MUST go through this to prevent DOM XSS.
const ESC_MAP = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
export function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ESC_MAP[c]);
}

export function fmtLat(us) {
  if (us === null || us === undefined || us === '') return '\u2014';
  const v = +us; if (!isFinite(v)) return '\u2014';
  const trim = (x) => (Math.round(x * 100) / 100).toString();
  if (v >= 500000) return trim(v / 1000000) + ' s';
  if (v >= 500) return trim(v / 1000) + ' ms';
  return Math.round(v) + ' \u03bcs';
}

// Format a min–max range with the UNIT STATED ONCE at the end. Both bounds are
// expressed in the same unit (chosen from the larger magnitude) so the result
// reads "40–120 µs" rather than "40 µs–120 µs".
export function fmtRange(a, b) {
  const lo = +a, hi = +b;
  if (!isFinite(lo) || !isFinite(hi)) return '\u2014';
  const mag = Math.max(Math.abs(lo), Math.abs(hi));
  let div = 1, unit = '\u03bcs';
  if (mag >= 500000) { div = 1000000; unit = 's'; }
  else if (mag >= 500) { div = 1000; unit = 'ms'; }
  const one = (v) => div === 1 ? String(Math.round(v)) : (Math.round((v / div) * 100) / 100).toString();
  return one(lo) + '\u2013' + one(hi) + ' ' + unit;
}

export function computeNodeScore(node) {
  // Live-stream nodes may lack the hardware-capability fields (bw_gbps, pps_mpps,
  // etc.) that only the static fleet.json carries. Coerce each to a number,
  // defaulting to 0, so a missing field can never poison the score into NaN
  // (which would render a node with width/height "NaNpx").
  const num = (v) => (Number.isFinite(+v) ? +v : 0);
  let s = 0;
  s += node.metal ? 40 : 0;
  s += (num(node.bw_gbps) / 200) * 25;
  s += (num(node.pps_mpps) / 30) * 20;
  s += (num(node.enis) / 15) * 10;
  s += (num(node.nitro_gen) / 6) * 15;
  s += (num(node.vcpus) / 192) * 8;
  s += (num(node.mem_gb) / 768) * 2;
  return s;
}
// Base radius raised so the private+public IP lines fit inside the body even for
// live nodes (whose capability score is 0). Static nodes are dominated by score.
export const nodeRadius = (node) => 44 + computeNodeScore(node) * 0.6;

const familyColors = {
  'c7i':  { bg: '#1a2a40', border: '#58a6ff' }, 'c6in': { bg: '#261a3d', border: '#a371f7' },
  'c6i':  { bg: '#1a2e1a', border: '#39d353' }, 'm7i':  { bg: '#2e2415', border: '#f0883e' },
  'r7i':  { bg: '#2e1515', border: '#da3633' }, 'm6i':  { bg: '#2e2a15', border: '#d29922' },
  'r6i':  { bg: '#2e1a1a', border: '#f85149' },
};
export const getNodeColors = (type) => familyColors[type.split('.')[0]] || { bg: '#1a2a40', border: '#58a6ff' };

export const dirSigma = (d) => (d && d.p99 > d.p50) ? (d.p99 - d.p50) / 2.326 : 0;
export function edgeSigma(ab, ba) { const v = [ab, ba].filter(Boolean).map(dirSigma); return v.length ? v.reduce((a, b) => a + b, 0) / v.length : 0; }

// jitter σ → teal → amber → rose (used for edge stroke color based on jitter)
export function jitterColor(sigma, minSigma, maxSigma) {
  const t = (maxSigma === minSigma) ? 0.5 : (sigma - minSigma) / (maxSigma - minSigma);
  const stops = [[57, 211, 83], [240, 136, 62], [248, 81, 73]];   // green → orange → red
  const seg = t <= 0.5 ? 0 : 1, lt = t <= 0.5 ? t * 2 : (t - 0.5) * 2, a = stops[seg], b = stops[seg + 1];
  return 'rgb(' + Math.round(a[0] + (b[0] - a[0]) * lt) + ',' + Math.round(a[1] + (b[1] - a[1]) * lt) + ',' + Math.round(a[2] + (b[2] - a[2]) * lt) + ')';
}

// p50 latency → green (#39d353) → orange (#f0883e) → red (#f85149).
// Lower latency = greener. t=0 at minP50, t=1 at maxP50.
export function latencyColor(p50, minP50, maxP50) {
  const t = (maxP50 === minP50) ? 0 : Math.max(0, Math.min(1, (p50 - minP50) / (maxP50 - minP50)));
  const stops = [[57, 211, 83], [240, 136, 62], [248, 81, 73]];
  const seg = t <= 0.5 ? 0 : 1, lt = t <= 0.5 ? t * 2 : (t - 0.5) * 2, a = stops[seg], b = stops[seg + 1];
  return 'rgb(' + Math.round(a[0] + (b[0] - a[0]) * lt) + ',' + Math.round(a[1] + (b[1] - a[1]) * lt) + ',' + Math.round(a[2] + (b[2] - a[2]) * lt) + ')';
}
