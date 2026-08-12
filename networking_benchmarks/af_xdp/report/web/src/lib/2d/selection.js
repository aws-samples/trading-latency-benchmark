// 2d/selection.js — hover/click selection state applied to nodes/edges/labels.

function neighborsOf(ctx, set) {
  const { N, matrix } = ctx;
  const nb = new Set();
  set.forEach(i => { nb.add(i); for (let j = 0; j < N; j++) if (j !== i && ((matrix[i] && matrix[i][j]) || (matrix[j] && matrix[j][i]))) nb.add(j); });
  return nb;
}

// hover: node index currently hovered, or -1.
export function applySel(ctx, hover) {
  const { selected, nodeEls, edgeElements, edgeLabelEls, deselectBtn } = ctx;
  const hasSel = selected.size > 0, vis = hasSel ? neighborsOf(ctx, selected) : null;
  nodeEls.forEach((el, i) => { if (!el) return; el.style.display = (!hasSel || vis.has(i)) ? '' : 'none'; el.classList.toggle('selected', selected.has(i)); });
  edgeElements.forEach(({ line, i, j }) => {
    const touchSel = hasSel && (selected.has(i) || selected.has(j)), touchHover = hover !== -1 && (i === hover || j === hover);
    line.classList.remove('highlighted', 'dimmed');
    if (hasSel) line.classList.add(touchSel ? 'highlighted' : 'dimmed');
    else if (hover !== -1) line.classList.add(touchHover ? 'highlighted' : 'dimmed');
  });
  for (const it of edgeLabelEls) it.el.style.display = ((selected.has(it.i) || selected.has(it.j)) || (hover !== -1 && (it.i === hover || it.j === hover))) ? '' : 'none';
  deselectBtn.style.display = hasSel ? 'inline-block' : 'none';
}
