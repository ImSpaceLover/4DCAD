// Scene model + (de)serialisation. Platform independent.
//
// A scene is a tree. Leaves reference a primitive recipe; groups combine their
// children with a boolean operator. Every node has a 4D transform and an
// optional list of domain modifiers (mirror / repeat).
//
// Nodes may be *anchored* to a "world" node: the node then lives on the 3D
// boundary hypersurface of that world (used by the Geodesic 3D view). Its
// `xf.pos` is the anchor point on the surface and `anchor.basis` holds the
// orthonormal frame (3 tangent vectors + surface normal) at that point. Inside
// an anchored node the local W coordinate means "signed distance from the world
// surface", which makes thin 3D objects hug a curved 4D surface.

import { BUILTIN_PRIMITIVES, type PrimitiveDef } from './primitives.ts';
import { identity, mul, rotationFromAngles, type Mat4, type Vec4 } from './math4.ts';

export type BoolOp =
  | 'union' | 'intersection' | 'subtraction'
  | 'smoothUnion' | 'smoothIntersection' | 'smoothSubtraction';

export const BOOL_OPS: BoolOp[] = ['union', 'intersection', 'subtraction', 'smoothUnion', 'smoothIntersection', 'smoothSubtraction'];

export type Modifier =
  | { type: 'mirror'; axis: number }
  | { type: 'repeat'; axis: number; spacing: number; count: number }; // count 0 => infinite, else copies on each side

export interface Transform {
  pos: Vec4;
  rot: number[]; // 6 plane angles in degrees: XY XZ XW YZ YW ZW
  scale: number;
}

export interface Anchor {
  world: string; // id of the world node
  basis: Mat4; // columns: tangent1, tangent2, tangent3, normal (at xf.pos)
  // Validity radius of the tangent-space approximation (~ radius of curvature of the
  // world at the anchor). Points whose offset along the anchor normal is below -reach
  // are ignored, which prevents a second "antipodal" image on closed worlds.
  reach: number;
}

interface NodeBase {
  id: string;
  name: string;
  xf: Transform;
  modifiers: Modifier[];
  hidden?: boolean;
  anchor?: Anchor;
}

export interface LeafNode extends NodeBase {
  kind: 'leaf';
  prim: string; // primitive id
  color: [number, number, number];
  thickness: number; // extent along unused axes for objects with dim < 4
  round?: number; // overrides primitive rounding when set
  shell?: number; // overrides primitive shell when set
}

export interface GroupNode extends NodeBase {
  kind: 'group';
  op: BoolOp;
  k: number; // smoothness for smooth operators
  children: SceneNode[];
}

export type SceneNode = LeafNode | GroupNode;

export interface CameraState {
  target: Vec4;
  distance: number;
  yaw: number;
  pitch: number;
  rot4: Mat4; // accumulated 4D orientation (rotations involving W)
}

export interface QualitySettings {
  resolutionScale: number; // 0.25 .. 1
  maxSteps: number;
  shadows: boolean;
  ao: boolean;
  geoMaxStep: number; // largest geodesic march step (controls accuracy on curved worlds)
  geoProjectIters: number; // Newton projections back onto the world surface per step
}

export interface SceneFile {
  format: '4dcad-scene';
  version: 1;
  root: GroupNode;
  primitives: PrimitiveDef[]; // custom (non built-in) primitives used by the scene
  world: string | null;
  camera?: CameraState;
}

export const DEFAULT_QUALITY: QualitySettings = {
  resolutionScale: 0.75,
  maxSteps: 160,
  shadows: true,
  ao: true,
  geoMaxStep: 0.25,
  geoProjectIters: 1,
};

// ---------------------------------------------------------------- helpers

let idCounter = 0;
export function newId(prefix = 'n'): string {
  idCounter++;
  return `${prefix}${Date.now().toString(36)}${idCounter.toString(36)}${Math.floor(Math.random() * 1e4).toString(36)}`;
}

export const defaultTransform = (pos: Vec4 = [0, 0, 0, 0]): Transform => ({ pos: [...pos] as Vec4, rot: [0, 0, 0, 0, 0, 0], scale: 1 });

export function makeLeaf(prim: string, name: string, pos: Vec4 = [0, 0, 0, 0], color: [number, number, number] = [0.8, 0.5, 0.3]): LeafNode {
  return { id: newId(), name, kind: 'leaf', prim, color, thickness: 0.1, xf: defaultTransform(pos), modifiers: [] };
}

export function makeGroup(op: BoolOp, children: SceneNode[], name?: string): GroupNode {
  return { id: newId('g'), name: name ?? op, kind: 'group', op, k: 0.3, children, xf: defaultTransform(), modifiers: [] };
}

/** Full rotation of a node: anchor basis (if any) followed by its own angles. */
export function nodeRotation(n: SceneNode): Mat4 {
  const r = rotationFromAngles(n.xf.rot);
  return n.anchor ? mul(n.anchor.basis, r) : r;
}

export function walk(n: SceneNode, fn: (n: SceneNode, parent: GroupNode | null, depth: number) => void, parent: GroupNode | null = null, depth = 0) {
  fn(n, parent, depth);
  if (n.kind === 'group') for (const c of n.children) walk(c, fn, n, depth + 1);
}

export function findNode(root: SceneNode, id: string): SceneNode | null {
  let r: SceneNode | null = null;
  walk(root, (n) => { if (n.id === id) r = n; });
  return r;
}

export function findParent(root: GroupNode, id: string): GroupNode | null {
  let r: GroupNode | null = null;
  walk(root, (n, p) => { if (n.id === id) r = p; });
  return r;
}

/** Path of nodes from root to (and including) the node with the given id. */
export function pathTo(root: SceneNode, id: string): SceneNode[] | null {
  if (root.id === id) return [root];
  if (root.kind === 'group') {
    for (const c of root.children) {
      const p = pathTo(c, id);
      if (p) return [root, ...p];
    }
  }
  return null;
}

export function isAncestor(root: SceneNode, ancestorId: string, id: string): boolean {
  const p = pathTo(root, id);
  return !!p && p.some((n) => n.id === ancestorId);
}

export function cloneNode<T extends SceneNode>(n: T, freshIds = true): T {
  const c = JSON.parse(JSON.stringify(n)) as T;
  if (freshIds) {
    const map = new Map<string, string>();
    walk(c, (m) => { const nid = newId(m.kind === 'group' ? 'g' : 'n'); map.set(m.id, nid); m.id = nid; });
  }
  return c;
}

export function removeNode(root: GroupNode, id: string): SceneNode | null {
  const p = findParent(root, id);
  if (!p) return null;
  const i = p.children.findIndex((c) => c.id === id);
  return p.children.splice(i, 1)[0];
}

// ---------------------------------------------------------------- primitive registry

export class PrimitiveRegistry {
  private map = new Map<string, PrimitiveDef>();
  constructor(custom: PrimitiveDef[] = []) {
    for (const p of BUILTIN_PRIMITIVES) this.map.set(p.id, p);
    for (const p of custom) this.map.set(p.id, p);
  }
  get(id: string): PrimitiveDef | undefined { return this.map.get(id); }
  set(p: PrimitiveDef) { this.map.set(p.id, p); }
  delete(id: string) { const p = this.map.get(id); if (p && !p.builtin) this.map.delete(id); }
  all(): PrimitiveDef[] { return [...this.map.values()]; }
  custom(): PrimitiveDef[] { return this.all().filter((p) => !p.builtin); }
}

// ---------------------------------------------------------------- serialisation

export function serializeScene(root: GroupNode, reg: PrimitiveRegistry, world: string | null, camera?: CameraState): SceneFile {
  const used = new Set<string>();
  walk(root, (n) => { if (n.kind === 'leaf') used.add(n.prim); });
  const primitives = [...used].map((id) => reg.get(id)).filter((p): p is PrimitiveDef => !!p && !p.builtin);
  return { format: '4dcad-scene', version: 1, root, primitives, world, camera };
}

/** Parse + validate a scene file; fills defaults for missing optional fields. */
export function parseScene(text: string): SceneFile {
  const data = JSON.parse(text);
  if (!data || data.format !== '4dcad-scene') throw new Error('Not a 4DCAD scene file (missing format: "4dcad-scene")');
  const fix = (n: any): SceneNode => {
    if (!n || typeof n !== 'object') throw new Error('Invalid node');
    n.id ??= newId();
    n.name ??= n.kind === 'group' ? n.op ?? 'group' : n.prim ?? 'object';
    n.xf ??= defaultTransform();
    n.xf.pos = [0, 1, 2, 3].map((i) => Number(n.xf.pos?.[i] ?? 0));
    n.xf.rot = [0, 1, 2, 3, 4, 5].map((i) => Number(n.xf.rot?.[i] ?? 0));
    n.xf.scale = Number(n.xf.scale ?? 1) || 1;
    n.modifiers ??= [];
    if (n.anchor) {
      if (!Array.isArray(n.anchor.basis) || n.anchor.basis.length !== 16) n.anchor.basis = identity();
      n.anchor.reach = Number(n.anchor.reach ?? 1e4) || 1e4;
    }
    if (n.kind === 'group') {
      n.op ??= 'union';
      n.k ??= 0.3;
      n.children = (n.children ?? []).map(fix);
    } else if (n.kind === 'leaf') {
      if (typeof n.prim !== 'string') throw new Error(`Leaf ${n.id} has no primitive`);
      n.color ??= [0.8, 0.5, 0.3];
      n.thickness ??= 0.1;
    } else throw new Error(`Unknown node kind: ${n.kind}`);
    return n as SceneNode;
  };
  const root = fix(data.root);
  if (root.kind !== 'group') throw new Error('Scene root must be a group');
  return { format: '4dcad-scene', version: 1, root, primitives: data.primitives ?? [], world: data.world ?? null, camera: data.camera };
}

// ---------------------------------------------------------------- library objects

/** A saved composite object (subtree) that can be reused / exported. */
export interface LibraryObject {
  format: '4dcad-object';
  version: 1;
  id: string;
  name: string;
  node: SceneNode;
  primitives: PrimitiveDef[];
}

export function makeLibraryObject(node: SceneNode, reg: PrimitiveRegistry, name: string): LibraryObject {
  const copy = cloneNode(node, false);
  delete copy.anchor;
  copy.xf.pos = [0, 0, 0, 0];
  const used = new Set<string>();
  walk(copy, (n) => { if (n.kind === 'leaf') used.add(n.prim); });
  const primitives = [...used].map((id) => reg.get(id)).filter((p): p is PrimitiveDef => !!p && !p.builtin);
  return { format: '4dcad-object', version: 1, id: newId('o'), name, node: copy, primitives };
}

/** Max primitive dimension used inside a subtree. */
export function nodeDim(n: SceneNode, reg: PrimitiveRegistry, primDim: (p: PrimitiveDef) => number): number {
  let d = 0;
  walk(n, (m) => {
    if (m.kind === 'leaf') { const p = reg.get(m.prim); if (p) d = Math.max(d, primDim(p)); }
  });
  return d;
}
