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

**CI builds:** every push is built and tested by GitHub Actions (`.github/workflows/build.yml`).
The latest Windows exe can be downloaded from the run's **4DCAD-windows** artifact
(Actions tab → the run → Artifacts). Pushing a tag such as `v1.2` also attaches the exe to a GitHub Release.

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
- **Navigation:** RMB/MMB drag orbits, with no angle limit, so you can go over the top. A **middle-click** on an object sets the **centre of rotation** (shown as a small crosshair; middle-click empty space to reset it). Shift+drag pans, the wheel or Ctrl+drag zooms, and Alt+drag rotates the view into W.
- **W slider** (left edge of the viewport): moves the visible 3D slice along the camera's 4th axis.
- **Slider axis** (the X / Y / Z / W buttons under the slider, or View > Slider axis): chooses which world axis is on the slider; the other three are the shown axes (e.g. Y on the slider shows X, W and Z with W pointing up). Switching resets the 4D orientation.
- **Standard views:** 1 / 3 / 7 / 9 give front / right / top / along W. Shift+W resets the 4D orientation.

**Edit mode, Geodesic view (game-like):** hold **RMB** to look around with the mouse. While holding it, WASD flies along the surface, Space/Shift move up/down, Q/E roll, and the wheel sets fly speed. The left mouse button stays free for selecting and the gizmo.

**Editing**
- **Tools:** **Q** Select, **W** Move, **E** Rotate.
- **Local / Global** (toolbar, or **L**): the gizmo and the G / R axis keys use either the object's own axes or the world axes. In Global an arrow moves the object along exactly one world coordinate (the three shown axes; G then X / Y / Z / W for any axis). If the view is turned into the 4th dimension, an arrow that leaves the visible slice is drawn faded. On a world surface, Global means the surface's directions at the object.
- **Snap** (toolbar; hold **Ctrl** while transforming to invert it): moves land on the world grid (on a world surface: the distance moved is rounded), rotations go in angle steps and scale in scale steps. The steps are set in Settings > Edit mode.
- **Gizmo:** always **3 arrows / 3 rings for the object's local X, Y and Z**. They are kept inside the visible slice and straightened into a clean 3D frame, so when the object or the view is turned into W, an arrow moves the object along a mix of world axes (e.g. X and W) in a straight line, without leaving the slice.
  - Move: drag an arrow to move along that local axis. Drag the centre square, or the object itself, to move in the screen plane (Shift: along depth).
  - Rotate: drag a ring to rotate about a local axis (red X, green Y, blue Z).
- **Blender-style transforms:** **G** grab, **R** rotate, **S** scale, then X / Y / Z = the object's local axis (as on the gizmo). Click or Enter confirms; Esc or RMB cancels.
- **Properties:** rotation is shown as 6 absolute angles (XY XZ XW YZ YW ZW).
- **Selection:** a click selects exactly the object under the cursor; **Ctrl+click** selects its group. Shift+click multi-selects, A selects all, Esc deselects.
- **Several objects selected:** Properties shows the last selected object's values (fields that differ are marked `*`), and a change applies to all of them. Colours, materials and other fields are copied; position, rotation and scale are applied relatively (offset, rotation about each object's own centre, scale ratio). Modifier edits reach all objects only when their modifier stacks match.
- **Editing:** Shift+D duplicate, X/Del delete, Ctrl+G group, F focus, Ctrl+Z / Ctrl+Shift+Z undo / redo.

**Explore mode (game-like)**
- **Edit and Explore share one camera**: switching modes in either direction never moves you or turns you.
- Click to capture the mouse. Esc releases it; Esc again returns to Edit.
- WASD moves, Space/Shift move up/down, and the wheel sets speed.
- Full 4D: R/F move along the slider axis (W by default), Q/E roll (as in Geodesic), Z/X turn into W, C/V turn sideways in W, Backspace resets the 4D orientation.
- Geodesic: you move on the world's surface and Q/E roll.
- **Your body** is part of the scene. It casts a shadow, and on closed worlds light comes back around so you can see yourself. On a hypersphere world every ray that misses everything returns to you, so the "sky" is your own back. Toggle it in Settings > Explore mode.

## Editor

* **Shapes are a point plus modifiers.** The modifier stack is applied top to bottom and works on any object or group:
  * **Extrude**: adds the next axis (X → Y → Z → W). Finite length, infinite, or half-space.
  * **Revolve**: adds the next axis by spinning an existing axis into it at a radius. R = 0 makes a disk/sphere; R > 0 makes a torus-like shape.
  * **Mirror** (axis, **offset**: the shape and its mirror image sit that far from the mirror plane) and **Repeat** (spacing, copies; 0 = infinite).
  * **Radial repeat**: copies the shape around an axis (a plane of rotation in 4D), with a **radius** (distance from the axis), number of **copies**, **angle step** between them and a **start angle**. Copies × step may be less than 360° (an arc); "Fill 360" spreads them around the full circle. One copy at radius 0 just rotates the shape. (It replaces the old Rotate modifier; files using Rotate load as a one-copy Radial repeat with the same result.)

  Examples: Point → Extrude → Extrude → Extrude → Extrude is a tesseract. Point → Extrude → Revolve → Extrude (infinite) is an infinite rod. A whole group can be extruded or revolved into the next dimension.
* **Library** (left):
  * Built-in folders (Basic, 3D shapes, 4D shapes) are read-only. Their items are just points with modifiers, so you can tweak them after placing.
  * "Save to library" (scene tree button or right-click) stores the selected object in **Custom**.
  * **+ Folder** adds folders. Right-click folders and items to rename, move, export or delete.
  * Drag items onto a folder to move them (built-ins are copied). Drag scene-tree rows onto a folder to save them there.
  * Drag items into the viewport or onto tree rows to place them. In the Geodesic view only ≤3D items can be placed, and they attach to the world surface.
* **Scene tree:**
  * Boolean groups: union, intersection, subtraction (first child minus the rest) and smooth versions.
  * **Group origin:** a new group's origin (its centre of rotation and scaling) is its **first object**, which then sits at 0,0,0,0. The other objects get coordinates relative to it. Grouping, ungrouping and dragging objects into or out of groups convert between group-local and global coordinates, so nothing moves in the world. **Recentre origin on first object** (Properties of a group) moves the origin again, e.g. after reordering.
  * A group whose last object is deleted or moved out is deleted too.
  * Drag rows onto groups (into), onto objects (union with it) or onto row edges (reorder).
* **Materials** (Properties): colour, **rounding** (inflates the solid, rounding its edges), **shell** (hollows it into a skin of that half-thickness), **emissive** (always fully lit, casts no shadow, does not light other objects), and a **procedural 4D texture** (checker, stripes, grid, noise, rings) with a second colour, scale and strength. Textures are solid 4D textures in the object's local space, so they move and rotate with it.
* **SDF code:** Properties > **View SDF code** shows any object (shape, group, whole tree) as a self-contained GLSL `float sdf(vec4 p)`. "Create editable custom SDF from it" turns it into a custom object.
* **Custom SDFs:** the "Custom SDF (GLSL)" item (4D shapes folder) holds your own GLSL defining `float sdf(vec4 p)` in the object's local 4D space.
  * You can write helper functions, and use `opExtrude(d, h)`, `opRadial(v, r, n, start, step)`, `sU / sI / sS(a, b, k)` and `INF`.
  * "Apply code" test-compiles it and shows compiler errors. Code that does not compile is drawn as its bounding box.
  * Set **Bound** so the shape fits inside it. The renderer uses it to skip far-away work, and on the CPU clicking, collisions and placement treat the shape as that box.
  * Custom SDFs combine with booleans and modifiers like any other object.
* **Geodesic sky:** a ray that hits nothing shows the colour (including the texture) of the world surface at the point it travelled to, so a multi-coloured or textured world gives a matching sky. The Full 4D sky is unchanged. An outside thumbnail shows the world from the outside with your position.

## Saving and settings

* Nothing is written to your scene file unless you save (**Ctrl+S**; **Ctrl+Shift+S** = save as). The toolbar shows "unsaved" when there are changes. Quitting, New, Open and loading an example ask whether to save first.
* On startup the last saved / opened file is reopened (falling back to the example scene).
* **Settings** (toolbar or View menu) has four tabs, saved per user:
  * **Edit mode**: field of view, local / global axes, snapping steps. Full 4D: orbit / pan / zoom / turn-into-W speeds, W slider, ground grid. Geodesic: fly speed, look speed, outside thumbnail and its size.
  * **Explore mode**: field of view, look speed, own body and its radius. Full 4D: move speed, turn-into-W speed, collisions. Geodesic: move speed, collisions.
  * **Rendering**:
    * Performance: resolution, and resolution while moving.
    * Ray marching: max steps, **max ray distance** (Full 4D and Geodesic, after which the sky colour is shown), hit precision, step scale (below 1 for inexact custom SDFs).
    * Lighting: soft shadows (sharpness, distance), ambient occlusion (strength), ambient light, light direction including its W component.
    * Sky and fog: sky colours and fog amount.
    * Geodesic rays: max step size and projections.
  * **Keys**: every shortcut and movement key can be rebound (two keys per action). Click a key and press the new one; right-click clears it. A red "!" marks keys that clash with another action in the same mode. "Reset all to defaults" restores the keys listed in this README.

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

Limits: about 90 nodes per scene (OpenGL uniform limit).

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
src/ui*.cpp, ui_common.hpp    ImGui interface (ui.cpp core, ui_toolbar / ui_library / ui_tree /
                              ui_properties / ui_viewport / ui_windows panels)
src/filedialog.*, main.cpp    native file dialogs, window
tests/                        core and app-level unit tests (ctest)
```

## Files

* **Scenes** `.4dscene.json` (`format: "4dcad-scene"`): a tree of nodes. Each
  node has `xf: {pos[4], rotm[16], scale}`, `modifiers`, an optional `anchor`,
  and either leaf fields or group fields.
  * Leaves: `prim` (always `point`; older files with other primitive types are converted), `color`, `thickness`, `round`, `shell`.
  * `modifiers`: a list of `{type: extrude|revolve|radial|mirror|repeat, ...}`, applied in order (old `rotate` entries load as `radial`).
  * Groups: `op`, `k`, `children`.
  * Custom primitives are embedded in the file.
  * Files from the earlier browser version (6 rotation angles) still load.
* **Objects** `.4dobj.json` and the **library** `.json` (folders of objects). Old `.4dprim.json` primitives and v1 libraries are converted to point + modifier objects on import.
* Your library and an autosave of the current scene live in `%APPDATA%\4DCAD`
  (Windows) or `~/.config/4dcad` (Linux).
