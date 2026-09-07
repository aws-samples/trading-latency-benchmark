// 2d/nodes.js — node circles, hover tooltip, multi-node click-to-pin.
//
// Hover   → transient floating tooltip (hidden while any table is pinned).
// Click   → pin a draggable/foldable latency table for this node.
//           Multiple nodes can be pinned simultaneously.
// Click again (or Deselect All) → unpin and remove the table.

import { nodeRadius, capabilityColor, buildCapabilityScale, esc } from './palette.js';
import { enhancePinned } from './panels.js';
import { nodeTipHTML } from './tables.js';
import { applySel } from './selection.js';

const ROLE_LABEL = { source: 'src', replicator: 'relay', destination: 'dst' };
const ROLE_CSS   = { source: 'src', replicator: 'relay', destination: 'dst' };

const PANEL_GAP = 8, PANEL_TOP = 80;

// Build the HTML content of a tooltip/pinned table for node i. Delegates to the
// SHARED builder (2d/tables.js) so the 2D and 3D latency panels are identical.
function tipHTML(ctx, i) {
  return nodeTipHTML(ctx.fleet, ctx.matrix, i);
}


// Pins live outside the mount: a live update rebuilds the view every few
// hundred ms and a pinned table must survive that. Keyed by instance id, and
// holding the position it was frozen at.
const PINS = new Map();
const nodeKey = (n) => (n && (n.instance_id || n.private_ip)) || '';

export function renderNodes(ctx) {
  const { fleet, root, positions, nodeEls, W, H } = ctx;

  // ── Transient hover tooltip ─────────────────────────────────────────────
  const makeTooltip = () => {
    const t = document.createElement('div'); t.className = 'node-tooltip'; root.appendChild(t);
    t.addEventListener('mouseover', (e) => {
      const tr = e.target.closest && e.target.closest('tr[data-peer]');
      if (tr && nodeEls[+tr.dataset.peer]) nodeEls[+tr.dataset.peer].classList.add('peer-hover');
    });
    t.addEventListener('mouseout', (e) => {
      const tr = e.target.closest && e.target.closest('tr[data-peer]');
      if (tr && nodeEls[+tr.dataset.peer]) nodeEls[+tr.dataset.peer].classList.remove('peer-hover');
    });
    return t;
  };
  let tooltip = makeTooltip();

  // ── Pinned tables ─────────────────────────────────────────────────────────
  // Map<nodeIndex → { el: HTMLElement, dispose: fn }>
  const pinned = new Map();
  // Hover always works - a pinned table no longer suppresses it.
  const hoverActive = () => true;

  const unpin = (i) => {
    const p = pinned.get(i); if (!p) return;
    p.dispose();
    if (p.el.parentNode) p.el.parentNode.removeChild(p.el);
    pinned.delete(i);
    PINS.delete(nodeKey(((fleet && fleet.nodes) || [])[i]));
    ctx.selected.delete(i);
    applySel(ctx, -1);                 // drop this node's persisted edge labels
    if (pinned.size === 0) ctx.deselectBtn.style.display = 'none';
  };

  const unpinAll = () => [...pinned.keys()].forEach(unpin);

  const pinNode = (i, at) => {
    if (pinned.has(i)) return;
    // Freeze where the click happened, or where a restored pin was left.
    const left = (at && at.left != null) ? at.left : PANEL_GAP * 2;
    const top  = (at && at.top  != null) ? at.top  : PANEL_TOP;

    // A pinned panel is the hover tooltip frozen in place. It carries ONLY
    // placement inline; width/height/padding come from the .node-tooltip base
    // styles, so it auto-sizes to its content exactly like the hover tooltip
    // (never trimmed) and pinning changes nothing but the frame/edge.
    const panelEl = document.createElement('div');
    panelEl.className = 'node-tooltip pinned';
    panelEl.style.cssText = [
      'z-index:200',
      'left:' + left + 'px',
      'top:' + top + 'px',
    ].join(';');
    panelEl.innerHTML = tipHTML(ctx, i);
    (ctx.viewport || root).appendChild(panelEl);

    const key = nodeKey(((fleet && fleet.nodes) || [])[i]);
    const dispose = enhancePinned(panelEl, {
      scale: () => (ctx.zoomScale && ctx.zoomScale()) || 1,
      onMove: (l, t) => PINS.set(key, { left: l, top: t }),
    });
    pinned.set(i, { el: panelEl, dispose });
    PINS.set(nodeKey(((fleet && fleet.nodes) || [])[i]), { left, top });
    ctx.selected.add(i);               // pinning a node also pins its edge labels
    applySel(ctx, -1);
    ctx.deselectBtn.style.display = 'inline-block';
  };

  // Recreate pins recorded before this mount. Deferred to the caller: it must
  // run once edges, nodes and the viewport exist.
  ctx.restorePins = () => {
    ((fleet && fleet.nodes) || []).forEach((n, k2) => {
      const at = PINS.get(nodeKey(n));
      if (at) pinNode(k2, at);
    });
  };

  ctx.unpinAll = () => { unpinAll(); PINS.clear(); applySel(ctx, -1); tooltip.classList.remove('visible'); };
  // Dispose detaches the DOM only; PINS keeps the pins so the next mount
  // restores them. Only a node click or Deselect all removes a pin.
  ctx.disposers.push(() => pinned.forEach((pn) => {
    pn.dispose();
    if (pn.el.parentNode) pn.el.parentNode.removeChild(pn.el);
  }));

  // ── Node elements ─────────────────────────────────────────────────────────
  const capScale = buildCapabilityScale(((fleet && fleet.nodes) || []));   // uniform blue→green over present types
  ((fleet && fleet.nodes) || []).forEach((node, i) => {
    const r = nodeRadius(node), colors = capabilityColor(node, capScale);
    const el = document.createElement('div');
    el.className = 'node' + (node.role ? ' role-' + node.role : '') + (node.online === false ? ' offline' : '');
    el.style.width = el.style.height = (r * 2) + 'px';
    el.style.left = (positions[i].x - r) + 'px';
    el.style.top  = (positions[i].y - r) + 'px';
    el.style.background = colors.bg;
    // Border: role colour (solid) overrides the capability tint when a role is set.
    const ROLE_BORDER = { replicator: '#f0883e', source: '#1f6feb', destination: '#2ea043' };
    el.style.borderColor = ROLE_BORDER[node.role] || colors.border;
    el.style.borderStyle = 'solid';

    const pgBadge = (node.cpg_name && node.cpg_name !== 'unknown')
      ? '<span class="pg-badge" title="' + esc(node.cpg_name) + '">' + esc(node.cpg_name.slice(0, 8)) + '</span>' : '';
    const roleLabel = node.role && ROLE_LABEL[node.role];
    const roleBadge = roleLabel
      ? '<span class="role-badge role-' + esc(ROLE_CSS[node.role] || node.role) + '">' + roleLabel + '</span>' : '';
    // Target checkbox (D1): contour is always visible; only the fill changes.
    const instanceId = node.instance_id || node.private_ip;
    const isTargeted = ctx.targetIds.has(instanceId);
    const targetBoxClass = 'target-box' + (isTargeted ? ' checked' : '');
    el.innerHTML = '<span class="' + targetBoxClass + '" data-target-box></span>'
      + '<span class="ip ip-private">' + esc(node.private_ip) + '</span>'
      + '<span class="ip ip-public">' + (node.public_ip ? esc(node.public_ip) : '\u2014') + '</span>'
      + pgBadge + roleBadge;
    if (isTargeted) el.classList.add('targeted');
    root.appendChild(el);
    nodeEls[i] = el;

    // Target checkbox click handler: toggle and stopPropagation.
    const targetBox = el.querySelector('[data-target-box]');
    targetBox.addEventListener('click', (e) => {
      e.stopPropagation();
      ctx.onToggleTarget(instanceId);
    });

    const positionTip = (e) => {
      // Accept both real MouseEvents and synthetic {clientX, clientY} objects.
      let tx = e.clientX + 16, ty = e.clientY - 10;
      const tw = tooltip.offsetWidth || 280, th = tooltip.offsetHeight || 200;
      if (tx + tw > W - 20) tx = e.clientX - tw - 16;
      if (ty + th > H - 20) ty = H - th - 20;
      if (ty < 10) ty = 10;
      tooltip.style.left = tx + 'px'; tooltip.style.top = ty + 'px';
    };

    el.addEventListener('mouseenter', () => {
      applySel(ctx, i);
      if (!hoverActive()) return;
      tooltip.innerHTML = tipHTML(ctx, i);
      tooltip.classList.add('visible');
      // Position immediately using the node's known position so the tooltip
      // never renders at 0,0 while waiting for the first mousemove.
      positionTip({ clientX: positions[i].x + r + 16, clientY: positions[i].y });
    });
    el.addEventListener('mousemove', (e) => { if (hoverActive()) positionTip(e); });
    el.addEventListener('mouseleave', () => {
      tooltip.classList.remove('visible');
      applySel(ctx, -1);
    });
    el.addEventListener('click', (e) => {
      // Shift+click on the node body = target toggle accelerator.
      if (e.shiftKey) { ctx.onToggleTarget(instanceId); return; }
      // Pin/unpin is tracked ONLY by the `pinned` map — independent of the
      // graph `selected` set — so opening or closing one node's latency panel
      // never hides, dims, or otherwise reconfigures the rest of the topology.
      if (pinned.has(i)) {
        // Detach this node's own table.
        unpin(i);
        if (hoverActive()) {
          tooltip.innerHTML = tipHTML(ctx, i);
          tooltip.classList.add('visible');
          positionTip({ clientX: positions[i].x + r + 16, clientY: positions[i].y });
          applySel(ctx, i);
        }
      } else {
        // Freeze at the mouse position, in viewport coordinates so the table
        // keeps its place relative to the node through zoom and pan.
        const vp = ctx.viewport || root;
        const vr = vp.getBoundingClientRect();
        const sc = (ctx.zoomScale && ctx.zoomScale()) || 1;
        tooltip.classList.remove('visible');
        pinNode(i, { left: (e.clientX - vr.left) / sc + 12, top: (e.clientY - vr.top) / sc + 12 });
      }
    });
  });
}
