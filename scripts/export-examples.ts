// Writes the built-in example scenes to examples/*.4dscene.json
import { writeFileSync } from 'node:fs';
import { EXAMPLES } from '../src/core/examples.ts';
import { PrimitiveRegistry, serializeScene } from '../src/core/scene.ts';

const reg = new PrimitiveRegistry();
for (const ex of EXAMPLES) {
  const s = ex.make();
  const file = serializeScene(s.root, reg, s.world, s.camera);
  writeFileSync(`examples/${ex.id}.4dscene.json`, JSON.stringify(file, null, 2) + '\n');
  console.log(`examples/${ex.id}.4dscene.json`);
}
