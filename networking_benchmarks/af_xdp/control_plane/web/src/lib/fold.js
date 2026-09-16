// One folding path for every panel, modelled on the control panel: a chevron
// toggles its content's display, the folded flag persists, and apply() can be
// called again after anything repaints so a re-render cannot reopen a panel.
//
// Handlers bind at construction. Binding lazily from a repaint hook meant a
// panel whose DOM was not ready on the first call never got a handler at all.

const KEY = 'afxdp-fold-state';

const state = (() => {
  try { return JSON.parse(localStorage.getItem(KEY)) || {}; } catch { return {}; }
})();

const save = () => {
  try { localStorage.setItem(KEY, JSON.stringify(state)); } catch { /* ignore */ }
};

// key -> apply(), so Fold all drives every panel through this one path.
const registry = new Map();

export function isFolded(key) { return !!state[key]; }

/**
 * Bind a chevron to a content element under a persistent key.
 * opts.defaultFolded  - state for a key never seen before.
 * opts.onChange(folded) - extra work a panel needs beyond hiding its content.
 * Returns { apply, toggle, isFolded }.
 */
export function makeFoldable(key, btn, content, opts = {}) {
  if (state[key] === undefined) state[key] = !!opts.defaultFolded;

  const apply = () => {
    const folded = !!state[key];
    if (content) content.style.display = folded ? 'none' : '';
    if (btn) btn.classList.toggle('collapsed', folded);
    if (opts.onChange) opts.onChange(folded);
  };

  const toggle = (want) => {
    const next = (want === undefined) ? !state[key] : !!want;
    if (next === !!state[key]) return;
    state[key] = next;
    save();
    apply();
  };

  if (btn && btn.dataset && !btn.dataset.foldBound) {
    btn.dataset.foldBound = '1';
    btn.addEventListener('click', (e) => { e.stopPropagation(); toggle(); });
  }

  registry.set(key, apply);
  apply();
  return { apply, toggle, isFolded: () => !!state[key] };
}

/** Drop a panel from the Fold all set when its DOM goes away. */
export function unregister(key) { registry.delete(key); }

/** Fold or unfold every registered panel in one step. */
export function setAllFolded(folded) {
  for (const key of registry.keys()) state[key] = !!folded;
  save();
  registry.forEach((apply) => apply());
}
