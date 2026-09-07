// grouplayout.js — SHARED group-aware layout helpers for the 2D map and 3D scene.
//
// The latency MDS/SMACOF layout places nodes purely by p50 distance, which
// interleaves members of different accounts/VPCs so their boundary boxes overlap.
// This module fixes that WITHOUT reshaping clusters:
//
//   • pathKeyOf(node, depth) — a hierarchy path key so boundaries nest as a
//     STRICT tree Account ⊃ Region ⊃ VPC ⊃ AZ (option "E"). Because each cell is
//     scoped to its parent path, the orthogonal VPC↔AZ cross-cut resolves — an
//     AZ spanning two VPCs becomes one cell per VPC.
//
//   • separateHierarchy(...) — rigid group separation (option "B"). After the
//     latency layout, each group is treated as a RIGID cluster and whole clusters
//     are pushed apart (bottom-up, per tier) until their boxes clear. Intra-group
//     latency structure is preserved exactly; only inter-group distance is traded
//     away for guaranteed non-overlapping, cleanly nested containers.

export const HIER = ['account', 'region', 'vpc_id', 'az', 'cpg_name'];

// Composite key for the node's group at `depth` (0=account … 3=az), scoped to
// its full ancestor path so sibling cells are per-parent.
export function pathKeyOf(node, depth) {
  let s = '';
  for (let d = 0; d <= depth; d++) s += (d ? '\u0001' : '') + (node[HIER[d]] || 'unknown');
  return s;
}

// Rigidly separate hierarchical groups in place.
//   nodes  : fleet.nodes (for group keys)
//   pts    : Array of coordinate arrays (one per node); length === axes (2 or 3).
//            MUTATED — each node translated so sibling groups no longer overlap.
//   axes   : 2 (x,y) for the 2D map, 3 (x,y,z) for the 3D scene.
//   radius : half-size each node is inflated by when boxing a group (node body).
//   gaps   : per-depth minimum gap between sibling group boxes (index = depth).
//
// Post-order: children are separated first, then the current tier's siblings are
// separated as rigid blocks — so moving a parent never re-introduces child
// overlap, and the result is a clean containment tree.
export function separateHierarchy(nodes, pts, axes, radius, gaps) {
  const bbox = (idxs) => {
    const mn = new Array(axes).fill(Infinity), mx = new Array(axes).fill(-Infinity);
    for (const i of idxs) for (let a = 0; a < axes; a++) {
      const v = pts[i][a];
      if (v - radius < mn[a]) mn[a] = v - radius;
      if (v + radius > mx[a]) mx[a] = v + radius;
    }
    return { mn, mx };
  };
  const translate = (idxs, a, d) => { for (const i of idxs) pts[i][a] += d; };

  // Iteratively push apart any intersecting pair (along the axis of least
  // overlap, cheapest separation) until a `gap` exists between every pair.
  function removeOverlaps(sibs, gap) {
    for (let iter = 0; iter < 20; iter++) {
      let moved = false;
      const boxes = sibs.map(bbox);
      for (let x = 0; x < sibs.length; x++) for (let y = x + 1; y < sibs.length; y++) {
        const A = boxes[x], B = boxes[y];
        let minOv = Infinity, minAx = -1, inter = true;
        for (let a = 0; a < axes; a++) {
          const ov = Math.min(A.mx[a], B.mx[a]) - Math.max(A.mn[a], B.mn[a]) + gap; // want ≥ gap clearance
          if (ov <= 0) { inter = false; break; }
          if (ov < minOv) { minOv = ov; minAx = a; }
        }
        if (!inter) continue;
        const push = minOv / 2;
        const aLower = (A.mn[minAx] + A.mx[minAx]) <= (B.mn[minAx] + B.mx[minAx]);
        translate(sibs[x], minAx, aLower ? -push : push);
        translate(sibs[y], minAx, aLower ? push : -push);
        boxes[x] = bbox(sibs[x]); boxes[y] = bbox(sibs[y]);
        moved = true;
      }
      if (!moved) break;
    }
  }

  function sep(idxs, depth) {
    const m = new Map();
    for (const i of idxs) { const k = nodes[i][HIER[depth]] || 'unknown'; let g = m.get(k); if (!g) { g = []; m.set(k, g); } g.push(i); }
    const sibs = [...m.values()];
    if (depth + 1 < HIER.length) for (const s of sibs) sep(s, depth + 1);   // children first (post-order)
    if (sibs.length > 1) removeOverlaps(sibs, gaps[depth] || 0);
  }
  sep([...Array(pts.length).keys()], 0);
}
