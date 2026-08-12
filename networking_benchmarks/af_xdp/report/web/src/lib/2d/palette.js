// 2d/palette.js — pure visual helpers (no DOM, no ctx). Shared by the 2D modules.

export function fmtLat(us) {
  if (us === null || us === undefined || us === '') return '\u2014';
  const v = +us; if (!isFinite(v)) return '\u2014';
  const trim = (x) => (Math.round(x * 100) / 100).toString();
  if (v >= 500000) return trim(v / 1000000) + ' s';
  if (v >= 500) return trim(v / 1000) + ' ms';
  return Math.round(v) + ' \u03bcs';
}

export function computeNodeScore(node) {
  let s = 0;
  s += node.metal ? 40 : 0; s += (node.bw_gbps / 200) * 25; s += (node.pps_mpps / 30) * 20;
  s += (node.enis / 15) * 10; s += (node.nitro_gen / 6) * 15; s += (node.vcpus / 192) * 8; s += (node.mem_gb / 768) * 2;
  return s;
}
export const nodeRadius = (node) => 30 + computeNodeScore(node) * 0.6;

const familyColors = {
  'c7i':  { bg: '#1a2a40', border: '#58a6ff' }, 'c6in': { bg: '#261a3d', border: '#a371f7' },
  'c6i':  { bg: '#1a2e1a', border: '#39d353' }, 'm7i':  { bg: '#2e2415', border: '#f0883e' },
  'r7i':  { bg: '#2e1515', border: '#da3633' }, 'm6i':  { bg: '#2e2a15', border: '#d29922' },
  'r6i':  { bg: '#2e1a1a', border: '#f85149' },
};
export const getNodeColors = (type) => familyColors[type.split('.')[0]] || { bg: '#1a2a40', border: '#58a6ff' };

export const dirSigma = (d) => (d && d.p99 > d.p50) ? (d.p99 - d.p50) / 2.326 : 0;
export function edgeSigma(ab, ba) { const v = [ab, ba].filter(Boolean).map(dirSigma); return v.length ? v.reduce((a, b) => a + b, 0) / v.length : 0; }

// jitter σ → teal → amber → rose
export function jitterColor(sigma, minSigma, maxSigma) {
  const t = (maxSigma === minSigma) ? 0.5 : (sigma - minSigma) / (maxSigma - minSigma);
  const stops = [[57, 211, 83], [240, 136, 62], [248, 81, 73]];   // green → orange → red
  const seg = t <= 0.5 ? 0 : 1, lt = t <= 0.5 ? t * 2 : (t - 0.5) * 2, a = stops[seg], b = stops[seg + 1];
  return 'rgb(' + Math.round(a[0] + (b[0] - a[0]) * lt) + ',' + Math.round(a[1] + (b[1] - a[1]) * lt) + ',' + Math.round(a[2] + (b[2] - a[2]) * lt) + ')';
}
