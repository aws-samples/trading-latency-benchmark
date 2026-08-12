// Target-set resolution, shared by the control panel, 2D and 3D.
//
// This mirrors backend/pairs.go. The backend is authoritative -- it re-resolves
// against its own Online() set before dispatching -- but the UI needs the same
// answer to label the run buttons and draw the pair overlay. The two are kept in
// step by identical test tables on both sides.

export const SCOPE_AMONG = 'among';
export const SCOPE_FANOUT = 'fanout';
export const SCOPE_FANIN = 'fanin';

export const SCOPES = [
  {
    id: SCOPE_AMONG,
    label: 'Between selected',
    hint: 'each selected node measures to every other selected node',
  },
  {
    id: SCOPE_FANOUT,
    label: 'Selected \u2192 everyone else',
    hint: 'each selected node measures to the whole fleet',
  },
  {
    id: SCOPE_FANIN,
    label: 'Everyone else \u2192 selected',
    hint: 'the whole fleet measures to each selected node (costly: one host transition per source)',
  },
];

/**
 * Selection presets. Each resolves against the current fleet to a set of node
 * ids, so the same chip works in 2D, 3D and the panel.
 */
export const PRESETS = [
  { id: 'pg', label: 'PG', attr: 'cpg_name' },
  { id: 'vpc', label: 'VPC', attr: 'vpc_id' },
  { id: 'az', label: 'AZ', attr: 'az' },
  { id: 'region', label: 'Region', attr: 'region' },
  { id: 'all', label: 'All' },
];

// Values that mean "unknown" rather than a real group. Grouping on these would
// lump unrelated nodes together, which is worse than selecting nothing.
const UNKNOWN = new Set(['', 'unknown', 'none', undefined, null]);

/**
 * Resolve a preset NAME into the node ids it selects.
 *
 * @param {string} name  preset id from PRESETS
 * @param {Array}  nodes fleet nodes
 * @param {string|null} anchorId  node whose group to match; when absent the
 *   largest group is used, which is both the most useful default and stable
 *   across repeated presses
 * @returns {Array<string>} node ids (never characters - callers spread this
 *   into a Set, and passing a bare string there yields one entry per letter)
 */
export function resolvePreset(name, nodes, anchorId) {
  const all = nodes || [];
  const up = all.filter((n) => n.online);

  if (name === 'clear') return [];
  if (name === 'all') return up.map(idOf);

  const preset = PRESETS.find((p) => p.id === name);
  if (!preset || !preset.attr) return [];
  const attr = preset.attr;

  // Group online nodes by the attribute, skipping unknowns.
  const groups = new Map();
  for (const n of up) {
    const v = n[attr];
    if (UNKNOWN.has(v)) continue;
    if (!groups.has(v)) groups.set(v, []);
    groups.get(v).push(idOf(n));
  }
  if (groups.size === 0) return [];

  // An anchor picks its own group. The anchor may itself be offline, in which
  // case its group still resolves but it is not selected (it cannot be measured).
  if (anchorId) {
    const anchor = all.find((n) => idOf(n) === anchorId || n.instance_id === anchorId);
    if (anchor && !UNKNOWN.has(anchor[attr]) && groups.has(anchor[attr])) {
      return groups.get(anchor[attr]);
    }
  }

  // No usable anchor: the largest group, ties broken by fleet order.
  let best = [];
  for (const g of groups.values()) if (g.length > best.length) best = g;
  return best;
}

const idOf = (n) => n.instance_id || n.instanceId || n.private_ip || n.name;

/**
 * Expand a target set into the ordered pairs a campaign would measure.
 *
 * @param {Array} nodes  fleet nodes; only `online` ones are measurable
 * @param {Array<string>|Set<string>} ids  selected instance ids ([] = full mesh)
 * @param {string} scope  among | fanout | fanin
 * @returns {{pairs: Array<[object,object]>, sources: Array, skipped: Array<string>, error: string|null}}
 */
export function resolvePairs(nodes, ids, scope) {
  const want = [...(ids || [])];
  const sc = scope || SCOPE_AMONG;
  const up = (nodes || []).filter((n) => n.online);

  const empty = { pairs: [], sources: [], skipped: [], error: null };
  if (![SCOPE_AMONG, SCOPE_FANOUT, SCOPE_FANIN].includes(sc)) {
    return { ...empty, error: `unknown scope "${sc}"` };
  }

  // Resolve targets in fleet order so a run is deterministic regardless of the
  // order the user clicked, and de-duplicate repeated ids.
  const wanted = new Set(want);
  const targets = up.filter((n) => wanted.has(idOf(n)));
  const found = new Set(targets.map(idOf));
  const skipped = [...new Set(want.filter((id) => !found.has(id)))];

  // A caller that named specific nodes must never widen to the full mesh when
  // none of them resolve: that would run the entire fleet by accident.
  if (want.length > 0 && targets.length === 0) {
    return {
      ...empty,
      skipped,
      error: `none of the ${want.length} selected node(s) are online`,
    };
  }

  const exclSelf = (src, pool) => pool.filter((d) => idOf(d) !== idOf(src));

  let sources = [];
  const destsFor = new Map();
  if (want.length === 0) {
    if (up.length < 2) return { ...empty, skipped, error: `need >=2 online nodes, have ${up.length}` };
    sources = up;
    for (const s of up) destsFor.set(idOf(s), exclSelf(s, up));
  } else if (sc === SCOPE_AMONG) {
    sources = targets;
    for (const s of targets) destsFor.set(idOf(s), exclSelf(s, targets));
  } else if (sc === SCOPE_FANOUT) {
    sources = targets;
    for (const s of targets) destsFor.set(idOf(s), exclSelf(s, up));
  } else {
    // fanin: every online node measures to the targets. Sources with no dests
    // left (the node is the only target) are omitted.
    for (const s of up) {
      const d = exclSelf(s, targets);
      if (d.length) {
        sources.push(s);
        destsFor.set(idOf(s), d);
      }
    }
  }

  const pairs = [];
  for (const s of sources) for (const d of destsFor.get(idOf(s)) || []) pairs.push([s, d]);

  if (pairs.length === 0) {
    return {
      ...empty,
      skipped,
      error: `${scopeDescription(sc, targets.length)} resolves to 0 pairs`,
    };
  }
  return { pairs, sources, skipped, error: null };
}

/**
 * Pair count without a fleet snapshot, for button labels.
 *
 * fanout and fanin are both k*(N-1): a fanin source that is itself a target
 * contributes k-1 dests rather than k, and k*(N-k) + k*(k-1) reduces to k*(N-1).
 */
export function countPairs(n, k, scope) {
  if (n < 2) return 0;
  if (k === 0) return n * (n - 1); // full mesh; scope is ignored
  if (k > n) k = n;
  if (scope === SCOPE_FANOUT || scope === SCOPE_FANIN) return k * (n - 1);
  return k * (k - 1); // among
}

/** Human wording for the resolved scope. Matches backend scopeDescription. */
export function scopeDescription(scope, k) {
  if (k === 0) return 'full mesh';
  const w = k === 1 ? 'node' : 'nodes';
  if (scope === SCOPE_FANOUT) return `${k} selected ${w} to all`;
  if (scope === SCOPE_FANIN) return `all to ${k} selected ${w}`;
  return `among ${k} selected ${w}`;
}

/**
 * Drop target ids that are no longer online members of the fleet.
 *
 * Called on every re-render so a terminated or dropped-out node cannot keep
 * silently scoping runs. Returns a new Set; the input is not mutated.
 */
export function prunedTargets(targetIds, nodes) {
  const live = new Set((nodes || []).filter((n) => n.online).map(idOf));
  return new Set([...(targetIds || [])].filter((id) => live.has(id)));
}
