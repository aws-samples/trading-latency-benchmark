// 2d/styles.js — scoped CSS for the 2D renderer (all selectors under .t2d-root
// so they never collide with the 3D app.css). Injected once by index.js.

export const CSS = `
.t2d-root { position: absolute; inset: 0; width: 100%; height: 100%; }
.t2d-root svg.edges { position: absolute; top: 0; left: 0; width: 100%; height: 100%; pointer-events: none; z-index: 1; }
.t2d-root .edge-label { position: absolute; z-index: 40; font-family: 'SF Mono','Fira Code',monospace;
  font-size: 13px; font-weight: 700; background: rgba(13,17,23,0.94); padding: 2px 7px; border-radius: 4px;
  border: 1px solid rgba(255,255,255,0.1); white-space: nowrap; cursor: default;
  transform: translate(-50%,-50%); transition: border-color 0.15s, background 0.15s; }
.t2d-root .edge-label:hover { border-color: rgba(88,166,255,0.5); background: rgba(22,27,34,0.98); }
.t2d-root .node { position: absolute; z-index: 20; border-radius: 50%; display: flex; flex-direction: column;
  align-items: center; justify-content: center; text-align: center; border: 2.5px solid rgba(255,255,255,0.25);
  box-shadow: 0 4px 24px rgba(0,0,0,0.6); cursor: pointer; }
.t2d-root .node.selected { box-shadow: 0 0 0 2px rgba(255,215,0,0.55), 0 0 10px 2px rgba(255,215,0,0.28); z-index: 30; }
.t2d-root .deselect-btn { position: fixed; top: 58px; left: 50%; transform: translateX(-50%); z-index: 1100;
  display: none; background: rgba(240,136,62,0.16); color: #f0883e; border: 1px solid #f0883e;
  border-radius: 6px; padding: 7px 15px; font-size: 13px; font-weight: 600; cursor: pointer; backdrop-filter: blur(8px); }
.t2d-root .deselect-btn:hover { background: rgba(240,136,62,0.3); }
.t2d-root .node .ip { font-size: 9px; color: #b1bac4; font-family: 'SF Mono',monospace; }
.t2d-root .node .ip-private { color: #fff; font-size: 11px; font-weight: 700; }
.t2d-root .node .ip-public { color: #8b949e; margin-top: 1px; }
.t2d-root .node.peer-hover { box-shadow: 0 0 0 3px rgba(88,166,255,0.9), 0 0 14px 3px rgba(88,166,255,0.5); z-index: 31; }
.t2d-root .panel-caret { display: inline-block; width: 12px; margin-right: 4px; font-size: 10px; color: #8b949e; }
.t2d-root .stats h3, .t2d-root .vis-legend h3, .t2d-root .instance-legend h3 { cursor: move; user-select: none; }
.t2d-root .node .pg-badge { position: absolute; top: -9px; left: 50%; background: #f0883e; color: #fff;
  font-size: 11px; font-weight: 700; min-width: 22px; height: 21px; padding: 0 9px; border-radius: 11px;
  display: flex; align-items: center; justify-content: center; border: 2px solid #0d1117; white-space: nowrap; letter-spacing: 0.2px; }
/* Relay (replicator) node badge — sits at the bottom, teal, to mark the fan-out hop. */
.t2d-root .node .role-badge.relay { position: absolute; bottom: -9px; left: 50%; transform: translateX(-50%);
  background: #2ea043; color: #fff; font-size: 9px; font-weight: 700; height: 16px; padding: 0 7px;
  border-radius: 8px; display: flex; align-items: center; border: 2px solid #0d1117; letter-spacing: .3px;
  text-transform: uppercase; }
.t2d-root .node.role-replicator { border-style: dashed; }
/* role badge inside the tooltip title */
.t2d-root .node-tooltip .role-badge { font-size: 9px; font-weight: 700; padding: 1px 6px; border-radius: 8px;
  vertical-align: middle; text-transform: uppercase; letter-spacing: .3px; }
.t2d-root .node-tooltip .role-badge.role-replicator { background: #2ea043; color: #fff; }
.t2d-root .node-tooltip .role-badge.role-source { background: #1f6feb; color: #fff; }
.t2d-root .node-tooltip .role-badge.role-destination { background: #8957e5; color: #fff; }
.t2d-root .node-tooltip .relay-note { color: #8b949e; font-size: 11px; line-height: 1.5; max-width: 260px; }
.t2d-root .node-tooltip { position: absolute; z-index: 100; background: rgba(22,27,34,0.97); border: 1px solid #30363d;
  border-radius: 8px; padding: 12px 14px; font-size: 11px; pointer-events: none; backdrop-filter: blur(8px);
  box-shadow: 0 8px 32px rgba(0,0,0,0.6); white-space: nowrap; opacity: 0; transition: opacity 0.15s; min-width: 260px; }
.t2d-root .node-tooltip.visible { opacity: 1; }
.t2d-root .node-tooltip h3 { font-size: 12px; color: #58a6ff; margin-bottom: 6px; }
.t2d-root .node-tooltip h4 { font-size: 12px; color: #58a6ff; margin-bottom: 6px; }
.t2d-root .node-tooltip table { border-collapse: collapse; width: 100%; }
.t2d-root .node-tooltip th { text-align: center; font-size: 10px; color: #8b949e; padding: 2px 6px; border-bottom: 1px solid #21262d; }
.t2d-root .node-tooltip td { text-align: center; font-family: 'SF Mono',monospace; font-size: 11px; padding: 3px 6px; color: #e6edf3; }
.t2d-root .node-tooltip td.peer-name { text-align: left; color: #79c0ff; font-family: inherit; }
.t2d-root .node-tooltip td.highlight { color: #f0883e; font-weight: 600; }
.t2d-root .node-tooltip tr.pg-group td { text-align: left; color: #8b949e; font-weight: 700; font-size: 10px; letter-spacing: 0.3px; padding: 6px 6px 2px; border-bottom: 1px solid #30363d; }
.t2d-root .node-tooltip .direction { font-size: 12px; color: #fff; font-weight: 600; }
.t2d-root .node-tooltip.pinned { position: fixed; pointer-events: auto; opacity: 1;
  padding: 0; max-width: 90vw; resize: horizontal; overflow: hidden; }
.t2d-root .node-tooltip.pinned h3 { cursor: move; user-select: none; }
.t2d-root .edge-tooltip { position: absolute; z-index: 100; background: rgba(22,27,34,0.97); border: 1px solid #30363d;
  border-radius: 8px; padding: 12px 14px; font-size: 11px; pointer-events: none; backdrop-filter: blur(8px);
  box-shadow: 0 8px 32px rgba(0,0,0,0.6); white-space: nowrap; opacity: 0; transition: opacity 0.15s; min-width: 240px; }
.t2d-root .edge-tooltip.visible { opacity: 1; }
.t2d-root .edge-tooltip h4 { font-size: 12px; color: #58a6ff; margin-bottom: 8px; }
.t2d-root .edge-tooltip .dir-block { margin-bottom: 8px; }
.t2d-root .edge-tooltip .dir-label { font-size: 10px; color: #8b949e; margin-bottom: 3px; }
.t2d-root .edge-tooltip .dir-values { display: grid; grid-template-columns: repeat(6,auto); gap: 2px 10px; }
.t2d-root .edge-tooltip .metric-label { font-size: 9px; color: #6e7681; }
.t2d-root .edge-tooltip .metric-val { font-family: 'SF Mono',monospace; font-size: 12px; color: #e6edf3; }
.t2d-root .edge-tooltip .metric-val.highlight { color: #f0883e; font-weight: 700; }
.t2d-root .edge-tooltip .asymmetry { font-size: 10px; color: #f0883e; margin-top: 4px; padding-top: 4px; border-top: 1px solid #21262d; }
.t2d-root svg.edges line.edge-line { transition: opacity 0.15s, stroke-width 0.15s; }
.t2d-root svg.edges line.edge-line.dimmed { opacity: 0.12 !important; }
.t2d-root svg.edges line.edge-line.highlighted { opacity: 1 !important; filter: drop-shadow(0 0 3px currentColor) brightness(1.12); }
.t2d-root .contour { position: absolute; z-index: 0; border-radius: 24px; border: 1.5px dashed; pointer-events: none; }
.t2d-root .contour .label { position: absolute; top: -10px; left: 16px; font-size: 10px; font-weight: 600; padding: 1px 8px; border-radius: 4px; white-space: nowrap; }
.t2d-root .contour.region { border-color: rgba(88,166,255,0.25); }
.t2d-root .contour.region .label { background: rgba(88,166,255,0.15); color: #58a6ff; }
.t2d-root .contour.az { border-color: rgba(163,113,247,0.25); }
.t2d-root .contour.az .label { background: rgba(163,113,247,0.15); color: #a371f7; }
.t2d-root .contour.vpc { border-color: rgba(57,211,83,0.2); }
.t2d-root .contour.vpc .label { background: rgba(57,211,83,0.12); color: #39d353; }
.t2d-root .contour.cpg { border-color: rgba(240,136,62,0.3); }
.t2d-root .contour.cpg .label { background: rgba(240,136,62,0.15); color: #f0883e; }
.t2d-root .contour.account { border-color: rgba(248,81,73,0.28); border-style: solid; }
.t2d-root .contour.account .label { background: rgba(248,81,73,0.15); color: #f85149; }
.t2d-root svg.edges line.peering-line { stroke: #58a6ff; stroke-width: 2.5; stroke-dasharray: 7 5; opacity: 0.75; }
.t2d-root svg.edges line.peering-hit { stroke: transparent; stroke-width: 18; pointer-events: stroke; cursor: help; }
.t2d-root .peering-label { position: absolute; z-index: 1; transform: translate(-50%,-50%); font-size: 10px; font-weight: 700; color: #58a6ff;
  background: rgba(13,17,23,0.9); border: 1px solid rgba(88,166,255,0.45); border-radius: 4px; padding: 1px 7px; white-space: nowrap; }
.t2d-root .instance-legend { position: fixed; bottom: 20px; left: 20px; z-index: 1000; background: rgba(22,27,34,0.96);
  border: 1px solid #30363d; border-radius: 8px; padding: 0; font-size: 12px; backdrop-filter: blur(8px);
  width: 360px; max-width: 90vw; resize: horizontal; overflow: hidden; }
.t2d-root .instance-legend h3 { font-size: 13px; margin-bottom: 10px; color: #58a6ff; }
.t2d-root .instance-legend .type-row { display: flex; align-items: center; gap: 12px; margin: 6px 0; padding: 5px 0; border-bottom: 1px solid rgba(48,54,61,0.5); }
.t2d-root .instance-legend .type-row:last-child { border-bottom: none; }
.t2d-root .instance-legend .type-dot { border-radius: 50%; flex-shrink: 0; }
.t2d-root .instance-legend .type-info { flex: 1; }
.t2d-root .instance-legend .type-name { font-weight: 600; color: #e6edf3; font-size: 12px; }
.t2d-root .instance-legend .type-specs { font-size: 11px; color: #8b949e; }
.t2d-root .instance-legend a { color: #58a6ff; text-decoration: none; font-size: 11px; border: 1px solid rgba(88,166,255,0.3); border-radius: 3px; padding: 2px 6px; white-space: nowrap; }
.t2d-root .instance-legend a:hover { background: rgba(88,166,255,0.15); }
.t2d-root .vis-legend { position: fixed; top: 20px; right: 20px; z-index: 1000; background: rgba(22,27,34,0.96);
  border: 1px solid #30363d; border-radius: 8px; padding: 0; font-size: 12px; backdrop-filter: blur(8px);
  width: 300px; max-width: 90vw; resize: horizontal; overflow: hidden; }
.t2d-root .vis-legend h3 { font-size: 13px; margin-bottom: 10px; color: #58a6ff; }
.t2d-root .vis-legend .row { display: flex; align-items: center; gap: 10px; margin: 5px 0; }
.t2d-root .vis-legend .swatch { width: 32px; height: 5px; border-radius: 2px; }
.t2d-root .vis-legend .contour-samples { margin-top: 8px; display: flex; flex-wrap: wrap; gap: 8px; }
.t2d-root .vis-legend .contour-samples span { border-radius: 4px; padding: 2px 8px; font-size: 11px; }
.t2d-root .vis-legend .ux-hint { margin-top: 10px; padding-top: 8px; border-top: 1px solid #30363d; font-size: 10px; color: #8b949e; line-height: 1.6; max-width: 300px; }
.t2d-root .vis-legend .ux-hint b { color: #e6edf3; }
.t2d-root .stats { position: fixed; top: 20px; left: 20px; z-index: 1000; background: rgba(22,27,34,0.96);
  border: 1px solid #30363d; border-radius: 8px; padding: 0; font-size: 13px; backdrop-filter: blur(8px);
  width: 240px; max-width: 90vw; resize: horizontal; overflow: hidden; }
.t2d-root .panel-scale { padding: 16px 18px; transform-origin: top left; }
.t2d-root .stats h3 { font-size: 14px; margin-bottom: 10px; color: #58a6ff; }
.t2d-root .stats .stat { display: flex; justify-content: space-between; margin: 3px 0; }
.t2d-root .stats .stat .val { color: #f0883e; font-weight: 600; font-family: 'SF Mono',monospace; font-size: 13px; }
.t2d-root .stats .stress { margin-top: 8px; padding-top: 8px; border-top: 1px solid #30363d; font-size: 11px; color: #8b949e; }
.t2d-root .stats .stress .val { color: #39d353; }
`;
