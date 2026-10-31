# 4DCAD

A browser-based 4D CSG modeller and viewer. Scenes are made of signed distance
fields (SDFs) and drawn by ray marching. It has two rendering methods:

* **Full 4D** – rays are marched through ℝ⁴. The camera's view directions span a
  3D hyperplane (right / up / forward), so you see the 3D cross-section through
  the eye. Rotating the camera into W brings other parts of the 4D scene into view.
* **Geodesic 3D** – you pick a 4D *world* object. Its boundary is a curved
  3D space, and both the light rays and the player stay inside it. Each march step
  advances along the tangent direction. The point is then pulled back onto the
  surface with a Newton step, `p -= f(p)·∇f/|∇f|²`, and the direction is
  re-projected onto the new tangent space. This is discrete parallel
  transport, so rays follow the surface's geodesics. On a hypersphere world, rays
  wrap around and objects near the antipode appear lensed.

```bash
npm install
npm run dev        # http://localhost:5173
npm test           # unit tests (SDF maths, geodesics, GPU bytecode vs reference)
npm run build      # typecheck + production build into dist/
npm run examples   # re-export examples/*.4dscene.json
```

Requires a browser with WebGL2 and float textures.

## Modes and controls

**Tab** switches between Edit and Explore. **V** (or the toolbar) switches between Full 4D and Geodesic 3D.

| | Edit (Blender-like) | Explore (game-like) |
|---|---|---|
| Look | MMB/RMB-drag orbit, Shift+drag pan, wheel / Ctrl+drag zoom | click to capture mouse, mouse look |
| 4D | Alt+drag rotates the view into W (Geodesic: roll); 9 = look along W; Shift+W reset | Z/X turn into W, C/V turn sideways in W, R reset |
| Move | – | WASD, Space/Shift up/down, Q/E along W (Geodesic: roll), wheel = speed |
| Edit | click select (click again = parent), Shift+click multi, A all, Esc none; G/R/S + X/Y/Z/W constraints (R with two axes = that plane, e.g. `R X W`); Shift+D duplicate; X/Del delete; Ctrl+G group; F/. focus; 1/3/7 views; Ctrl+Z / Ctrl+Shift+Z | collisions (toggle in Quality) |

In Geodesic + Edit mode, only objects of dimension ≤ 3 can be placed, and they
are *attached* to the world surface. Grabbing them (G) slides them along the
surface, carrying their orientation with them.

## Editor

* **Library** (left): built-in primitives grouped by dimension, plus your own
  primitives and saved objects. Drag an item into the viewport (it lands where
  the ray hits) or into the scene tree, or double-click it.
* **Scene tree**: groups combine their children with union, intersection,
  subtraction (first child minus the rest), and smooth versions of each (with
  smoothness `k`). Drag rows onto a group to move them into it, onto an object to
  union the two, or onto the top/bottom edge to reorder. Group / Dup / Del /
  **Save obj** (store the subtree in the library) / **Export obj** (`.4dobj.json`).
* **Properties**: name, primitive, colour, thickness (extent along unused axes),
  rounding, shell, 4D position, 6 rotation-plane angles (XY XZ XW YZ YW ZW),
  scale, **modifiers** (Mirror on an axis; Repeat with spacing and count, 0 = infinite),
  "Use as world", "Attach to / Detach from world surface".
* **Primitive builder** (`+ Primitive`): start from a point and add steps. Each
  Extrude/Revolve adds the next axis (X → Y → Z → W):
  * *Extrude* – finite length, infinite, or half-space.
  * *Revolve* – spin an existing axis into the new axis at radius R. R = 0
    revolves around the centre; R > 0 gives torus-like shapes.
  * *Rotate* – rotate the profile within its current space. This matters before
    a Revolve, because it changes the profile's orientation relative to the
    rotation axis.

  Rounding and shell are applied at the end. A live preview has a W-slice slider.
  Primitives are saved to the library (browser storage) and can be exported as
  `.4dprim.json`.

  Examples: point → segment → square → cube → tesseract; point → segment →
  (revolve) disk → (extrude ∞) rod; disk → (revolve R) torus → (revolve) torisphere.

## Rendering details

* **Lower-dimensional objects in 4D** are extruded by `thickness` along their
  unused axes, so a 3D object is a thin slab in W.
* **Attached objects** are anchored to a world. They are evaluated in the
  tangent space at their anchor point:
  * In the Geodesic view, W is set to 0.
  * In the Full 4D view, local W is replaced by the signed distance to the world
    surface. The object is then a shell that hugs the curved surface rather than
    a flat slab sticking out of it.
  * A *reach* (about the local radius of curvature) stops the tangent projection
    from creating a second, antipodal copy on closed worlds.
* **Other 4D objects** remain visible in the Geodesic view where they intersect
  the world surface.
* **Quality menu**: resolution, march steps, soft shadows, ambient occlusion,
  geodesic max step / projection iterations, FOV, collisions.

## Architecture (portable core)

```
src/core/      platform-independent TypeScript (no DOM / WebGL)
  math4.ts       4D vectors, 4×4 matrices, plane rotations
  primitives.ts  extrude / revolve recipes, CPU evaluation, built-ins
  scene.ts       scene tree, transforms, anchors, (de)serialisation, library objects
  evaluator.ts   reference CPU SDF (picking, placement, collisions, camera on surfaces)
  bounds.ts      conservative bounding hyperspheres per node
  program.ts     scene → bytecode + node data packed into a float texture (+ JS emulator)
  geodesic.ts    projection onto surfaces, frame transport, geodesic shooting
  camera.ts      Full-4D orbit/fly camera and surface-bound geodesic camera
  surface.ts     picking rays, anchoring, curvature estimate
src/render/
  shaders.ts     fixed GLSL interpreter + ray marcher (GLSL ES 3.00 / desktop 3.30 body)
  renderer.ts    the only WebGL2-specific file
src/ui/          DOM editor (panels, builder, HUD, input → App)
```

The GPU does not compile per-scene shaders. `program.ts` turns the tree into a
small stack-machine program (BEGIN / LEAF / COMBINE / END / EMPTY) plus per-node
data. This is uploaded as one RGBA32F texture and executed by a fixed
interpreter in `shaders.ts`. The shader compiles once per view mode, and editing
the tree never triggers a recompile. Bounding hyperspheres let it skip distant
subtrees.

Porting to a native app means keeping `core/` and the GLSL in `shaders.ts`
(swap the `#version` header) and re-implementing `renderer.ts` (one texture, one
full-screen triangle, a handful of uniforms) and the UI. A unit test checks the
bytecode emulator against the reference evaluator, so the encoding is verified
independently of the GPU.

## File formats (JSON)

* **Scene** `.4dscene.json`: `{ format: "4dcad-scene", version: 1, root, primitives, world, camera }`.
  * `root` is a group node.
  * Every node has `id`, `name`, `xf: { pos[4], rot[6] (degrees), scale }` and
    `modifiers` (`{type:"mirror",axis}` / `{type:"repeat",axis,spacing,count}`).
    It may also have `hidden` and `anchor: { world, basis[16], reach }`.
  * Leaves: `kind:"leaf", prim, color[3], thickness, round?, shell?`.
  * Groups: `kind:"group", op, k, children`.
  * Custom primitives used by the scene are embedded, so files are self-contained.
* **Primitive** `.4dprim.json`: `{ format: "4dcad-primitive", primitive: { id, name, steps, round, shell } }`.
  Steps are `{op:"extrude", mode:"finite"|"infinite"|"half", length}`,
  `{op:"revolve", radius, axis?}` and `{op:"rotate", i, j, angle}`.
* **Object** `.4dobj.json`: `{ format: "4dcad-object", name, node, primitives }`.
* **Library** `.json`: `{ format: "4dcad-library", primitives, objects }`.

See `examples/` for complete scene files.
