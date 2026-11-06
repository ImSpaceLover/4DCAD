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

**Windows:** run `release/4DCAD.exe`. No installation is needed. It draws with
**Vulkan** when the graphics driver supports it (better on AMD cards) and otherwise with
**OpenGL 3.3+**; Settings > Rendering > Graphics API (or `--vulkan` / `--gl`) chooses. Keep the
driver up to date.

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
Rendering from the command line (renders like the Render window, from the active camera, then quits):
`4DCAD scene.4dscene.json --render out.png|.jpg|.gif|.avi|.mp4|.webm [--animation] [--render-size 1920x1080] [--samples 64] [--aa 2]`
(`--animation` with `.png` writes a numbered PNG sequence).

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
- **Debug ray** (T): shows the path of the ray leaving your eye along the view direction, marched exactly like the renderer's rays (it follows the geodesic in the Geodesic view, straight in Full 4D). It is drawn as a thin glowing tube; the ball at its end shows why it stopped (red: hit an object, blue: max ray distance, yellow: max march steps, grey: left the scene), and the status line gives its length and step count. It is re-traced from your view every frame; **Shift+T** freezes it so you can walk around it and see it bend. The outside thumbnail also shows its path.
- **Your body** is part of the scene. It casts a shadow, and on closed worlds light comes back around so you can see yourself. On a hypersphere world every ray that misses everything returns to you, so the "sky" is your own back. Toggle it in Settings > Explore mode.

## Curved spaces: Hyperbolic and Spherical 3D

**File > New** asks what space the scene lives in:

* **Flat 4D**: four-dimensional Euclidean space, with the Full 4D and Geodesic 3D views described above.
* **Hyperbolic 3D**: three-dimensional space of constant negative curvature (−1/R²).
  * Parallel lines spread apart and triangles have less than 180°.
  * There is exponentially more room further away, so distant objects shrink very fast.
  * A flat ground plane seen from above looks like a dome: its edge at infinity is a circle below the horizon.
* **Spherical 3D**: three-dimensional space of constant positive curvature (+1/R², a 3-sphere).
  * Straight lines close up after 2πR, so looking far enough ahead you see the back of things.
  * Objects near the opposite point of the space look magnified.

The **curvature radius R** is set in the dialog and in the toolbar ("R = …", drag to change).
* Smaller is more strongly curved; large R is nearly flat.
* Changing R keeps every object's coordinates, so space bends around them.
* It can be undone, is saved in the scene, and can be **animated**: right-click the R field to insert keyframes ("Space: Curvature radius" in the timeline).

How it works:
* Points use the hyperboloid / 3-sphere model in four coordinates.
* Rays follow **exact geodesics**, the straight lines of the space.
* Objects, groups and the camera are placed by isometries of the space.
* A position (Properties: Position XYZ) is a pair of **normal coordinates**: walk that far in that direction from the parent's origin.
* Rotations are 3D only.
* Every shape is evaluated in normal coordinates around its own origin:
  * a ball is an exact geodesic ball;
  * a cube has very slightly curved faces;
  * marching stays safe in both spaces.
* **Precision.** Hyperbolic coordinates grow like e^(distance / R), so:
  * Each object's transform relative to the camera is computed in double precision on the CPU. The GPU only sees small numbers near you.
  * Infinite objects are re-anchored next to the camera: planes and half-spaces at the nearest point, infinite repeats at the nearest copy, honeycombs at your cell.
  * Objects beyond the rays' reach are skipped.
  * Hyperbolic rays stop after 12R (7R with a honeycomb); anything further is below a pixel.
  * You can wander about 15R from the scene's origin; beyond that, double precision on the CPU runs out.
* **Infinite shapes are exact.**
  * An infinite extrusion is measured from the point's nearest spot on its plane or line, so slabs and tubes have a true constant thickness.
  * A Repeat places exact copies along a straight line of the space (in spherical space they come round again after 2πR).

Everything else works as in Flat 4D:
* Materials, textures, lights, shadows, glass and mirrors, stochastic sampling.
* The ground grid (normal coordinates on the plane y = 0) and the sky (hyperbolic: by the point at infinity a ray heads for).
* Explore walking with collisions.
* The move / rotate gizmo and G / R / S, which move objects along geodesics and rotate them about their centre.
  * Snapping rounds the distance moved (there is no grid in curved space).
  * Global axes are the world axes carried to the object.
* Grouping, ungrouping and reparenting, which keep objects in place.
* Group scale spreads the children along geodesics.
* Cameras, animation and the Render window.

**Polyhedra & tilings** (library folder, in any space):
* **Polyhedra**: tetrahedron, cube, octahedron, dodecahedron, icosahedron.
  * Their faces are truly flat planes of the space (exact distances), set by the inradius.
  * In hyperbolic space the same inradius gives sharper corners than in flat space; in spherical space, blunter ones.
* **Honeycombs**: regular tilings that fill the whole space with copies of one cell, drawn as edges or as cells with gaps. Neighbouring cells can alternate two colours.
  * Flat: cubic {4,3,4} (cell size set by you).
  * Hyperbolic: order-5 cubic {4,3,5} (5 cubes around each edge), order-4 and order-5 dodecahedral {5,3,4} / {5,3,5}, icosahedral {3,5,3}.
  * Spherical: the regular 4D polytopes seen from inside: tesseract {4,3,3}, 120-cell {5,3,3}, 24-cell {3,4,3}, 5-cell, 16-cell, 600-cell.
  * In curved space the cell size is fixed by the angles, so it follows the curvature radius (animate R and the tiling breathes).
  * Rendering folds each point into the central cell by mirroring it across faces.
  * The tiling is anchored, in double precision, at the cell you are in, so it stays sharp wherever you walk.
* **Overview** (Edit mode, bottom right; drag to orbit, wheel to zoom; Settings > Edit mode > Outside thumbnail):
  * Hyperbolic: the **Poincaré ball**, the whole infinite space inside a ball whose surface is infinity.
  * Spherical: the stereographic view of the half of the space around the origin, and some of the rest.
  * A marker shows where you are and which way you look.

The **debug ray** (Explore, T / Shift+T) also works in curved spaces. It follows the same exact geodesics and distance limits as the renderer. Its path also shows in the ball overview: in hyperbolic space it's a circular arc that meets the ball's surface at a right angle.

There are no world objects or W slider: the curved spaces are 3D. 4D shapes show their 3D cross-section (w = 0).

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
  * Each row has an **eye** (show / hide) and a **padlock** (locked objects cannot be clicked or moved in the viewport; they stay editable in Properties). A **search** box filters the tree by name, and **- / +** collapse or expand everything.
  * **Isolate** (Numpad / or /, or right-click > Isolate): only the selected objects (and the world) are drawn and clickable until you press it again. It is a view setting: nothing is hidden in the saved scene.
  * **Duplicate linked** (Alt+D): the copies share their shape, material, modifiers and contents, so editing any of them (including objects inside a linked group) edits all; each keeps its own position, rotation and scale. Properties shows "Linked (N copies)" with an **Unlink** button.
  * **Copy / paste** (Ctrl+C / Ctrl+V) uses the system clipboard, so objects can be moved between scenes (or between two running copies of 4DCAD). Pasted objects go to the scene root at the same place in the world.
* **Cameras** are scene objects (View > Cameras > Add camera at the view): they appear in the scene tree, are drawn in the viewport as small frustums and move / rotate like any object. Each has a field of view and (Full 4D) a slider axis; one added in the Geodesic view is attached to the world surface and moves along it. The **active camera** (orange) is the one used for rendering. **Numpad 0** looks through the selected (or active) camera, **Ctrl+Alt+Numpad 0** moves it to your view. Camera bookmarks from older files become camera objects.
  * Drag rows onto groups (into), onto objects (union with it) or onto row edges (reorder).
* **Materials** (Properties): colour, **rounding** (inflates the solid, rounding its edges), **shell** (hollows it into a skin of that half-thickness), **emissive** (always fully lit, casts no shadow, does not light other objects), and a **procedural 4D texture** (checker, stripes, grid, noise, rings, dots, waves, marble, wood, cells, bricks, gradient) with a second colour, scale and strength. Textures are solid 4D textures in the object's local space, so they move and rotate with it.
  * **Roughness / Specular**: size and strength of highlights. **Metallic**: a mirror (1) that reflects the scene, tinted by the colour. **Transparency** with a **refraction index**: light bends entering and leaving (1 = none, 1.33 water, 1.5 glass); light can also be reflected inside (total internal reflection). Settings > Rendering > Bounces limits how many reflections / refractions a pixel follows.
* **Stochastic sampling** (Settings > Rendering): while nothing moves, the view refines by averaging random samples ("Refining n / N" in the status line): glass reflects and refracts by its Fresnel ratio, rough mirrors give blurry reflections, the sun (Sun size) and point lights (Light radius) give soft shadows, ambient occlusion is sampled, edges are anti-aliased, and cameras with an Aperture show depth of field (Focus distance) when you look through them. As soon as anything moves, the normal fast shading is used again.
* **Fast mode** (Settings > Rendering > Fast mode): the rendering used in Explore, adjustable separately from the normal settings, with Fast / Balanced / Full presets: resolution, march steps, **bounces** (reflections / refractions; with 0, mirrors and glass look like plain surfaces), textures, highlights, point lights, sun and point light shadows, ambient occlusion and its samples, shadow ray steps. "Also while moving in Edit mode" uses it whenever the view or objects move in Edit mode too.
* **Lights** (View > Lights): point lights with a 4D position, colour, intensity, range and optional shadows, saved in the scene. Emissive objects also light their surroundings (Settings > Rendering). Up to 8 lights are used at once.
* **SDF code:** Properties > **View SDF code** shows any object (shape, group, whole tree) as a self-contained GLSL `float sdf(vec4 p)`. "Create editable custom SDF from it" turns it into a custom object.
* **Custom SDFs:** the "Custom SDF (GLSL)" item (4D shapes folder) holds your own GLSL defining `float sdf(vec4 p)` in the object's local 4D space.
  * You can write helper functions, and use `opExtrude(d, h)`, `opRadial(v, r, n, start, step)`, `sU / sI / sS(a, b, k)` and `INF`.
  * "Apply code" test-compiles it and shows compiler errors. Code that does not compile is drawn as its bounding box.
  * Set **Bound** so the shape fits inside it. The renderer uses it to skip far-away work, and on the CPU clicking, collisions and placement treat the shape as that box.
  * Custom SDFs combine with booleans and modifiers like any other object.
* **Geodesic sky:** a ray that hits nothing shows the colour (including the texture) of the world surface at the point it travelled to, so a multi-coloured or textured world gives a matching sky. The Full 4D sky is unchanged. An outside thumbnail shows the world from the outside with your position.

## Animation

* **Timeline** (bottom of the viewport): play / pause (**Space**), step (**Left / Right**), jump to the first / last frame (**Shift+Left / Right**), current frame, frame range and frames per second (default 24 fps, frames 1–120). Drag on the ruler to scrub.
* **Keyframes:** **I** keys the selected objects (position / rotation / scale, or all values); **Alt+I** removes their keys at this frame. In Properties (and the Lights window) right-click any value to key just that value; a diamond next to it shows it is animated (filled: keyed on this frame). **Auto-key** keys every change you make. Everything numeric can be animated: transforms, material values and colours, modifier values (copies and axes step), smoothness, visibility, point lights and cameras (position, orientation, field of view).
* The timeline shows the selected objects' keys (a summary row plus one row per value): click / Shift+click to select, drag to move, right-click for interpolation or delete, **X / Del** deletes, **Ctrl+C / Ctrl+V** copy and paste at the current frame, wheel zooms, middle-drag pans, **Home** frames everything.
* **Interpolation** (per key, to the next key): constant, linear, bezier (smooth), ease in / out / in-out. **4D rotations** take the shortest 4D rotation path (both rotation planes turn together at constant speed). Objects and cameras on a world surface travel along the surface between keys.
* **Objects on a world surface** (attached; Properties > World surface):
  * **Follow surface** bends the object's X axis along the surface: along the geodesic through its anchor, with its frame carried along.
    * A long rod lies along a curved world and goes through a wormhole's throat instead of sticking out straight.
  * **Surface motion**: **Travel** (distance walked along the surface, along the object's X axis), **Heading** (turn about its Y axis first) and **Spin** (roll about its X axis), measured from a rest pose.
    * Keying any of them (right-click, or the diamonds) switches the object to surface motion; from then on **I** keys these instead of position and rotation.
    * The motion follows the surface exactly (also through a throat) and doesn't distort the object.
    * Moving the object by hand moves its rest pose, i.e. the whole motion.
  * Example: `examples/rod_through_wormhole.4dscene.json`, a rod travelling through the double wormhole (Render > Render animation).
* **Extrapolation** (timeline combo next to Interpolation, or right-click keys): what a value does before its first and after its last key.
  * **Hold** stays at the key.
  * **Linear** keeps moving at the speed the curve has there, like Blender's linear extrapolation: a spin keeps turning, a move keeps going.
  * Bezier end keys then point along their segment. Visibility and other on/off values always hold.
* **Graph editor** (Graph button): every value's curve; drag keys (frame and value) and bezier handles (handles you move stop being automatic); wheel zooms time (Ctrl: values), middle-drag pans, "Frame all" / Home fits the curves.
* **In Explore**, **P** plays / pauses the animation while you walk around and **Shift+P** plays it from the first frame; it repeats indefinitely, starting over when it reaches the last keyframe.
* **Camera view** (timeline button, or **Ctrl+Numpad 0**): the viewport shows the active camera, to preview the animation; navigating leaves it.
* Animation is saved in the scene, and its changes can be undone.

## Rendering images and videos

* **Render window** (Render menu): renders from the **active camera**, or from the current view
  when the scene has no camera. The camera's depth of field is used.
  * **F12**: render an image of the current frame. **Ctrl+F12**: render the animation.
  * **Resolution** (with common sizes under "..."), **anti-aliasing** (renders 2× / 3× larger and
    averages down), and **samples**. Samples are stochastic samples per pixel: soft shadows, rough
    reflections, glass that reflects or refracts at random, and depth of field. 1 gives the plain image.
  * Quality (march steps, bounces, shadows, AO, lighting) comes from Settings > Rendering, or from a preset.
  * **Still image:** PNG or JPEG.
  * **Animation:** GIF, AVI (Motion JPEG; plays without extra codecs), PNG sequence (`name_0001.png`, ...),
    or MP4 (H.264) / WebM (VP9) through **ffmpeg** if it is installed. Set the ffmpeg command or path in
    the window, where "found / not found" shows whether it runs.
  * Frame range and fps come from the timeline, or can be set in the window.
  * The output file defaults to the scene's folder (`scene_render.png`, `scene_animation.gif`). The
    extension follows the chosen format.
* Rendering runs in the background: the app stays usable, with a progress bar, a preview of the
  image being refined, and **Cancel**. A cancelled animation keeps the frames rendered so far. The
  render uses the scene as it was when it started, so editing meanwhile does not change it.
* **Render presets** (in the Render window and Settings > Rendering): named sets of all rendering
  settings. That covers quality, lighting, sky, fast mode and viewport samples, plus the Render
  window's resolution, anti-aliasing, samples, formats and frame range. Output paths are not
  included.
  * **Load** (or double-click in the list), **Save as**, **Rename**, **Delete**, and **Export / Import** as `.json` files to share.
  * User presets are stored in the settings folder (`presets/`).
  * **Fast / Balanced / Full** are built in and read-only. They set quality and image size, and leave the look (light, sky, fog) and the output format as they are.

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
    * Presets: load / save / rename / delete / import / export render presets (see Rendering images and videos).
  * **Keys**: every shortcut and movement key can be rebound (two keys per action). Click a key and press the new one; right-click clears it. A red "!" marks keys that clash with another action in the same mode. "Reset all to defaults" restores the keys listed in this README.

## Performance

* **Per-scene shaders.** Each scene structure (the tree, operations, modifier
  types, primitives) is compiled into its own GLSL program. Every number
  (positions, colours, materials, sizes, extrusion lengths, repeat spacing,
  shell / rounding, texture type, honeycomb style) is node data, so editing
  values never recompiles. When the structure changes, the new program is
  compiled on a background thread (OpenGL: a hidden shared context; Vulkan:
  glslang and pipeline creation) while the last image stays on screen with
  "Compiling shader..." in the viewport. The last 16 programs are kept.
* **Render threads.** The viewport and the thumbnail each render on their own
  thread (OpenGL: a shared context per thread; Vulkan: their own command pools,
  with one lock around the queue), so a slow image never freezes the UI or the
  other view. The UI posts a snapshot of what to draw; the thread renders it
  (large images tile by tile, each tile its own GPU submission), copies the
  finished image into one of three display images, and the UI shows the newest.
  A display image is reused only after the UI frames that drew it have finished
  on the GPU. Refinement samples leave the GPU idle as long as they used it, so
  the UI always gets its turn. Offline renders and headless screenshots still
  render on the UI thread (`--threads` forces the threads in headless runs;
  `FD_FRAME_STATS=1` prints UI frame times at exit).
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
src/renderer.*, gl.*          renderer (shared part), OpenGL / Vulkan backends, function loader
src/media.*                   PNG / JPEG / GIF / AVI writers, ffmpeg pipe
src/render_job.cpp            offline renders (Render window)
src/space.*                   curved spaces: hyperboloid / 3-sphere model, isometries, geodesics
src/polytope.*                regular polyhedra and honeycombs
src/app.*                     state, tools, undo, explore movement, thumbnail
src/ui*.cpp, ui_common.hpp    ImGui interface (ui.cpp core, ui_toolbar / ui_library / ui_tree /
                              ui_properties / ui_viewport / ui_windows / ui_timeline / ui_render panels)
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
* **Autosave and recovery:** while there are unsaved changes, a recovery copy is written every 60 seconds (Settings > Edit mode; 0 turns it off). Saving or quitting normally removes it. If 4DCAD crashes or is killed, the next start asks whether to **recover** the changes (they come back as unsaved, and Save writes them to the original file) or **discard** them.
* Your library, settings and the recovery copy live in `%APPDATA%\4DCAD`
  (Windows) or `~/.config/4dcad` (Linux).
