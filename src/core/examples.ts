// Built-in example scenes (also exported to /examples/*.json by `npm run examples`).

import { makeGroup, makeLeaf, type GroupNode, type LeafNode, type SceneFile } from './scene.ts';
import { addScaled, normalize, type Vec4 } from './math4.ts';
import { basisFromNormal } from './surface.ts';

function anchored(prim: string, name: string, center: Vec4, radius: number, dir: Vec4, color: [number, number, number], scale = 0.5): LeafNode {
  const n = normalize(dir);
  const l = makeLeaf(prim, name, addScaled(center, n, radius), color);
  l.anchor = { world: '', basis: basisFromNormal(n), reach: radius };
  l.xf.scale = scale;
  return l;
}

export function playgroundScene(): SceneFile {
  const ground = makeLeaf('hyperplane', 'Ground', [0, 0, 0, 0], [0.45, 0.46, 0.5]);
  ground.id = 'ground';

  const tess = makeLeaf('tesseract', 'Tesseract', [0, 1.2, 0, 0], [0.85, 0.35, 0.3]);
  tess.xf.rot = [0, 0, 30, 0, 20, 0];
  const glome = makeLeaf('hypersphere', 'Hypersphere', [0, 1.2, 0, 0], [0.95, 0.75, 0.3]);
  glome.xf.scale = 1.25;
  const carved = makeGroup('subtraction', [tess, glome], 'Carved tesseract');

  const duo = makeLeaf('duocylinder', 'Duocylinder', [-3.5, 1.1, 0, 0], [0.3, 0.6, 0.9]);
  const tiger = makeLeaf('tiger', 'Tiger', [3.5, 1.6, 0, 0], [0.4, 0.85, 0.5]);

  const a = makeLeaf('hypersphere', 'Blob A', [0, 1, -4, 0], [0.8, 0.4, 0.8]);
  const b = makeLeaf('hypersphere', 'Blob B', [0.9, 1.5, -4, 0.4], [0.4, 0.4, 0.9]);
  b.xf.scale = 0.7;
  const blobs = makeGroup('smoothUnion', [a, b], 'Smooth blobs');
  blobs.k = 0.5;

  const pillar = makeLeaf('cubinder', 'Pillars', [0, 1, 5, 0], [0.75, 0.7, 0.6]);
  pillar.xf.scale = 0.4;
  pillar.modifiers.push({ type: 'repeat', axis: 0, spacing: 2, count: 3 });

  const root = makeGroup('union', [ground, carved, duo, tiger, blobs, pillar], 'Scene');
  root.id = 'root';
  return { format: '4dcad-scene', version: 1, root, primitives: [], world: null };
}

export function planetScene(): SceneFile {
  const c: Vec4 = [0, 0, 0, 0];
  const R = 3;
  const planet = makeLeaf('hypersphere', 'Planet (world)', c, [0.35, 0.5, 0.75]);
  planet.id = 'planet';
  planet.xf.scale = R;
  const objs: LeafNode[] = [
    anchored('cube', 'Red cube', c, R, [0, 1, 0, 0], [0.9, 0.3, 0.25]),
    anchored('sphere', 'Yellow sphere', c, R, [0, 1, 0.5, 0], [0.95, 0.8, 0.25]),
    anchored('torus', 'Green torus', c, R, [0.6, 1, 0, 0], [0.3, 0.85, 0.4]),
    anchored('cylinder', 'Blue cylinder', c, R, [0, 1, -0.6, 0.2], [0.3, 0.5, 0.95]),
    anchored('cube', 'Far cube', c, R, [0, 0, 1, 0], [0.9, 0.5, 0.9]),
    anchored('sphere', 'W-side sphere', c, R, [0, 0.3, 0, 1], [0.9, 0.9, 0.9]),
    anchored('cube', 'Antipode cube', c, R, [0, -1, 0, 0], [0.95, 0.55, 0.2]),
  ];
  // rings of objects along two great circles of the 3-sphere (walk along them in Explore)
  for (let i = 0; i < 10; i++) {
    const a = (i / 10) * Math.PI * 2 + 0.3;
    objs.push(anchored(i % 2 ? 'cube' : 'cylinder', `Ring A ${i + 1}`, c, R, [Math.cos(a), 0, Math.sin(a), 0.0001], [0.95, 0.35 + 0.05 * i, 0.3], 0.3));
  }
  for (let i = 0; i < 8; i++) {
    const a = (i / 8) * Math.PI * 2 + 0.2;
    objs.push(anchored(i % 2 ? 'torus' : 'sphere', `Ring B ${i + 1}`, c, R, [0.0001, Math.cos(a), 0, Math.sin(a)], [0.3, 0.6 + 0.04 * i, 0.95], 0.35));
  }
  for (const o of objs) { o.anchor!.world = planet.id; o.thickness = 0.4; }
  const root: GroupNode = makeGroup('union', [planet, ...objs], 'Scene');
  root.id = 'root';
  return {
    format: '4dcad-scene', version: 1, root, primitives: [], world: planet.id,
    camera: { target: [0, 0, 0, 0], distance: 11, yaw: 0.5, pitch: -0.3, rot4: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] },
  };
}

export const EXAMPLES: { id: string; name: string; make: () => SceneFile }[] = [
  { id: 'playground', name: '4D playground', make: playgroundScene },
  { id: 'planet', name: 'Hypersphere planet (geodesic)', make: planetScene },
];
