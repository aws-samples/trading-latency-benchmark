// 2d/zoom.js - zoom state and transform math for the 2D viewport.
// Applies a single CSS transform to compose with the existing pan.

export const MIN_SCALE = 0.3;
export const MAX_SCALE = 5;
const ZOOM_FACTOR = 0.002;

/**
 * Apply a wheel/pinch zoom delta around a cursor point.
 * Mutates `state` in place: { scale, tx, ty }.
 */
export function applyZoom(state, deltaY, cx, cy) {
  const prev = state.scale;
  const factor = 1 - deltaY * ZOOM_FACTOR;
  const next = Math.min(MAX_SCALE, Math.max(MIN_SCALE, prev * factor));
  if (next === prev) return;
  // Keep the point under the cursor fixed:
  // new_tx = cx - (cx - old_tx) * (next / prev)
  state.tx = cx - (cx - state.tx) * (next / prev);
  state.ty = cy - (cy - state.ty) * (next / prev);
  state.scale = next;
}

/** Reset to identity (1x, no translation). */
export function resetZoom(state) {
  state.scale = 1;
  state.tx = 0;
  state.ty = 0;
}

/** Read current zoom state (for external queries). */
export function getZoomState(state) {
  return { scale: state.scale, tx: state.tx, ty: state.ty };
}
