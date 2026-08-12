// 2d/nodes.js — node circles, hover tooltip, multi-node click-to-pin.
//
// Hover   → transient floating tooltip (hidden while any table is pinned).
// Click   → pin a draggable/foldable latency table for this node.
//           Multiple nodes can be pinned simultaneously.
// Click again (or Deselect All) → unpin and remove the table.

import { nodeRadius, getNodeColors, esc } from './palette.js';
import { enhancePinned } from './panels.js';
import { buildPeerTable } from './tables.js';
import { applySel } from './selection.js';

const ROLE_LABEL = { source: 'src', replicator: 'relay', destination: 'dst' };
const ROLE_CSS   = { source: 'src', replicator: 'relay', destination: 'dst' };

const PANEL_GAP = 8, PANEL_TOP = 80;

// Build the HTML content of a tooltip/pinned table for node i.
function tipHTML(ctx, i) {
  const node = ctx.fleet.nodes[i];
  const roleBadge = (node.role && ROLE_LABEL[node.role])
    ? ' <span class="role-badge role-' + esc(ROLE_CSS[node.role] || node.role) + '">' + ROLE_LABEL[node.role] + '</span>' : '';
  return '<h3>' + esc(node.public_ip || node.private_ip || node.ec2_name) + roleBadge + '</h3>'
    + '<div class="direction">Outbound</div>' + buildPeerTable(ctx, i, false)
    + '<div class="direction" style="margin-top:6px">Inbound</div>' + buildPeerTable(ctx, i, true);
}


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
  const hoverActive = () => pinned.size === 0;

  const unpin = (i) => {
    const p = pinned.get(i); if (!p) return;
    p.dispose();
    if (p.el.parentNode) p.el.parentNode.removeChild(p.el);
    pinned.delete(i);
    ctx.selected.delete(i);
    applySel(ctx, -1);                 // drop this node's persisted edge labels
    if (pinned.size === 0) ctx.deselectBtn.style.display = 'none';
  };

  const unpinAll = () => [...pinned.keys()].forEach(unpin);

  const pinNode = (i, fromRect) => {
    if (pinned.has(i)) return;

    let left, top;

    if (fromRect) {
      // First pin (tooltip was visible): freeze at the hover tooltip's exact position.
      const rootRect = root.getBoundingClientRect();
      left = fromRect.left - rootRect.left;
      top  = fromRect.top  - rootRect.top;
    } else if (pinned.size > 0) {
      // Subsequent pin: place to the right of the last panel at the same top.
      const lastEntry = [...pinned.values()].at(-1);
      const rootRect = root.getBoundingClientRect();
      const lastR = lastEntry.el.getBoundingClientRect();
      left = lastR.right - rootRect.left + PANEL_GAP;
      top  = lastR.top   - rootRect.top;
    } else {
      left = PANEL_GAP * 2;
      top  = PANEL_TOP;
    }

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
    root.appendChild(panelEl);

    const dispose = enhancePinned(panelEl, { left: left + 'px', top: top + 'px' });
    pinned.set(i, { el: panelEl, dispose });
    ctx.selected.add(i);               // pinning a node also pins its edge labels
    applySel(ctx, -1);
    ctx.deselectBtn.style.display = 'inline-block';
  };

  ctx.unpinAll = () => { unpinAll(); applySel(ctx, -1); tooltip.classList.remove('visible'); };
  ctx.disposers.push(unpinAll);

  // ── Node elements ─────────────────────────────────────────────────────────
  fleet.nodes.forEach((node, i) => {
    const r = nodeRadius(node), colors = getNodeColors(node.type);
    const el = document.createElement('div');
    el.className = 'node' + (node.role ? ' role-' + node.role : '') + (node.online === false ? ' offline' : '');
    el.style.width = el.style.height = (r * 2) + 'px';
    el.style.left = (positions[i].x - r) + 'px';
    el.style.top  = (positions[i].y - r) + 'px';
    el.style.background = colors.bg;
    // Border: role colour (solid) overrides the instance-family colour when a role is set.
    const ROLE_BORDER = { replicator: '#f0883e', source: '#1f6feb', destination: '#2ea043' };
    el.style.borderColor = ROLE_BORDER[node.role] || colors.border;
    el.style.borderStyle = 'solid';

    const pgBadge = (node.cpg_name && node.cpg_name !== 'unknown')
      ? '<span class="pg-badge">' + esc(node.cpg_name) + '</span>' : '';
    const roleLabel = node.role && ROLE_LABEL[node.role];
    const roleBadge = roleLabel
      ? '<span class="role-badge role-' + esc(ROLE_CSS[node.role] || node.role) + '">' + roleLabel + '</span>' : '';
    el.innerHTML = '<span class="ip ip-private">' + esc(node.private_ip) + '</span>'
      + '<span class="ip ip-public">' + (node.public_ip ? esc(node.public_ip) : '\u2014') + '</span>'
      + pgBadge + roleBadge;
    root.appendChild(el);
    nodeEls[i] = el;

    const positionTip = (e) => {
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
    });
    el.addEventListener('mousemove', (e) => { if (hoverActive()) positionTip(e); });
    el.addEventListener('mouseleave', () => {
      tooltip.classList.remove('visible');
      applySel(ctx, -1);
    });
    el.addEventListener('click', () => {
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
        // Attach: snapshot tooltip position before hiding it, pin a fresh panel there.
        const rect = tooltip.classList.contains('visible')
          ? tooltip.getBoundingClientRect()
          : null;
        tooltip.classList.remove('visible');
        pinNode(i, rect);
      }
    });
  });
}
