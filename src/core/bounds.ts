// Conservative bounding hyperspheres for scene nodes. Used by the shader to
// skip evaluating a subtree when the sample point is far from it (the distance
// to the bounding sphere is a valid lower bound of the distance to the geometry).

import { primitiveDim, type PrimitiveDef } from './primitives.ts';
import { addScaled, mulVec, type Vec4 } from './math4.ts';
import { nodeRotation, type LeafNode, type PrimitiveRegistry, type SceneNode } from './scene.ts';

export interface Bound { c: Vec4; r: number } // r = Infinity => unbounded

export function primitiveExtents(def: PrimitiveDef): number[] {
  const e = [0, 0, 0, 0];
  let n = 0;
  for (const s of def.steps) {
    if (s.op === 'extrude') { e[n] = s.mode === 'finite' ? s.length / 2 : Infinity; n++; }
    else if (s.op === 'revolve') { const a = s.axis ?? n - 1; const v = s.radius + e[a]; e[a] = v; e[n] = v; n++; }
    else { const v = Math.hypot(e[s.i], e[s.j]); e[s.i] = v; e[s.j] = v; }
  }
  return e;
}

export function leafLocalRadius(n: LeafNode, def: PrimitiveDef | undefined): number {
  if (!def) return 0;
  const e = primitiveExtents(def);
  for (let a = primitiveDim(def); a < 4; a++) e[a] = n.thickness / 2;
  const round = Math.max(0, n.round ?? def.round ?? 0);
  const shell = Math.max(0, n.shell ?? def.shell ?? 0);
  return Math.hypot(...e) + round + shell;
}

/** Bounds of every node, expressed in its parent's coordinate frame. */
export function computeBounds(root: SceneNode, reg: PrimitiveRegistry): Map<string, Bound> {
  const out = new Map<string, Bound>();
  const visit = (n: SceneNode): Bound => {
    // local bound (in the node's own frame, after its modifiers)
    let c: Vec4 = [0, 0, 0, 0];
    let r: number;
    if (n.kind === 'leaf') r = leafLocalRadius(n, reg.get(n.prim));
    else {
      const kids = n.children.map(visit);
      if (!kids.length) r = 0;
      else if (n.op === 'subtraction' || n.op === 'smoothSubtraction' || n.op === 'intersection' || n.op === 'smoothIntersection') {
        // result lies inside the first child
        c = kids[0].c; r = kids[0].r;
      } else {
        const fin = kids.filter((k) => Number.isFinite(k.r));
        if (fin.length < kids.length) r = Infinity;
        else {
          c = [0, 0, 0, 0];
          for (const k of fin) c = addScaled(c, k.c, 1 / fin.length);
          r = Math.max(...fin.map((k) => Math.hypot(k.c[0] - c[0], k.c[1] - c[1], k.c[2] - c[2], k.c[3] - c[3]) + k.r));
        }
      }
      if (n.op.startsWith('smooth')) r += n.k;
    }
    for (const m of n.modifiers) {
      if (m.type === 'mirror') { r += Math.abs(c[m.axis]); c = [...c] as Vec4; c[m.axis] = 0; }
      else r = m.count > 0 ? r + m.count * m.spacing : Infinity;
    }
    // to parent frame
    const s = n.xf.scale;
    let b: Bound;
    if (!Number.isFinite(r)) b = { c: [...n.xf.pos] as Vec4, r: Infinity };
    else {
      const pc = addScaled(n.xf.pos, mulVec(nodeRotation(n), c), s);
      b = { c: pc, r: r * s };
      // anchored objects bend along the world surface: pad generously
      if (n.anchor) b.r = b.r * 1.5 + 0.1;
    }
    out.set(n.id, b);
    return b;
  };
  visit(root);
  return out;
}
