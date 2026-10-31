// Minimal 4D linear algebra. Platform independent (no DOM / WebGL).
// Matrices are 4x4, column-major (same layout as GLSL `mat4`): m[col * 4 + row].

export type Vec4 = [number, number, number, number];
export type Mat4 = number[]; // length 16, column-major

export const AXES = ['x', 'y', 'z', 'w'] as const;
export type AxisName = (typeof AXES)[number];

/** The six rotation planes of 4D space, in canonical order. */
export const PLANES: ReadonlyArray<[number, number]> = [
  [0, 1], [0, 2], [0, 3], [1, 2], [1, 3], [2, 3],
];
export const PLANE_NAMES = ['XY', 'XZ', 'XW', 'YZ', 'YW', 'ZW'] as const;

export const vec4 = (x = 0, y = 0, z = 0, w = 0): Vec4 => [x, y, z, w];
export const add = (a: Vec4, b: Vec4): Vec4 => [a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]];
export const sub = (a: Vec4, b: Vec4): Vec4 => [a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]];
export const scale = (a: Vec4, s: number): Vec4 => [a[0] * s, a[1] * s, a[2] * s, a[3] * s];
export const addScaled = (a: Vec4, b: Vec4, s: number): Vec4 =>
  [a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s, a[3] + b[3] * s];
export const dot = (a: Vec4, b: Vec4): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
export const length = (a: Vec4): number => Math.sqrt(dot(a, a));
export const normalize = (a: Vec4): Vec4 => {
  const l = length(a);
  return l > 1e-12 ? scale(a, 1 / l) : [0, 0, 0, 0];
};
/** Remove the component of `a` along unit vector `n`. */
export const reject = (a: Vec4, n: Vec4): Vec4 => addScaled(a, n, -dot(a, n));

export function identity(): Mat4 {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

export function col(m: Mat4, c: number): Vec4 {
  return [m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]];
}

export function fromCols(c0: Vec4, c1: Vec4, c2: Vec4, c3: Vec4): Mat4 {
  return [...c0, ...c1, ...c2, ...c3];
}

export function mulVec(m: Mat4, v: Vec4): Vec4 {
  const r: Vec4 = [0, 0, 0, 0];
  for (let row = 0; row < 4; row++) {
    r[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
  }
  return r;
}

export function mul(a: Mat4, b: Mat4): Mat4 {
  const r = new Array<number>(16).fill(0);
  for (let c = 0; c < 4; c++) {
    for (let row = 0; row < 4; row++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  }
  return r;
}

export function transpose(m: Mat4): Mat4 {
  const r = new Array<number>(16);
  for (let c = 0; c < 4; c++) for (let row = 0; row < 4; row++) r[row * 4 + c] = m[c * 4 + row];
  return r;
}

/** Rotation by `angle` (radians) in the plane spanned by axes i and j (i -> j). */
export function planeRotation(i: number, j: number, angle: number): Mat4 {
  const m = identity();
  const c = Math.cos(angle), s = Math.sin(angle);
  m[i * 4 + i] = c; m[j * 4 + j] = c;
  m[i * 4 + j] = s; // column i, row j
  m[j * 4 + i] = -s; // column j, row i
  return m;
}

/** Rotation from 6 plane angles in degrees, order XY, XZ, XW, YZ, YW, ZW (applied right-to-left). */
export function rotationFromAngles(deg: number[]): Mat4 {
  let m = identity();
  for (let k = 0; k < 6; k++) {
    const a = deg[k] ?? 0;
    if (a !== 0) m = mul(m, planeRotation(PLANES[k][0], PLANES[k][1], (a * Math.PI) / 180));
  }
  return m;
}

/** Re-orthonormalise the columns of m (Gram-Schmidt, column 0 first). */
export function orthonormalize(m: Mat4): Mat4 {
  const cols: Vec4[] = [col(m, 0), col(m, 1), col(m, 2), col(m, 3)];
  for (let i = 0; i < 4; i++) {
    let v = cols[i];
    for (let j = 0; j < i; j++) v = addScaled(v, cols[j], -dot(v, cols[j]));
    cols[i] = normalize(v);
  }
  return fromCols(cols[0], cols[1], cols[2], cols[3]);
}

/**
 * Complete a set of orthonormal vectors to a full orthonormal basis of R^4 by
 * Gram-Schmidt against the standard axes. Returns the input vectors followed by new ones.
 */
export function completeBasis(vs: Vec4[]): Vec4[] {
  const out = vs.slice();
  for (let a = 0; a < 4 && out.length < 4; a++) {
    let v: Vec4 = [0, 0, 0, 0];
    v[a] = 1;
    for (const u of out) v = addScaled(v, u, -dot(v, u));
    if (length(v) > 1e-4) out.push(normalize(v));
  }
  return out;
}

export const clamp = (x: number, a: number, b: number) => Math.min(b, Math.max(a, x));
export const mix = (a: number, b: number, t: number) => a + (b - a) * t;
