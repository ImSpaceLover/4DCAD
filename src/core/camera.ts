// Camera models. Platform independent: controllers receive abstract input
// deltas (mouse pixels, key booleans), not DOM events.
//
// Full 4D camera: orientation = rot4 * E(yaw, pitch), where E is an ordinary
// 3D turntable rotation embedded in XYZ and rot4 accumulates rotations that
// involve the W axis. Columns of the frame: right, up, forward, ana.
// Rays leave the eye within the (right, up, forward) hyperplane.
//
// Geodesic camera: a pivot point on the world surface plus an orthonormal frame
// (right, up, forward tangent; normal). In Edit mode the eye sits `distance`
// behind the pivot along a geodesic (Blender-like orbit); in Explore mode
// distance is 0 and the eye is the pivot.

import { addScaled, col, identity, mul, normalize, planeRotation, transpose, type Mat4, type Vec4 } from './math4.ts';
import { projectToSurface, rotateFrame, shoot, tangentFrame, type SurfaceField } from './geodesic.ts';
import type { CameraState } from './scene.ts';

export interface View {
  eye: Vec4;
  frame: Mat4; // columns: right, up, forward, (ana | normal)
}

export class FullCamera implements CameraState {
  target: Vec4 = [0, 0.5, 0, 0];
  distance = 8;
  yaw = 0.6;
  pitch = -0.35;
  rot4: Mat4 = identity();

  load(s: CameraState) {
    this.target = [...s.target] as Vec4;
    this.distance = s.distance;
    this.yaw = s.yaw;
    this.pitch = s.pitch;
    this.rot4 = s.rot4.slice();
  }
  save(): CameraState {
    return { target: [...this.target] as Vec4, distance: this.distance, yaw: this.yaw, pitch: this.pitch, rot4: this.rot4.slice() };
  }

  private e3(): Mat4 {
    return mul(planeRotation(2, 0, this.yaw), planeRotation(2, 1, this.pitch));
  }
  frame(): Mat4 {
    return mul(this.rot4, this.e3());
  }
  view(): View {
    const fr = this.frame();
    return { eye: addScaled(this.target, col(fr, 2), -this.distance), frame: fr };
  }

  // --- Blender-like (Edit) ---
  orbit(dx: number, dy: number) {
    this.yaw += dx * 0.008;
    this.pitch = Math.max(-1.55, Math.min(1.55, this.pitch - dy * 0.008));
  }
  pan(dx: number, dy: number) {
    const fr = this.frame();
    const k = this.distance * 0.0015;
    this.target = addScaled(addScaled(this.target, col(fr, 0), -dx * k), col(fr, 1), dy * k);
  }
  zoom(delta: number) {
    this.distance = Math.max(0.2, Math.min(200, this.distance * Math.exp(delta * 0.001)));
  }
  /** Rotate the view in a camera-relative plane involving ana (a,b in 0..3 = right, up, forward, ana). */
  rotate4(a: number, b: number, angle: number) {
    const e = this.e3();
    this.rot4 = mul(mul(this.rot4, e), mul(planeRotation(a, b, angle), transpose(e)));
  }
  reset4() { this.rot4 = identity(); }

  // --- game-like (Explore) ---
  look(dx: number, dy: number) {
    // first-person: rotate around the eye, so keep the eye fixed
    const eye = this.view().eye;
    this.yaw += dx * 0.003;
    this.pitch = Math.max(-1.55, Math.min(1.55, this.pitch - dy * 0.003));
    this.target = addScaled(eye, col(this.frame(), 2), this.distance);
  }
  rotate4AroundEye(a: number, b: number, angle: number) {
    const eye = this.view().eye;
    this.rotate4(a, b, angle);
    this.target = addScaled(eye, col(this.frame(), 2), this.distance);
  }
  /** Move in camera space: (right, up, forward, ana). Up uses world Y-ish camera up. */
  move(d: Vec4, collide?: (from: Vec4, to: Vec4) => boolean) {
    const fr = this.frame();
    let delta: Vec4 = [0, 0, 0, 0];
    for (let c = 0; c < 4; c++) delta = addScaled(delta, col(fr, c), d[c]);
    const eye = this.view().eye;
    const to = addScaled(eye, delta, 1);
    if (collide && collide(eye, to)) return;
    this.target = addScaled(this.target, delta, 1);
  }
}

export class GeoCamera {
  pivot: Vec4 = [0, 0, 0, 0];
  frame: Mat4 = identity();
  distance = 4;
  valid = false;

  /** Place the camera on the surface from an arbitrary 4D view. */
  initFrom(f: SurfaceField, v: View, distance: number) {
    this.pivot = projectToSurface(f, v.eye, 30);
    this.frame = tangentFrame(f, this.pivot, v.frame);
    this.distance = distance;
    this.valid = Number.isFinite(this.pivot[0]);
  }

  view(f: SurfaceField, maxStep: number): View {
    if (this.distance <= 1e-6) return { eye: this.pivot, frame: this.frame };
    const r = shoot(f, this.pivot, this.frame, [0, 0, -1], this.distance, maxStep);
    // after walking backwards the transported frame still faces the pivot
    return { eye: r.pos, frame: r.frame };
  }

  // Edit (orbit about pivot)
  orbit(dx: number, dy: number) {
    this.frame = rotateFrame(this.frame, 2, 0, dx * 0.008);
    this.frame = rotateFrame(this.frame, 2, 1, -dy * 0.008);
  }
  pan(f: SurfaceField, dx: number, dy: number, maxStep: number) {
    const k = Math.max(this.distance, 1) * 0.0015;
    const len = Math.hypot(dx, dy) * k;
    if (len < 1e-6) return;
    const r = shoot(f, this.pivot, this.frame, [-dx, dy, 0], len, maxStep);
    this.pivot = r.pos;
    this.frame = r.frame;
  }
  zoom(delta: number) {
    this.distance = Math.max(0.2, Math.min(50, this.distance * Math.exp(delta * 0.001)));
  }

  // Explore (first person, eye = pivot)
  look(dx: number, dy: number) {
    this.frame = rotateFrame(this.frame, 2, 0, dx * 0.003);
    this.frame = rotateFrame(this.frame, 2, 1, -dy * 0.003);
  }
  roll(angle: number) {
    this.frame = rotateFrame(this.frame, 0, 1, angle);
  }
  move(f: SurfaceField, d: [number, number, number], maxStep: number, collide?: (to: Vec4) => boolean) {
    const len = Math.hypot(d[0], d[1], d[2]);
    if (len < 1e-9) return;
    const r = shoot(f, this.pivot, this.frame, [d[0] / len, d[1] / len, d[2] / len], len, maxStep);
    if (collide && collide(r.pos)) return;
    this.pivot = r.pos;
    this.frame = r.frame;
  }

  /** Convert between edit (orbit) and explore (eye) representations without a visual jump. */
  toExplore(f: SurfaceField, maxStep: number) {
    const v = this.view(f, maxStep);
    this.pivot = v.eye;
    this.frame = v.frame;
    this.distance = 0;
  }
  toEdit(f: SurfaceField, distance: number, maxStep: number) {
    const r = shoot(f, this.pivot, this.frame, [0, 0, 1], distance, maxStep);
    this.pivot = r.pos;
    this.frame = r.frame;
    this.distance = distance;
  }
}

export const forward = (v: View): Vec4 => normalize(col(v.frame, 2));
