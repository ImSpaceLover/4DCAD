# Changelog

Versions follow `project(4DCAD VERSION ...)` in CMakeLists.txt; `4DCAD --version` and Help show the
running one, and scene files record the version that saved them (`appVersion`). Pushing a tag
`v<version>` builds the release (see README > Releases).

## 1.0

The first numbered release. Everything below is new compared with the unversioned builds.

### Modelling
- 4D CSG with ray-marched signed distance fields: every shape is a point plus a stack of modifiers;
  groups combine with union / intersection / subtraction and their smooth versions.
- **Data-driven modifiers:** every modifier is a definition (code in a GLSL subset + parameters
  edited with sliders, checkboxes, drop-down lists and colour pickers, all keyable). The same code
  runs in the shader and exactly on the CPU (picking, placement, collisions, bounds) through a
  built-in interpreter.
  - Extrude, Revolve, Mirror, Repeat, Radial repeat.
  - **Round** and **Onion** (replaces Shell; can be stacked for several skins).
  - From Inigo Quilez's SDF operators, adapted to 4D: **Elongate, Twist, Bend, Displace, Noise
    displace, Taper, Soft mirror, Stretch**.
- Custom SDF objects (GLSL), now exact on the CPU too.
- Regular 4D polytopes (5-, 8-, 16-, 24-, 120-, 600-cell), polyhedra and regular honeycombs.
- Curved spaces: Hyperbolic and Spherical 3D scenes with a full editor, the Poincaré ball view.
- World surfaces with the Geodesic 3D view; objects attached to and following surfaces.

### Materials and rendering
- **Textures on every channel:** colour, roughness, metallic, specular, emission, transparency and
  **bump**. Textures are definitions too (masks, colours or colour + coverage), with remap (range,
  contrast, invert, curve) and mapping (object / world / parent space, offset, rotation in all six
  4D planes, per-axis scale, shared mappings, fit to object). 14 built-in textures, including a
  cosine palette and a colour gradient.
- **Texture editing mode:** "Save as texture" turns an object or group into a texture made of shapes
  (object colours or a mask; inside / outline / distance bands / falloff coverage; tiling; fills;
  exposed values as texture parameters), edited in a limited Full 4D editor with Shapes / Preview
  views.
- Vulkan and OpenGL renderers, render threads, a shader disk cache; values are shader data, so edits
  never recompile.
- Simple lighting and path-traced PBR (GGX, emissive area lights, tone mapping, denoiser).
- Render window: stills and animations (PNG, JPEG, GIF, AVI, MP4 / WebM through ffmpeg), presets.

### Editing
- Library of objects, modifiers and textures with a **definition editor** (parameter table + code,
  checked on Apply); `.4dobj.json`, `.4dmod.json` and `.4dtex.json` import / export.
- Keyframe animation of every numeric value with a timeline and graph editor.
- Cameras as scene objects, linked duplicates, copy / paste, undo with a memory cap, autosave and
  crash recovery, rebindable keys, dockable panels.

### Files
- Older scenes load unchanged: the old Round / Shell fields become Round / Onion modifiers, the old
  single texture becomes the colour slot (including their animation tracks).

### Release package
- `4DCAD-1.0-windows.zip` / `4DCAD-1.0-linux.tar.gz`: the program, README, this changelog and a
  `library` folder with example scenes, worlds, showcase scenes, every built-in modifier and
  texture as an importable file and the built-in object library.
