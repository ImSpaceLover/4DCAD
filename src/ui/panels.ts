// Side panels: library (drag source), scene tree (drag & drop), properties.

import { BOOL_OPS, findNode, findParent, makeLibraryObject, nodeDim, type BoolOp, type GroupNode, type LeafNode, type Modifier, type SceneNode } from '../core/scene.ts';
import { PLANE_NAMES } from '../core/math4.ts';
import { primitiveDim, type PrimitiveDef } from '../core/primitives.ts';
import { downloadText, h, hexToRgb, numberField, openTextFile, rgbToHex, select, toast } from './dom.ts';
import type { App, LibraryItem } from './app.ts';
import { primitiveFile } from './library.ts';
import { openBuilder } from './builder.ts';

export const OP_LABEL: Record<BoolOp, string> = {
  union: 'Union', intersection: 'Intersection', subtraction: 'Subtraction',
  smoothUnion: 'Smooth union', smoothIntersection: 'Smooth intersection', smoothSubtraction: 'Smooth subtraction',
};
const OP_ICON: Record<BoolOp, string> = { union: '∪', intersection: '∩', subtraction: '−', smoothUnion: '⊎', smoothIntersection: '⋒', smoothSubtraction: '⊖' };
const DIM_LABEL = ['0D', '1D', '2D', '3D', '4D'];
const DND_MIME = 'application/x-4dcad';

// ================================================================ library
export function libraryPanel(app: App, el: HTMLElement) {
  const render = () => {
    el.replaceChildren();
    el.append(h('div', { class: 'panel-head' }, h('span', {}, 'Library'),
      h('div', { class: 'btns' },
        h('button', { title: 'Create a primitive by extruding / revolving', onclick: () => openBuilder(app) }, '+ Primitive'),
        h('button', { title: 'Import primitive / object / library JSON', onclick: importLib }, 'Import'),
        h('button', { title: 'Export the whole user library', onclick: () => downloadText('4dcad-library.json', JSON.stringify(app.lib.bundle(), null, 2)) }, 'Export'))));
    el.append(h('div', { class: 'hint' }, app.viewMode === 'geo' ? 'Geodesic view: drag 3D items onto the world surface.' : 'Drag items into the viewport (or double-click).'));
    const prims = app.reg.all();
    for (let d = 0; d <= 4; d++) {
      const list = prims.filter((p) => primitiveDim(p) === d);
      if (!list.length) continue;
      el.append(h('div', { class: 'lib-section' }, `${DIM_LABEL[d]} primitives`));
      el.append(h('div', { class: 'lib-grid' }, ...list.map((p) => primItem(p))));
    }
    if (app.lib.objects.length) {
      el.append(h('div', { class: 'lib-section' }, 'Saved objects'));
      el.append(h('div', { class: 'lib-grid' }, ...app.lib.objects.map((o) => {
        const item: LibraryItem = { kind: 'object', id: o.id };
        return libItem(item, o.name, `${DIM_LABEL[nodeDim(o.node, app.reg, primitiveDim)]} object`, [
          h('button', { title: 'Export object', onclick: () => downloadText(`${slug(o.name)}.4dobj.json`, JSON.stringify(o, null, 2)) }, '⤓'),
          h('button', { title: 'Delete', onclick: () => { if (confirm(`Delete object "${o.name}"?`)) app.lib.deleteObject(o.id); } }, '✕'),
        ]);
      })));
    }
  };

  const primItem = (p: PrimitiveDef) => libItem({ kind: 'prim', id: p.id }, p.name, p.description ?? DIM_LABEL[primitiveDim(p)],
    p.builtin
      ? [h('button', { title: 'Copy into the builder', onclick: () => openBuilder(app, { ...p, id: '', name: p.name + ' copy', builtin: false }) }, '✎')]
      : [
        h('button', { title: 'Edit', onclick: () => openBuilder(app, p) }, '✎'),
        h('button', { title: 'Export primitive', onclick: () => downloadText(`${slug(p.name)}.4dprim.json`, JSON.stringify(primitiveFile(p), null, 2)) }, '⤓'),
        h('button', { title: 'Delete', onclick: () => { if (confirm(`Delete primitive "${p.name}"?`)) app.lib.deletePrimitive(p.id); } }, '✕'),
      ]);

  const libItem = (item: LibraryItem, name: string, desc: string, actions: HTMLElement[]) => {
    const allowed = app.canPlace(item).ok || app.mode !== 'edit';
    const dim = app.itemDim(item);
    const div = h('div', { class: `lib-item ${allowed ? '' : 'disabled'}`, draggable: allowed ? 'true' : 'false', title: desc },
      h('span', { class: `dim d${dim}` }, DIM_LABEL[dim]),
      h('span', { class: 'name' }, name),
      h('span', { class: 'actions' }, ...actions));
    div.addEventListener('dragstart', (e) => {
      e.dataTransfer!.setData(DND_MIME, JSON.stringify(item));
      e.dataTransfer!.effectAllowed = 'copy';
    });
    div.addEventListener('dblclick', () => app.placeItem(item));
    return div;
  };

  const importLib = async () => {
    const f = await openTextFile();
    if (!f) return;
    try { toast('Imported ' + app.lib.importText(f.text)); } catch (e) { toast(String(e), 'error'); }
  };

  app.on('library', render);
  app.on('mode', render);
  render();

  // viewport drop target
  app.canvas.addEventListener('dragover', (e) => {
    if (e.dataTransfer?.types.includes(DND_MIME)) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; }
  });
  app.canvas.addEventListener('drop', (e) => {
    const raw = e.dataTransfer?.getData(DND_MIME);
    if (!raw) return;
    e.preventDefault();
    app.placeItem(JSON.parse(raw), e.clientX, e.clientY);
  });
}

// ================================================================ tree
export function treePanel(app: App, el: HTMLElement) {
  const TREE_MIME = 'application/x-4dcad-node';
  const opSel = select(BOOL_OPS.map((o) => ({ value: o, label: OP_LABEL[o] })), 'union', () => {}, { title: 'Operator for grouping' });
  const head = h('div', { class: 'panel-head' }, h('span', {}, 'Scene'),
    h('div', { class: 'btns' },
      opSel,
      h('button', { title: 'Group selection with the chosen operator (Ctrl+G = union)', onclick: () => app.groupSelection(opSel.value as BoolOp) }, 'Group'),
      h('button', { title: 'Duplicate (Shift+D)', onclick: () => app.duplicateSelection() }, 'Dup'),
      h('button', { title: 'Delete (X / Del)', onclick: () => app.deleteSelection() }, 'Del'),
      h('button', { title: 'Save selected subtree to the library as a reusable object', onclick: saveObject }, 'Save obj'),
      h('button', { title: 'Export selected subtree as an object file', onclick: exportObject }, 'Export obj')));
  const list = h('div', { class: 'tree' });
  el.append(head, list);

  function saveObject() {
    const n = app.selected();
    if (!n) { toast('Select something to save', 'error'); return; }
    const name = prompt('Object name', n.name);
    if (!name) return;
    app.lib.saveObject(makeLibraryObject(n, app.reg, name));
    toast(`Saved "${name}" to the library`);
  }
  function exportObject() {
    const n = app.selected();
    if (!n) { toast('Select something to export', 'error'); return; }
    const o = makeLibraryObject(n, app.reg, n.name);
    downloadText(`${slug(n.name)}.4dobj.json`, JSON.stringify(o, null, 2));
  }

  const row = (n: SceneNode, depth: number): HTMLElement[] => {
    const isRoot = n.id === app.root.id;
    const sel = app.selection.includes(n.id);
    const icon = n.kind === 'group' ? OP_ICON[n.op] : DIM_LABEL[primitiveDim(app.reg.get(n.prim) ?? { steps: [] } as any)];
    const r = h('div', { class: `tree-row ${sel ? 'sel' : ''} ${n.hidden ? 'hidden' : ''}`, style: { paddingLeft: `${6 + depth * 14}px` }, draggable: isRoot ? 'false' : 'true' },
      h('span', { class: `ticon ${n.kind}` }, icon),
      h('span', { class: 'tname' }, n.name),
      n.id === app.world ? h('span', { class: 'badge world', title: 'World surface for the Geodesic view' }, 'world') : null,
      n.anchor ? h('span', { class: 'badge anchor', title: 'Attached to a world surface' }, '⚓') : null,
      n.modifiers.length ? h('span', { class: 'badge', title: 'Has modifiers' }, '⁂') : null,
      !isRoot ? h('button', { class: 'eye', title: 'Toggle visibility', onclick: (e: Event) => { e.stopPropagation(); n.hidden = !n.hidden; app.commit('structure'); } }, n.hidden ? '◌' : '●') : null);
    r.addEventListener('click', (e) => {
      if (e.shiftKey) app.select(app.selection.includes(n.id) ? app.selection.filter((s) => s !== n.id) : [...app.selection, n.id]);
      else app.select([n.id]);
    });
    r.addEventListener('dragstart', (e) => { e.dataTransfer!.setData(TREE_MIME, n.id); e.dataTransfer!.effectAllowed = 'move'; });
    r.addEventListener('dragover', (e) => {
      if (!e.dataTransfer?.types.includes(TREE_MIME) && !e.dataTransfer?.types.includes(DND_MIME)) return;
      e.preventDefault();
      const zone = dropZone(e, r, n);
      r.dataset.drop = zone;
    });
    r.addEventListener('dragleave', () => { delete r.dataset.drop; });
    r.addEventListener('drop', (e) => {
      e.preventDefault();
      const zone = dropZone(e, r, n);
      delete r.dataset.drop;
      const id = e.dataTransfer!.getData(TREE_MIME);
      if (id) app.reparent(id, n.id, zone);
      const lib = e.dataTransfer!.getData(DND_MIME);
      if (lib) {
        // library item dropped on the tree: instantiate at the camera target then move under this node
        const before = new Set(app.root.children.map((c) => c.id));
        app.placeItem(JSON.parse(lib));
        const added = app.root.children.find((c) => !before.has(c.id));
        if (added) app.reparent(added.id, n.id, zone);
      }
    });
    const rows: HTMLElement[] = [r];
    if (n.kind === 'group') for (const c of n.children) rows.push(...row(c, depth + 1));
    return rows;
  };

  const dropZone = (e: DragEvent, r: HTMLElement, n: SceneNode): 'into' | 'before' | 'after' | 'wrap' => {
    const b = r.getBoundingClientRect();
    const y = (e.clientY - b.top) / b.height;
    if (n.id === app.root.id) return 'into';
    if (y < 0.25) return 'before';
    if (y > 0.75) return 'after';
    return n.kind === 'group' ? 'into' : 'wrap';
  };

  const render = () => list.replaceChildren(...row(app.root, 0));
  app.on('structure', render);
  app.on('selection', render);
  render();
}

// ================================================================ properties
export function propertiesPanel(app: App, el: HTMLElement) {
  const render = () => {
    el.replaceChildren(h('div', { class: 'panel-head' }, h('span', {}, 'Properties')));
    const n = app.selected();
    if (!n) {
      el.append(h('div', { class: 'hint' }, 'Nothing selected. Click an object in the viewport or the scene tree.'));
      return;
    }
    if (app.transform) el.append(h('div', { class: 'hint warn' }, app.transformHint()));
    const live = () => app.live();
    const done = () => app.commit('values', 'structure');
    const num = (label: string, get: () => number, set: (v: number) => void, opts: Parameters<typeof numberField>[3] = {}) =>
      numberField(label, get(), (v, final) => { set(v); final ? done() : live(); }, opts);

    const nameIn = h('input', { type: 'text', value: n.name });
    nameIn.addEventListener('change', () => { n.name = nameIn.value || n.name; app.commit('structure'); });
    el.append(section('Object', h('div', { class: 'field' }, h('label', {}, 'Name'), nameIn)));

    const isRoot = n.id === app.root.id;
    if (n.kind === 'leaf') leafProps(n);
    else groupProps(n);

    if (!isRoot) {
      const anchoredHere = !!n.anchor;
      const tr = section(anchoredHere ? 'Transform (tangent space of the world)' : 'Transform',
        h('div', { class: 'row4' }, ...[0, 1, 2, 3].map((i) => num('XYZW'[i], () => n.xf.pos[i], (v) => { n.xf.pos[i] = v; }, { step: 0.1 }))),
        h('div', { class: 'row3' }, ...[0, 1, 2, 3, 4, 5].filter((i) => !anchoredHere || [0, 1, 3].includes(i)).map((i) =>
          num(PLANE_NAMES[i] + '°', () => n.xf.rot[i], (v) => { n.xf.rot[i] = v; }, { step: 1, title: `Rotation in the ${PLANE_NAMES[i]} plane` }))),
        num('Scale', () => n.xf.scale, (v) => { n.xf.scale = Math.max(0.01, v); }, { step: 0.05, min: 0.01 }));
      if (anchoredHere) tr.append(h('div', { class: 'hint' }, 'Position is the anchor point on the world surface. Move with G to slide along the surface.'));
      el.append(tr);
    }

    // modifiers
    const mods = section('Modifiers');
    n.modifiers.forEach((m, idx) => mods.append(modifierRow(n, m, idx)));
    mods.append(h('div', { class: 'btns' },
      h('button', { onclick: () => { n.modifiers.push({ type: 'mirror', axis: 0 }); app.commit('structure', 'values'); } }, '+ Mirror'),
      h('button', { onclick: () => { n.modifiers.push({ type: 'repeat', axis: 0, spacing: 3, count: 2 }); app.commit('structure', 'values'); } }, '+ Repeat')));
    el.append(mods);

    // world / anchoring
    const ws = section('World surface');
    const dim = nodeDim(n, app.reg, primitiveDim);
    if (n.id === app.world) ws.append(h('div', { class: 'hint' }, 'This object is the world: its 3D boundary is the space of the Geodesic view.'),
      h('button', { onclick: () => app.setWorld(null) }, 'Stop using as world'));
    else if (!isRoot && dim === 4 && !n.anchor) ws.append(h('button', { onclick: () => app.setWorld(n.id) }, 'Use as world'));
    if (n.anchor) {
      const w = findNode(app.root, n.anchor.world);
      ws.append(h('div', { class: 'hint' }, `Attached to "${w?.name ?? 'missing world'}" (tangent-space reach ${n.anchor.reach >= 1e4 ? '∞' : n.anchor.reach.toFixed(2)}).`),
        h('button', { onclick: () => app.detach(n.id) }, 'Detach from surface'));
    } else if (app.world && n.id !== app.world && !isRoot && dim <= 3) {
      ws.append(h('button', { onclick: () => app.attachToWorld(n.id) }, 'Attach to world surface'));
    }
    if (ws.childElementCount > 1) el.append(ws);
  };

  const leafProps = (n: LeafNode) => {
    const prims = app.reg.all();
    const s = section('Primitive',
      h('div', { class: 'field' }, h('label', {}, 'Type'),
        select(prims.map((p) => ({ value: p.id, label: `${p.name} (${primitiveDim(p)}D)` })), n.prim, (v) => { n.prim = v; app.commit('structure', 'values'); })));
    const color = h('input', { type: 'color', value: rgbToHex(n.color) });
    color.addEventListener('input', () => { n.color = hexToRgb(color.value); app.live(); });
    color.addEventListener('change', () => app.commit('values'));
    s.append(h('div', { class: 'field' }, h('label', {}, 'Color'), color));
    const def = app.reg.get(n.prim);
    const dim = def ? primitiveDim(def) : 4;
    if (dim < 4) s.append(numberField('Thickness', n.thickness, (v, f) => { n.thickness = Math.max(0.001, v); f ? app.commit('values') : app.live(); },
      { step: 0.01, min: 0.001, title: `Extent along the ${4 - dim} unused axis/axes${n.anchor ? ' (here: across the world surface)' : ''}` }));
    s.append(numberField('Rounding', n.round ?? def?.round ?? 0, (v, f) => { n.round = v; if (f) app.commit('structure', 'values'); }, { step: 0.01, title: 'Rounding radius (overrides the primitive)' }));
    s.append(numberField('Shell', n.shell ?? def?.shell ?? 0, (v, f) => { n.shell = Math.max(0, v); if (f) app.commit('structure', 'values'); }, { step: 0.01, min: 0, title: 'Hollow shell thickness (0 = solid)' }));
    el.append(s);
  };

  const groupProps = (n: GroupNode) => {
    const s = section('Boolean',
      h('div', { class: 'field' }, h('label', {}, 'Operator'),
        select(BOOL_OPS.map((o) => ({ value: o, label: OP_LABEL[o] })), n.op, (v) => { n.op = v; app.commit('structure', 'values'); })));
    if (n.op.startsWith('smooth')) s.append(numberField('Smoothness', n.k, (v, f) => { n.k = Math.max(0.001, v); f ? app.commit('values') : app.live(); }, { step: 0.02, min: 0.001 }));
    if (n.op.includes('ubtraction')) s.append(h('div', { class: 'hint' }, 'First child minus all following children.'));
    const parent = findParent(app.root, n.id);
    if (parent) s.append(h('button', { onclick: () => app.ungroup(n.id) }, 'Ungroup'));
    el.append(s);
  };

  const modifierRow = (n: SceneNode, m: Modifier, idx: number) => {
    const axis = select([0, 1, 2, 3].map((a) => ({ value: String(a), label: 'XYZW'[a] })), String(m.axis), (v) => { m.axis = Number(v); app.commit('structure'); });
    const del = h('button', { title: 'Remove', onclick: () => { n.modifiers.splice(idx, 1); app.commit('structure', 'values'); } }, '✕');
    if (m.type === 'mirror') return h('div', { class: 'modrow' }, h('b', {}, 'Mirror'), axis, del);
    return h('div', { class: 'modrow' }, h('b', {}, 'Repeat'), axis,
      numberField('Δ', m.spacing, (v, f) => { m.spacing = Math.max(0.01, v); if (f) app.commit('structure'); }, { step: 0.1, min: 0.01, title: 'Spacing' }),
      numberField('±n', m.count, (v, f) => { m.count = Math.max(0, Math.round(v)); if (f) app.commit('structure'); }, { step: 1, min: 0, title: 'Copies on each side (0 = infinite)' }),
      del);
  };

  app.on('selection', render);
  app.on('values', render);
  app.on('structure', () => { if (!el.contains(document.activeElement)) render(); });
  app.on('mode', render);
  render();
}

function section(title: string, ...children: (HTMLElement | null)[]) {
  return h('div', { class: 'section' }, h('div', { class: 'section-title' }, title), ...children);
}

export const slug = (s: string) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'item';
