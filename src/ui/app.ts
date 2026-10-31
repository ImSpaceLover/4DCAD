// Application state, render loop, camera input, picking, placement and the
// Blender-style modal transform tools (G / R / S).

import { FullCamera, GeoCamera, type View } from '../core/camera.ts';
import { buildPrograms } from '../core/program.ts';
import { SceneEvaluator } from '../core/evaluator.ts';
import { shoot } from '../core/geodesic.ts';
import { addScaled, col, dot, length, normalize, PLANES, sub, type Vec4 } from '../core/math4.ts';
import { primitiveDim } from '../core/primitives.ts';
import {
  cloneNode, DEFAULT_QUALITY, findNode, findParent, isAncestor, makeGroup, makeLeaf, nodeDim, parseScene, pathTo, PrimitiveRegistry,
  removeNode, serializeScene, walk, type Anchor, type BoolOp, type GroupNode, type LibraryObject, type QualitySettings, type SceneFile, type SceneNode, type Transform,
} from '../core/scene.ts';
import { castRay, curvatureRadius, makeAnchor, rayDir, worldField } from '../core/surface.ts';
import { Renderer } from '../render/renderer.ts';
import { toast } from './dom.ts';
import { LibraryStore } from './library.ts';

export type Mode = 'edit' | 'explore';
export type ViewMode = 'full' | 'geo';
export type LibraryItem = { kind: 'prim'; id: string } | { kind: 'object'; id: string };

type Topic = 'structure' | 'selection' | 'values' | 'mode' | 'library';

interface ModalTransform {
  kind: 'grab' | 'rotate' | 'scale';
  ids: string[];
  orig: Map<string, { xf: Transform; anchor?: Anchor }>;
  x0: number;
  y0: number;
  x: number;
  y: number;
  axes: number[];
  view: View;
}

const AUTOSAVE = '4dcad.autosave.v1';

export class App {
  root!: GroupNode;
  reg = new PrimitiveRegistry();
  lib: LibraryStore;
  world: string | null = null;
  selection: string[] = [];
  mode: Mode = 'edit';
  viewMode: ViewMode = 'full';
  fullCam = new FullCamera();
  geoCam = new GeoCamera();
  quality: QualitySettings = { ...DEFAULT_QUALITY };
  fov = 60;
  light: Vec4 = [0.5, 0.8, -0.35, 0.25];
  collide = true;
  moveSpeed = 3;
  ev!: SceneEvaluator;
  renderer: Renderer;
  fps = 0;
  status = '';

  private history: string[] = [];
  private future: string[] = [];
  private committed = '';
  private listeners = new Map<Topic, (() => void)[]>();
  private keys = new Set<string>();
  private drag: { button: number; x: number; y: number; kind: 'orbit' | 'pan' | 'rot4' | 'zoom' } | null = null;
  transform: ModalTransform | null = null;
  private lastFrame = performance.now();
  private version = 0;
  private lastKey = '';
  private lastErrors = '';

  constructor(readonly canvas: HTMLCanvasElement) {
    this.renderer = new Renderer(canvas);
    this.lib = new LibraryStore(this.reg);
    this.lib.onChange(() => { this.rebuild(); this.emit('library'); });
  }

  // ------------------------------------------------------------ events
  on(topic: Topic, fn: () => void) {
    if (!this.listeners.has(topic)) this.listeners.set(topic, []);
    this.listeners.get(topic)!.push(fn);
  }
  emit(...topics: Topic[]) {
    for (const t of topics) for (const fn of this.listeners.get(t) ?? []) fn();
  }

  // ------------------------------------------------------------ scene lifecycle
  loadScene(file: SceneFile, keepCamera = false) {
    for (const p of file.primitives) this.reg.set({ ...p, builtin: false });
    this.root = file.root;
    this.world = file.world && findNode(this.root, file.world) ? file.world : null;
    this.selection = [];
    if (file.camera && !keepCamera) this.fullCam.load(file.camera);
    this.viewMode = 'full';
    this.mode = 'edit';
    this.geoCam.valid = false;
    this.history = [];
    this.future = [];
    this.rebuild();
    this.committed = this.snapshot();
    this.emit('structure', 'selection', 'mode');
  }

  loadSceneText(text: string) {
    this.loadScene(parseScene(text));
  }

  serialize(): SceneFile {
    return serializeScene(this.root, this.reg, this.world, this.fullCam.save());
  }

  private snapshot(): string {
    return JSON.stringify({ root: this.root, world: this.world });
  }

  /** Rebuild the CPU evaluator after any change (the GPU data is rebuilt per rendered frame). */
  rebuild() {
    this.version++;
    this.ev = new SceneEvaluator(this.root, this.reg, this.world);
  }

  /** Call after a finished user edit: records undo history and refreshes panels. */
  commit(...topics: Topic[]) {
    this.rebuild();
    const snap = this.snapshot();
    if (snap !== this.committed) {
      this.history.push(this.committed);
      if (this.history.length > 200) this.history.shift();
      this.future = [];
      this.committed = snap;
      try { localStorage.setItem(AUTOSAVE, JSON.stringify(this.serialize())); } catch { /* ignore */ }
    }
    this.emit(...(topics.length ? topics : (['structure', 'values'] as Topic[])));
  }

  /** Live (non-committed) change, e.g. while dragging a field. */
  live() { this.rebuild(); }

  undo() { this.step(this.history, this.future); }
  redo() { this.step(this.future, this.history); }
  private step(from: string[], to: string[]) {
    const s = from.pop();
    if (!s) return;
    to.push(this.committed);
    const d = JSON.parse(s);
    this.root = d.root;
    this.world = d.world;
    this.committed = s;
    this.selection = this.selection.filter((id) => findNode(this.root, id));
    this.rebuild();
    this.emit('structure', 'selection', 'values');
  }

  static autosaved(): string | null {
    try { return localStorage.getItem(AUTOSAVE); } catch { return null; }
  }

  // ------------------------------------------------------------ selection / tree ops
  select(ids: string[]) {
    this.selection = ids;
    this.emit('selection');
  }
  selected(): SceneNode | null {
    return this.selection.length ? findNode(this.root, this.selection[this.selection.length - 1]) : null;
  }
  highlightSet(): Set<string> {
    const s = new Set<string>();
    if (this.mode !== 'edit') return s;
    for (const id of this.selection) {
      const n = findNode(this.root, id);
      if (n) walk(n, (m) => s.add(m.id));
    }
    return s;
  }

  deleteSelection() {
    for (const id of this.selection) if (id !== this.root.id) removeNode(this.root, id);
    if (this.world && !findNode(this.root, this.world)) this.world = null;
    this.selection = [];
    this.commit('structure', 'selection');
  }

  duplicateSelection() {
    const out: string[] = [];
    for (const id of this.selection) {
      const n = findNode(this.root, id);
      const p = findParent(this.root, id);
      if (!n || !p) continue;
      const c = cloneNode(n);
      c.name = n.name + ' copy';
      p.children.splice(p.children.indexOf(n) + 1, 0, c);
      out.push(c.id);
    }
    this.selection = out;
    this.commit('structure', 'selection');
    if (out.length) this.startTransform('grab');
  }

  /** Wrap the selected nodes into a new boolean group (placed where the first one was). */
  groupSelection(op: BoolOp) {
    const nodes = this.selection.map((id) => findNode(this.root, id)).filter((n): n is SceneNode => !!n && n.id !== this.root.id);
    // drop nodes whose ancestor is also selected
    const top = nodes.filter((n) => !nodes.some((m) => m !== n && isAncestor(this.root, m.id, n.id)));
    if (!top.length) return;
    const parent = findParent(this.root, top[0].id)!;
    const idx = parent.children.indexOf(top[0]);
    for (const n of top) removeNode(this.root, n.id);
    const g = makeGroup(op, top, op);
    parent.children.splice(Math.min(idx, parent.children.length), 0, g);
    this.selection = [g.id];
    this.commit('structure', 'selection');
  }

  ungroup(id: string) {
    const g = findNode(this.root, id);
    const p = findParent(this.root, id);
    if (!g || g.kind !== 'group' || !p) return;
    const i = p.children.indexOf(g);
    p.children.splice(i, 1, ...g.children);
    this.selection = g.children.map((c) => c.id);
    this.commit('structure', 'selection');
  }

  /** Move node `id` into group `target` (or next to a leaf by wrapping both in a union). */
  reparent(id: string, targetId: string, where: 'into' | 'before' | 'after' | 'wrap') {
    if (id === targetId || id === this.root.id || isAncestor(this.root, id, targetId)) return;
    const target = findNode(this.root, targetId);
    if (!target) return;
    const n = removeNode(this.root, id);
    if (!n) return;
    if (where === 'into' && target.kind === 'group') target.children.push(n);
    else if (where === 'wrap') {
      const p = findParent(this.root, targetId)!;
      const i = p.children.indexOf(target);
      p.children.splice(i, 1, makeGroup('union', [target, n], 'union'));
    } else {
      const p = findParent(this.root, targetId) ?? this.root;
      const i = p.children.indexOf(target);
      p.children.splice(where === 'after' ? i + 1 : i, 0, n);
    }
    this.commit('structure');
  }

  setWorld(id: string | null) {
    this.world = id;
    if (!id && this.viewMode === 'geo') this.setViewMode('full');
    this.geoCam.valid = false;
    if (this.viewMode === 'geo') { this.rebuild(); this.initGeoCamera(); }
    this.commit('structure', 'mode');
  }

  // ------------------------------------------------------------ modes & cameras
  setMode(m: Mode) {
    if (m === this.mode) return;
    this.cancelTransform();
    if (this.viewMode === 'geo' && this.geoCam.valid) {
      const f = worldField(this.ev);
      if (m === 'explore') this.geoCam.toExplore(f, this.quality.geoMaxStep);
      else this.geoCam.toEdit(f, 4, this.quality.geoMaxStep);
    }
    this.mode = m;
    if (m === 'edit' && document.pointerLockElement) document.exitPointerLock();
    this.emit('mode', 'selection');
  }

  setViewMode(v: ViewMode) {
    if (v === this.viewMode) return;
    if (v === 'geo' && !this.world) {
      toast('Select a world object first (Properties → "Use as world", or the World menu).', 'error');
      this.emit('mode');
      return;
    }
    this.cancelTransform();
    this.viewMode = v;
    if (v === 'geo') this.initGeoCamera();
    this.emit('mode');
  }

  private initGeoCamera() {
    const f = worldField(this.ev);
    // stand where the 4D camera is (projected onto the surface), looking the same way
    this.geoCam.initFrom(f, this.fullCam.view(), 0);
    if (!this.geoCam.valid) { toast('Could not place the camera on the world surface', 'error'); return; }
    if (this.mode === 'edit') this.geoCam.toEdit(f, 4, this.quality.geoMaxStep * 0.5);
  }

  currentView(): View {
    if (this.viewMode === 'geo' && this.geoCam.valid) return this.geoCam.view(worldField(this.ev), this.quality.geoMaxStep * 0.5);
    return this.fullCam.view();
  }

  private tanHalf() { return Math.tan((this.fov * Math.PI) / 360); }

  /** Screen point -> ray (in current view). */
  screenRay(clientX: number, clientY: number) {
    const r = this.canvas.getBoundingClientRect();
    const sx = ((clientX - r.left) / r.width) * 2 - 1;
    const sy = 1 - ((clientY - r.top) / r.height) * 2;
    const v = this.currentView();
    return { view: v, dir: rayDir(v, sx * (r.width / r.height), sy, this.tanHalf()) };
  }

  pick(clientX: number, clientY: number) {
    const { view, dir } = this.screenRay(clientX, clientY);
    return castRay(this.ev, this.viewMode, view.eye, dir, { maxStep: this.quality.geoMaxStep, frame: view.frame });
  }

  focusSelection() {
    const n = this.selected();
    if (!n) return;
    if (this.viewMode === 'full') {
      const path = pathTo(this.root, n.id) ?? [];
      // approximate world position: sum of translations along the path
      let p: Vec4 = [0, 0, 0, 0];
      for (const m of path) p = addScaled(p, m.xf.pos, 1);
      this.fullCam.target = p;
      this.fullCam.distance = Math.max(2, 4 * n.xf.scale);
    } else if (n.anchor && n.anchor.world === this.world) {
      this.geoCam.pivot = [...n.xf.pos] as Vec4;
      this.geoCam.frame = n.anchor.basis.slice();
      this.geoCam.distance = Math.max(2, 4 * n.xf.scale);
    }
  }

  setStandardView(kind: 'front' | 'right' | 'top' | 'ana') {
    this.fullCam.reset4();
    if (kind === 'front') { this.fullCam.yaw = 0; this.fullCam.pitch = 0; }
    if (kind === 'right') { this.fullCam.yaw = -Math.PI / 2; this.fullCam.pitch = 0; }
    if (kind === 'top') { this.fullCam.yaw = 0; this.fullCam.pitch = -Math.PI / 2 + 0.001; }
    if (kind === 'ana') { this.fullCam.yaw = 0; this.fullCam.pitch = 0; this.fullCam.rotate4(2, 3, Math.PI / 2); }
  }

  // ------------------------------------------------------------ placement (drag & drop)
  canPlace(item: LibraryItem): { ok: boolean; reason?: string } {
    if (this.mode !== 'edit') return { ok: false, reason: 'Switch to Edit mode to place objects' };
    if (this.viewMode === 'geo') {
      const dim = this.itemDim(item);
      if (dim > 3) return { ok: false, reason: 'Geodesic view: only 3D (or lower) objects can be placed on the world surface' };
    }
    return { ok: true };
  }

  itemDim(item: LibraryItem): number {
    if (item.kind === 'prim') { const p = this.reg.get(item.id); return p ? primitiveDim(p) : 0; }
    const o = this.lib.objects.find((x) => x.id === item.id);
    return o ? nodeDim(o.node, this.reg, primitiveDim) : 0;
  }

  private instantiate(item: LibraryItem): SceneNode | null {
    if (item.kind === 'prim') {
      const p = this.reg.get(item.id);
      if (!p) return null;
      const hue = Math.random();
      const c = hsl(hue, 0.55, 0.6);
      return makeLeaf(p.id, p.name, [0, 0, 0, 0], c);
    }
    const o: LibraryObject | undefined = this.lib.objects.find((x) => x.id === item.id);
    if (!o) return null;
    for (const p of o.primitives) this.reg.set(p);
    const n = cloneNode(o.node);
    n.name = o.name;
    return n;
  }

  placeItem(item: LibraryItem, clientX?: number, clientY?: number) {
    const chk = this.canPlace(item);
    if (!chk.ok) { toast(chk.reason!, 'error'); return; }
    const node = this.instantiate(item);
    if (!node) return;
    const view = this.currentView();
    let hitPos: Vec4 | null = null;
    let dir = col(view.frame, 2);
    let frame = view.frame;
    if (clientX !== undefined && clientY !== undefined) {
      const r = this.screenRay(clientX, clientY);
      dir = r.dir;
      const hit = castRay(this.ev, this.viewMode, view.eye, r.dir, { maxStep: this.quality.geoMaxStep, frame: view.frame });
      if (hit && hit.t < 40) {
        hitPos = hit.pos;
        dir = hit.dir;
        if (hit.frame) frame = hit.frame;
      }
    }
    if (this.viewMode === 'geo') {
      const f = worldField(this.ev);
      // walk along the (geodesic) ray to the drop point, backing off from any hit
      const dist = hitPos ? Math.max(0.5, length(sub(hitPos, view.eye)) - 1) : this.geoCam.distance || 3;
      const local: [number, number, number] = [dot(dir, col(view.frame, 0)), dot(dir, col(view.frame, 1)), dot(dir, col(view.frame, 2))];
      const r = shoot(f, view.eye, view.frame, local, dist, this.quality.geoMaxStep * 0.5);
      // orient the object like the (transported) camera, so it appears upright
      const a = makeAnchor(this.ev, this.world!, r.pos, r.frame);
      node.xf.pos = a.pos;
      node.anchor = a.anchor;
      // thicker across the surface so the object still reads as solid in the Full 4D slice
      walk(node, (m) => { if (m.kind === 'leaf') m.thickness = Math.max(m.thickness, 0.4); });
      void frame;
    } else {
      if (hitPos) {
        const n = this.normalAt(hitPos);
        node.xf.pos = addScaled(hitPos, n, 1);
      } else {
        node.xf.pos = clientX !== undefined ? addScaled(view.eye, dir, this.fullCam.distance) : [...this.fullCam.target] as Vec4;
      }
    }
    // never place inside the world subtree or an anchored group: add to root
    this.root.children.push(node);
    this.selection = [node.id];
    this.commit('structure', 'selection');
  }

  private normalAt(p: Vec4): Vec4 {
    const e = 1e-3;
    const d0 = this.ev.eval(p, this.viewMode).d;
    const g: Vec4 = [0, 0, 0, 0];
    for (let i = 0; i < 4; i++) { const q = [...p] as Vec4; q[i] += e; g[i] = this.ev.eval(q, this.viewMode).d - d0; }
    return normalize(g);
  }

  /** Attach an existing node to the active world surface (projects it onto the surface). */
  attachToWorld(id: string) {
    const n = findNode(this.root, id);
    if (!n || !this.world || n.id === this.world || isAncestor(this.root, n.id, this.world)) return;
    if (nodeDim(n, this.reg, primitiveDim) > 3) { toast('Only 3D (or lower) objects can be attached to a world surface', 'error'); return; }
    const a = makeAnchor(this.ev, this.world, n.xf.pos, this.currentView().frame);
    n.xf.pos = a.pos;
    n.anchor = a.anchor;
    // move to root so it is not removed together with the world in the geodesic view
    if (findParent(this.root, id) !== this.root) { removeNode(this.root, id); this.root.children.push(n); }
    this.commit('structure', 'values');
  }
  detach(id: string) {
    const n = findNode(this.root, id);
    if (!n?.anchor) return;
    delete n.anchor;
    this.commit('structure', 'values');
  }

  // ------------------------------------------------------------ modal transforms
  startTransform(kind: ModalTransform['kind']) {
    if (this.mode !== 'edit' || !this.selection.length) return;
    const ids = this.selection.filter((id) => id !== this.root.id);
    if (!ids.length) return;
    const orig = new Map<string, { xf: Transform; anchor?: Anchor }>();
    for (const id of ids) {
      const n = findNode(this.root, id)!;
      orig.set(id, JSON.parse(JSON.stringify({ xf: n.xf, anchor: n.anchor })));
    }
    const x = this.mouse.x, y = this.mouse.y;
    this.transform = { kind, ids, orig, x0: x, y0: y, x, y, axes: [], view: this.currentView() };
    this.emit('values');
  }

  private applyTransform() {
    const t = this.transform!;
    const dx = t.x - t.x0, dy = t.y - t.y0;
    const r = this.canvas.getBoundingClientRect();
    const right = col(t.view.frame, 0), up = col(t.view.frame, 1);
    for (const id of t.ids) {
      const n = findNode(this.root, id);
      const o = t.orig.get(id);
      if (!n || !o) continue;
      n.xf = JSON.parse(JSON.stringify(o.xf));
      n.anchor = o.anchor ? JSON.parse(JSON.stringify(o.anchor)) : undefined;
      if (!n.anchor) delete n.anchor;
      const depth = Math.max(0.5, Math.abs(dot(sub(o.xf.pos, t.view.eye), col(t.view.frame, 2))));
      const k = (2 * depth * this.tanHalf()) / r.height;
      if (t.kind === 'grab') {
        let delta: Vec4 = addScaled(scaleV(right, dx * k), up, -dy * k);
        if (t.axes.length === 1) {
          const a = t.axes[0];
          const axis: Vec4 = n.anchor ? col(n.anchor.basis, a) : unit(a);
          const sa = [dot(axis, right), dot(axis, up)];
          const sl = Math.hypot(sa[0], sa[1]);
          const amt = sl > 0.15 ? ((dx * sa[0] - dy * sa[1]) / sl) * k / sl : dx * k;
          delta = scaleV(axis, amt);
        }
        if (n.anchor && n.anchor.world === this.world && this.world) {
          const f = worldField(this.ev);
          const b = o.anchor!.basis;
          const local: [number, number, number] = [dot(delta, col(b, 0)), dot(delta, col(b, 1)), dot(delta, col(b, 2))];
          const len = Math.hypot(...local);
          if (len > 1e-6) {
            const s = shoot(f, o.xf.pos, b, local, len, this.quality.geoMaxStep * 0.5);
            n.xf.pos = s.pos;
            n.anchor.basis = s.frame;
            n.anchor.reach = curvatureRadius(f, s.pos, s.frame);
          }
        } else {
          n.xf.pos = addScaled(o.xf.pos, delta, 1);
        }
      } else if (t.kind === 'rotate') {
        const plane = rotationPlane(t.axes, !!n.anchor);
        const deg = dx * 0.5;
        n.xf.rot[plane] = round1(o.xf.rot[plane] + deg);
      } else {
        n.xf.scale = Math.max(0.01, o.xf.scale * Math.exp(dx * 0.005));
      }
    }
    this.live();
  }

  confirmTransform() {
    if (!this.transform) return;
    this.transform = null;
    this.commit('values');
  }
  cancelTransform() {
    const t = this.transform;
    if (!t) return;
    for (const id of t.ids) {
      const n = findNode(this.root, id);
      const o = t.orig.get(id);
      if (n && o) { n.xf = o.xf; if (o.anchor) n.anchor = o.anchor; else delete n.anchor; }
    }
    this.transform = null;
    this.live();
    this.emit('values');
  }

  transformHint(): string {
    const t = this.transform;
    if (!t) return '';
    const ax = t.axes.map((a) => 'XYZW'[a]).join('');
    if (t.kind === 'rotate') return `Rotate in plane ${['XY', 'XZ', 'XW', 'YZ', 'YW', 'ZW'][rotationPlane(t.axes, false)]} — type X/Y/Z/W (one axis = rotate about it in 3D, two = that plane) · click/Enter confirm · Esc cancel`;
    if (t.kind === 'scale') return 'Scale — move mouse · click/Enter confirm · Esc cancel';
    return `Grab${ax ? ' along ' + ax : ''} — X/Y/Z/W constrain${this.viewMode === 'geo' ? ' (anchored objects slide along the world surface)' : ''} · click/Enter confirm · Esc cancel`;
  }

  // ------------------------------------------------------------ input
  mouse = { x: 0, y: 0 };

  attachInput() {
    const c = this.canvas;
    c.addEventListener('contextmenu', (e) => e.preventDefault());
    c.addEventListener('pointerdown', (e) => this.onPointerDown(e));
    window.addEventListener('pointermove', (e) => this.onPointerMove(e));
    window.addEventListener('pointerup', (e) => { if (this.drag && e.button === this.drag.button) { this.drag = null; } });
    c.addEventListener('wheel', (e) => {
      e.preventDefault();
      if (this.mode === 'explore') { this.moveSpeed = Math.min(30, Math.max(0.2, this.moveSpeed * Math.exp(-e.deltaY * 0.001))); return; }
      if (this.viewMode === 'geo') this.geoCam.zoom(e.deltaY); else this.fullCam.zoom(e.deltaY);
    }, { passive: false });
    window.addEventListener('keydown', (e) => this.onKey(e, true));
    window.addEventListener('keyup', (e) => this.onKey(e, false));
    window.addEventListener('blur', () => this.keys.clear());
    document.addEventListener('pointerlockchange', () => this.emit('mode'));
  }

  private onPointerDown(e: PointerEvent) {
    this.mouse = { x: e.clientX, y: e.clientY };
    if (this.mode === 'explore') {
      if (e.button === 0 && document.pointerLockElement !== this.canvas) this.canvas.requestPointerLock();
      return;
    }
    if (this.transform) {
      if (e.button === 0) this.confirmTransform();
      else this.cancelTransform();
      return;
    }
    if (e.button === 1 || e.button === 2 || (e.button === 0 && e.altKey)) {
      e.preventDefault();
      const kind = e.shiftKey ? 'pan' : e.ctrlKey ? 'zoom' : e.altKey && e.button !== 0 ? 'rot4' : 'orbit';
      this.drag = { button: e.button, x: e.clientX, y: e.clientY, kind };
      return;
    }
    if (e.button === 0) {
      const hit = this.pick(e.clientX, e.clientY);
      let id = hit?.id ?? null;
      if (id && !e.shiftKey && this.selection.length === 1 && this.selection[0] === id) {
        // clicking an already selected leaf selects its parent group (walk up the tree)
        const p = findParent(this.root, id);
        if (p && p.id !== this.root.id) id = p.id;
      } else if (id && !e.shiftKey && this.selection.length === 1) {
        // keep the selected ancestor group if the hit is inside it
        const sel = this.selection[0];
        if (sel !== this.root.id && isAncestor(this.root, sel, id) && sel !== id) id = this.selection[0];
      }
      if (e.shiftKey && id) this.select(this.selection.includes(id) ? this.selection.filter((s) => s !== id) : [...this.selection, id]);
      else this.select(id ? [id] : []);
    }
  }

  private onPointerMove(e: PointerEvent) {
    const dx = e.clientX - this.mouse.x, dy = e.clientY - this.mouse.y;
    this.mouse = { x: e.clientX, y: e.clientY };
    if (this.mode === 'explore' && document.pointerLockElement === this.canvas) {
      if (this.viewMode === 'geo') this.geoCam.look(e.movementX, e.movementY);
      else this.fullCam.look(e.movementX, e.movementY);
      return;
    }
    if (this.transform) {
      this.transform.x = e.clientX;
      this.transform.y = e.clientY;
      this.applyTransform();
      return;
    }
    if (!this.drag) return;
    const geo = this.viewMode === 'geo';
    const f = geo ? worldField(this.ev) : null;
    switch (this.drag.kind) {
      case 'orbit': geo ? this.geoCam.orbit(dx, dy) : this.fullCam.orbit(dx, dy); break;
      case 'pan': geo ? this.geoCam.pan(f!, dx, dy, this.quality.geoMaxStep * 0.5) : this.fullCam.pan(dx, dy); break;
      case 'zoom': geo ? this.geoCam.zoom(dy * 3) : this.fullCam.zoom(dy * 3); break;
      case 'rot4':
        if (geo) this.geoCam.roll(dx * 0.005);
        else { this.fullCam.rotate4(0, 3, dx * 0.006); this.fullCam.rotate4(1, 3, -dy * 0.006); }
        break;
    }
  }

  private onKey(e: KeyboardEvent, down: boolean) {
    const tag = (e.target as HTMLElement)?.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
    if (document.querySelector('.modal-backdrop')) return;
    const k = e.key.toLowerCase();
    if (!down) { this.keys.delete(e.code); return; }
    this.keys.add(e.code);
    const ctrl = e.ctrlKey || e.metaKey;
    if (ctrl && k === 'z') { e.preventDefault(); e.shiftKey ? this.redo() : this.undo(); return; }
    if (ctrl && k === 'y') { e.preventDefault(); this.redo(); return; }
    if (ctrl && k === 's') { e.preventDefault(); this.emit('library'); document.dispatchEvent(new CustomEvent('4dcad:save')); return; }
    if (ctrl && k === 'g') { e.preventDefault(); this.groupSelection('union'); return; }
    if (k === 'tab') { e.preventDefault(); this.setMode(this.mode === 'edit' ? 'explore' : 'edit'); return; }
    if (k === 'v' && !ctrl && this.mode === 'edit' && !this.transform) { this.setViewMode(this.viewMode === 'full' ? 'geo' : 'full'); return; }
    if (this.mode === 'explore') {
      if (k === 'r' && this.viewMode === 'full') this.fullCam.reset4();
      if (k === 'escape' && document.pointerLockElement) document.exitPointerLock();
      return;
    }
    // edit-mode shortcuts
    if (this.transform) {
      const ax = 'xyzw'.indexOf(k);
      if (ax >= 0 && k.length === 1) {
        const t = this.transform;
        if (t.kind === 'rotate') t.axes = t.axes.length >= 2 || t.axes.includes(ax) ? [ax] : [...t.axes, ax];
        else t.axes = t.axes[0] === ax ? [] : [ax];
        this.applyTransform();
        this.emit('values');
      } else if (k === 'enter') this.confirmTransform();
      else if (k === 'escape') this.cancelTransform();
      return;
    }
    if (k === 'g') this.startTransform('grab');
    else if (k === 'r') this.startTransform('rotate');
    else if (k === 's' && !ctrl) this.startTransform('scale');
    else if (k === 'delete' || k === 'x' || k === 'backspace') { if (this.selection.length) this.deleteSelection(); }
    else if (k === 'd' && e.shiftKey) this.duplicateSelection();
    else if (k === '.' || e.code === 'NumpadDecimal' || k === 'f') this.focusSelection();
    else if (k === 'escape' || (k === 'a' && e.altKey)) this.select([]);
    else if (k === 'a') this.select(this.root.children.map((c) => c.id));
    else if (e.code === 'Numpad1' || k === '1') this.setStandardView('front');
    else if (e.code === 'Numpad3' || k === '3') this.setStandardView('right');
    else if (e.code === 'Numpad7' || k === '7') this.setStandardView('top');
    else if (e.code === 'Numpad9' || k === '9') this.setStandardView('ana');
    else if (k === 'w' && e.shiftKey) this.fullCam.reset4();
  }

  private updateExplore(dt: number) {
    if (this.mode !== 'explore') return;
    const K = (c: string) => (this.keys.has(c) ? 1 : 0);
    const sp = this.moveSpeed * dt;
    const mr = K('KeyD') - K('KeyA'), mf = K('KeyW') - K('KeyS'), mu = K('Space') - (K('ShiftLeft') | K('ShiftRight'));
    const ma = K('KeyE') - K('KeyQ');
    const rot = 1.2 * dt;
    if (this.viewMode === 'geo' && this.geoCam.valid) {
      const f = worldField(this.ev);
      if (ma) this.geoCam.roll(ma * rot);
      if (mr || mf || mu) {
        this.geoCam.move(f, [mr * sp, mu * sp, mf * sp], this.quality.geoMaxStep * 0.5,
          this.collide ? (to) => this.ev.evalGeo(to).d < 0.08 : undefined);
      }
    } else {
      if (K('KeyZ') - K('KeyX')) this.fullCam.rotate4AroundEye(2, 3, (K('KeyZ') - K('KeyX')) * rot);
      if (K('KeyC') - K('KeyV')) this.fullCam.rotate4AroundEye(0, 3, (K('KeyC') - K('KeyV')) * rot);
      if (mr || mf || mu || ma) {
        this.fullCam.move([mr * sp, mu * sp, mf * sp, ma * sp],
          this.collide ? (_from, to) => this.ev.evalFull(to).d < 0.08 : undefined);
      }
    }
  }

  // ------------------------------------------------------------ frame loop
  start(onFrame: () => void) {
    const loop = () => {
      const now = performance.now();
      const dt = Math.min(0.1, (now - this.lastFrame) / 1000);
      this.lastFrame = now;
      this.fps = this.fps * 0.9 + (dt > 0 ? 1 / dt : 0) * 0.1;
      this.updateExplore(dt);
      const view = this.currentView();
      // only re-render when something visible changed (ray marching is expensive)
      const key = JSON.stringify([view, this.version, this.selection, this.mode, this.viewMode, this.quality, this.fov,
        this.canvas.clientWidth, this.canvas.clientHeight]);
      if (key !== this.lastKey) {
        this.lastKey = key;
        const progs = buildPrograms(this.root, this.reg, this.world, this.highlightSet());
        if (progs.errors.join() !== this.lastErrors) { this.lastErrors = progs.errors.join(); if (progs.errors.length) toast(progs.errors.join('; '), 'error'); }
        this.renderer.setScene(progs);
        this.renderer.render({
          view, mode: this.viewMode, grid: this.mode === 'edit', fovDeg: this.fov, quality: this.quality, light: this.light,
        });
      }
      onFrame();
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
  }
}

// ------------------------------------------------------------ small helpers
const unit = (a: number): Vec4 => { const v: Vec4 = [0, 0, 0, 0]; v[a] = 1; return v; };
const scaleV = (v: Vec4, s: number): Vec4 => [v[0] * s, v[1] * s, v[2] * s, v[3] * s];
const round1 = (x: number) => Math.round(x * 10) / 10;

/** Plane index for R + typed axes. One axis: rotate "about" it within XYZ (Blender convention). */
export function rotationPlane(axes: number[], anchored: boolean): number {
  const find = (a: number, b: number) => PLANES.findIndex(([i, j]) => (i === Math.min(a, b) && j === Math.max(a, b)));
  if (axes.length >= 2) return find(axes[0], axes[1]);
  if (axes.length === 1) {
    const a = axes[0];
    if (a === 0) return find(1, 2);
    if (a === 1) return find(0, 2);
    if (a === 2) return find(0, 1);
    return find(0, 3);
  }
  return anchored ? find(0, 2) : find(0, 2);
}

function hsl(h: number, s: number, l: number): [number, number, number] {
  const f = (n: number) => {
    const k = (n + h * 12) % 12;
    return l - s * Math.min(l, 1 - l) * Math.max(-1, Math.min(k - 3, 9 - k, 1));
  };
  return [f(0), f(8), f(4)];
}
