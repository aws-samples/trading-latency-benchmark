// topology3d.js — 3D latency topology renderer (three.js). Independent of the
// 2D renderer (no shared module).
//
// mountTopology3D(container, fleet): renders the shared "afxdp.topology/v1"
// fleet object (nodes + NxN matrix) into `container`. Orbit/zoom/pan. Node
// hover => latency table; click => select node + 1-hop neighbours (gold rim);
// "Deselect all" resets. Returns { dispose() }.

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { CSS2DRenderer, CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';

export function mountTopology3D(container, fleet) {
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

  // ── formatters / palettes ─────────────────────────────────────────────────
  function fmtLat(us) {
    if (us === null || us === undefined || us === '') return '\u2014';
    const v = +us; if (!isFinite(v) || v <= 0) return '\u2014';
    const trim = (x) => (Math.round(x * 100) / 100).toString();
    if (v >= 500000) return trim(v / 1000000) + ' s';
    if (v >= 500) return trim(v / 1000) + ' ms';
    return Math.round(v) + ' \u03bcs';
  }
  function jitterColorRGB(t) {
    const stops = [[57,211,83],[240,136,62],[248,81,73]];   // green → orange → red
    const seg = t <= 0.5 ? 0 : 1, lt = t <= 0.5 ? t*2 : (t-0.5)*2;
    const a = stops[seg], b = stops[seg+1];
    return [Math.round(a[0]+(b[0]-a[0])*lt), Math.round(a[1]+(b[1]-a[1])*lt), Math.round(a[2]+(b[2]-a[2])*lt)];
  }
  const jitterColorCss = (t) => 'rgb(' + jitterColorRGB(t).join(',') + ')';

  function nodeScore(n) {
    let s = 0;
    s += n.metal ? 40 : 0; s += (n.bw_gbps/200)*25; s += (n.pps_mpps/30)*20;
    s += (n.enis/15)*10; s += (n.nitro_gen/6)*15; s += (n.vcpus/192)*8; s += (n.mem_gb/768)*2;
    return s;
  }
  const nodeRadius3D = (n) => 2.6 + nodeScore(n) * 0.045;
  const SCORE_MIN = 15, SCORE_MAX = 120;
  const capT = (n) => Math.min(1, Math.max(0, (nodeScore(n) - SCORE_MIN) / (SCORE_MAX - SCORE_MIN)));
  function capColor(n) {
    const t = Math.pow(capT(n), 0.85);
    const hue = (210 + t * 150) % 360, sat = 0.72 + t * 0.23, light = 0.55 - t * 0.30;
    return new THREE.Color().setHSL(hue / 360, sat, light);
  }

  const dirSigma = (d) => (d && d.p99 > d.p50) ? (d.p99 - d.p50) / 2.326 : 0;
  function edgeSigma(i, j) {
    const ab = mat[i] && mat[i][j], ba = mat[j] && mat[j][i];
    const s = [ab, ba].filter(Boolean).map(dirSigma);
    return s.length ? s.reduce((a, b) => a + b, 0) / s.length : 0;
  }

  // ── global ranges ─────────────────────────────────────────────────────────
  let allP50 = [], allP99 = [], allSig = [];
  for (let i=0;i<N;i++) for (let j=0;j<N;j++) if (mat[i] && mat[i][j]) { allP50.push(mat[i][j].p50); allP99.push(mat[i][j].p99); }
  for (let i=0;i<N;i++) for (let j=i+1;j<N;j++) { if ((mat[i]&&mat[i][j])||(mat[j]&&mat[j][i])) allSig.push(edgeSigma(i,j)); }
  const minP50 = allP50.length?Math.min(...allP50):0, maxP50 = allP50.length?Math.max(...allP50):100;
  const minP99 = allP99.length?Math.min(...allP99):0, maxP99 = allP99.length?Math.max(...allP99):100;
  const minSig = allSig.length?Math.min(...allSig):0, maxSig = allSig.length?Math.max(...allSig):1;
  const sigT = (s) => maxSig===minSig ? 0.5 : (s-minSig)/(maxSig-minSig);

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
  const sphereGeo = new THREE.SphereGeometry(1, 24, 18);
  const nodeMeshes = [];
  fleet.nodes.forEach((n, i) => {
    const col = capColor(n), emissive = col.clone().multiplyScalar(0.3);
    const m = new THREE.MeshStandardMaterial({color: col, emissive, roughness: 0.5, metalness: 0.15});
    const mesh = new THREE.Mesh(sphereGeo, m);
    mesh.scale.setScalar(nodeRadius3D(n)); mesh.position.copy(positions[i]);
    mesh.userData.idx = i; mesh.userData.baseEmissive = emissive.clone();
    scene.add(mesh); nodeMeshes.push(mesh);
    const div = document.createElement('div'); div.className = 'node-label';
    div.innerHTML = (n.public_ip ? '<div class="ipp">' + n.public_ip + '</div>' : '')
      + '<div class="ipv">' + n.private_ip + '</div>';
    const lab = new CSS2DObject(div); lab.position.set(0, 0, 0); mesh.add(lab);   // centered on node
  });

  // ── edges + edge labels ───────────────────────────────────────────────────
  const edges = [], edgeLabelEls = [];
  for (let i=0;i<N;i++) for (let j=i+1;j<N;j++) {
    if (!(mat[i]&&mat[i][j]) && !(mat[j]&&mat[j][i])) continue;
    const t = sigT(edgeSigma(i, j));
    const col = new THREE.Color('rgb('+jitterColorRGB(t).join(',')+')');
    const geo = new THREE.BufferGeometry().setFromPoints([positions[i], positions[j]]);
    const line = new THREE.Line(geo, new THREE.LineBasicMaterial({color: col, transparent: true, opacity: 0.5}));
    scene.add(line); edges.push({line, i, j, baseColor: col});
    const ab = mat[i]&&mat[i][j], ba = mat[j]&&mat[j][i];
    const avgP50 = Math.round(((ab?ab.p50:0)+(ba?ba.p50:0)) / ((ab?1:0)+(ba?1:0) || 1));
    const div = document.createElement('div'); div.className = 'edge-label';
    div.style.color = jitterColorCss(t);
    div.textContent = fmtLat(avgP50) + ' \u00b1' + fmtLat(edgeSigma(i, j));
    const ci = i, cj = j;
    div.addEventListener('mouseenter', () => showEdgeTooltip(ci, cj));
    div.addEventListener('mousemove', (e) => positionTip(edgeTip, e));
    div.addEventListener('mouseleave', () => edgeTip.classList.remove('visible'));
    const obj = new CSS2DObject(div);
    obj.position.copy(positions[i]).add(positions[j]).multiplyScalar(0.5);
    obj.visible = false; scene.add(obj);
    edgeLabelEls.push({obj, i, j});
  }

  // ── boundary volumes: PG ⊂ AZ ⊂ VPC ───────────────────────────────────────
  const baseR = Math.max(...fleet.nodes.map(nodeRadius3D));
  const groupsOf = (key) => { const g = {}; fleet.nodes.forEach((n, i) => { const v = n[key]; if (!v || v === 'unknown') return; (g[v] = g[v] || []).push(i); }); return g; };
  const tightBox = (idxs) => new THREE.Box3().setFromPoints(idxs.map(i => positions[i]));
  function safePad(groupArrays, desired) {
    const tb = groupArrays.map(tightBox); let pad = desired;
    for (let a = 0; a < tb.length; a++) for (let b = a+1; b < tb.length; b++) {
      const A = tb[a], B = tb[b];
      const gap = Math.max(B.min.x-A.max.x, A.min.x-B.max.x, B.min.y-A.max.y, A.min.y-B.max.y, B.min.z-A.max.z, A.min.z-B.max.z);
      if (gap > 0) pad = Math.min(pad, gap/2 - 0.6);
    }
    return Math.max(pad, 0.5);
  }
  const vpcPad = safePad(Object.values(groupsOf('vpc_id')), baseR + 12);
  const azPad  = Math.min(safePad(Object.values(groupsOf('az')), baseR + 6), vpcPad);
  const pgPad  = Math.min(safePad(Object.values(groupsOf('cpg_name')), baseR + 2), azPad);
  function cornerGeo(box, armFactor) {
    const mn = box.min, mx = box.max, size = box.getSize(new THREE.Vector3());
    const L = Math.max(Math.min(size.x, size.y, size.z) * 0.22 * armFactor, 1.0);
    const x = mn.x, y = mx.y, z = mx.z;                 // top-left-front corner only
    const p = [ x,y,z, x+L,y,z,  x,y,z, x,y-L,z,  x,y,z, x,y,z-L ];
    const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(p, 3)); return g;
  }
  const outerBox = new THREE.Box3().setFromPoints(positions);
  function drawVol(key, prefix, color, pad, style, withSub, fillOp, edgeOp) {
    fillOp = fillOp ?? 0.04; edgeOp = edgeOp ?? 0.22;
    const groups = groupsOf(key);
    const hex = '#' + color.toString(16).padStart(6, '0');
    Object.keys(groups).forEach(k => {
      const box = tightBox(groups[k]).expandByScalar(pad); outerBox.union(box);
      const size = box.getSize(new THREE.Vector3()), center = box.getCenter(new THREE.Vector3());
      if (style === 'angles') {
        scene.add(new THREE.LineSegments(cornerGeo(box, 1/3), new THREE.LineBasicMaterial({color, transparent: true, opacity: 0.85})));
      } else {
        const geo = new THREE.BoxGeometry(size.x, size.y, size.z);
        const fill = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({color, transparent: true, opacity: fillOp, depthWrite: false}));
        fill.position.copy(center); fill.renderOrder = -1; scene.add(fill);
        const ed = new THREE.LineSegments(new THREE.EdgesGeometry(geo), new THREE.LineBasicMaterial({color, transparent: true, opacity: edgeOp}));
        ed.position.copy(center); scene.add(ed);
      }
      const div = document.createElement('div'); div.className = 'boundary-label';
      div.style.color = hex;
      let html = '<div class="bl-main">' + prefix + ': ' + k + '</div>';
      if (withSub) { const n = fleet.nodes[groups[k][0]];
        html += '<div class="bl-sub" style="color:#3fb950">Region: ' + n.region + '</div>'
             +  '<div class="bl-sub" style="color:#db61a2">Account: ' + n.account + '</div>'; }
      div.innerHTML = html;
      const lo = new CSS2DObject(div); lo.center.set(0, 0);
      lo.position.set(box.min.x, box.max.y, box.max.z); scene.add(lo);
    });
  }
  drawVol('cpg_name', 'PG',  0xf0883e, pgPad,  'solid',  false, 0.12, 0.42);
  drawVol('az',       'AZ',  0xa371f7, azPad,  'solid',  false, 0.04, 0.22);
  drawVol('vpc_id',   'VPC', 0x58a6ff, vpcPad, 'angles', true);

  // ── camera fit ────────────────────────────────────────────────────────────
  const sphere = outerBox.getBoundingSphere(new THREE.Sphere());
  const R = Math.max(sphere.radius, 20);
  controls.target.copy(sphere.center);
  camera.position.copy(sphere.center).add(new THREE.Vector3(R*1.6, R*1.1, R*1.8));
  camera.near = R/100; camera.far = R*100; camera.updateProjectionMatrix();

  // ── latency table (grouped by PG, sorted by p50) ──────────────────────────
  function buildPeerTable(i, inbound) {
    const rows = [];
    for (let j=0;j<N;j++){ if(i===j) continue;
      const data = inbound ? (mat[j]&&mat[j][i]) : (mat[i]&&mat[i][j]);
      if (!data) continue; rows.push({peer: fleet.nodes[j], data}); }
    const groups = {};
    rows.forEach(r => { const pg = (r.peer.cpg_name && r.peer.cpg_name!=='unknown') ? r.peer.cpg_name : 'no PG'; (groups[pg]=groups[pg]||[]).push(r); });
    const keys = Object.keys(groups).sort((a,b)=> Math.min(...groups[a].map(r=>r.data.p50)) - Math.min(...groups[b].map(r=>r.data.p50)));
    let h = '<table><tr><th>Peer</th><th>p50</th><th>p90</th><th>p99</th><th>p99.9</th><th>max</th><th>loss</th></tr>';
    keys.forEach(pg => {
      h += '<tr class="pg-group"><td colspan="7">' + pg + '</td></tr>';
      groups[pg].sort((a,b)=>a.data.p50-b.data.p50).forEach(r => { const d = r.data;
        h += '<tr><td class="peer-name">' + r.peer.ec2_name + '</td><td class="highlight">' + fmtLat(d.p50) + '</td><td>' + (d.p90?fmtLat(d.p90):'\u2014') + '</td><td>' + fmtLat(d.p99) + '</td><td>' + (d.p999?fmtLat(d.p999):'\u2014') + '</td><td>' + (d.max?fmtLat(d.max):'\u2014') + '</td><td>' + (d.loss!==undefined?d.loss+'%':'\u2014') + '</td></tr>'; });
    });
    return h + '</table>';
  }

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

  // ── selection + gold-glow halos ───────────────────────────────────────────
  const selected = new Set();
  const halos = nodeMeshes.map((m, i) => {
    const h = new THREE.Mesh(sphereGeo, new THREE.MeshBasicMaterial({color: 0xffd700, transparent: true, opacity: 0.32, side: THREE.BackSide, blending: THREE.AdditiveBlending, depthWrite: false}));
    h.scale.setScalar(nodeRadius3D(fleet.nodes[i]) * 1.15); h.position.copy(positions[i]); h.visible = false; scene.add(h); return h;
  });
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
      halos[i].visible = selected.has(i);
      if (selected.has(i)) m.material.emissive.set(0xffd700).multiplyScalar(0.28);
      else m.material.emissive.copy(m.userData.baseEmissive).multiplyScalar(i === hover ? 1.5 : 1);
    });
    edges.forEach(e => {
      const touchSel = hasSel && (selected.has(e.i) || selected.has(e.j));
      const touchHover = hover !== -1 && (e.i === hover || e.j === hover);
      e.line.visible = !hasSel || touchSel;
      e.line.material.opacity = hasSel ? 0.95 : (hover !== -1 ? (touchHover ? 1.0 : 0.06) : 0.5);
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
    const node = fleet.nodes[i];
    let html = '<h4>' + node.ec2_name + ' \u2192 peers</h4>' + buildPeerTable(i, false);
    html += '<div class="direction" style="margin-top:6px">\u2190 Inbound (peers \u2192 this node):</div>' + buildPeerTable(i, true);
    nodeTip.innerHTML = html; nodeTip.classList.add('visible'); positionTip(nodeTip, ev);
  }
  renderer.domElement.addEventListener('pointermove', (ev) => {
    const i = pick(ev);
    if (i !== -1) { hoverIdx = i; showNodeTip(i, ev); render(i); }
    else if (hoverIdx !== -1) { hoverIdx = -1; nodeTip.classList.remove('visible'); render(-1); }
  });
  renderer.domElement.addEventListener('click', (ev) => {
    const i = pick(ev); if (i === -1) return;
    if (selected.has(i)) selected.delete(i); else selected.add(i);
    render(hoverIdx);
  });
  deselectBtn.addEventListener('click', () => { selected.clear(); render(hoverIdx); });

  // ── panels ────────────────────────────────────────────────────────────────
  function median(a){ if(!a.length) return 0; const s=[...a].sort((x,y)=>x-y); return s[Math.floor(s.length/2)]; }
  const uniq = (k) => [...new Set(fleet.nodes.map(n=>n[k]))].filter(v=>v && v!=='unknown');
  (function buildStats(){
    const regs=uniq('region'), azs=uniq('az'), pgs=uniq('cpg_name'), accts=uniq('account');
    const row = (k,v) => '<div class="stat"><span>'+k+'</span><span class="val">'+v+'</span></div>';
    let scope = '';
    if (pgs.length===1) scope += row('Placement Group', pgs[0]); else if (pgs.length>1) scope += row('Placement Groups', pgs.length);
    if (azs.length===1) scope += row('AZ', azs[0]); else if (azs.length>1) scope += row('AZs', azs.join(', '));
    if (regs.length===1) scope += row('Region', regs[0]); else if (regs.length>1) scope += row('Regions', regs.join(', '));
    if (accts.length===1) scope += row('Account', accts[0]); else if (accts.length>1) scope += row('Accounts', accts.length);
    statsEl.innerHTML = '<h3>Summary</h3>'
      + row('Nodes', N) + row('Edges', allSig.length)
      + row('p50 range', fmtLat(minP50)+'\u2013'+fmtLat(maxP50))
      + row('p99 range', fmtLat(minP99)+'\u2013'+fmtLat(maxP99))
      + row('Jitter \u03c3', fmtLat(minSig)+'\u2013'+fmtLat(maxSig))
      + row('Median p50', fmtLat(median(allP50)))
      + '<div class="scope">' + scope + '</div>';
  })();
  legendEl.innerHTML = '<h3>Legend</h3>'
    + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,#39d353,#f0883e,#f85149)"></div><span>Edge colour = jitter \u03c3 ('+fmtLat(minSig)+' \u2192 '+fmtLat(maxSig)+')</span></div>'
    + '<div class="row"><div class="swatch" style="background:linear-gradient(to right,hsl(210,72%,55%),hsl(300,85%,45%),hsl(0,95%,32%))"></div><span>Node colour = capability (weak \u2192 metal / top-net), size = capability</span></div>'
    + '<div class="row"><span>Distance \u221d log(p50 latency)</span></div>'
    + '<div class="row"><span style="color:#79c0ff;font-weight:700">Public IP</span><span style="color:#8b949e">&nbsp;/&nbsp;Private IP</span><span>&nbsp;on each node</span></div>'
    + '<div class="ux-hint"><b>Hover</b> a node \u2014 highlight edges + latency table. <b>Click</b> a node \u2014 select it + its 1-hop neighbours & links (gold glow); click again to deselect. <b>Deselect all</b> restores the full view. <b>Drag</b> = rotate, <b>scroll</b> = zoom, <b>right-drag</b> = pan; drag a panel title to move, click it to fold.</div>';
  (function buildITypes(){
    const seen = new Map(); fleet.nodes.forEach(n => { if(!seen.has(n.type)) seen.set(n.type, n); });
    let rows = '';
    for (const [type, n] of seen) {
      const css = capColor(n).getStyle(), r = Math.round(nodeRadius3D(n) * 2.4), fam = type.split('.')[0];
      rows += '<div class="type-row"><div class="type-dot" style="width:'+(r*2)+'px;height:'+(r*2)+'px;background:'+css+';border:2px solid '+css+'"></div>'
        + '<div style="flex:1"><div class="type-name">'+type+'</div><div class="type-specs">'+n.vcpus+'vCPU \u00b7 '+n.mem_gb+'GB \u00b7 '+n.bw_gbps+'Gbps \u00b7 '+n.pps_mpps+'Mpps \u00b7 '+n.enis+' ENIs \u00b7 Nitro '+n.nitro_gen+'</div></div>'
        + '<a href="https://instances.vantage.sh/?selected='+type+'&region='+region+'" target="_blank">specs\u2197</a>'
        + '<a href="https://aws.amazon.com/ec2/instance-types/'+fam+'/" target="_blank">family\u2197</a></div>';
    }
    itypesEl.innerHTML = '<h3>Instance Types</h3>' + rows;
  })();

  const panelCleanups = [];
  function enhancePanel(el) {
    const h = el.querySelector('h3'); if (!h) return;
    const body = document.createElement('div'); body.className = 'panel-body';
    while (h.nextSibling) body.appendChild(h.nextSibling); el.appendChild(body);
    const caret = document.createElement('span'); caret.className = 'panel-caret'; caret.textContent = '\u25be';
    h.insertBefore(caret, h.firstChild);
    let collapsed = false, dragging = false, moved = false, sx=0, sy=0, ox=0, oy=0;
    // content scales with panel width (drag the right edge to resize)
    const scaler = document.createElement('div'); scaler.className = 'panel-scale';
    while (el.firstChild) scaler.appendChild(el.firstChild); el.appendChild(scaler);
    let base = 0;
    const update = () => {
      if (!base) { base = el.clientWidth || 1; scaler.style.width = base + 'px'; }
      const k = el.clientWidth / base;
      scaler.style.transform = 'scale(' + k + ')';
      el.style.height = Math.ceil(scaler.offsetHeight * k) + 'px';
    };
    h.addEventListener('mousedown', (e) => { dragging=true; moved=false; sx=e.clientX; sy=e.clientY;
      const r=el.getBoundingClientRect(); ox=r.left; oy=r.top; el.style.right='auto'; el.style.bottom='auto'; el.style.left=ox+'px'; el.style.top=oy+'px'; e.preventDefault(); });
    const onMove = (e) => { if(!dragging) return; const dx=e.clientX-sx, dy=e.clientY-sy; if(Math.abs(dx)+Math.abs(dy)>3) moved=true; el.style.left=(ox+dx)+'px'; el.style.top=(oy+dy)+'px'; };
    const onUp = () => { dragging=false; };
    window.addEventListener('mousemove', onMove); window.addEventListener('mouseup', onUp);
    h.addEventListener('click', () => { if(moved){moved=false;return;} collapsed=!collapsed; body.style.display=collapsed?'none':''; caret.textContent=collapsed?'\u25b8':'\u25be'; update(); });
    const ro = new ResizeObserver(update); ro.observe(el);
    panelCleanups.push(() => { ro.disconnect(); window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp); });
  }
  [statsEl, legendEl, itypesEl].forEach(enhancePanel);

  // ── resize + render loop ──────────────────────────────────────────────────
  const onResize = () => { camera.aspect=W()/H(); camera.updateProjectionMatrix(); renderer.setSize(W(),H()); labelRenderer.setSize(W(),H()); };
  window.addEventListener('resize', onResize);
  let rafId;
  (function animate(){ rafId = requestAnimationFrame(animate); controls.update(); renderer.render(scene,camera); labelRenderer.render(scene,camera); })();
  render(-1);

  return { dispose() { cancelAnimationFrame(rafId); window.removeEventListener('resize', onResize); panelCleanups.forEach(fn => fn()); } };
}
