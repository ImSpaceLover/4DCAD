import { test } from 'node:test';
import assert from 'node:assert/strict';
import { BUILTIN_PRIMITIVES, evalPrimitive, validatePrimitive } from '../src/core/primitives.ts';
import { SceneEvaluator } from '../src/core/evaluator.ts';
import { PrimitiveRegistry, makeGroup, makeLeaf, parseScene, serializeScene } from '../src/core/scene.ts';
import { buildPrograms, runScene } from '../src/core/program.ts';
import { planetScene, playgroundScene } from '../src/core/examples.ts';
import { worldField, castRay } from '../src/core/surface.ts';
import { projectToSurface, shoot } from '../src/core/geodesic.ts';
import { length, sub, type Vec4 } from '../src/core/math4.ts';

const P = (id: string) => BUILTIN_PRIMITIVES.find((p) => p.id === id)!;
const close = (a: number, b: number, eps = 1e-6) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b}`);

test('all built-ins are valid', () => {
  for (const p of BUILTIN_PRIMITIVES) assert.deepEqual(validatePrimitive(p), [], p.id);
});

test('extrusion chain gives exact box / tesseract distances', () => {
  close(evalPrimitive(P('segment'), [3, 0, 0, 0]), 2);
  close(evalPrimitive(P('cube'), [0, 0, 0, 0]), -1);
  close(evalPrimitive(P('cube'), [2, 2, 0, 0]), Math.SQRT2);
  close(evalPrimitive(P('tesseract'), [0, 0, 0, 3]), 2);
  close(evalPrimitive(P('tesseract'), [0.5, 0, 0, 0]), -0.5);
});

test('revolutions give spheres, tori and 4D variants', () => {
  close(evalPrimitive(P('sphere'), [0, 0, 2, 0]), 1);
  close(evalPrimitive(P('sphere'), [1, 1, 1, 0]), Math.sqrt(3) - 1);
  close(evalPrimitive(P('hypersphere'), [0, 0, 0, 3]), 2);
  close(evalPrimitive(P('hypersphere'), [1, 1, 1, 1]), 1);
  close(evalPrimitive(P('torus'), [1, 0, 0, 0]), -0.3);
  close(evalPrimitive(P('duocylinder'), [0, 0, 0, 0]), -1);
  close(evalPrimitive(P('duocylinder'), [2, 0, 0, 0]), 1);
  close(evalPrimitive(P('spheritorus'), [1.2, 0, 0, 0]), -0.4);
  close(evalPrimitive(P('plane'), [0, -2, 0, 0]), -2);
  close(evalPrimitive(P('plane'), [5, 3, 1, 0]), 3);
});

test('3D object is thin along W in 4D', () => {
  const reg = new PrimitiveRegistry();
  const s = makeLeaf('sphere', 's');
  const ev = new SceneEvaluator(makeGroup('union', [s]), reg, null);
  close(ev.evalFull([0, 0, 0, 0]).d, -0.05);
  close(ev.evalFull([0, 0, 0, 1.05]).d, 1, 1e-6);
  close(ev.evalFull([2, 0, 0, 0]).d, 1);
});

test('booleans and modifiers', () => {
  const reg = new PrimitiveRegistry();
  const a = makeLeaf('tesseract', 'a');
  const b = makeLeaf('hypersphere', 'b');
  b.xf.scale = 0.5;
  const ev = new SceneEvaluator(makeGroup('subtraction', [a, b]), reg, null);
  close(ev.evalFull([0, 0, 0, 0]).d, 0.5);
  const c = makeLeaf('hypersphere', 'c');
  c.modifiers.push({ type: 'repeat', axis: 0, spacing: 4, count: 0 });
  const ev2 = new SceneEvaluator(makeGroup('union', [c]), reg, null);
  close(ev2.evalFull([8, 0, 0, 0]).d, -1);
  const m = makeLeaf('hypersphere', 'm', [2, 0, 0, 0]);
  m.modifiers.push({ type: 'mirror', axis: 0 });
  const ev3 = new SceneEvaluator(makeGroup('union', [m]), reg, null);
  close(ev3.evalFull([2, 0, 0, 0]).d, -1);
});

test('scene round-trips through JSON and compiles', () => {
  const reg = new PrimitiveRegistry();
  for (const ex of [playgroundScene(), planetScene()]) {
    const f = serializeScene(ex.root, reg, ex.world);
    const back = parseScene(JSON.stringify(f));
    assert.equal(JSON.stringify(back.root), JSON.stringify(ex.root));
    const P = buildPrograms(back.root, reg, back.world);
    assert.deepEqual(P.errors, []);
  }
});

test('geodesic walk stays on the hypersphere and wraps around', () => {
  const ex = planetScene();
  const reg = new PrimitiveRegistry();
  const ev = new SceneEvaluator(ex.root, reg, ex.world);
  const f = worldField(ev);
  const start: Vec4 = [0, 3, 0, 0];
  const frame = [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0]; // right x, up z, fwd w, normal y
  const r = shoot(f, start, frame, [0, 0, 1], Math.PI * 3, 0.05); // half circumference
  close(length(r.pos), 3, 1e-3);
  assert.ok(length(sub(r.pos, [0, -3, 0, 0])) < 0.05, `ended at ${r.pos}`);
});

test('anchored objects: single image on a closed world, visible in geo mode', () => {
  const ex = planetScene();
  const reg = new PrimitiveRegistry();
  const ev = new SceneEvaluator(ex.root, reg, ex.world);
  // red cube anchored at (0,3,0,0): inside in geo mode, not duplicated at the antipode
  assert.ok(ev.evalGeo([0, 3, 0, 0]).d < 0);
  const red = ex.root.children[1];
  const ev2 = new SceneEvaluator(makeGroup('union', [ex.root.children[0], red]), reg, ex.world);
  assert.ok(ev2.evalGeo([0, -3, 0, 0]).d > 0.5);
  // hugging thin shell in full mode at the anchor
  assert.ok(ev2.evalFull([0, 3.02, 0, 0]).d < 0);
  // geodesic ray from the antipode toward the red cube hits it
  const hit = castRay(ev2, 'geo', [0, -3, 0, 0], [0, 0, 1, 0]);
  assert.ok(hit && hit.id === red.id, 'expected to hit the red cube around the planet');
});

test('bounding spheres never exceed the true distance', async () => {
  const { computeBounds } = await import('../src/core/bounds.ts');
  const reg = new PrimitiveRegistry();
  for (const ex of [playgroundScene(), planetScene()]) {
    const bounds = computeBounds(ex.root, reg);
    for (const child of ex.root.children) {
      const b = bounds.get(child.id)!;
      if (!Number.isFinite(b.r)) continue;
      const ev = new SceneEvaluator(makeGroup('union', [ex.root.children.find((c) => c.id === ex.world) ?? makeLeaf('point', 'x', [99, 99, 99, 99]), child].filter((c, i, a) => a.indexOf(c) === i)), reg, ex.world);
      let seed = 1;
      const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647) * 16 - 8;
      for (let k = 0; k < 400; k++) {
        const p: Vec4 = [rnd(), rnd(), rnd(), rnd()];
        const bd = length(sub(p, b.c)) - b.r;
        if (bd <= 0) continue;
        // compare against the child alone
        const only = new SceneEvaluator(makeGroup('union', [child]), reg, null);
        void ev;
        assert.ok(only.evalFull(p).d >= bd - 1e-6 || child.anchor, `${child.name}: bound ${bd} > dist ${only.evalFull(p).d}`);
      }
    }
  }
});

test('GPU bytecode (JS emulation) matches the reference evaluator', () => {
  const reg = new PrimitiveRegistry();
  for (const ex of [playgroundScene(), planetScene()]) {
    const P = buildPrograms(ex.root, reg, ex.world);
    const ev = new SceneEvaluator(ex.root, reg, ex.world);
    let seed = 7, checked = 0;
    const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647) * 10 - 5;
    for (let k = 0; k < 3000; k++) {
      const p0: Vec4 = [rnd(), rnd() * 0.6 + 1, rnd(), rnd() * 0.3];
      for (const mode of ['full', 'geo'] as const) {
        if (mode === 'geo' && !ex.world) continue;
        // geodesic rays live on the world surface: sample there
        const p = mode === 'geo' ? projectToSurface(worldField(ev), p0, 20) : p0;
        const ref = ev.eval(p, mode).d;
        const got = runScene(P, mode, p);
        if (Math.abs(ref) < 0.4) { checked++; assert.ok(Math.abs(ref - got) < 1e-6, `${mode} at ${p}: ref ${ref} got ${got}`); }
        else assert.ok(Math.sign(ref) === Math.sign(got) || Math.abs(got) < 0.5, `${mode} sign at ${p}: ref ${ref} got ${got}`);
      }
    }
    assert.ok(checked > 20, `only ${checked} near-surface samples`);
    // world distance
    if (ex.world) {
      const p: Vec4 = [1, 2, 0.5, 0.3];
      assert.ok(Math.abs(ev.worldDist(p) - runScene({ ...P, geo: P.worlds[0] }, 'geo', p)) < 1e-6);
    }
  }
});
