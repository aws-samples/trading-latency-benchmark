// 2d/selection.js — hover/click selection state applied to nodes/edges/labels.
// Edge labels:
//   - appear (opacity:1) for edges touching the currently hovered node
//   - stay visible (pinned) for edges touching any clicked/selected node
//   - hidden otherwise (opacity:0)

function neighborsOf(ctx, set) {
  const { N, matrix } = ctx;
  const nb = new Set();
  set.forEach(i => { nb.add(i); for (let j = 0; j < N; j++) if (j !== i && ((matrix[i] && matrix[i][j]) || (matrix[j] && matrix[j][i]))) nb.add(j); });
  return nb;
}

// hover: node index currently hovered, or -1.
export function applySel(ctx, hover) {
  const { selected, nodeEls, edgeElements, edgeLabelEls, linksHidden, targetIds, fleet } = ctx;
  // Nodes are never hidden — a pinned node only gets the selected ring. Pinning
  // a latency panel must not reconfigure the rest of the graph.
  nodeEls.forEach((el, i) => { if (!el) return; el.classList.toggle('selected', selected.has(i)); });

  // Build the set of target-pair edges (indices) when target set is non-empty.
  const hasTargets = targetIds && targetIds.size > 0;
  let targetEdgeSet = null;
  if (hasTargets && fleet && fleet.nodes) {
    targetEdgeSet = new Set();
    const idOf = (n) => n.instance_id || n.private_ip;
    for (let i = 0; i < fleet.nodes.length; i++) {
      for (let j = i + 1; j < fleet.nodes.length; j++) {
        const a = idOf(fleet.nodes[i]), b = idOf(fleet.nodes[j]);
        if (targetIds.has(a) && targetIds.has(b)) {
          targetEdgeSet.add(`${i}:${j}`);
        }
      }
    }
  }

  // Edge dim/highlight follows hover AND pinned selection: pinning a node FIXES
  // its edges in the hovered state (neighbours highlighted, the rest dimmed) and
  // that persists while the node stays pinned. Unpinning (selected empties) drops
  // back to no-focus → default opacity, restoring the default view.
  // When links are hidden (Links toggle off), edges are invisible at rest but a
  // hovered/pinned node still reveals its own edges.
  const anyFocus = hover !== -1 || selected.size > 0;
  edgeElements.forEach(({ line, i, j }) => {
    const active = (hover !== -1 && (i === hover || j === hover)) || selected.has(i) || selected.has(j);
    const baseOp = line.dataset.baseOp || '0.4';
    line.classList.remove('highlighted', 'dimmed', 'target-edge');

    // D4: target-edge overlay when target set is non-empty.
    const isTargetEdge = targetEdgeSet && targetEdgeSet.has(`${i}:${j}`);
    if (isTargetEdge) line.classList.add('target-edge');

    if (linksHidden) {
      line.style.opacity = active || isTargetEdge ? '' : '0';
      if (active) line.classList.add('highlighted');
    } else if (hasTargets && !anyFocus) {
      // Target overlay mode: highlight target edges, dim all others.
      if (isTargetEdge) { line.style.opacity = ''; }
      else { line.classList.add('dimmed'); line.style.opacity = ''; }
    } else if (anyFocus) {
      if (active) { line.classList.add('highlighted'); line.style.opacity = ''; }
      else { line.classList.add('dimmed'); line.style.opacity = ''; }
    } else {
      line.style.opacity = baseOp;
    }
  });
  // Edge labels: PERSIST for edges touching a pinned (selected) node, and show
  // transiently for the hovered node's edges.
  for (const it of edgeLabelEls) {
    const pinned  = selected.has(it.i) || selected.has(it.j);   // stays while node is pinned
    const hovered = hover !== -1 && (it.i === hover || it.j === hover); // transient on hover
    const show = pinned || hovered;
    it.el.style.opacity = show ? '1' : '0';
    it.el.style.pointerEvents = show ? 'auto' : 'none';
  }
}
