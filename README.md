# 4DCAD

A native 4D CSG modeller and viewer for Windows and Linux, written in C++17
with OpenGL 3.3, GLFW and Dear ImGui. Scenes are made of signed distance
fields (SDFs) and drawn by ray marching on the GPU. It has two rendering methods:

* **Full 4D**: rays are marched through ℝ⁴ inside the camera's 3D hyperplane,
  so you see the 3D slice through the eye. Rotate into W, or move the slice with
  the **W slider**, to see other parts of the 4D scene.
* **Geodesic 3D**: choose a 4D *world* object. Its boundary is a curved 3D space,
  and both the light rays and the camera stay inside it. Each march step goes
  along the tangent direction, then is pulled back onto the surface by a Newton
  step `p -= f·∇f/|∇f|²`, and the direction is re-projected onto the new tangent
  space (discrete parallel transport). A **thumbnail** shows the world from the
  outside, with the camera drawn as a yellow sphere.

## Running

**Windows:** run `release/4DCAD.exe`. No installation is needed. It uses
your graphics driver's OpenGL 3.3+, so keep the driver up to date.

**Building from source** (Windows or Linux) needs CMake ≥ 3.16, a C++17
compiler and git. Dependencies (GLFW, Dear ImGui, nlohmann/json) are downloaded
automatically.

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release
```

The executable is `build/4DCAD` (or `build/Release/4DCAD.exe` with Visual
Studio). On Linux, install the X11/GL development packages first:
`sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev`.
Run the tests with `ctest --test-dir build`.

To cross-compile the Windows exe on Linux (MinGW-w64):

```bash
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DFD_BUILD_TESTS=OFF
```

```bash
cmake --build build-win
```

Optional command line: `4DCAD [scene.json] [--example playground|planet] [--geo] [--explore]`.

## Modes and controls

**Tab** switches between Edit and Explore. **V** (or the toolbar) switches between Full 4D and Geodesic 3D.

**Edit mode (Blender-like)**
- **Navigation:** RMB/MMB drag orbits, Shift+drag pans, the wheel or Ctrl+drag zooms, and Alt+drag rotates the view into W (Geodesic: roll).
- **Standard views:** 1 / 3 / 7 / 9 give front / right / top / along W. Shift+W resets the 4D orientation.
- **W slider** (left edge of the viewport): moves the visible 3D slice along the camera's 4th axis. The "W=0" button resets it.
- **Tools:** **Q** Select, **W** Move, **E** Rotate.
  - Move and rotate work relative to the camera and **always stay inside the visible slice**.
  - Move: drag an object across the screen; Shift moves it along the depth axis.
  - Rotate: drag horizontally or vertically to turn it; Shift rolls it.
  - Objects attached to a world surface slide along that surface.
- **Blender-style transforms:** **G** grab, **R** rotate, **S** scale. Then X / Y / Z constrain to screen-right / screen-up / depth. Click or Enter confirms; Esc or RMB cancels.
- **Selection:** click selects (click again for the parent group), Shift+click multi-selects, A selects all, Esc deselects.
- **Editing:** Shift+D duplicate, X/Del delete, Ctrl+G group, F focus, Ctrl+Z / Ctrl+Shift+Z undo / redo.
- **Player sphere:** the yellow sphere is where the Explore player is.
  - In Full 4D it is projected into the visible slice, and its label shows `dW`, its distance from the slice.
  - In the Geodesic view it sits on the world surface.
  - A line shows which way the player faces.

**Explore mode (game-like)**
- Click to capture the mouse. Esc releases it; Esc again returns to Edit.
- WASD moves, Space/Shift move up/down, and the wheel sets speed.
- Full 4D: Q/E move along W, Z/X turn into W, C/V turn sideways in W, R resets.
- Geodesic: you move on the world's surface and Q/E roll. Collisions can be turned off in Quality.

The player and the edit camera are separate, so switching modes doesn't move either one.

## Editor

* **Library** (left):
  * Built-in 0D–4D primitives, your own primitives, and saved objects.
  * Drag an item into the viewport (it lands where the ray hits) or onto a tree row, or double-click it.
  * Right-click an item to edit, export or delete it.
  * In the Geodesic view, only ≤3D items can be placed, and they attach to the world surface.
* **Scene tree** (right):
  * Groups combine their children with union, intersection, subtraction (first child minus the rest) or a smooth version of each.
  * Drag rows onto groups to move them in, onto objects to union with them, or onto row edges to reorder.
  * Right-click a row to hide, duplicate, delete, ungroup or group it.
  * "Save as library object" keeps the subtree for reuse; "Export object" writes it to a file.
* **Properties**:
  * name, primitive, colour, thickness (extent along unused axes), rounding, shell
  * 4D position, scale, and rotation (drag in any of the 6 planes)
  * modifiers: Mirror, and Repeat with spacing and count (0 = infinite)
  * Use as world / Attach to / Detach from the world surface
* **Primitive builder** (`+ Primitive`):
  * Start from a point and add steps; each Extrude or Revolve adds the next axis (X → Y → Z → W).
  * *Extrude*: finite length, infinite, or half-space.
  * *Revolve*: spin an existing axis into the new axis at radius R. R = 0 gives a disk/sphere; R > 0 gives a torus-like shape.
  * *Rotate*: turns the profile before a later Revolve.
  * Rounding and shell are applied at the end. A live preview has W-slice and ZW sliders.
  * Example chains: point → segment → square → cube → tesseract; point → segment → disk → rod.

## Performance

* **Per-scene shaders.** Each scene structure is compiled into its own GLSL
  program; desktop drivers compile these quickly. Moving or recolouring objects
  only updates uniforms, so it never recompiles.
* **Bounding spheres.** Each node has a bounding hypersphere, so distant
  subtrees are skipped while marching.
* **One `map()` call site.** The shader's main loop is a single state machine
  (march → normal → AO → shadow) with one call to `map()`, which keeps
  compile time proportional to scene size.
* **Adaptive resolution.** Frames render at reduced resolution while the
  camera or objects move, then at full resolution once things are still. The
  viewport only redraws when something changed.
* **Quality settings** (toolbar → Quality): resolution, resolution while moving,
  march steps, shadows, AO, geodesic step size.

Limits: about 100 nodes per scene (OpenGL uniform limit), at most 4 modifiers
are honoured by the bounding logic, and the tree can't nest deeper than the GLSL
compiler allows.

## Code layout

```
src/math4.hpp, primitives.*   4D maths, extrude/revolve recipes (CPU eval + GLSL emission)
src/scene.*                   scene tree, JSON (de)serialisation, library objects
src/evaluator.*               reference CPU SDF + bounding hyperspheres
src/geodesic.*, surface.*     surface projection, frame transport, picking, anchoring
src/camera.*                  Full-4D orbit/fly camera, surface-bound geodesic camera
src/codegen.*, shaders.hpp    scene → GLSL, ray-marching shader
src/renderer.*, gl.*          OpenGL renderer and function loader
src/app.*                     state, tools, undo, explore movement, thumbnail
src/ui.*, filedialog.*, main.cpp   ImGui interface, native file dialogs, window
tests/                        core and app-level unit tests (ctest)
```

## Files

* **Scenes** `.4dscene.json` (`format: "4dcad-scene"`): a tree of nodes. Each
  node has `xf: {pos[4], rotm[16], scale}`, `modifiers`, an optional `anchor`,
  and either leaf fields or group fields.
  * Leaves: `prim`, `color`, `thickness`.
  * Groups: `op`, `k`, `children`.
  * Custom primitives are embedded in the file.
  * Files from the earlier browser version (6 rotation angles) still load.
* **Primitives** `.4dprim.json`, **objects** `.4dobj.json`, and the **library** `.json`.
* Your library and an autosave of the current scene live in `%APPDATA%\4DCAD`
  (Windows) or `~/.config/4dcad` (Linux).
