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
  const { selected, nodeEls, edgeElements, edgeLabelEls } = ctx;
  // Nodes are never hidden — a pinned node only gets the selected ring. Pinning
  // a latency panel must not reconfigure the rest of the graph.
  nodeEls.forEach((el, i) => { if (!el) return; el.classList.toggle('selected', selected.has(i)); });
  // Edge dim/highlight follows HOVER only, so pinning does not dim the graph.
  edgeElements.forEach(({ line, i, j }) => {
    const touchHover = hover !== -1 && (i === hover || j === hover);
    const baseOp = line.dataset.baseOp || '0.4';
    line.classList.remove('highlighted', 'dimmed');
    if (hover !== -1) {
      if (touchHover) { line.classList.add('highlighted'); line.style.opacity = ''; }
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
