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

**Edit mode, Full 4D view (Blender-like)**
- **Navigation:** RMB/MMB drag orbits. A **middle-click** on an object sets the **centre of rotation** (shown as a small crosshair; middle-click empty space to reset it). Shift+drag pans, the wheel or Ctrl+drag zooms, and Alt+drag rotates the view into W.
- **W slider** (left edge of the viewport): moves the visible 3D slice along the camera's 4th axis.
- **Standard views:** 1 / 3 / 7 / 9 give front / right / top / along W. Shift+W resets the 4D orientation.

**Edit mode, Geodesic view (game-like):** hold **RMB** to look around with the mouse. While holding it, WASD flies along the surface, Space/Shift move up/down, Q/E roll, and the wheel sets fly speed. The left mouse button stays free for selecting and the gizmo.

**Editing**
- **Tools:** **Q** Select, **W** Move, **E** Rotate.
- **Gizmo:** it shows the selected object's **own X / Y / Z / W axes**, projected into the visible slice. The axes stay visible however the 4D view is turned; an axis pointing straight out of the slice is hidden.
  - Move: drag an arrow to move along that axis. Drag the centre square, or the object itself, to move in the screen plane (Shift: along depth).
  - Rotate: drag a ring to rotate in the plane of two object axes. Ring colours: XY blue, XZ green, YZ red, planes with W purple.
- **Blender-style transforms:** **G** grab, **R** rotate, **S** scale, then X / Y / Z / W = object axis. For R, one key rotates about that axis and two keys (e.g. `R X W`) rotate in that plane. Click or Enter confirms; Esc or RMB cancels.
- **Properties:** rotation is shown as 6 absolute angles (XY XZ XW YZ YW ZW).
- **Selection:** click selects (click again for the parent group), Shift+click multi-selects, A selects all, Esc deselects.
- **Editing:** Shift+D duplicate, X/Del delete, Ctrl+G group, F focus, Ctrl+Z / Ctrl+Shift+Z undo / redo.
- **Player sphere:** the yellow sphere with a "nose" shows where the Explore player is and which way it faces.

**Explore mode (game-like)**
- You always **start where the editor camera currently is**.
- Click to capture the mouse. Esc releases it; Esc again returns to Edit.
- WASD moves, Space/Shift move up/down, and the wheel sets speed.
- Full 4D: Q/E move along W, Z/X turn into W, C/V turn sideways in W, R resets.
- Geodesic: you move on the world's surface and Q/E roll.
- **Your body** is part of the scene. It casts a shadow, and on closed worlds light comes back around so you can see yourself. On a hypersphere world every ray that misses everything returns to you, so the "sky" is your own back. Toggle it in View > Show own body.

## Editor

* **Shapes are a point plus modifiers.** The modifier stack is applied top to bottom and works on any object or group:
  * **Extrude**: adds the next axis (X → Y → Z → W). Finite length, infinite, or half-space.
  * **Revolve**: adds the next axis by spinning an existing axis into it at a radius. R = 0 makes a disk/sphere; R > 0 makes a torus-like shape.
  * **Rotate**: rotates the shape built so far within a plane (useful before a Revolve).
  * **Mirror** and **Repeat** (spacing, copies; 0 = infinite).

  Examples: Point → Extrude → Extrude → Extrude → Extrude is a tesseract. Point → Extrude → Revolve → Extrude (infinite) is an infinite rod. A whole group can be extruded or revolved into the next dimension.
* **Library** (left):
  * Built-in folders (Basic, 3D shapes, 4D shapes) are read-only. Their items are just points with modifiers, so you can tweak them after placing.
  * "Save to library" (scene tree button or right-click) stores the selected object in **Custom**.
  * **+ Folder** adds folders. Right-click folders and items to rename, move, export or delete.
  * Drag items onto a folder to move them (built-ins are copied). Drag scene-tree rows onto a folder to save them there.
  * Drag items into the viewport or onto tree rows to place them. In the Geodesic view only ≤3D items can be placed, and they attach to the world surface.
* **Scene tree:**
  * Boolean groups: union, intersection, subtraction (first child minus the rest) and smooth versions.
  * Drag rows onto groups (into), onto objects (union with it) or onto row edges (reorder).
* **Geodesic sky:** the sky takes the colour of the world surface you live on. An outside thumbnail shows the world from the outside with your position.

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

Limits: about 100 nodes per scene (OpenGL uniform limit).

## Code layout

```
src/math4.hpp, primitives.*   4D maths (incl. rotation <-> plane angles), built-in shape recipes
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
  * Leaves: `prim` (always `point`; older files with other primitive types are converted), `color`, `thickness`, `round`, `shell`.
  * `modifiers`: a list of `{type: extrude|revolve|rotate|mirror|repeat, ...}`, applied in order.
  * Groups: `op`, `k`, `children`.
  * Custom primitives are embedded in the file.
  * Files from the earlier browser version (6 rotation angles) still load.
* **Objects** `.4dobj.json` and the **library** `.json` (folders of objects). Old `.4dprim.json` primitives and v1 libraries are converted to point + modifier objects on import.
* Your library and an autosave of the current scene live in `%APPDATA%\4DCAD`
  (Windows) or `~/.config/4dcad` (Linux).
