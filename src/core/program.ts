// Compiles a scene into a small bytecode program + node data, packed into one
// RGBA float texture that a *fixed* interpreter shader evaluates (see
// render/shaders.ts). Structural edits therefore never recompile shaders, and
// the shader stays the same size for any scene.
//
// Texture = array of vec4 "texels" (row-major, width TEX_W):
//   [instructions...][node blocks (NODE_STRIDE each)...][primitive blocks (PRIM_STRIDE each)...]
//
// Instruction texel: (opcode, nodeBase, arg, arg2)
//   BEGIN   (1, node, role, skipTo)  push the node's local frame; if the bounding sphere is
//                                    farther than SKIP_DIST, push that distance and jump to skipTo
//   LEAF    (2, node, 0, 0)          evaluate the node's primitive in the current frame, push value
//   COMBINE (3, node, op, 0)         pop b, a; push op(a, b) using the group's smoothness
//   END     (4, node, role, 0)       pop frame; scale value back to the parent frame
//   EMPTY   (5, 0, 0, 0)             push INF
// Roles: 0 normal, 1 tangent (W := 0), 2 hug (W := world distance). Ops: see OPS.
//
// Node block (NODE_STRIDE texels):
//   0 bound centre              1 (bound radius, nMods, primBase | -1, primDim)
//   2 position                  3 (scale, k, thickness, anchor reach)
//   4..7 inverse rotation (columns)
//   8 colour (rgb, highlight)   9 (round, shell, hug world slot, skip distance)
//   10..13 modifiers (type 1 mirror / 2 repeat, axis, spacing, count)
// Primitive block: 0 (nSteps, dim, 0, 0); 1.. steps
//   extrude (1, mode 0 finite / 1 infinite / 2 half, length, newAxis)
//   revolve (2, radius, axis, newAxis)
//   rotate  (3, i, j, -angle radians)

import { computeBounds } from './bounds.ts';
import { nodeRole, type EvalContext } from './evaluator.ts';
import { transpose } from './math4.ts';
import { primitiveDim, type PrimitiveDef } from './primitives.ts';
import { findNode, nodeRotation, pathTo, walk, type BoolOp, type GroupNode, type PrimitiveRegistry, type SceneNode } from './scene.ts';

export const TEX_W = 1024;
export const NODE_STRIDE = 14;
export const PRIM_STRIDE = 17; // header + up to 16 steps
export const MAX_STACK = 16;
export const MAX_WORLDS = 4;
export const SKIP_DIST = 0.5;
export const OPS: Record<BoolOp, number> = { union: 0, intersection: 1, subtraction: 2, smoothUnion: 3, smoothIntersection: 4, smoothSubtraction: 5 };
const ROLE = { normal: 0, tangent: 1, hug: 2 } as const;

export interface ProgramRange { start: number; len: number }

export interface ScenePrograms {
  data: Float32Array; // TEX_W * height * 4
  height: number;
  full: ProgramRange;
  geo: ProgramRange;
  worlds: ProgramRange[]; // world slots used by 'hug' nodes (and the active world)
  activeWorld: number; // slot of the active world, -1 if none
  hugFull: boolean; // does the full / geo program contain 'hug' nodes (needs world values)?
  hugGeo: boolean;
  errors: string[];
}

export function buildPrograms(root: GroupNode, reg: PrimitiveRegistry, activeWorldId: string | null, highlight: Set<string> = new Set()): ScenePrograms {
  const errors: string[] = [];
  const nodes: SceneNode[] = [];
  walk(root, (n) => nodes.push(n));
  const nodeIndex = new Map(nodes.map((n, i) => [n.id, i]));
  const exists = (id: string) => nodeIndex.has(id);
  const world = activeWorldId && findNode(root, activeWorldId) ? activeWorldId : null;

  // world slots
  const worldIds: string[] = [];
  if (world) worldIds.push(world);
  walk(root, (n) => { if (n.anchor && exists(n.anchor.world) && !worldIds.includes(n.anchor.world)) worldIds.push(n.anchor.world); });
  if (worldIds.length > MAX_WORLDS) errors.push(`At most ${MAX_WORLDS} different world surfaces can have attached objects`);

  // primitives
  const prims: PrimitiveDef[] = [];
  const primIndex = new Map<string, number>();
  for (const n of nodes) {
    if (n.kind !== 'leaf' || primIndex.has(n.prim)) continue;
    const def = reg.get(n.prim);
    if (!def) continue;
    if (def.steps.length > PRIM_STRIDE - 1) { errors.push(`Primitive "${def.name}" has too many steps`); continue; }
    primIndex.set(n.prim, prims.length);
    prims.push(def);
  }

  // instructions
  const code: number[][] = [];
  const emit = (ins: number[]) => { code.push(ins); return code.length - 1; };
  let maxDepth = 0;
  const nodeBaseSlot = (n: SceneNode) => nodeIndex.get(n.id)!; // patched to texel index later

  const emitNode = (n: SceneNode, ctx: EvalContext, depth: number) => {
    maxDepth = Math.max(maxDepth, depth);
    const role = nodeRole(n, ctx, world, exists);
    if (role === 'empty' || (n.kind === 'leaf' && !primIndex.has(n.prim)) || (n.kind === 'group' && n.children.length === 0)) {
      emit([5, 0, 0, 0]);
      return;
    }
    const r = ROLE[role];
    const begin = emit([1, nodeBaseSlot(n), r, -1]);
    if (n.kind === 'leaf') emit([2, nodeBaseSlot(n), 0, 0]);
    else {
      n.children.forEach((c, i) => {
        emitNode(c, ctx, depth + 1);
        if (i > 0) emit([3, nodeBaseSlot(n), OPS[n.op], 0]);
      });
    }
    emit([4, nodeBaseSlot(n), r, 0]);
    code[begin][3] = n.id === root.id ? -1 : code.length; // root is never skipped
  };

  const range = (fn: () => void): ProgramRange => { const start = code.length; fn(); return { start, len: code.length - start }; };
  const full = range(() => emitNode(root, 'full', 1));
  const geo = range(() => emitNode(root, 'geo', 1));
  const worlds = worldIds.map((wid) => range(() => {
    const path = pathTo(root, wid)!;
    // ancestors: frames only (never skipped), then the world subtree
    for (let a = 0; a < path.length - 1; a++) emit([1, nodeBaseSlot(path[a]), 0, -1]);
    emitNode(path[path.length - 1], 'world', path.length);
    for (let a = path.length - 2; a >= 0; a--) emit([4, nodeBaseSlot(path[a]), 0, 0]);
    maxDepth = Math.max(maxDepth, path.length);
  }));
  if (maxDepth >= MAX_STACK - 1) errors.push(`Scene tree is too deep (max ${MAX_STACK - 2} levels)`);

  // layout
  const nodeBase = code.length;
  const primBase = nodeBase + nodes.length * NODE_STRIDE;
  const total = primBase + prims.length * PRIM_STRIDE;
  const height = Math.max(1, Math.ceil(total / TEX_W));
  const data = new Float32Array(TEX_W * height * 4);
  const put = (texel: number, v: ArrayLike<number>) => { for (let i = 0; i < 4; i++) data[texel * 4 + i] = v[i] ?? 0; };

  code.forEach((ins, i) => {
    const op = ins[0];
    const nb = op >= 1 && op <= 4 ? nodeBase + ins[1] * NODE_STRIDE : 0;
    put(i, [op, nb, ins[2], ins[3]]);
  });

  const bounds = computeBounds(root, reg);
  const parents = new Map<string, GroupNode>();
  walk(root, (n, p) => { if (p) parents.set(n.id, p); });
  nodes.forEach((n, i) => {
    const b = nodeBase + i * NODE_STRIDE;
    const bd = bounds.get(n.id)!;
    const def = n.kind === 'leaf' ? reg.get(n.prim) : undefined;
    const pi = n.kind === 'leaf' && primIndex.has(n.prim) ? primBase + primIndex.get(n.prim)! * PRIM_STRIDE : -1;
    const mods = n.modifiers.slice(0, 4);
    if (n.modifiers.length > 4) errors.push(`"${n.name}": at most 4 modifiers are supported`);
    put(b, bd.c);
    put(b + 1, [Number.isFinite(bd.r) ? bd.r : 1e9, mods.length, pi, def ? primitiveDim(def) : 4]);
    put(b + 2, n.xf.pos);
    put(b + 3, [n.xf.scale || 1, n.kind === 'group' ? Math.max(n.k, 1e-4) : 0, n.kind === 'leaf' ? n.thickness : 0, n.anchor?.reach ?? 1e4]);
    const inv = transpose(nodeRotation(n));
    for (let c = 0; c < 4; c++) put(b + 4 + c, inv.slice(c * 4, c * 4 + 4));
    put(b + 8, n.kind === 'leaf' ? [...n.color, highlight.has(n.id) ? 1 : 0] : [0, 0, 0, 0]);
    const slot = n.anchor ? worldIds.indexOf(n.anchor.world) : -1;
    const leaf = n.kind === 'leaf' ? n : null;
    // skip threshold: a skipped child must not affect a smooth blend of its parent
    const parent = parents.get(n.id);
    const skip = SKIP_DIST + (parent && parent.op.startsWith('smooth') ? Math.max(parent.k, 0) : 0);
    put(b + 9, [leaf && def ? leaf.round ?? def.round ?? 0 : 0, leaf && def ? leaf.shell ?? def.shell ?? 0 : 0, slot, skip]);
    mods.forEach((m, k) => put(b + 10 + k, m.type === 'mirror' ? [1, m.axis, 0, 0] : [2, m.axis, m.spacing, m.count]));
  });

  prims.forEach((def, i) => {
    const b = primBase + i * PRIM_STRIDE;
    put(b, [def.steps.length, primitiveDim(def), 0, 0]);
    let dim = 0;
    def.steps.forEach((s, k) => {
      if (s.op === 'extrude') put(b + 1 + k, [1, s.mode === 'finite' ? 0 : s.mode === 'infinite' ? 1 : 2, s.length, dim++]);
      else if (s.op === 'revolve') { put(b + 1 + k, [2, s.radius, s.axis ?? dim - 1, dim]); dim++; }
      else put(b + 1 + k, [3, s.i, s.j, (-s.angle * Math.PI) / 180]);
    });
  });

  const hasHug = (r: ProgramRange) => code.slice(r.start, r.start + r.len).some((ins) => ins[0] === 1 && ins[2] === ROLE.hug);
  return { data, height, full, geo, worlds, activeWorld: world ? 0 : -1, hugFull: hasHug(full), hugGeo: hasHug(geo), errors };
}

// ------------------------------------------------------------------------------------------
// JavaScript emulation of the interpreter shader (used by tests to verify the encoding
// against the reference SceneEvaluator; mirrors render/shaders.ts line by line).

const INF = 1e9;

function texel(P: ScenePrograms, i: number): number[] {
  return [P.data[i * 4], P.data[i * 4 + 1], P.data[i * 4 + 2], P.data[i * 4 + 3]];
}

function opExtrude(d: number, wy: number) {
  return Math.min(Math.max(d, wy), 0) + Math.hypot(Math.max(d, 0), Math.max(wy, 0));
}

function primEval(P: ScenePrograms, base: number, c: number[]): number {
  const n = texel(P, base)[0];
  const tv: number[] = [];
  for (let k = n - 1; k >= 0; k--) {
    const s = texel(P, base + 1 + k);
    if (s[0] === 1) tv[k] = c[s[3]];
    else if (s[0] === 2) c[s[2]] = Math.hypot(c[s[2]], c[s[3]]) - s[1];
    else if (s[0] === 3) {
      const ci = c[s[1]], cj = c[s[2]];
      c[s[1]] = Math.cos(s[3]) * ci - Math.sin(s[3]) * cj;
      c[s[2]] = Math.sin(s[3]) * ci + Math.cos(s[3]) * cj;
    }
  }
  let d = -1e9;
  for (let k = 0; k < n; k++) {
    const s = texel(P, base + 1 + k);
    if (s[0] === 1 && s[1] !== 1) d = opExtrude(d, s[1] === 2 ? tv[k] : Math.abs(tv[k]) - s[2] * 0.5);
  }
  return d;
}

export function runProgram(P: ScenePrograms, r: ProgramRange, p: number[], worldVals: number[]): number {
  const fq: number[][] = [p.slice()];
  const fs: number[] = [1];
  const fnd: number[] = [0];
  const vd: number[] = [];
  let pc = r.start;
  const end = r.start + r.len;
  while (pc < end) {
    const ins = texel(P, pc);
    const op = ins[0], nb = ins[1];
    pc++;
    if (op === 1) {
      const P0 = fq[fq.length - 1];
      const bc = texel(P, nb), b1 = texel(P, nb + 1);
      if (ins[3] >= 0) {
        const bd = Math.hypot(P0[0] - bc[0], P0[1] - bc[1], P0[2] - bc[2], P0[3] - bc[3]) - b1[0];
        if (bd > texel(P, nb + 9)[3]) { vd.push(bd); pc = ins[3]; continue; }
      }
      const pos = texel(P, nb + 2), par = texel(P, nb + 3);
      const rel = [P0[0] - pos[0], P0[1] - pos[1], P0[2] - pos[2], P0[3] - pos[3]];
      const q = [0, 0, 0, 0];
      for (let c = 0; c < 4; c++) { const col = texel(P, nb + 4 + c); for (let k = 0; k < 4; k++) q[k] += col[k] * rel[c]; }
      for (let k = 0; k < 4; k++) q[k] /= par[0];
      const sAcc = fs[fs.length - 1] * par[0];
      const nd = q[3];
      if (ins[2] === 1) q[3] = 0;
      else if (ins[2] === 2) q[3] = worldVals[texel(P, nb + 9)[2]] / sAcc;
      for (let m = 0; m < b1[1]; m++) {
        const md = texel(P, nb + 10 + m);
        if (md[0] === 1) q[md[1]] = Math.abs(q[md[1]]);
        else if (md[2] > 0) {
          let rr = Math.round(q[md[1]] / md[2]);
          if (md[3] > 0) rr = Math.min(md[3], Math.max(-md[3], rr));
          q[md[1]] -= md[2] * rr;
        }
      }
      fq.push(q); fs.push(sAcc); fnd.push(nd);
    } else if (op === 2) {
      const q = fq[fq.length - 1].slice();
      const b1 = texel(P, nb + 1), par = texel(P, nb + 3), b9 = texel(P, nb + 9);
      let d = primEval(P, b1[2], q.slice());
      for (let a = b1[3]; a < 4; a++) d = opExtrude(d, Math.abs(q[a]) - 0.5 * par[2]);
      if (b9[1] > 0) d = Math.abs(d) - b9[1];
      vd.push(d - b9[0]);
    } else if (op === 3) {
      const b = vd.pop()!, a = vd.pop()!;
      const k = texel(P, nb + 3)[1];
      let d: number;
      switch (ins[2]) {
        case 0: d = Math.min(a, b); break;
        case 1: d = Math.max(a, b); break;
        case 2: d = Math.max(a, -b); break;
        case 3: { const h = Math.min(1, Math.max(0, 0.5 + (0.5 * (b - a)) / k)); d = b + (a - b) * h - k * h * (1 - h); break; }
        case 4: { const h = Math.min(1, Math.max(0, 0.5 - (0.5 * (b - a)) / k)); d = b + (a - b) * h + k * h * (1 - h); break; }
        default: { const h = Math.min(1, Math.max(0, 0.5 - (0.5 * (a + b)) / k)); d = a + (-b - a) * h + k * h * (1 - h); }
      }
      vd.push(d);
    } else if (op === 4) {
      fq.pop(); fs.pop();
      const nd = fnd.pop()!;
      let d = vd.pop()!;
      if (d < INF * 0.5) {
        const par = texel(P, nb + 3);
        d *= par[0];
        if (ins[2] > 0) d = Math.max(d, -nd * par[0] - par[3]);
      }
      vd.push(d);
    } else vd.push(INF);
  }
  return vd.length ? vd[vd.length - 1] : INF;
}

export function runScene(P: ScenePrograms, mode: 'full' | 'geo', p: number[]): number {
  const wv = P.worlds.map((w) => runProgram(P, w, p, []));
  return runProgram(P, mode === 'geo' ? P.geo : P.full, p, wv);
}
