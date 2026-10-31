// Primitive recipes: a primitive starts as a 0D point and is raised one
// dimension at a time by Extrude / Revolve steps. Each dimension-raising step
// introduces the next axis in the order x, y, z, w.
//
// Signed-distance construction (see README "Primitive construction"):
//   * The 0D point is "everything" in 0D space, so its distance is -INF.
//   * Extrude (finite, length L) along new axis t:
//         w = (d, |t| - L/2);  d' = min(max(w.x, w.y), 0) + length(max(w, 0))
//     Infinite: d' = d.   Half (keep t <= 0): w = (d, t).
//   * Revolve (radius R) of existing axis a into new axis t: earlier steps see
//         coord[a] := length(coord[a], t) - R
//   * Rotate (plane i,j, angle): earlier steps see coordinates rotated in that plane.
// The raw recipe distance is returned here; the scene evaluator then adds the
// thin extrusion over unused axes (objects of dim < 4 live in 4D with a small
// `thickness`), then the optional shell (|d| - shell) and rounding (d - round).
//
// The same recipe is evaluated on the CPU (evalPrimitive) and emitted as GLSL
// (primitiveGLSL) -- both follow the identical two-pass algorithm below.

import type { Vec4 } from './math4.ts';

export type ExtrudeMode = 'finite' | 'infinite' | 'half';

export type Step =
  | { op: 'extrude'; mode: ExtrudeMode; length: number }
  | { op: 'revolve'; radius: number; axis?: number } // axis: existing axis spun into the new one (default: newest)
  | { op: 'rotate'; i: number; j: number; angle: number }; // degrees, among existing axes

export interface PrimitiveDef {
  id: string;
  name: string;
  steps: Step[];
  round?: number; // rounding radius (subtracted at the end)
  shell?: number; // if > 0, hollow shell of this half-thickness
  builtin?: boolean;
  description?: string;
}

export const NEG_INF = -1e9;

export function primitiveDim(def: PrimitiveDef): number {
  return def.steps.filter((s) => s.op !== 'rotate').length;
}

/** Validates a recipe; returns a list of human-readable problems (empty = ok). */
export function validatePrimitive(def: PrimitiveDef): string[] {
  const errs: string[] = [];
  let dim = 0;
  def.steps.forEach((s, k) => {
    if (s.op === 'extrude') {
      if (dim >= 4) errs.push(`step ${k + 1}: cannot exceed 4 dimensions`);
      if (s.mode === 'finite' && !(s.length > 0)) errs.push(`step ${k + 1}: length must be > 0`);
      dim++;
    } else if (s.op === 'revolve') {
      if (dim === 0) errs.push(`step ${k + 1}: revolve needs at least a 1D profile`);
      if (dim >= 4) errs.push(`step ${k + 1}: cannot exceed 4 dimensions`);
      const a = s.axis ?? dim - 1;
      if (a < 0 || a >= dim) errs.push(`step ${k + 1}: revolve axis must be an existing axis`);
      dim++;
    } else {
      if (s.i === s.j || s.i >= dim || s.j >= dim) errs.push(`step ${k + 1}: rotation plane must use two existing axes`);
    }
  });
  return errs;
}

// ---------------------------------------------------------------- CPU eval

export function evalPrimitive(def: PrimitiveDef, p: Vec4): number {
  const c = [p[0], p[1], p[2], p[3]];
  const steps = def.steps;
  // Pass 1 (outside-in): resolve coordinates each step sees.
  const dims: number[] = [];
  let dim = 0;
  for (const s of steps) { dims.push(dim); if (s.op !== 'rotate') dim++; }
  const t = new Array<number>(steps.length).fill(0);
  for (let k = steps.length - 1; k >= 0; k--) {
    const s = steps[k];
    const n = dims[k]; // index of the axis this step introduces
    if (s.op === 'extrude') {
      t[k] = c[n];
    } else if (s.op === 'revolve') {
      const a = s.axis ?? n - 1;
      c[a] = Math.hypot(c[a], c[n]) - s.radius;
    } else {
      const ang = (-s.angle * Math.PI) / 180;
      const ci = c[s.i], cj = c[s.j];
      c[s.i] = Math.cos(ang) * ci - Math.sin(ang) * cj;
      c[s.j] = Math.sin(ang) * ci + Math.cos(ang) * cj;
    }
  }
  // Pass 2 (inside-out): accumulate distance.
  let d = NEG_INF;
  for (let k = 0; k < steps.length; k++) {
    const s = steps[k];
    if (s.op !== 'extrude' || s.mode === 'infinite') continue;
    const wy = s.mode === 'half' ? t[k] : Math.abs(t[k]) - s.length / 2;
    d = Math.min(Math.max(d, wy), 0) + Math.hypot(Math.max(d, 0), Math.max(wy, 0));
  }
  return d;
}

// ---------------------------------------------------------------- GLSL

export const glslFloat = (x: number) => {
  const s = Number.isFinite(x) ? x.toString() : '0.0';
  return /[.eE]/.test(s) ? s : s + '.0';
};
const C = 'xyzw';

export function primitiveGLSL(def: PrimitiveDef, fnName: string): string {
  const steps = def.steps;
  const dims: number[] = [];
  let dim = 0;
  for (const s of steps) { dims.push(dim); if (s.op !== 'rotate') dim++; }
  const lines: string[] = [`float ${fnName}(vec4 c) {`];
  for (let k = steps.length - 1; k >= 0; k--) {
    const s = steps[k];
    const n = dims[k];
    if (s.op === 'extrude') {
      if (s.mode !== 'infinite') lines.push(`  float t${k} = c.${C[n]};`);
    } else if (s.op === 'revolve') {
      const a = s.axis ?? n - 1;
      lines.push(`  c.${C[a]} = length(vec2(c.${C[a]}, c.${C[n]})) - ${glslFloat(s.radius)};`);
    } else {
      const ang = (-s.angle * Math.PI) / 180;
      lines.push(`  c.${C[s.i]}${C[s.j]} = mat2(${glslFloat(Math.cos(ang))}, ${glslFloat(Math.sin(ang))}, ${glslFloat(-Math.sin(ang))}, ${glslFloat(Math.cos(ang))}) * c.${C[s.i]}${C[s.j]};`);
    }
  }
  lines.push(`  float d = ${glslFloat(NEG_INF)};`);
  for (let k = 0; k < steps.length; k++) {
    const s = steps[k];
    if (s.op !== 'extrude' || s.mode === 'infinite') continue;
    const wy = s.mode === 'half' ? `t${k}` : `abs(t${k}) - ${glslFloat(s.length / 2)}`;
    lines.push(`  d = opExtrude(d, ${wy});`);
  }
  lines.push(`  return d;`, `}`);
  return lines.join('\n');
}

// ---------------------------------------------------------------- built-ins

const ext = (length: number): Step => ({ op: 'extrude', mode: 'finite', length });
const inf: Step = { op: 'extrude', mode: 'infinite', length: 0 };
const half: Step = { op: 'extrude', mode: 'half', length: 0 };
const rev = (radius = 0, axis?: number): Step => (axis === undefined ? { op: 'revolve', radius } : { op: 'revolve', radius, axis });

const B = (id: string, name: string, steps: Step[], description: string, extra: Partial<PrimitiveDef> = {}): PrimitiveDef =>
  ({ id, name, steps, description, builtin: true, ...extra });

export const BUILTIN_PRIMITIVES: PrimitiveDef[] = [
  // lower dimensional (rounded so they are visible)
  B('point', 'Point', [], '0D point (rounded to be visible)', { round: 0.08 }),
  B('segment', 'Segment', [ext(2)], '1D line segment along X', { round: 0.05 }),
  B('square', 'Square', [ext(2), ext(2)], '2D square in XY', { round: 0.02 }),
  B('disk', 'Disk', [ext(2), rev()], '2D disk in XY: segment revolved', { round: 0.02 }),
  // 3D
  B('cube', 'Cube', [ext(2), ext(2), ext(2)], 'segment → square → cube'),
  B('sphere', 'Sphere', [ext(2), rev(), rev(0, 0)], 'segment → disk → sphere'),
  B('cylinder', 'Cylinder', [ext(2), rev(), ext(2)], 'disk extruded along Z'),
  B('rod', 'Infinite rod', [ext(0.5), rev(), inf], 'disk extruded infinitely along Z'),
  B('torus', 'Torus', [ext(0.6), rev(), rev(1, 0)], 'disk revolved around Y at radius 1 (in XZ)'),
  B('plane', 'Plane', [inf, half, inf], 'infinite ground half-space y ≤ 0 (3D)'),
  B('slab', 'Slab', [inf, ext(0.2), inf], 'infinite thin slab, normal Y (3D)'),
  // 4D
  B('tesseract', 'Tesseract', [ext(2), ext(2), ext(2), ext(2)], 'cube extruded along W'),
  B('hypersphere', 'Hypersphere', [ext(2), rev(), rev(0, 0), rev(0, 0)], 'sphere revolved into W (glome)'),
  B('spherinder', 'Spherinder', [ext(2), rev(), rev(0, 0), ext(2)], 'sphere extruded along W'),
  B('cubinder', 'Cubinder', [ext(2), rev(), ext(2), ext(2)], 'cylinder extruded along W'),
  B('duocylinder', 'Duocylinder', [ext(2), rev(), ext(2), rev(0, 2)], 'disk × disk (XY and ZW)'),
  B('spheritorus', 'Spheritorus', [ext(0.8), rev(), rev(0, 0), rev(1.2, 0)], 'sphere revolved into W at radius 1.2'),
  B('torisphere', 'Torisphere', [ext(0.6), rev(), rev(1.2, 0), rev(0, 0)], 'torus revolved into W'),
  B('tiger', 'Tiger', [ext(0.6), rev(), rev(1.2, 0), rev(1.2, 1)], 'disk with two revolutions (XZ and YW)'),
  B('ditorus', 'Ditorus', [ext(0.4), rev(), rev(0.8, 0), rev(1.6, 0)], 'torus revolved into W at radius 1.6'),
  B('hyperplane', 'Hyperplane', [inf, half, inf, inf], 'infinite 4D ground half-space y ≤ 0'),
];
