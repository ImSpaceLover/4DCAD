// CPU evaluation of the scene SDF. Mirrors the GLSL emitted by codegen.ts
// exactly (same roles, same operators) and is used for picking, placement,
// collision and for moving the camera along a world surface.

import { evalPrimitive, primitiveDim, type PrimitiveDef } from './primitives.ts';
import { clamp, mix, mulVec, sub, transpose, type Mat4, type Vec4 } from './math4.ts';
import { findNode, nodeRotation, pathTo, type BoolOp, type GroupNode, type Modifier, type PrimitiveRegistry, type SceneNode } from './scene.ts';

export const INF = 1e9;

/** full: plain 4D scene.  geo: Geodesic-3D view of the active world.  world: evaluating a world's own SDF. */
export type EvalContext = 'full' | 'geo' | 'world';

/**
 * How a node participates in a given evaluation context:
 *   empty   – contributes nothing (distance = INF)
 *   normal  – ordinary 4D SDF
 *   tangent – anchored to the active world, evaluated in the anchor's tangent space (W := 0)
 *   hug     – anchored, local W := signed distance to its world surface (thin shell hugging it)
 */
export type Role = 'empty' | 'normal' | 'tangent' | 'hug';

export function nodeRole(n: SceneNode, ctx: EvalContext, activeWorld: string | null, worldExists: (id: string) => boolean): Role {
  if (n.hidden) return 'empty';
  if (ctx === 'geo' && n.id === activeWorld) return 'empty';
  if (n.anchor) {
    if (ctx === 'world') return 'empty';
    if (!worldExists(n.anchor.world)) return 'normal';
    if (ctx === 'geo' && n.anchor.world === activeWorld) return 'tangent';
    return 'hug';
  }
  return 'normal';
}

export function applyModifier(q: Vec4, m: Modifier) {
  if (m.type === 'mirror') q[m.axis] = Math.abs(q[m.axis]);
  else {
    const s = m.spacing;
    if (s <= 0) return;
    let r = Math.round(q[m.axis] / s);
    if (m.count > 0) r = clamp(r, -m.count, m.count);
    q[m.axis] -= s * r;
  }
}

export function opExtrude(d: number, wy: number): number {
  return Math.min(Math.max(d, wy), 0) + Math.hypot(Math.max(d, 0), Math.max(wy, 0));
}

export interface Sample {
  d: number;
  color: [number, number, number];
  id: string | null; // leaf id of the closest surface
}

function combine(op: BoolOp, k: number, a: Sample, b: Sample): Sample {
  switch (op) {
    case 'union': return b.d < a.d ? b : a;
    case 'intersection': return b.d > a.d ? b : a;
    case 'subtraction': return -b.d > a.d ? { ...b, d: -b.d } : a;
    case 'smoothUnion': {
      const h = clamp(0.5 + (0.5 * (b.d - a.d)) / k, 0, 1);
      return { d: mix(b.d, a.d, h) - k * h * (1 - h), color: mixC(b.color, a.color, h), id: h > 0.5 ? a.id : b.id };
    }
    case 'smoothIntersection': {
      const h = clamp(0.5 - (0.5 * (b.d - a.d)) / k, 0, 1);
      return { d: mix(b.d, a.d, h) + k * h * (1 - h), color: mixC(b.color, a.color, h), id: h > 0.5 ? a.id : b.id };
    }
    case 'smoothSubtraction': {
      const h = clamp(0.5 - (0.5 * (a.d + b.d)) / k, 0, 1);
      return { d: mix(a.d, -b.d, h) + k * h * (1 - h), color: mixC(a.color, b.color, h), id: h > 0.5 ? b.id : a.id };
    }
  }
}
const mixC = (a: number[], b: number[], t: number): [number, number, number] => [mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t)];

const EMPTY: Sample = { d: INF, color: [0, 0, 0], id: null };

interface Prepared {
  inv: Mat4;
  prim?: PrimitiveDef;
  dim: number;
}

export class SceneEvaluator {
  private prep = new Map<string, Prepared>();
  readonly worldNode: SceneNode | null;
  private root: GroupNode;
  readonly activeWorld: string | null;

  constructor(root: GroupNode, reg: PrimitiveRegistry, activeWorld: string | null) {
    this.root = root;
    this.worldNode = activeWorld ? findNode(root, activeWorld) : null;
    this.activeWorld = this.worldNode ? activeWorld : null;
    const visit = (n: SceneNode) => {
      const prim = n.kind === 'leaf' ? reg.get(n.prim) : undefined;
      this.prep.set(n.id, { inv: transpose(nodeRotation(n)), prim, dim: prim ? primitiveDim(prim) : 4 });
      if (n.kind === 'group') n.children.forEach(visit);
    };
    visit(root);
  }

  private worldExists = (id: string) => this.prep.has(id);

  private wcache = new Map<string, number>();
  evalFull(p: Vec4): Sample { this.wcache.clear(); return this.evalNode(this.root, p, p, 1, 'full'); }
  evalGeo(p: Vec4): Sample { this.wcache.clear(); return this.evalNode(this.root, p, p, 1, 'geo'); }
  eval(p: Vec4, ctx: 'full' | 'geo'): Sample { return ctx === 'geo' ? this.evalGeo(p) : this.evalFull(p); }

  /** Signed distance to the boundary of the given world node (default: active world). */
  worldDist(g: Vec4, worldId: string | null = this.activeWorld): number {
    if (!worldId) return INF;
    const path = pathTo(this.root, worldId);
    if (!path) return INF;
    let p: Vec4 = [...g] as Vec4;
    let s = 1;
    for (let i = 0; i < path.length - 1; i++) {
      const a = path[i];
      p = this.toLocal(a, p);
      s *= a.xf.scale;
      for (const m of a.modifiers) applyModifier(p, m);
    }
    return this.evalNode(path[path.length - 1], p, g, s, 'world').d * s;
  }

  worldGrad(g: Vec4, worldId: string | null = this.activeWorld): Vec4 {
    const e = 1e-3;
    const f0 = this.worldDist(g, worldId);
    const r: Vec4 = [0, 0, 0, 0];
    for (let i = 0; i < 4; i++) {
      const q: Vec4 = [...g] as Vec4;
      q[i] += e;
      r[i] = (this.worldDist(q, worldId) - f0) / e;
    }
    return r;
  }

  private toLocal(n: SceneNode, p: Vec4): Vec4 {
    const q = mulVec(this.prep.get(n.id)!.inv, sub(p, n.xf.pos));
    const s = n.xf.scale;
    return [q[0] / s, q[1] / s, q[2] / s, q[3] / s];
  }

  /** p: point in the parent's local frame. g: global point. sAcc: product of ancestor scales. */
  private evalNode(n: SceneNode, p: Vec4, g: Vec4, sAcc: number, ctx: EvalContext): Sample {
    const role = nodeRole(n, ctx, this.activeWorld, this.worldExists);
    if (role === 'empty') return EMPTY;
    const s = n.xf.scale;
    const q = this.toLocal(n, p);
    const nd = q[3]; // offset along the anchor normal (local units)
    if (role === 'tangent') q[3] = 0;
    else if (role === 'hug') {
      const wid = n.anchor!.world;
      let wd = this.wcache.get(wid);
      if (wd === undefined) { wd = this.worldDist(g, wid); this.wcache.set(wid, wd); }
      q[3] = wd / (sAcc * s);
    }
    for (const m of n.modifiers) applyModifier(q, m);
    let r: Sample;
    if (n.kind === 'leaf') {
      const pr = this.prep.get(n.id)!;
      if (!pr.prim) return EMPTY;
      let d = evalPrimitive(pr.prim, q);
      for (let a = pr.dim; a < 4; a++) d = opExtrude(d, Math.abs(q[a]) - n.thickness / 2);
      const shell = n.shell ?? pr.prim.shell ?? 0;
      if (shell > 0) d = Math.abs(d) - shell;
      d -= n.round ?? pr.prim.round ?? 0;
      r = { d, color: n.color, id: n.id };
    } else {
      r = EMPTY;
      let first = true;
      for (const c of n.children) {
        const cs = this.evalNode(c, q, g, sAcc * s, ctx);
        if (first) { r = cs; first = false; continue; }
        r = combine(n.op, Math.max(n.k, 1e-4), r, cs);
      }
    }
    if (r.d >= INF * 0.5) return r;
    let d = r.d * s;
    if (role === 'tangent' || role === 'hug') d = Math.max(d, -nd * s - n.anchor!.reach);
    return { ...r, d };
  }
}
