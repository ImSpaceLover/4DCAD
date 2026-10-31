import './style.css';
import { App } from './ui/app.ts';
import { downloadText, h, openTextFile, select, toast } from './ui/dom.ts';
import { libraryPanel, propertiesPanel, slug, treePanel } from './ui/panels.ts';
import { Hud } from './ui/hud.ts';
import { EXAMPLES } from './core/examples.ts';
import { walk } from './core/scene.ts';
import { primitiveDim } from './core/primitives.ts';

const canvas = document.getElementById('view') as HTMLCanvasElement;
const hudCanvas = document.getElementById('hud') as HTMLCanvasElement;

let app: App;
try {
  app = new App(canvas);
} catch (e) {
  document.body.replaceChildren(h('div', { class: 'fatal' }, String(e)));
  throw e;
}

// initial scene: autosave or example
const saved = App.autosaved();
try {
  if (saved) app.loadSceneText(saved);
  else app.loadScene(EXAMPLES[0].make());
} catch {
  app.loadScene(EXAMPLES[0].make());
}

// ---------------------------------------------------------------- toolbar
const toolbar = document.getElementById('toolbar')!;
const modeBtns = {
  edit: h('button', { onclick: () => app.setMode('edit') }, 'Edit'),
  explore: h('button', { onclick: () => app.setMode('explore') }, 'Explore'),
};
const viewBtns = {
  full: h('button', { onclick: () => app.setViewMode('full') }, 'Full 4D'),
  geo: h('button', { onclick: () => app.setViewMode('geo') }, 'Geodesic 3D'),
};
const worldSlot = h('span', {});
const saveScene = () => {
  downloadText(`${slug(app.root.name || 'scene')}.4dscene.json`, JSON.stringify(app.serialize(), null, 2));
};
document.addEventListener('4dcad:save', saveScene);

const qualityPanel = h('div', { class: 'popover', hidden: true });
const renderQuality = () => {
  const q = app.quality;
  const row = (label: string, input: HTMLElement) => h('div', { class: 'field' }, h('label', {}, label), input);
  const range = (min: number, max: number, step: number, val: number, set: (v: number) => void) => {
    const r = h('input', { type: 'range', min: String(min), max: String(max), step: String(step), value: String(val) });
    const out = h('span', { class: 'val' }, String(val));
    r.addEventListener('input', () => { set(parseFloat(r.value)); out.textContent = r.value; });
    return h('span', { class: 'rangewrap' }, r, out);
  };
  const check = (val: boolean, set: (v: boolean) => void) => {
    const c = h('input', { type: 'checkbox', checked: val });
    c.addEventListener('change', () => set(c.checked));
    return c;
  };
  qualityPanel.replaceChildren(
    h('b', {}, 'Rendering quality'),
    row('Resolution', range(0.25, 1, 0.05, q.resolutionScale, (v) => { q.resolutionScale = v; })),
    row('Max steps', range(32, 512, 8, q.maxSteps, (v) => { q.maxSteps = v; })),
    row('Soft shadows', check(q.shadows, (v) => { q.shadows = v; })),
    row('Ambient occl.', check(q.ao, (v) => { q.ao = v; })),
    h('b', {}, 'Geodesic rays'),
    row('Max step', range(0.02, 1, 0.01, q.geoMaxStep, (v) => { q.geoMaxStep = v; })),
    row('Projections', range(1, 4, 1, q.geoProjectIters, (v) => { q.geoProjectIters = v; })),
    h('b', {}, 'Camera'),
    row('FOV', range(30, 110, 1, app.fov, (v) => { app.fov = v; })),
    row('Collisions', check(app.collide, (v) => { app.collide = v; })),
    h('div', { class: 'hint' }, 'Lower "Max step" = more accurate light paths on strongly curved worlds (slower).'),
  );
};

toolbar.append(
  h('span', { class: 'brand' }, '4DCAD'),
  h('button', { title: 'New empty scene', onclick: () => { if (confirm('Start a new empty scene?')) { const s = EXAMPLES[0].make(); s.root.children = s.root.children.filter((c) => c.id === 'ground'); app.loadScene(s); } } }, 'New'),
  h('button', { title: 'Open a scene file', onclick: async () => { const f = await openTextFile(); if (!f) return; try { app.loadSceneText(f.text); toast(`Opened ${f.name}`); } catch (e) { toast(String(e), 'error'); } } }, 'Open'),
  h('button', { title: 'Save scene to a file (Ctrl+S)', onclick: saveScene }, 'Save'),
  select([{ value: '', label: 'Examples…' }, ...EXAMPLES.map((e) => ({ value: e.id, label: e.name }))], '' as string, (v) => {
    const ex = EXAMPLES.find((e) => e.id === v);
    if (ex && confirm(`Load example "${ex.name}"? Unsaved changes are kept only in the autosave.`)) app.loadScene(ex.make());
  }),
  h('span', { class: 'sep' }),
  h('span', { class: 'seg' }, modeBtns.edit, modeBtns.explore),
  h('span', { class: 'seg' }, viewBtns.full, viewBtns.geo),
  worldSlot,
  h('span', { class: 'sep' }),
  h('button', { onclick: () => app.undo(), title: 'Undo (Ctrl+Z)' }, '↶'),
  h('button', { onclick: () => app.redo(), title: 'Redo (Ctrl+Shift+Z)' }, '↷'),
  h('button', { onclick: () => app.fullCam.reset4(), title: 'Reset 4D orientation of the camera (Shift+W)' }, 'Reset W'),
  h('span', { class: 'grow' }),
  h('span', { class: 'qwrap' }, h('button', { onclick: () => { qualityPanel.hidden = !qualityPanel.hidden; renderQuality(); } }, 'Quality ▾'), qualityPanel),
  h('button', { onclick: () => document.getElementById('help')!.toggleAttribute('hidden') }, 'Help'),
);

const refreshToolbar = () => {
  modeBtns.edit.classList.toggle('active', app.mode === 'edit');
  modeBtns.explore.classList.toggle('active', app.mode === 'explore');
  viewBtns.full.classList.toggle('active', app.viewMode === 'full');
  viewBtns.geo.classList.toggle('active', app.viewMode === 'geo');
  const options = [{ value: '', label: 'World: none' }];
  walk(app.root, (n) => {
    if (n.id === app.root.id || n.anchor) return;
    let dim = 0;
    walk(n, (m) => { if (m.kind === 'leaf') { const p = app.reg.get(m.prim); if (p) dim = Math.max(dim, primitiveDim(p)); } });
    if (dim === 4) options.push({ value: n.id, label: `World: ${n.name}` });
  });
  worldSlot.replaceChildren(select(options, app.world ?? '', (v) => app.setWorld(v || null), { title: 'The 4D object whose surface is the space of the Geodesic view' }));
  document.body.classList.toggle('explore', app.mode === 'explore');
};
app.on('mode', refreshToolbar);
app.on('structure', refreshToolbar);
refreshToolbar();

// ---------------------------------------------------------------- panels
libraryPanel(app, document.getElementById('library')!);
treePanel(app, document.getElementById('tree')!);
propertiesPanel(app, document.getElementById('props')!);

const hud = new Hud(hudCanvas, app);
app.attachInput();
app.start(() => hud.draw());

// exposed for debugging / automation
(window as any).app = app;
