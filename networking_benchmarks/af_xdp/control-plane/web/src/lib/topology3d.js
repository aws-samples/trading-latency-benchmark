// topology3d.js — 3D latency topology renderer (three.js). Independent of the
// 2D renderer (no shared module).
//
// mountTopology3D(container, fleet): renders the shared "afxdp.topology/v1"
// fleet object (nodes + NxN matrix) into `container`. Orbit/zoom/pan. Node
// hover => latency table; click => select node + 1-hop neighbours (selected
// node's body turns gold); "Deselect all" resets. Returns { dispose() }.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { CSS2DRenderer, CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';
import { ConvexGeometry } from 'three/addons/geometries/ConvexGeometry.js';
// Shared, single-source-of-truth helpers (identical maths as the 2D map): number
// formatting, per-edge jitter, latency→colour, capability→colour, fixed node size.
import { fmtLat, edgeSigma, latencyColor, capabilityColor, buildCapabilityScale, nodeRadius3D, CAP_GRADIENT_CSS } from './2d/palette.js';
import { enhancePanel, placePanel, enhancePinned, buildBoundaryToggles, buildSummaryHTML, buildInstanceTypesHTML } from './2d/panels.js';
import { nodeTipHTML } from './2d/tables.js';
import { HIER, pathKeyOf, separateHierarchy } from './grouplayout.js';

export function mountTopology3D(container, fleet, opts = {}) {
  const N = fleet.nodes.length;
  const region = fleet.region || 'us-east-1';
  const mat = fleet.matrix;

  // ── DOM scaffolding (panels / tooltips / button) ──────────────────────────
  const mk = (tag, cls, id) => { const e = document.createElement(tag); if (cls) e.className = cls; if (id) e.id = id; container.appendChild(e); return e; };
  const deselectBtn = mk('button', 'deselect-btn', 'deselect'); deselectBtn.textContent = 'Deselect all';
  const statsEl = mk('div', 'panel', 'stats');
  const legendEl = mk('div', 'panel', 'legend');
  const itypesEl = mk('div', 'panel', 'itypes');
  const nodeTip = mk('div', 'node-tooltip', 'node-tooltip');
  const edgeTip = mk('div', 'edge-tooltip', 'edge-tooltip');

  // ── formatters / palettes — shared with the 2D renderer (see 2d/palette.js) ─
  // Node size is fixed (nodeRadius3D) and body colour encodes capability
  // (capabilityColor, blue→green); latency→colour and fmtLat are the same maths
  // the 2D map uses. Only the pair-jitter wrapper (matrix indices → shared
  // per-direction edgeSigma) is local.
  const pairSigma = (i, j) => edgeSigma(mat[i] && mat[i][j], mat[j] && mat[j][i]);

  // ── global ranges ─────────────────────────────────────────────────────────
  let allP50 = [], allP99 = [], allSig = [];
  for (let i=0;i<N;i++) for (let j=0;j<N;j++) if (mat[i] && mat[i][j]) { allP50.push(mat[i][j].p50); allP99.push(mat[i][j].p99); }
  for (let i=0;i<N;i++) for (let j=i+1;j<N;j++) { if ((mat[i]&&mat[i][j])||(mat[j]&&mat[j][i])) allSig.push(pairSigma(i,j)); }
    const arrMin = (a) => { let m=Infinity; for(let k=0;k<a.length;k++) if(a[k]<m) m=a[k]; return a.length?m:0; };
  const arrMax = (a) => { let m=-Infinity; for(let k=0;k<a.length;k++) if(a[k]>m) m=a[k]; return a.length?m:0; };
  allP50.sort((a,b)=>a-b);
  const minP50=arrMin(allP50), maxP50=arrMax(allP50);
  const minP99=arrMin(allP99), maxP99=arrMax(allP99);
  const minSig=arrMin(allSig), maxSig=arrMax(allSig);
  // Edge colour is the shared p50→green/orange/red scale (latencyColor), matching 2D.
  // Edge opacity: p50>p60 → 0.07; else lerp 0.6→0.3.
  const p60 = allP50.length ? allP50[Math.floor(allP50.length*0.6)] : Infinity;
  const pMin = allP50[0]||0;
  function edgeOpacity(avg) {
    if (avg > p60) return 0.07;
    const t = p60>pMin ? (avg-pMin)/(p60-pMin) : 0;
    return 0.18 - t*0.06;   // fast/green de-emphasised further
  }

  // ── latency -> distance (log-compressed avg p50) ──────────────────────────
  function p50pair(i, j) {
    const ab = mat[i]&&mat[i][j]?mat[i][j].p50:null, ba = mat[j]&&mat[j][i]?mat[j][i].p50:null;
    if (ab!=null && ba!=null) return (ab+ba)/2;
    return ab!=null?ab:(ba!=null?ba:35);
  }
  const LSCALE = 26;
  const targetDist = (i, j) => Math.log10(p50pair(i, j) + 1) * LSCALE;

  // ── 3D SMACOF ─────────────────────────────────────────────────────────────
  function layout() {
    if (N <= 1) return [new THREE.Vector3()];
    let pos = [];
    for (let i=0;i<N;i++){ const t=Math.acos(1-2*(i+0.5)/N), p=Math.PI*(1+Math.sqrt(5))*i;
      pos.push(new THREE.Vector3(Math.sin(t)*Math.cos(p),Math.sin(t)*Math.sin(p),Math.cos(t)).multiplyScalar(40)); }
    for (let it=0; it<600; it++){
      const np = [];
      for (let i=0;i<N;i++){
        const acc = new THREE.Vector3(); let wsum = 0;
        for (let j=0;j<N;j++){ if(i===j) continue;
          const d = pos[i].distanceTo(pos[j]) || 1e-4, tgt = targetDist(i,j), w = 1/((tgt*tgt)||1);
          const dir = new THREE.Vector3().subVectors(pos[i],pos[j]).multiplyScalar(tgt/d);
          acc.add(new THREE.Vector3().addVectors(pos[j],dir).multiplyScalar(w)); wsum += w;
        }
        np.push(acc.multiplyScalar(1/(wsum||1)));
      }
      pos = np;
    }
    const c = new THREE.Vector3(); pos.forEach(p=>c.add(p)); c.multiplyScalar(1/N); pos.forEach(p=>p.sub(c));
    return pos;
  }
  const positions = layout();
  // Group-aware separation (B+E): after the latency layout, rigidly push apart
  // sibling groups per tier (Account ⊃ Region ⊃ VPC ⊃ AZ) so boundary containers
  // never intersect. Clusters move as rigid blocks, so intra-group latency
  // structure is preserved; only inter-group distance is traded for cleanliness.
  { const pts = positions.map((p) => [p.x, p.y, p.z]);
    separateHierarchy(fleet.nodes, pts, 3, 1.7 * nodeRadius3D() + 2, [22, 19, 16, 13, 10]);
    positions.forEach((p, i) => p.set(pts[i][0], pts[i][1], pts[i][2])); }

  // ── scene / renderers ─────────────────────────────────────────────────────
  const W = () => container.clientWidth || window.innerWidth;
  const H = () => container.clientHeight || window.innerHeight;
  const scene = new THREE.Scene(); scene.background = new THREE.Color('#0d1117');
  const camera = new THREE.PerspectiveCamera(55, W()/H(), 0.1, 20000);
  const renderer = new THREE.WebGLRenderer({antialias:true});
  renderer.setSize(W(), H()); renderer.setPixelRatio(window.devicePixelRatio);
  container.appendChild(renderer.domElement);
  const labelRenderer = new CSS2DRenderer();
  labelRenderer.setSize(W(), H());
  labelRenderer.domElement.style.position = 'absolute';
  labelRenderer.domElement.style.top = '0';
  labelRenderer.domElement.style.pointerEvents = 'none';
  container.appendChild(labelRenderer.domElement);
  const controls = new OrbitControls(camera, renderer.domElement); controls.enableDamping = true;
  scene.add(new THREE.AmbientLight(0xffffff, 0.8));
  const dl = new THREE.DirectionalLight(0xffffff, 0.75); dl.position.set(1,1,1); scene.add(dl);

  // ── nodes ─────────────────────────────────────────────────────────────────
  // Body = an elongated square dipyramid (a square prism capped by a square
  // pyramid top and bottom — an elongated square bipyramid / Johnson-solid-like
  // crystal). Built as the convex hull of its 10 vertices; unit-ish, scaled by
  // the fixed nodeRadius3D(). EdgesGeometry gives the crystal's silhouette edges,
  // reused for the role outline and the gold selection outline.
  const DIP = [
    [0, 1.7, 0],                                                   // top apex
    [0.8, 0.7, 0.8], [-0.8, 0.7, 0.8], [-0.8, 0.7, -0.8], [0.8, 0.7, -0.8],   // upper square
    [0.8, -0.7, 0.8], [-0.8, -0.7, 0.8], [-0.8, -0.7, -0.8], [0.8, -0.7, -0.8], // lower square
    [0, -1.7, 0],                                                  // bottom apex
  ].map((p) => new THREE.Vector3(p[0], p[1], p[2]));
  const bodyGeo = new ConvexGeometry(DIP);
  const bodyEdgesGeo = new THREE.EdgesGeometry(bodyGeo, 1);   // crystal silhouette edges
  const capScale = buildCapabilityScale(fleet.nodes);        // uniform blue→green over present types
  const R3D = nodeRadius3D();
  // Node body (dipyramid) reach, so nothing (planes, PG discs) cuts through a body.
  const NODE_HALF_Y = 1.7 * R3D + 2;    // apex half-height (vertical clearance)
  const NODE_HALF_XZ = 0.8 * R3D + 2;   // half-width (side-plane clearance)
  const EDGE_GREY = 0x8b949e;   // muted grey outline so the crystal facets read against any fill
  const PG_DISC_COL = 0xf0883e; // orange PG marker disc

  const nodeMeshes = [];
  const nodeEdges = [];   // per node { front, back } grey outline; recoloured gold on select
  fleet.nodes.forEach((n, i) => {
    const col = new THREE.Color(capabilityColor(n, capScale).border), emissive = col.clone().multiplyScalar(0.3);
    // polygonOffset pushes the faces slightly back so the coincident edge lines
    // win the depth test — lets the outline sit EXACTLY on the body (no inset)
    // without z-fighting.
    const m = new THREE.MeshStandardMaterial({ color: col, emissive, roughness: 0.5, metalness: 0.15, flatShading: true, polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1 });
    const mesh = new THREE.Mesh(bodyGeo, m);
    mesh.scale.setScalar(R3D); mesh.position.copy(positions[i]);
    mesh.userData = { idx: i, baseColor: col.clone(), baseEmissive: emissive.clone() };
    scene.add(mesh); nodeMeshes.push(mesh);
    // Grey outline drawn at the SAME scale as the body so it hugs the crystal
    // edges (covers the body, no floating contour). Two passes fake a depth fade:
    // a depth-TESTED crisp front + a depth-IGNORING faint back. On select both
    // recolour to gold (see render()).
    const front = new THREE.LineSegments(bodyEdgesGeo, new THREE.LineBasicMaterial({ color: EDGE_GREY, transparent: true, opacity: 0.9 }));
    front.scale.setScalar(R3D); front.position.copy(positions[i]); front.renderOrder = 1; scene.add(front);
    const back = new THREE.LineSegments(bodyEdgesGeo, new THREE.LineBasicMaterial({ color: EDGE_GREY, transparent: true, opacity: 0.20, depthTest: false, depthWrite: false }));
    back.scale.setScalar(R3D); back.position.copy(positions[i]); scene.add(back);
    nodeEdges.push({ front, back });

    const div = document.createElement('div'); div.className = 'node-label' + (n.role && n.role !== 'unknown' ? ' role-' + n.role : '');
    const ROLE3D = {source:'src',replicator:'relay',destination:'dst'};
    const roleTag = ROLE3D[n.role] ? '<div class="role-badge role-'+ROLE3D[n.role]+'" style="background:'+({replicator:'#f0883e',source:'#1f6feb',destination:'#2ea043'}[n.role]||'#888')+';color:#fff;font-size:8px;font-weight:700;padding:1px 5px;border-radius:6px;text-transform:uppercase;margin-top:2px">'+ROLE3D[n.role]+'</div>' : '';
    div.innerHTML = (n.public_ip ? '<div class="ipp">' + n.public_ip + '</div>' : '')
      + '<div class="ipv">' + n.private_ip + '</div>'
      + roleTag;
    const lab = new CSS2DObject(div); lab.position.set(0, 0, 0); mesh.add(lab);   // centered on node
  });

  // ── edges + edge labels ───────────────────────────────────────────────────
  const edges = [], edgeLabelEls = [];
  for (let i=0;i<N;i++) for (let j=i+1;j<N;j++) {
    if (!(mat[i]&&mat[i][j]) && !(mat[j]&&mat[j][i])) continue;
    const ab = mat[i]&&mat[i][j], ba = mat[j]&&mat[j][i];
    const avgP50 = Math.round(((ab?ab.p50:0)+(ba?ba.p50:0))/((ab?1:0)+(ba?1:0)||1));
    const css = latencyColor(avgP50, minP50, maxP50);
    const col = new THREE.Color(css);
    const op = edgeOpacity(avgP50);
    const geo = new THREE.BufferGeometry().setFromPoints([positions[i], positions[j]]);
    const line = new THREE.Line(geo, new THREE.LineBasicMaterial({color: col, transparent: true, opacity: op}));
    scene.add(line); edges.push({line, i, j, baseColor: col, baseOp: op});
    // ab, ba, avgP50 already computed above for edge colour.
    const div = document.createElement('div'); div.className = 'edge-label';
    div.style.color = css;
    div.textContent = fmtLat(avgP50) + ' \u00b1' + fmtLat(pairSigma(i, j));
    const ci = i, cj = j;
    div.addEventListener('mouseenter', () => showEdgeTooltip(ci, cj));
    div.addEventListener('mousemove', (e) => positionTip(edgeTip, e));
    div.addEventListener('mouseleave', () => edgeTip.classList.remove('visible'));
    const obj = new CSS2DObject(div);
    obj.position.copy(positions[i]).add(positions[j]).multiplyScalar(0.5);
    obj.visible = false; scene.add(obj);
    edgeLabelEls.push({obj, i, j});
  }

  // ── boundary volumes ───────────────────────────────────────────────────────
  // Tiers are NOT nested inside one another — each is drawn at its own scale and,
  // within a tier, members are pulled apart into SIBLINGS (never one-inside-
  // another). Tiers are told apart by colour + shelf height, not containment:
  //   Account → sibling bottom planes (top tier, lowest shelf)
  //   VPC     → sibling bottom planes (higher shelf)
  //   Region  → LABEL only (no plane)
  //   AZ      → sibling side planes on the BACK face (-z), same side for all AZs
  // Hovering a plane draws contour lines (in the tier colour) to its member nodes.
  // (PG is a per-node disc, above — not a box.) Colours: account red, region
  // GREEN, VPC BLUE, AZ purple. Each level's objects are collected so the
  // legend's Boundaries toggles can show/hide them.
  const ACC_COL = 0xf85149, REGION_COL = 0x39d353, VPC_COL = 0x58a6ff, AZ_COL = 0xa371f7;
  const V = () => new THREE.Vector3();
  // Path-scoped cells for tier `depth` (0=account…3=az): key = full ancestor path
  // so cells are per-parent (option E). Each entry carries the leaf name + node
  // indices. Returns an array of { leaf, idxs }.
  const cellsOf = (depth) => {
    const m = new Map();
    fleet.nodes.forEach((n, i) => {
      if (!n[HIER[depth]] || n[HIER[depth]] === 'unknown') return;
      const key = pathKeyOf(n, depth); let e = m.get(key);
      if (!e) { e = { leaf: n[HIER[depth]], idxs: [] }; m.set(key, e); }
      e.idxs.push(i);
    });
    return [...m.values()];
  };
  const tightBox = (idxs) => new THREE.Box3().setFromPoints(idxs.map(i => positions[i]));
  const outerBox = new THREE.Box3().setFromPoints(positions);
  const hex = (c) => '#' + c.toString(16).padStart(6, '0');

  // Per-level object buckets — the legend's Boundaries toggles flip .visible on these.
  const boundaryObjs = { account: [], region: [], vpc: [], az: [], pg: [] };

  function boundaryLabel(text, colorCss, pos, into) {
    const div = document.createElement('div'); div.className = 'boundary-label'; div.style.color = colorCss;
    div.innerHTML = '<div class="bl-main">' + text + '</div>';
    const lo = new CSS2DObject(div); lo.center.set(0, 0); lo.position.copy(pos); scene.add(lo);
    if (into) into.push(lo);
  }
  // One filled + outlined rectangle. `axis` = plane normal ('x'|'y'|'z'); `at` =
  // that axis's constant coordinate; the box supplies the in-plane span.
  function planeAt(box, axis, at, color, { fillOp = 0.08, edgeOp = 0.5 } = {}, into) {
    const s = box.getSize(V()), c = box.getCenter(V());
    let geo; const rot = V(), pos = V();
    if (axis === 'y') { geo = new THREE.PlaneGeometry(s.x, s.z); rot.set(-Math.PI / 2, 0, 0); pos.set(c.x, at, c.z); }
    else if (axis === 'x') { geo = new THREE.PlaneGeometry(s.z, s.y); rot.set(0, Math.PI / 2, 0); pos.set(at, c.y, c.z); }
    else { geo = new THREE.PlaneGeometry(s.x, s.y); pos.set(c.x, c.y, at); }
    const fill = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ color, transparent: true, opacity: fillOp, side: THREE.DoubleSide, depthWrite: false }));
    fill.rotation.setFromVector3(rot); fill.position.copy(pos); fill.renderOrder = -1; scene.add(fill);
    const ed = new THREE.LineSegments(new THREE.EdgesGeometry(geo), new THREE.LineBasicMaterial({ color, transparent: true, opacity: edgeOp }));
    ed.rotation.setFromVector3(rot); ed.position.copy(pos); scene.add(ed);
    if (into) into.push(fill, ed);
    return { fill, ed };
  }
  // Hover map: each boundary plane + its member-node "contour" lines (node → its
  // foot on the plane), shown only while that plane is hovered.
  const hoverBoundaries = [];   // { mesh, lines }

  // Draw a tier (by hierarchy depth) as one plane per path-cell on the given
  // side: horizontal 'bottom'|'top', or a vertical face '-x'|'+x'|'-z'|'+z'.
  // The group-aware layout already separated the clusters, so cells are drawn
  // tight around their own nodes (no shrink) and never intersect. `off` =
  // distance beyond the node bodies. Also builds the hover contour lines.
  function planeTier(depth, color, prefix, place, off, into) {
    const rects = cellsOf(depth).map((c) => {
      const nb = tightBox(c.idxs);
      return { leaf: c.leaf, idxs: c.idxs, minX: nb.min.x - NODE_HALF_XZ, maxX: nb.max.x + NODE_HALF_XZ, minZ: nb.min.z - NODE_HALF_XZ, maxZ: nb.max.z + NODE_HALF_XZ, yBot: nb.min.y, yTop: nb.max.y };
    });
    const horizontal = place === 'top' || place === 'bottom';
    rects.forEach((r) => {
      const xlo = r.minX, xhi = r.maxX, zlo = r.minZ, zhi = r.maxZ;
      let box, axis, at, labelPos, foot;
      if (horizontal) {
        axis = 'y';
        at = place === 'top' ? r.yTop + (NODE_HALF_Y + off) : r.yBot - (NODE_HALF_Y + off);
        box = new THREE.Box3(new THREE.Vector3(xlo, 0, zlo), new THREE.Vector3(xhi, 0, zhi));
        labelPos = new THREE.Vector3(xlo, at, zhi);
        foot = (p) => new THREE.Vector3(p.x, at, p.z);
      } else {
        axis = place[1]; const sign = place[0];
        const ylo = r.yBot - NODE_HALF_Y, yhi = r.yTop + NODE_HALF_Y;
        box = new THREE.Box3(new THREE.Vector3(xlo, ylo, zlo), new THREE.Vector3(xhi, yhi, zhi));
        if (axis === 'z') { at = sign === '+' ? zhi + off : zlo - off; labelPos = new THREE.Vector3(xlo, yhi, at); foot = (p) => new THREE.Vector3(p.x, p.y, at); }
        else { at = sign === '+' ? xhi + off : xlo - off; labelPos = new THREE.Vector3(at, yhi, zhi); foot = (p) => new THREE.Vector3(at, p.y, p.z); }
      }
      const { fill } = planeAt(box, axis, at, color, { fillOp: 0.05, edgeOp: 0.45 }, into);
      boundaryLabel(prefix + ': ' + r.leaf, hex(color), labelPos, into);
      // Hover contour lines: each member node → its perpendicular foot on the plane.
      const pts = [];
      r.idxs.forEach((ni) => { const p = positions[ni], f = foot(p); pts.push(p.x, p.y, p.z, f.x, f.y, f.z); });
      const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(pts, 3));
      const lines = new THREE.LineSegments(g, new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.95 }));
      lines.visible = false; lines.renderOrder = 3; scene.add(lines);
      hoverBoundaries.push({ mesh: fill, lines });
      outerBox.expandByPoint(foot(new THREE.Vector3(xlo, r.yBot - NODE_HALF_Y, zlo)));
      outerBox.expandByPoint(foot(new THREE.Vector3(xhi, r.yTop + NODE_HALF_Y, zhi)));
    });
  }

  // Account → sibling BOTTOM planes (top tier), lowest shelf.
  planeTier(0, ACC_COL, 'Account', 'bottom', 10, boundaryObjs.account);
  // Region → LABEL only (no plane), one per account|region cell, sat just above
  // the cluster's own nodes (close to the instances, not a padded box corner).
  cellsOf(1).forEach((c) => {
    const nb = tightBox(c.idxs); outerBox.union(nb);
    const ctr = nb.getCenter(new THREE.Vector3());
    boundaryLabel('Region: ' + c.leaf, hex(REGION_COL), new THREE.Vector3(ctr.x, nb.max.y + NODE_HALF_Y * 1.8, ctr.z), boundaryObjs.region);
  });
  // VPC → sibling BOTTOM planes, higher shelf (just below the bodies).
  planeTier(2, VPC_COL, 'VPC', 'bottom', 2, boundaryObjs.vpc);
  // VPC peering — synthetic dashed connectors between VPC planes (as in 2D). Tied
  // to the VPC toggle. Anchored at each VPC plane's centre (its bottom-shelf y).
  { const anchors = cellsOf(2).map((c) => { const nb = tightBox(c.idxs); const ctr = nb.getCenter(V()); return new THREE.Vector3(ctr.x, nb.min.y - (NODE_HALF_Y + 2), ctr.z); });
    for (let a = 0; a < anchors.length; a++) for (let b = a + 1; b < anchors.length; b++) {
      const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints([anchors[a], anchors[b]]),
        new THREE.LineDashedMaterial({ color: VPC_COL, transparent: true, opacity: 0.4, dashSize: 3, gapSize: 3 }));
      line.computeLineDistances(); scene.add(line); boundaryObjs.vpc.push(line);
    }
  }
  // AZ → sibling side planes on the BACK face (-z), one per VPC∩AZ cell.
  planeTier(3, AZ_COL, 'AZ', '-z', 4, boundaryObjs.az);
  // PG → ONE side plane on the LEFT face (-x) — adjacent to the AZ back (-z)
  // plane, on the opposite side from before. Per-PG cells are x/z-separated so
  // they don't overlap each other or the AZ plane.
  planeTier(4, PG_DISC_COL, 'PG', '-x', 3, boundaryObjs.pg);

  // ── camera fit ────────────────────────────────────────────────────────────
  const sphere = outerBox.getBoundingSphere(new THREE.Sphere());
  const R = Math.max(sphere.radius, 20);
  camera.near = R / 100; camera.far = R * 100; camera.updateProjectionMatrix();
  // Restore the prior camera view across live-update remounts (continuous UX —
  // zoom/rotation/pan are preserved); auto-fit only on the first mount.
  if (opts.view && opts.view.pos && opts.view.target) {
    camera.position.fromArray(opts.view.pos);
    controls.target.fromArray(opts.view.target);
  } else {
    controls.target.copy(sphere.center);
    camera.position.copy(sphere.center).add(new THREE.Vector3(R * 1.6, R * 1.1, R * 1.8));
  }
  controls.update();

  // ── latency panel: SHARED with the 2D map (2d/tables.js) ───────────────────
  // Peer tables + the full tooltip HTML come from nodeTipHTML(), so the latency
  // panel's content and layout are one implementation across both renderers.

  function positionTip(tip, ev) {
    let tx = ev.clientX + 16, ty = ev.clientY - 10;
    const tw = tip.offsetWidth || 280, th = tip.offsetHeight || 200;
    if (tx + tw > window.innerWidth - 20) tx = ev.clientX - tw - 16;
    if (ty + th > window.innerHeight - 20) ty = window.innerHeight - th - 20;
    if (ty < 10) ty = 10;
    tip.style.left = tx + 'px'; tip.style.top = ty + 'px';
  }
  function showEdgeTooltip(i, j) {
    const ab = mat[i]&&mat[i][j], ba = mat[j]&&mat[j][i];
    const a = fleet.nodes[i], b = fleet.nodes[j];
    const diff = ab&&ba ? Math.abs(ab.p50-ba.p50) : 0;
    const pct = ab&&ba&&Math.min(ab.p50,ba.p50)>0 ? ((diff/Math.min(ab.p50,ba.p50))*100).toFixed(1) : '0';
    const block = (d, label) => !d ? '' :
      '<div class="dir-block"><div class="dir-label">' + label + '</div><div class="dir-values">'
      + '<span class="metric-label">p50</span><span class="metric-label">p90</span><span class="metric-label">p99</span><span class="metric-label">p99.9</span><span class="metric-label">max</span><span class="metric-label">loss</span>'
      + '<span class="metric-val highlight">' + fmtLat(d.p50) + '</span><span class="metric-val">' + (d.p90?fmtLat(d.p90):'\u2014') + '</span><span class="metric-val">' + fmtLat(d.p99) + '</span><span class="metric-val">' + (d.p999?fmtLat(d.p999):'\u2014') + '</span><span class="metric-val">' + (d.max?fmtLat(d.max):'\u2014') + '</span><span class="metric-val">' + (d.loss!==undefined?d.loss+'%':'\u2014') + '</span></div></div>';
    let html = '<h4>' + a.ec2_name + ' \u2194 ' + b.ec2_name + '</h4>';
    html += block(ab, '\u2192 ' + a.ec2_name + ' \u2192 ' + b.ec2_name);
    html += block(ba, '\u2190 ' + b.ec2_name + ' \u2192 ' + a.ec2_name);
    if (diff > 0) html += '<div class="asymmetry">Asymmetry: \u0394' + fmtLat(diff) + ' (' + pct + '%)</div>';
    edgeTip.innerHTML = html; edgeTip.classList.add('visible');
  }

  // ── selection: the selected node's BODY turns gold (faces + edges) ─────────
  // No halo/rim — the dipyramid's own planes recolour to gold and its grey
  // outline edges (nodeEdges[i]) recolour to gold too. Neighbours keep their
  // capability colour.
  const GOLD = 0xffd700;
  const selected = new Set();
  let linksHidden = false;   // Links toggle (edge visibility); hover/select still reveals
  function neighborsOf(set) {
    const nb = new Set();
    set.forEach(i => { nb.add(i); for (let j=0;j<N;j++) if (j!==i && ((mat[i]&&mat[i][j])||(mat[j]&&mat[j][i]))) nb.add(j); });
    return nb;
  }
  function render(hover) {
    const hasSel = selected.size > 0;
    const vis = hasSel ? neighborsOf(selected) : null;
    nodeMeshes.forEach((m, i) => {
      m.visible = !hasSel || vis.has(i);
      const sel = selected.has(i), e = nodeEdges[i];
      e.front.visible = e.back.visible = m.visible;
      if (sel) {
        m.material.color.set(GOLD); m.material.emissive.set(GOLD).multiplyScalar(0.45);
        e.front.material.color.set(GOLD); e.back.material.color.set(GOLD);
      } else {
        m.material.color.copy(m.userData.baseColor);
        m.material.emissive.copy(m.userData.baseEmissive).multiplyScalar(i === hover ? 1.6 : 1);
        e.front.material.color.set(EDGE_GREY); e.back.material.color.set(EDGE_GREY);
      }
    });
    edges.forEach(e => {
      const touchSel = hasSel && (selected.has(e.i) || selected.has(e.j));
      const touchHover = hover !== -1 && (e.i === hover || e.j === hover);
      // Links toggle: when hidden, edges only appear for a hovered/selected node.
      e.line.visible = linksHidden ? (touchSel || touchHover) : (!hasSel || touchSel);
      e.line.material.opacity = hasSel ? 0.95 : (hover !== -1 ? (touchHover ? 1.0 : 0.06) : (e.baseOp||0.4));
      e.line.material.color.copy(e.baseColor);
    });
    edgeLabelEls.forEach(e => { e.obj.visible = (selected.has(e.i) || selected.has(e.j)) || (hover !== -1 && (e.i === hover || e.j === hover)); });
    deselectBtn.style.display = hasSel ? 'inline-block' : 'none';
  }

  const raycaster = new THREE.Raycaster(); const pointer = new THREE.Vector2();
  let hoverIdx = -1;
  function pick(ev) {
    const rect = renderer.domElement.getBoundingClientRect();
    pointer.x = ((ev.clientX-rect.left)/rect.width)*2 - 1;
    pointer.y = -((ev.clientY-rect.top)/rect.height)*2 + 1;
    raycaster.setFromCamera(pointer, camera);
    const hit = raycaster.intersectObjects(nodeMeshes.filter(m => m.visible), false)[0];
    return hit ? hit.object.userData.idx : -1;
  }
  function showNodeTip(i, ev) {
    nodeTip.innerHTML = nodeTipHTML(fleet, mat, i);   // shared 2D/3D latency panel
    nodeTip.classList.add('visible'); positionTip(nodeTip, ev);
  }
  // Boundary hover: show a plane's member "contour" lines while it's hovered.
  let hoverBoundary = null;
  function setBoundaryHover(hb) {
    if (hb === hoverBoundary) return;
    if (hoverBoundary) hoverBoundary.lines.visible = false;
    hoverBoundary = hb;
    if (hoverBoundary) hoverBoundary.lines.visible = true;
  }
  function pickBoundary(ev) {
    const rect = renderer.domElement.getBoundingClientRect();
    pointer.x = ((ev.clientX - rect.left) / rect.width) * 2 - 1;
    pointer.y = -((ev.clientY - rect.top) / rect.height) * 2 + 1;
    raycaster.setFromCamera(pointer, camera);
    const meshes = hoverBoundaries.filter((h) => h.mesh.visible).map((h) => h.mesh);
    const hit = raycaster.intersectObjects(meshes, false)[0];
    return hit ? hoverBoundaries.find((h) => h.mesh === hit.object) : null;
  }
  renderer.domElement.addEventListener('pointermove', (ev) => {
    const i = pick(ev);
    if (i !== -1) { hoverIdx = i; if (pinned.size === 0) showNodeTip(i, ev); render(i); setBoundaryHover(null); }
    else {
      if (hoverIdx !== -1) { hoverIdx = -1; nodeTip.classList.remove('visible'); render(-1); }
      setBoundaryHover(pickBoundary(ev));   // nodes take priority; else a boundary plane
    }
  });

  // ── pinned latency panels (SHARED with 2D: enhancePinned + nodeTipHTML) ─────
  // Clicking a node selects it (gold body) AND pins a draggable/foldable latency
  // table — the same "selection mode" panel behaviour as the 2D map. Clicking
  // again (or Deselect all) removes it.
  const pinned = new Map();   // idx -> { el, dispose }
  function pinPanel(i) {
    if (pinned.has(i)) return;
    const el = document.createElement('div'); el.className = 'node-tooltip pinned';
    el.innerHTML = nodeTipHTML(fleet, mat, i);
    // Anchor near the node's projected screen position, clamped into view.
    const v = positions[i].clone().project(camera);
    const left = Math.min(Math.max(20, (v.x * 0.5 + 0.5) * W() + 16), Math.max(20, W() - 320));
    const top  = Math.min(Math.max(70, (-v.y * 0.5 + 0.5) * H()), Math.max(70, H() - 220));
    el.style.cssText = 'left:' + left + 'px;top:' + top + 'px';
    container.appendChild(el);
    const dispose = enhancePinned(el, { left: left + 'px', top: top + 'px' });
    pinned.set(i, { el, dispose });
  }
  function unpinPanel(i) {
    const p = pinned.get(i); if (!p) return;
    p.dispose(); if (p.el.parentNode) p.el.parentNode.removeChild(p.el); pinned.delete(i);
  }

  renderer.domElement.addEventListener('click', (ev) => {
    const i = pick(ev); if (i === -1) return;
    if (selected.has(i)) { selected.delete(i); unpinPanel(i); }
    else { selected.add(i); pinPanel(i); nodeTip.classList.remove('visible'); }
    render(hoverIdx);
  });
  deselectBtn.addEventListener('click', () => { [...selected].forEach(unpinPanel); selected.clear(); render(hoverIdx); });

  // ── panels (shared content builders from 2d/panels.js) ─────────────────────
  statsEl.innerHTML = buildSummaryHTML({
    N, pairs: allSig.length, minP50, maxP50, minP99: arrMin(allP99), maxP99: arrMax(allP99),
    minSigma: minSig, maxSigma: maxSig, nodes: fleet.nodes,
  });
  legendEl.innerHTML = '<h3>Legend</h3>'
    + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,#39d353,#f0883e,#f85149)"></div><span>Edge colour = p50 latency (green=fast, red=slow)</span></div>'
    + '<div class="row"><div class="swatch" style="background:' + CAP_GRADIENT_CSS + '"></div><span>Node colour = capability (blue=basic \u2192 green=metal/top-net)</span></div>'
    + '<div class="row"><span>Distance \u221d log(p50 latency)</span></div>'
    + '<div class="row"><span style="color:#79c0ff;font-weight:700">Public IP</span><span style="color:#8b949e">&nbsp;/&nbsp;Private IP</span><span>&nbsp;on each node</span></div>'
    + '<div class="ux-hint"><b>Hover</b> a node \u2014 highlight edges + latency table; <b>hover</b> a boundary plane \u2014 draw contour lines to its member nodes. <b>Click</b> a node \u2014 select it + its 1-hop neighbours & links; the selected node\u2019s body turns gold (faces + edges). Click again to deselect. <b>Deselect all</b> restores the full view. <b>Drag</b> = rotate, <b>scroll</b> = zoom, <b>right-drag</b> = pan; drag a panel title to move, click it to fold.</div>';
  // Shared Boundaries toggles — flip .visible on each level's collected objects.
  legendEl.appendChild(buildBoundaryToggles((key, on) => {
    (boundaryObjs[key] || []).forEach((o) => { o.visible = on; });
    setBoundaryHover(null);   // drop any active hover contour when visibility changes
  }, {}, [{ label: 'Links', checked: true, onChange: (on) => { linksHidden = !on; render(hoverIdx); } }]));
  const itHtml = buildInstanceTypesHTML(fleet.nodes, region, capScale);
  if (itHtml) itypesEl.innerHTML = '<h3>Instance Types</h3>' + itHtml;

  // Panels use the SHARED enhancer (2d/panels.js): identical drag/fold/resize to
  // 2D, and — crucially — they register in the same module-level foldables set,
  // so the control panel's "fold all" button collapses/expands the 3D panels too
  // (previously it toggled its icon but did nothing here). Cleanups run on dispose
  // to unregister them when the view is torn down (e.g. switching to 2D).
  const panelCtx = { disposers: [] };
  // Same corner distribution as the 2D map (shared placePanel via enhancePanel):
  // summary top-right, instance types bottom-left, legend bottom-right. The
  // control panel owns top-left, so nothing collides there anymore.
  enhancePanel(panelCtx, statsEl, true, 'tr');
  enhancePanel(panelCtx, itypesEl, true, 'bl');
  enhancePanel(panelCtx, legendEl, true, 'br');

  // ── resize + render loop ──────────────────────────────────────────────────
  const onResize = () => { camera.aspect=W()/H(); camera.updateProjectionMatrix(); renderer.setSize(W(),H()); labelRenderer.setSize(W(),H()); };
  window.addEventListener('resize', onResize);
  let rafId;
  (function animate(){ rafId = requestAnimationFrame(animate); controls.update(); renderer.render(scene,camera); labelRenderer.render(scene,camera); })();
  render(-1);

  return {
    getView() { return { pos: camera.position.toArray(), target: controls.target.toArray() }; },
    dispose() { cancelAnimationFrame(rafId); window.removeEventListener('resize', onResize); panelCtx.disposers.forEach(fn => fn()); pinned.forEach((p) => { p.dispose(); if (p.el.parentNode) p.el.parentNode.removeChild(p.el); }); pinned.clear(); },
  };
}
