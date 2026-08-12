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
// ── Node size (FIXED) ────────────────────────────────────────────────────────
// Capability is encoded by COLOUR now, not size — so every node is the same
// size in each renderer. Both the 2D map (px) and the 3D scene (world units)
// import these, so node sizing lives in exactly one place.
//   2D: large enough to fit the private+public IP lines + PG/role badges.
export const NODE_RADIUS_2D = 38;
export const NODE_RADIUS_3D = 4.6;
// Kept as (node)=>… call signatures so existing call sites are untouched; the
// node argument is ignored now that size is constant.
export const nodeRadius   = () => NODE_RADIUS_2D;
export const nodeRadius3D = () => NODE_RADIUS_3D;

// ── Node body colour = capability, as a subtle blue→green tint ───────────────
// computeNodeScore() maps hardware (bandwidth, PPS, ENIs, Nitro gen, vCPU/mem,
// metal) to a score; normalise over a fixed band and lerp a calm blue (basic)
// → green (metal / top-net). Returns { t, border, bg } where `border` is the
// vivid tint (node outline / 3D mesh colour) and `bg` a dark, low-saturation
// fill — the "slight neat tint" shown as the node body.
const CAP_MIN = 15, CAP_MAX = 120;
// Multi-stop capability ramp: blue (lowest) → violet (mid) → green (highest).
// Only blue / violet / green (+ the tints the lerp makes between them). Both
// transitions stay cool/neutral — NO yellowish tints, NO gold (selection), NO cyan.
const CAP_STOPS = [
  [88, 166, 255],   // #58a6ff  blue    — lowest
  [163, 113, 247],  // #a371f7  violet  — mid
  [63, 185, 80],    // #3fb950  green   — highest
];
const CAP_DARK  = [13, 17, 23];    // #0d1117 — canvas; mixed in for the body tint
const lerp3 = (a, b, t) => [Math.round(a[0] + (b[0] - a[0]) * t), Math.round(a[1] + (b[1] - a[1]) * t), Math.round(a[2] + (b[2] - a[2]) * t)];
const rgbStr = (a) => 'rgb(' + a[0] + ',' + a[1] + ',' + a[2] + ')';
// Sample the multi-stop ramp at t∈[0,1].
function rampRGB(t) {
  const segs = CAP_STOPS.length - 1;
  const x = Math.max(0, Math.min(1, t)) * segs;
  const i = Math.min(segs - 1, Math.floor(x));
  return lerp3(CAP_STOPS[i], CAP_STOPS[i + 1], x - i);
}

// Absolute fallback: capability score → [0,1] over a fixed band. Used only when
// no fleet-derived scale is supplied (a single lone node, etc.).
export function capabilityT(node) {
  const s = computeNodeScore(node);
  return Math.max(0, Math.min(1, (s - CAP_MIN) / (CAP_MAX - CAP_MIN)));
}

// Rank-based scale: spread the ramp UNIFORMLY across the distinct capability
// scores actually present, instead of by absolute magnitude. Raw hardware scores
// cluster tightly for the small/mid instance types and then jump to a lone high
// value (metal / top-net), which under a linear absolute map crushed the small
// types into indistinguishable colour with an empty middle. Ranking by position
// guarantees each distinct instance type gets a visibly different hue. Returns a
// `scale(node) -> t in [0,1]`.
export function buildCapabilityScale(nodes) {
  const scores = [...new Set((nodes || []).map(computeNodeScore))].sort((a, b) => a - b);
  const n = scores.length;
  const rank = new Map(scores.map((s, i) => [s, n > 1 ? i / (n - 1) : 0.5]));
  return (node) => {
    const s = computeNodeScore(node);
    if (rank.has(s)) return rank.get(s);
    if (n <= 1) return 0.5;
    return Math.max(0, Math.min(1, (s - scores[0]) / (scores[n - 1] - scores[0] || 1)));
  };
}

// Node body colour = capability, across the multi-hue ramp. Pass a `scale` from
// buildCapabilityScale(fleet.nodes) so the spread is uniform across the types
// present; without it, the absolute band (capabilityT) is used. Returns
// { t, border, bg } — `border` the vivid ramp colour (outline / 3D mesh colour),
// `bg` a darker, lower-saturation fill (the node body) that keeps overlaid text
// readable while still carrying the hue.
export function capabilityColor(node, scale) {
  const t = scale ? scale(node) : capabilityT(node);
  const vivid = rampRGB(t);
  const bg = lerp3(vivid, CAP_DARK, 0.72);   // toward canvas → darker but hue-carrying tint
  return { t, border: rgbStr(vivid), bg: rgbStr(bg) };
}
// Multi-stop swatch for the legends (single source of truth for the gradient).
export const CAP_GRADIENT_CSS = 'linear-gradient(to right,' + CAP_STOPS.map(rgbStr).join(',') + ')';

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
