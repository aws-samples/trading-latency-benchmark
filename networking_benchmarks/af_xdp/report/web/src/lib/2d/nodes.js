// 2d/nodes.js — node circles, hover latency tooltip, and click-to-pin.
//
// Clicking a node freezes the *very* hover window currently shown into a fixed,
// resizable panel (no duplicate element) and spawns a fresh transient tooltip
// for subsequent hovers. Clicking the node again (or Deselect all) closes it.

import { nodeRadius, getNodeColors } from './palette.js';
import { buildPeerTable } from './tables.js';
import { applySel } from './selection.js';
import { enhancePanel } from './panels.js';

export function renderNodes(ctx) {
  const { fleet, root, positions, selected, nodeEls, N, W, H } = ctx;

  const makeTooltip = () => {
    const t = document.createElement('div'); t.className = 'node-tooltip'; root.appendChild(t);
    // hovering a peer row in the latency table highlights that node in the graph
    t.addEventListener('mouseover', (e) => { const tr = e.target.closest && e.target.closest('tr[data-peer]'); if (tr && nodeEls[+tr.dataset.peer]) nodeEls[+tr.dataset.peer].classList.add('peer-hover'); });
    t.addEventListener('mouseout', (e) => { const tr = e.target.closest && e.target.closest('tr[data-peer]'); if (tr && nodeEls[+tr.dataset.peer]) nodeEls[+tr.dataset.peer].classList.remove('peer-hover'); });
    return t;
  };
  let tooltip = makeTooltip();
  let pinnedEl = null, pinnedDispose = null;

  const tipHTML = (i) => {
    const node = fleet.nodes[i];
    const roleBadge = (node.role && node.role !== 'unknown')
      ? ' <span class="role-badge role-' + node.role + '">' + node.role + '</span>' : '';
    // The relay's hops are real edges now (src->relay, relay->dst), so the normal
    // peer tables show them: Inbound = hop1 (from source), Outbound = hop2 (to dest).
    return '<h3>' + node.ec2_name + roleBadge + '</h3>'
      + '<div class="direction">Outbound</div>' + buildPeerTable(ctx, i, false)
      + '<div class="direction" style="margin-top:6px">Inbound</div>' + buildPeerTable(ctx, i, true);
  };
  const positionTip = (t, e) => {
    let tx = e.clientX + 16, ty = e.clientY - 10;
    const tw = t.offsetWidth || 280, th = t.offsetHeight || 200;
    if (tx + tw > W - 20) tx = e.clientX - tw - 16;
    if (ty + th > H - 20) ty = H - th - 20;
    if (ty < 10) ty = 10;
    t.style.left = tx + 'px'; t.style.top = ty + 'px';
  };
  const unpin = () => {
    if (pinnedDispose) { pinnedDispose(); pinnedDispose = null; }
    if (pinnedEl && pinnedEl.parentNode) pinnedEl.parentNode.removeChild(pinnedEl);
    pinnedEl = null;
  };
  const pinCurrent = (i) => {
    unpin();
    tooltip.innerHTML = tipHTML(i);          // ensure it shows this node
    pinnedEl = tooltip;                       // freeze the very hover window
    pinnedEl.classList.remove('visible'); pinnedEl.classList.add('pinned');
    pinnedDispose = enhancePanel(ctx, pinnedEl, false);
    tooltip = makeTooltip();                  // fresh transient for future hovers
  };
  ctx.unpinAll = unpin;
  ctx.disposers.push(unpin);

  fleet.nodes.forEach((node, i) => {
    const r = nodeRadius(node), colors = getNodeColors(node.type);
    const el = document.createElement('div'); el.className = 'node' + (node.role && node.role !== 'unknown' ? ' role-' + node.role : '');
    el.style.width = el.style.height = (r * 2) + 'px';
    el.style.left = (positions[i].x - r) + 'px'; el.style.top = (positions[i].y - r) + 'px';
    el.style.background = colors.bg; el.style.borderColor = colors.border;
    el.innerHTML = '<span class="ip ip-private">' + node.private_ip + '</span>'
      + '<span class="ip ip-public">' + (node.public_ip || '\u2014') + '</span>'
      + ((node.cpg_name && node.cpg_name !== 'unknown') ? '<span class="pg-badge" title="Placement group">' + node.cpg_name + '</span>' : '')
      + (node.role === 'replicator' ? '<span class="role-badge relay" title="Relay / fan-out hop — carries hop1+hop2 of the flows through it">relay</span>' : '');
    root.appendChild(el); nodeEls[i] = el;

    el.addEventListener('mouseenter', () => { tooltip.innerHTML = tipHTML(i); tooltip.classList.add('visible'); if (selected.size === 0) applySel(ctx, i); });
    el.addEventListener('mousemove', (e) => positionTip(tooltip, e));
    el.addEventListener('mouseleave', () => { tooltip.classList.remove('visible'); if (selected.size === 0) applySel(ctx, -1); });
    el.addEventListener('click', () => {
      if (selected.has(i)) selected.delete(i); else selected.add(i);
      applySel(ctx, -1);
      if (selected.has(i)) pinCurrent(i);
      else unpin();
    });
  });
}
