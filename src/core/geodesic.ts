// Movement constrained to the 3D boundary hypersurface of a 4D "world" object.
//
// A point is kept on the surface f(p) = 0 by Newton projection
//     p <- p - f(p) * grad f / |grad f|^2
// and a tangent frame is carried along by projecting each frame vector onto the
// new tangent space and re-orthonormalising (discrete parallel transport).
// Moving in small steps this way traces geodesics of the surface; the GLSL ray
// marcher (render/shaders.ts) does exactly the same per ray.

import { addScaled, completeBasis, dot, fromCols, col, length, normalize, reject, type Mat4, type Vec4 } from './math4.ts';

export interface SurfaceField {
  dist(p: Vec4): number;
  grad(p: Vec4): Vec4;
}

export function projectToSurface(f: SurfaceField, p: Vec4, iters = 6): Vec4 {
  let q: Vec4 = [...p] as Vec4;
  for (let i = 0; i < iters; i++) {
    const d = f.dist(q);
    if (Math.abs(d) < 1e-5) break;
    const g = f.grad(q);
    const gg = dot(g, g);
    if (gg < 1e-10) break;
    q = addScaled(q, g, -d / gg);
  }
  return q;
}

/**
 * Builds a tangent frame at p: columns 0..2 are tangent (derived from `hint`'s
 * first three columns, in order of priority forward(2), up(1), right(0)), column 3 the normal.
 */
export function tangentFrame(f: SurfaceField, p: Vec4, hint: Mat4): Mat4 {
  const n = normalize(f.grad(p));
  const order = [2, 1, 0];
  const t: Vec4[] = [];
  for (const c of order) {
    let v = reject(col(hint, c), n);
    for (const u of t) v = reject(v, u);
    if (length(v) > 1e-4) t.push(normalize(v));
    else t.push([0, 0, 0, 0]);
  }
  // fill any degenerate directions
  const good = [n, ...t.filter((v) => length(v) > 0.5)];
  const full = completeBasis(good);
  let k = good.length;
  for (let i = 0; i < 3; i++) if (length(t[i]) < 0.5) t[i] = full[k++];
  const [fwd, up, right] = t;
  // keep right-handed-ish orientation consistent with the hint
  const r = dot(right, col(hint, 0)) < 0 ? ([-right[0], -right[1], -right[2], -right[3]] as Vec4) : right;
  return fromCols(r, up, fwd, n);
}

/** Transport the frame from its current point to `p` (already on the surface). */
export function transportFrame(f: SurfaceField, p: Vec4, frame: Mat4): Mat4 {
  const n = normalize(f.grad(p));
  const cols: Vec4[] = [];
  for (let c = 0; c < 3; c++) {
    let v = reject(col(frame, c), n);
    for (const u of cols) v = reject(v, u);
    cols.push(normalize(v));
  }
  if (cols.some((v) => length(v) < 0.5)) return tangentFrame(f, p, frame);
  return fromCols(cols[0], cols[1], cols[2], n);
}

/**
 * Walk a geodesic: start at p (on surface) with `frame`, direction given in frame
 * coordinates (right, up, forward), for arc length `dist`.
 */
export function shoot(f: SurfaceField, p: Vec4, frame: Mat4, dirLocal: [number, number, number], dist: number, maxStep = 0.1): { pos: Vec4; frame: Mat4 } {
  let pos: Vec4 = [...p] as Vec4;
  let fr = frame;
  const steps = Math.max(1, Math.ceil(Math.abs(dist) / maxStep));
  const h = dist / steps;
  for (let i = 0; i < steps; i++) {
    let dir: Vec4 = [0, 0, 0, 0];
    for (let c = 0; c < 3; c++) dir = addScaled(dir, col(fr, c), dirLocal[c]);
    dir = normalize(dir);
    pos = projectToSurface(f, addScaled(pos, dir, h), 3);
    fr = transportFrame(f, pos, fr);
  }
  return { pos, frame: fr };
}

/** Rotate a frame within one of its own planes (column a toward column b). */
export function rotateFrame(frame: Mat4, a: number, b: number, angle: number): Mat4 {
  const ca = col(frame, a), cb = col(frame, b);
  const c = Math.cos(angle), s = Math.sin(angle);
  const na: Vec4 = [0, 0, 0, 0], nb: Vec4 = [0, 0, 0, 0];
  for (let i = 0; i < 4; i++) { na[i] = c * ca[i] + s * cb[i]; nb[i] = -s * ca[i] + c * cb[i]; }
  const m = frame.slice();
  m.splice(a * 4, 4, ...na);
  m.splice(b * 4, 4, ...nb);
  return m;
}
