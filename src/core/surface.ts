// Helpers tying the evaluator to the geodesic machinery: picking rays,
// anchoring nodes to world surfaces, collision.

import { addScaled, col, completeBasis, dot, fromCols, length, normalize, sub, type Mat4, type Vec4 } from './math4.ts';
import { projectToSurface, tangentFrame, transportFrame, type SurfaceField } from './geodesic.ts';
import { INF, type SceneEvaluator } from './evaluator.ts';
import type { Anchor } from './scene.ts';
import type { View } from './camera.ts';

export function worldField(ev: SceneEvaluator, worldId: string | null = ev.activeWorld): SurfaceField {
  return { dist: (p) => ev.worldDist(p, worldId), grad: (p) => ev.worldGrad(p, worldId) };
}

/** Ray direction (4D) for normalised screen coordinates (x, y in [-aspect..aspect] x [-1..1]). */
export function rayDir(v: View, sx: number, sy: number, tanHalf: number): Vec4 {
  let d: Vec4 = [0, 0, 0, 0];
  d = addScaled(d, col(v.frame, 0), sx * tanHalf);
  d = addScaled(d, col(v.frame, 1), sy * tanHalf);
  d = addScaled(d, col(v.frame, 2), 1);
  return normalize(d);
}

export interface RayHit {
  pos: Vec4;
  dir: Vec4; // direction at the hit (transported for geodesic rays)
  t: number;
  id: string | null;
  frame?: Mat4; // transported frame (geodesic rays)
}

/** CPU ray march, matching the shader. */
export function castRay(ev: SceneEvaluator, mode: 'full' | 'geo', eye: Vec4, dir: Vec4, opts: { maxDist?: number; maxStep?: number; frame?: Mat4 } = {}): RayHit | null {
  const maxDist = opts.maxDist ?? (mode === 'geo' ? 60 : 120);
  let p: Vec4 = [...eye] as Vec4;
  let d4 = dir;
  let t = 0;
  const wf = mode === 'geo' ? worldField(ev) : null;
  let frame = opts.frame;
  for (let i = 0; i < 600; i++) {
    const s = ev.eval(p, mode);
    if (s.d < 0.0015 * (1 + t * 0.02)) return { pos: p, dir: d4, t, id: s.id, frame };
    if (t > maxDist || s.d >= INF * 0.5 && !wf) break;
    if (wf) {
      const h = Math.min(s.d, opts.maxStep ?? 0.25);
      p = projectToSurface(wf, addScaled(p, d4, h), 2);
      const n = normalize(wf.grad(p));
      d4 = normalize(addScaled(d4, n, -dot(d4, n)));
      if (frame) frame = transportFrame(wf, p, frame);
      t += h;
    } else {
      p = addScaled(p, d4, s.d);
      t += s.d;
    }
  }
  return null;
}

/** Estimate the smallest radius of curvature of a surface at p (used as anchor reach). */
export function curvatureRadius(f: SurfaceField, p: Vec4, frame: Mat4): number {
  const n0 = normalize(f.grad(p));
  const e = 0.05;
  let kmax = 0;
  for (let c = 0; c < 3; c++) {
    const q = projectToSurface(f, addScaled(p, col(frame, c), e), 4);
    const n1 = normalize(f.grad(q));
    const dist = length(sub(q, p));
    if (dist > 1e-6) kmax = Math.max(kmax, length(sub(n1, n0)) / dist);
  }
  return kmax > 1e-4 ? Math.min(1 / kmax, 1e4) : 1e4;
}

/** Build an anchor at (near) point p on a world surface, oriented by `hint`. */
export function makeAnchor(ev: SceneEvaluator, worldId: string, p: Vec4, hint: Mat4): { pos: Vec4; anchor: Anchor } {
  const f = worldField(ev, worldId);
  const pos = projectToSurface(f, p, 30);
  const basis = tangentFrame(f, pos, hint);
  return { pos, anchor: { world: worldId, basis, reach: curvatureRadius(f, pos, basis) } };
}

/** Basis whose last column is the given unit normal (for programmatic anchoring). */
export function basisFromNormal(n: Vec4): Mat4 {
  const b = completeBasis([normalize(n)]);
  return fromCols(b[1], b[2], b[3], b[0]);
}

/** Is a sphere of radius r at p free of geometry? */
export function isFree(ev: SceneEvaluator, mode: 'full' | 'geo', p: Vec4, r: number): boolean {
  return ev.eval(p, mode).d > r;
}
