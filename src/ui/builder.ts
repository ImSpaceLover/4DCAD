// Primitive builder: start from a point and raise the dimension with
// Extrude / Revolve steps (plus optional Rotate steps), with a live preview.

import { primitiveDim, validatePrimitive, type PrimitiveDef, type Step } from '../core/primitives.ts';
import { buildPrograms } from '../core/program.ts';
import { makeGroup, makeLeaf, newId, PrimitiveRegistry } from '../core/scene.ts';
import { FullCamera } from '../core/camera.ts';
import { DEFAULT_QUALITY } from '../core/scene.ts';
import { Renderer } from '../render/renderer.ts';
import { h, numberField, select, toast } from './dom.ts';
import type { App } from './app.ts';

const AX = 'XYZW';

export function openBuilder(app: App, initial?: PrimitiveDef) {
  const def: PrimitiveDef = initial
    ? JSON.parse(JSON.stringify(initial))
    : { id: '', name: 'My primitive', steps: [{ op: 'extrude', mode: 'finite', length: 2 }], round: 0, shell: 0 };
  const editing = !!initial?.id && !initial.builtin;

  const stepsEl = h('div', { class: 'steps' });
  const info = h('div', { class: 'hint' });
  const canvas = h('canvas', { class: 'preview' });
  const nameIn = h('input', { type: 'text', value: def.name });
  nameIn.addEventListener('input', () => { def.name = nameIn.value; });

  const reg = new PrimitiveRegistry();
  const cam = new FullCamera();
  cam.target = [0, 0, 0, 0];
  cam.distance = 6;
  let wSlice = 0;
  let renderer: Renderer | null = null;
  try { renderer = new Renderer(canvas); } catch { /* preview unavailable */ }

  const refresh = () => {
    stepsEl.replaceChildren(h('div', { class: 'step origin' }, h('b', {}, 'Point'), h('span', { class: 'hint' }, '0D start')));
    let dim = 0;
    def.steps.forEach((s, i) => {
      const before = dim;
      if (s.op !== 'rotate') dim++;
      stepsEl.append(stepRow(s, i, before));
    });
    const errs = validatePrimitive(def);
    info.textContent = errs.length ? '⚠ ' + errs.join(' · ') : `Result: ${dim}D primitive${dim < 4 ? ` (thin along ${AX.slice(dim)} when placed in 4D)` : ''}`;
    info.classList.toggle('warn', errs.length > 0);
    updatePreview();
  };

  const stepRow = (s: Step, i: number, dimBefore: number) => {
    const del = h('button', { title: 'Remove step', onclick: () => { def.steps.splice(i, 1); refresh(); } }, '✕');
    const up = h('button', { title: 'Move up', disabled: i === 0, onclick: () => { [def.steps[i - 1], def.steps[i]] = [def.steps[i], def.steps[i - 1]]; refresh(); } }, '↑');
    const ctrls: HTMLElement[] = [];
    if (s.op === 'extrude') {
      ctrls.push(h('b', {}, `Extrude → ${AX[dimBefore] ?? '?'}`));
      ctrls.push(select([{ value: 'finite', label: 'finite' }, { value: 'infinite', label: 'infinite' }, { value: 'half', label: 'half-space (≤ 0)' }], s.mode, (v) => { s.mode = v; refresh(); }));
      if (s.mode === 'finite') ctrls.push(numberField('Length', s.length, (v, f) => { s.length = Math.max(0.001, v); f ? refresh() : updatePreview(); }, { step: 0.1, min: 0.001 }));
    } else if (s.op === 'revolve') {
      ctrls.push(h('b', {}, `Revolve → ${AX[dimBefore] ?? '?'}`));
      const axes = Array.from({ length: dimBefore }, (_, a) => ({ value: String(a), label: `spin ${AX[a]} into ${AX[dimBefore]}` }));
      if (axes.length) ctrls.push(select(axes, String(s.axis ?? dimBefore - 1), (v) => { s.axis = Number(v); refresh(); }));
      ctrls.push(numberField('Radius', s.radius, (v, f) => { s.radius = Math.max(0, v); f ? refresh() : updatePreview(); }, { step: 0.1, min: 0, title: 'Radius of revolution (distance of the profile from the axis; 0 = around the centre)' }));
    } else {
      ctrls.push(h('b', {}, 'Rotate'));
      const planes: { value: string; label: string }[] = [];
      for (let a = 0; a < dimBefore; a++) for (let b = a + 1; b < dimBefore; b++) planes.push({ value: `${a}${b}`, label: AX[a] + AX[b] });
      if (planes.length) ctrls.push(select(planes, `${s.i}${s.j}`, (v) => { s.i = Number(v[0]); s.j = Number(v[1]); refresh(); }));
      ctrls.push(numberField('Angle°', s.angle, (v, f) => { s.angle = v; f ? refresh() : updatePreview(); }, { step: 5 }));
    }
    return h('div', { class: 'step' }, ...ctrls, h('span', { class: 'grow' }), up, del);
  };

  const curDim = () => primitiveDim(def);
  const addBtns = h('div', { class: 'btns' },
    h('button', { onclick: () => { if (curDim() >= 4) return toast('Already 4D', 'error'); def.steps.push({ op: 'extrude', mode: 'finite', length: 2 }); refresh(); } }, '+ Extrude'),
    h('button', { onclick: () => { if (curDim() >= 4) return toast('Already 4D', 'error'); if (curDim() < 1) return toast('Revolve needs at least a 1D profile', 'error'); def.steps.push({ op: 'revolve', radius: 0 }); refresh(); } }, '+ Revolve'),
    h('button', { title: 'Rotate the current profile within its own space (useful before a Revolve)', onclick: () => { if (curDim() < 2) return toast('Rotate needs at least 2 dimensions', 'error'); def.steps.push({ op: 'rotate', i: 0, j: 1, angle: 45 }); refresh(); } }, '+ Rotate'));

  const finish = numberField('Rounding', def.round ?? 0, (v, f) => { def.round = v; if (f) updatePreview(); }, { step: 0.01 });
  const shell = numberField('Shell', def.shell ?? 0, (v, f) => { def.shell = Math.max(0, v); if (f) updatePreview(); }, { step: 0.01, min: 0, title: 'Hollow shell thickness (0 = solid)' });
  const wSlider = h('input', { type: 'range', min: '-2', max: '2', step: '0.01', value: '0' });
  wSlider.addEventListener('input', () => { wSlice = parseFloat(wSlider.value); });
  const rotSlider = h('input', { type: 'range', min: '-180', max: '180', step: '1', value: '0' });
  rotSlider.addEventListener('input', () => { cam.reset4(); cam.rotate4(2, 3, (parseFloat(rotSlider.value) * Math.PI) / 180); });

  // preview orbit
  canvas.addEventListener('pointerdown', (e) => {
    canvas.setPointerCapture(e.pointerId);
    let x = e.clientX, y = e.clientY;
    const mv = (ev: PointerEvent) => { cam.orbit(ev.clientX - x, ev.clientY - y); x = ev.clientX; y = ev.clientY; };
    const upf = () => { canvas.removeEventListener('pointermove', mv); canvas.removeEventListener('pointerup', upf); };
    canvas.addEventListener('pointermove', mv);
    canvas.addEventListener('pointerup', upf);
  });
  canvas.addEventListener('wheel', (e) => { e.preventDefault(); cam.zoom(e.deltaY); }, { passive: false });

  const leaf = makeLeaf('__draft', 'draft', [0, 0, 0, 0], [0.9, 0.6, 0.3]);
  const root = makeGroup('union', [leaf]);
  let previewOk = true;
  function updatePreview() {
    if (!renderer) return;
    if (validatePrimitive(def).length) { previewOk = false; return; }
    reg.set({ ...def, id: '__draft' });
    renderer.setScene(buildPrograms(root, reg, null));
    previewOk = true;
  }

  let alive = true;
  const loop = () => {
    if (!alive) return;
    if (renderer && previewOk) {
      const v = cam.view();
      const eye = [...v.eye] as typeof v.eye;
      eye[3] += wSlice;
      renderer.render({ view: { eye, frame: v.frame }, mode: 'full', grid: true, fovDeg: 50, quality: { ...DEFAULT_QUALITY, resolutionScale: 1 }, light: [0.5, 0.8, -0.4, 0.2] });
    }
    requestAnimationFrame(loop);
  };

  const save = (place: boolean) => {
    const errs = validatePrimitive(def);
    if (errs.length) { toast(errs.join('; '), 'error'); return; }
    const out: PrimitiveDef = { ...def, id: editing ? def.id : newId('p'), name: def.name.trim() || 'Primitive', builtin: false,
      description: def.description ?? `${primitiveDim(def)}D custom primitive` };
    app.lib.savePrimitive(out);
    close();
    toast(`Saved primitive "${out.name}"`);
    if (place) app.placeItem({ kind: 'prim', id: out.id });
    else app.commit('structure', 'values');
  };

  const backdrop = h('div', { class: 'modal-backdrop' });
  const close = () => { alive = false; backdrop.remove(); };
  const modal = h('div', { class: 'modal builder' },
    h('div', { class: 'modal-head' }, h('b', {}, editing ? 'Edit primitive' : 'New primitive'), h('button', { onclick: close }, '✕')),
    h('div', { class: 'builder-body' },
      h('div', { class: 'builder-left' },
        h('div', { class: 'field' }, h('label', {}, 'Name'), nameIn),
        h('div', { class: 'hint' }, 'Each Extrude / Revolve adds the next axis (X → Y → Z → W).'),
        stepsEl, addBtns, finish, shell, info),
      h('div', { class: 'builder-right' }, canvas,
        h('div', { class: 'field' }, h('label', {}, 'W slice'), wSlider),
        h('div', { class: 'field' }, h('label', {}, 'ZW turn'), rotSlider),
        h('div', { class: 'hint' }, 'Drag to orbit, wheel to zoom. 4D shapes are shown as the 3D slice at the chosen W.'))),
    h('div', { class: 'modal-foot' },
      h('button', { onclick: close }, 'Cancel'),
      h('button', { onclick: () => save(false) }, 'Save to library'),
      h('button', { class: 'primary', onclick: () => save(true) }, 'Save & place')));
  backdrop.append(modal);
  backdrop.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); });
  document.body.append(backdrop);
  refresh();
  requestAnimationFrame(loop);
}
