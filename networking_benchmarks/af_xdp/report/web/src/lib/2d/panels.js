// 2d/panels.js — Summary/Legend/Instance-Types panels + enhancePanel
// (draggable title, fold, resize with content that scales to the panel width).

import { fmtLat, nodeRadius, getNodeColors } from './palette.js';

// Wrap a panel's content in a scaler that transform-scales with the panel width.
// Returns a cleanup(); pushes it to ctx.disposers when track is true.
export function enhancePanel(ctx, el, track = true) {
  const h = el.querySelector('h3'); if (!h) return () => {};
  const body = document.createElement('div'); body.className = 'panel-body';
  while (h.nextSibling) body.appendChild(h.nextSibling); el.appendChild(body);
  const caret = document.createElement('span'); caret.className = 'panel-caret'; caret.textContent = '\u25be'; h.insertBefore(caret, h.firstChild);
  let collapsed = false, dragging = false, moved = false, sx = 0, sy = 0, ox = 0, oy = 0;
  const scaler = document.createElement('div'); scaler.className = 'panel-scale';
  while (el.firstChild) scaler.appendChild(el.firstChild); el.appendChild(scaler);
  let base = 0;
  const update = () => {
    if (!base) { base = el.clientWidth || 1; scaler.style.width = base + 'px'; }
    const k = el.clientWidth / base;
    scaler.style.transform = 'scale(' + k + ')';
    el.style.height = Math.ceil(scaler.offsetHeight * k) + 'px';
  };
  h.addEventListener('mousedown', (e) => { dragging = true; moved = false; sx = e.clientX; sy = e.clientY;
    const r = el.getBoundingClientRect(); ox = r.left; oy = r.top; el.style.right = 'auto'; el.style.bottom = 'auto'; el.style.left = ox + 'px'; el.style.top = oy + 'px'; e.preventDefault(); });
  const onMove = (e) => { if (!dragging) return; const dx = e.clientX - sx, dy = e.clientY - sy; if (Math.abs(dx) + Math.abs(dy) > 3) moved = true; el.style.left = (ox + dx) + 'px'; el.style.top = (oy + dy) + 'px'; };
  const onUp = () => { dragging = false; };
  window.addEventListener('mousemove', onMove); window.addEventListener('mouseup', onUp);
  h.addEventListener('click', () => { if (moved) { moved = false; return; } collapsed = !collapsed; body.style.display = collapsed ? 'none' : ''; caret.textContent = collapsed ? '\u25b8' : '\u25be'; update(); });
  const ro = new ResizeObserver(update); ro.observe(el);
  const cleanup = () => { ro.disconnect(); window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp); };
  if (track) ctx.disposers.push(cleanup);
  return cleanup;
}

export function renderPanels(ctx) {
  const { fleet, root, statsEl, N, region, stress } = ctx;
  const { minP50, maxP50, minP99, maxP99, minSigma, maxSigma, allP50 } = ctx.ranges;

  // Instance-type legend
  (function () {
    const seen = new Map(); fleet.nodes.forEach(n => { if (!seen.has(n.type)) seen.set(n.type, n); });
    let rows = '';
    for (const [type, node] of seen) {
      const colors = getNodeColors(type), r = Math.round(nodeRadius(node) * 0.35), family = type.split('.')[0];
      rows += '<div class="type-row"><div class="type-dot" style="width:' + (r * 2) + 'px;height:' + (r * 2) + 'px;background:' + colors.bg + ';border:2px solid ' + colors.border + '"></div>'
        + '<div class="type-info"><div class="type-name">' + type + '</div><div class="type-specs">' + node.vcpus + 'vCPU \u00b7 ' + node.mem_gb + 'GB \u00b7 ' + node.bw_gbps + 'Gbps \u00b7 ' + node.pps_mpps + 'Mpps \u00b7 ' + node.enis + ' ENIs \u00b7 Nitro ' + node.nitro_gen + '</div></div>'
        + '<a href="https://instances.vantage.sh/?selected=' + type + '&region=' + region + '" target="_blank">specs\u2197</a>'
        + '<a href="https://aws.amazon.com/ec2/instance-types/' + family + '/" target="_blank">family\u2197</a></div>';
    }
    const el = document.createElement('div'); el.className = 'instance-legend'; el.innerHTML = '<h3>Instance Types</h3>' + rows; root.appendChild(el);
  })();

  // Visual-encoding legend
  (function () {
    const el = document.createElement('div'); el.className = 'vis-legend';
    el.innerHTML = '<h3>Legend</h3>'
      + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,#39d353,#f0883e,#f85149)"></div><span>Edge color = jitter \u03c3 (' + fmtLat(minSigma) + ' \u2192 ' + fmtLat(maxSigma) + ')</span></div>'
      + '<div class="row"><span>Node size = f(BW, PPS, ENIs, Nitro, CPU, Mem, metal)</span></div>'
      + '<div class="row"><span>Distance \u221d p50 (1:1 \u2264 40\u03bcs, log-compressed above) \u2014 stress ' + (stress * 100).toFixed(1) + '%</span></div>'
      + '<div class="row"><span style="color:#fff;font-weight:700">Public IP</span><span style="color:#8b949e">&nbsp;/&nbsp;</span><span style="color:#8b949e">Private IP</span><span>&nbsp;\u2014 shown on each node</span></div>'
      + '<div class="contour-samples">'
      + '<span style="border:1.5px dashed rgba(57,211,83,0.3);color:#39d353">VPC</span>'
      + '<span style="border:1.5px dashed rgba(163,113,247,0.3);color:#a371f7">AZ</span>'
      + '<span style="border:1.5px dashed rgba(88,166,255,0.3);color:#58a6ff">Region</span>'
      + '<span style="border:1.5px solid rgba(248,81,73,0.5);color:#f85149">Account</span></div>'
      + '<div class="ux-hint"><b>Hover</b> a node \u2014 reveal its edge latencies &amp; highlight. <b>Click</b> a node \u2014 pin its latency window (click again to close). <b>Drag</b> a panel\u2019s title to move it; <b>click</b> the title to fold; drag the edge to resize.</div>';
    root.appendChild(el);
  })();

  // Summary / stats
  const medP50 = allP50.length ? [...allP50].sort((a, b) => a - b)[Math.floor(allP50.length / 2)] : 0;
  const uniq = (k) => [...new Set(fleet.nodes.map(n => n[k]))].filter(v => v !== 'unknown');
  const uRegions = uniq('region'), uAZs = uniq('az'), uCPGs = uniq('cpg_name'), uAccounts = uniq('account');
  const stat = (label, val) => '<div class="stat"><span>' + label + '</span><span class="val">' + val + '</span></div>';
  let scopeHtml = '';
  if (uCPGs.length === 1) scopeHtml += stat('Placement Group', uCPGs[0]); else if (uCPGs.length > 1) scopeHtml += stat('Placement Groups', uCPGs.length);
  if (uAZs.length === 1) scopeHtml += stat('AZ', uAZs[0]); else if (uAZs.length > 1) scopeHtml += stat('AZs', uAZs.join(', '));
  if (uRegions.length === 1) scopeHtml += stat('Region', uRegions[0]); else if (uRegions.length > 1) scopeHtml += stat('Regions', uRegions.join(', '));
  if (uAccounts.length === 1) scopeHtml += stat('Account', uAccounts[0]); else if (uAccounts.length > 1) scopeHtml += stat('Accounts', uAccounts.length);
  statsEl.innerHTML = '<h3>Summary</h3>'
    + stat('Nodes', N) + stat('Pairs', allP50.length)
    + stat('p50 range', fmtLat(minP50) + '\u2013' + fmtLat(maxP50))
    + stat('p99 range', fmtLat(minP99) + '\u2013' + fmtLat(maxP99))
    + stat('Jitter \u03c3', fmtLat(minSigma) + '\u2013' + fmtLat(maxSigma))
    + stat('Median p50', fmtLat(medP50))
    + stat('Spread', fmtLat(maxP50 - minP50))
    + '<div style="margin-top:8px;border-top:1px solid #30363d;padding-top:6px">' + scopeHtml + '</div>'
    + '<div class="stress">Layout fidelity (SMACOF): <span class="val">' + (100 - stress * 100).toFixed(1) + '%</span> \u2014 stress ' + (stress * 100).toFixed(1) + '%</div>';

  root.querySelectorAll('.stats, .vis-legend, .instance-legend').forEach(el => enhancePanel(ctx, el));
}
