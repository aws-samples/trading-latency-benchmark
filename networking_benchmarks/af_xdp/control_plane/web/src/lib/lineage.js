// lineage.js — Phase 3 utilities: relative-age formatting, D7 age-fade, and
// compare-mode delta + colour.

// Format a unix timestamp (seconds) relative to `now` (ms since epoch).
// Returns e.g. "2 min ago", "1 h ago", "3 d ago".
export function relativeAge(unixSec, nowMs) {
  if (!unixSec) return '';
  const diffSec = Math.max(0, Math.floor((nowMs - unixSec * 1000) / 1000));
  if (diffSec < 60) return diffSec + ' s ago';
  if (diffSec < 3600) return Math.floor(diffSec / 60) + ' min ago';
  if (diffSec < 86400) return Math.floor(diffSec / 3600) + ' h ago';
  return Math.floor(diffSec / 86400) + ' d ago';
}

// D7 age-fade thresholds. Returns an opacity value and whether a dashed border
// should be applied. Input: age in seconds.
export function ageFade(ageSec) {
  if (ageSec > 3600) return { opacity: 0.4, dashed: true };
  if (ageSec > 300) return { opacity: 0.7, dashed: false };
  return { opacity: 1.0, dashed: false };
}

// Build a tiny inline SVG sparkline from a Sample[] history ring.
// Each sample has { u, p50, p99 }. Renders the p50 line.
export function sparklineSVG(history, { width = 120, height = 24 } = {}) {
  if (!history || history.length < 2) return '';
  const vals = history.map((s) => s.p50);
  const mn = Math.min(...vals), mx = Math.max(...vals);
  const range = mx - mn || 1;
  const points = vals.map((v, i) => {
    const x = (i / (vals.length - 1)) * width;
    const y = height - ((v - mn) / range) * (height - 4) - 2;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');
  return `<svg class="sparkline" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}"><polyline points="${points}" fill="none" stroke="#58a6ff" stroke-width="1.5"/></svg>`;
}

// Compare-mode: compute delta p50 (xdp minus kernel). Returns null if either is missing.
export function compareDelta(kernelCell, xdpCell) {
  if (!kernelCell || !xdpCell) return null;
  if (kernelCell.p50 == null || xdpCell.p50 == null) return null;
  return xdpCell.p50 - kernelCell.p50;
}

// Diverging colour scale centred on 0 for compare view.
// Negative (xdp faster) = green, positive (kernel faster) = red/orange.
export function compareColor(delta, maxAbs) {
  if (delta == null || !isFinite(delta)) return 'transparent';
  const m = maxAbs || 50;
  const t = Math.min(1, Math.abs(delta) / m);
  if (delta < 0) {
    // xdp is faster - green
    const g = Math.round(60 + t * 150);
    return `rgba(46,${g},67,${(0.3 + t * 0.6).toFixed(2)})`;
  }
  // kernel is faster - red
  const r = Math.round(100 + t * 148);
  return `rgba(${r},50,50,${(0.3 + t * 0.6).toFixed(2)})`;
}
