// Scene model: a tree of boolean groups and primitive leaves with 4D
// transforms, modifiers and optional anchors to a "world" surface.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "math4.hpp"
#include "primitives.hpp"
#include "polytope.hpp"
#include "space.hpp"

namespace fd {

enum class NodeKind { Leaf, Group, Camera };
enum class BoolOp { Union, Intersection, Subtraction, SmoothUnion, SmoothIntersection, SmoothSubtraction };
inline const char* BOOL_OP_NAMES[6] = {"union", "intersection", "subtraction", "smoothUnion", "smoothIntersection", "smoothSubtraction"};
inline const char* BOOL_OP_LABELS[6] = {"Union", "Intersection", "Subtraction", "Smooth union", "Smooth intersection", "Smooth subtraction"};
inline bool isSmooth(BoolOp o) { return o >= BoolOp::SmoothUnion; }

// Modifier stack of a node, listed in the order they are applied to the shape
// (first = innermost). Dimension-raising modifiers (Extrude, Revolve) each add
// the next axis (X -> Y -> Z -> W) after the node's base dimension.
// MOD_RADIAL replaced the old Rotate modifier (an old rotation = one radial copy at its angle).
// MOD_DEF: a data-driven modifier (defs.hpp): code + parameters, `def` names its definition. The
// five engine types are built-in definitions too (with native code paths), named by their type.
enum ModType { MOD_MIRROR = 1, MOD_REPEAT = 2, MOD_EXTRUDE = 3, MOD_REVOLVE = 4, MOD_RADIAL = 5, MOD_DEF = 6 };
inline const char* MOD_NAMES[7] = {"", "Mirror", "Repeat", "Extrude", "Revolve", "Radial repeat", "Modifier"};

struct Modifier {
  int type = MOD_MIRROR;
  int axis = 0;          // mirror / repeat axis; revolve: existing axis spun into the new one (-1 = newest)
  double spacing = 3;    // repeat
  int count = 2;         // repeat: copies on each side, 0 = infinite; radial: number of copies (>= 1)
  ExtrudeMode mode = ExtrudeMode::Finite;  // extrude
  double length = 2;     // extrude
  double radius = 0;     // revolve; radial: distance of the copies from the axis
  int i = 0, j = 2;      // radial: plane of rotation (i -> j)
  double angle = 60;     // radial: degrees between copies
  double startAngle = 0; // radial: angle of the first copy (degrees)
  double offset = 0;     // mirror: shape and mirror image sit this far from the mirror plane
  // (new fields go last: code brace-initialises the leading ones)
  std::string def;             // MOD_DEF: the definition's id
  std::vector<double> params;  // MOD_DEF: parameter values (packed scalars, see defs.hpp)
  /** Adds a dimension (Extrude, Revolve, or a definition flagged so). */
  bool raises() const;
  bool operator==(const Modifier& o) const {
    return type == o.type && axis == o.axis && spacing == o.spacing && count == o.count && mode == o.mode && length == o.length &&
           radius == o.radius && i == o.i && j == o.j && angle == o.angle && startAngle == o.startAngle && offset == o.offset && def == o.def &&
           params == o.params;
  }
};

struct Transform {
  V4 pos{0, 0, 0, 0};
  M4 rot = identity();  // rotation matrix (columns = rotated axes)
  double scale = 1;
};

struct Anchor {
  std::string world;     // id of the world node
  M4 basis = identity(); // columns: tangent1, tangent2, tangent3, normal at xf.pos
  double reach = 1e4;    // validity radius of the tangent-space approximation
  // Follow surface: the object's local X axis bends along the surface geodesic through its
  // anchor (a long rod lies along the surface, through a wormhole's throat ...)
  bool follow = false;
  // Surface motion (animatable): from a rest pose, turn by Heading (degrees, about local Y), walk
  // Travel along the surface (local X, geodesic, frame carried along), roll by Spin (about local X)
  bool hasRest = false;
  V4 restPos{0, 0, 0, 0};
  M4 restFrame = identity();  // object frame at rest (basis * rotation; columns X, Y, Z, normal)
  double travel = 0, heading = 0, spin = 0;
  // cache (not saved, rebuilt by App::rebuild): the follow path, NPATH+1 samples from -len to +len
  struct PathPt { V4 pos, y, z, n; };
  std::vector<PathPt> path;
  double pathLen = 0;
};
constexpr int NPATH = 32;  // follow-surface path: segments

// Surface material of a leaf. Every channel can take a texture (a definition: code + parameters,
// see defs.hpp), evaluated at the point's texture coordinates (the slot's mapping).
enum TextureType { TEX_NONE = 0, TEX_CHECKER, TEX_STRIPES, TEX_GRID, TEX_NOISE, TEX_RINGS,
                   TEX_DOTS, TEX_WAVES, TEX_MARBLE, TEX_WOOD, TEX_CELLS, TEX_BRICKS, TEX_GRADIENT, TEX_COUNT };  // (old files)
enum TexChannel { CH_COLOR = 0, CH_ROUGHNESS, CH_METALLIC, CH_SPECULAR, CH_EMISSION, CH_TRANSPARENCY, CH_BUMP, CH_COUNT };
inline const char* CHANNEL_NAMES[CH_COUNT] = {"Colour", "Roughness", "Metallic", "Specular", "Emission", "Transparency", "Bump"};
inline const char* CHANNEL_IDS[CH_COUNT] = {"color", "roughness", "metallic", "specular", "emission", "transparency", "bump"};
// denoisers of refined images (path tracing and simple lighting), guided by the surfaces' colour / depth
inline const char* DENOISE_NAMES[5] = {"Off", "Edge-aware (wavelet)", "Bilateral", "Median (fireflies)", "Median + edge-aware"};
// debug views of the viewport: one material value at each point (shaders.hpp debugView)
constexpr int VIEW_COUNT = 11;
inline const char* VIEW_NAMES[VIEW_COUNT] = {"Lit", "Colour", "Roughness", "Metallic", "Specular", "Emission", "Transparency", "Bump relief", "Normals",
                                             "Colour texture mask", "Cost (SDF evaluations)"};
enum TexSpace { TS_OBJECT = 0, TS_WORLD, TS_PARENT, TS_COUNT };
inline const char* TEX_SPACE_NAMES[TS_COUNT] = {"Object", "World", "Parent"};

// Where a texture is evaluated: tex coordinate = scale * axisScale * rotation^-1 * (x - offset), with x
// the point in the chosen space (curved spaces: object space).
struct TexMapping {
  int space = TS_OBJECT;
  std::array<double, 4> offset{0, 0, 0, 0};
  std::array<double, 6> rot{0, 0, 0, 0, 0, 0};  // degrees in the planes XY XZ XW YZ YW ZW
  double scale = 1;                              // cells per unit
  std::array<double, 4> axisScale{1, 1, 1, 1};
  bool operator==(const TexMapping& o) const {
    return space == o.space && offset == o.offset && rot == o.rot && scale == o.scale && axisScale == o.axisScale;
  }
  bool operator!=(const TexMapping& o) const { return !(*this == o); }
};

constexpr int TEX_CURVE_MAX = 8;
struct TexSlot {
  std::string def;             // texture definition id ("" = no texture)
  std::vector<double> params;  // its parameter values (see defs.hpp)
  double strength = 1;         // how much the texture changes the channel
  std::array<double, 3> color2{0.12, 0.12, 0.14};  // colour channel: the colour a mask blends to
  double value2 = 1;           // other channels: the value where the mask is 1 (bump: relief height)
  // remap of the mask: low..high -> 0..1, contrast (S-curve, -1..1), invert, then the curve
  double low = 0, high = 1, contrast = 0;
  bool invert = false;
  std::vector<std::array<double, 2>> curve;  // points (in, out) sorted by in; < 2 points = straight
  TexMapping map;
  int mapFrom = -1;  // use the mapping of this channel instead of its own
  bool used() const { return !def.empty(); }
  bool operator==(const TexSlot& o) const {
    return def == o.def && params == o.params && strength == o.strength && color2 == o.color2 && value2 == o.value2 && low == o.low &&
           high == o.high && contrast == o.contrast && invert == o.invert && curve == o.curve && map == o.map && mapFrom == o.mapFrom;
  }
  bool operator!=(const TexSlot& o) const { return !(*this == o); }
};

struct Material {
  double emissive = 0;   // 0..1: always lit, casts no shadow, does not light others
  std::array<double, 3> color2{0.12, 0.12, 0.14};  // honeycombs: the alternate colour
  double roughness = 0.75;   // 0 = sharp highlights, 1 = matte
  double specular = 0.35;    // highlight strength
  double transparency = 0;   // 0 = opaque, 1 = invisible (see-through surfaces)
  double ior = 1.45;         // index of refraction of transparent materials (1 = no bending)
  double metallic = 0;       // 0 = plain, 1 = mirror (reflections tinted by the colour)
  double emissionStrength = 4;  // path-traced lighting: emitted light = colour x emissive x this
  std::array<TexSlot, CH_COUNT> tex;  // a texture per channel
  bool textured() const { for (auto& t : tex) if (t.used()) return true; return false; }
};
/** The slot whose mapping channel ch uses (follows mapFrom, no cycles). */
int texMappingChannel(const Material& m, int ch);
/** A colour-channel slot made from an old texture type (texScale, strength, colour 2). */
TexSlot legacyColorSlot(int type, double scale, double strength, const std::array<double, 3>& color2);

/** Default code of a custom SDF leaf (GLSL). */
inline const char* DEFAULT_CUSTOM_SDF =
    "// Signed distance in the object's local 4D space.\n"
    "// Helpers: opExtrude(d, h), opRadial(v, r, n, start, step), sU/sI/sS(a, b, k) smooth booleans, INF.\n"
    "float sdf(vec4 p) {\n"
    "  // a hypersphere of radius 1 with a wavy surface\n"
    "  return length(p) - 1.0 + 0.05 * sin(8.0 * p.x) * sin(8.0 * p.y);\n"
    "}\n";

struct Node {
  std::string id, name;
  NodeKind kind = NodeKind::Leaf;
  Transform xf;
  std::vector<Modifier> mods;
  bool hidden = false;
  bool locked = false;   // cannot be selected / moved in the viewport
  std::string linkId;    // linked duplicates share one: editing one edits all (except their own transform)
  std::optional<Anchor> anchor;
  // leaf
  std::string prim;
  std::array<double, 3> color{0.8, 0.5, 0.3};
  double thickness = 0.1;  // extent along unused axes for objects with dim < 4
  // (round / shell are modifiers now: Round, Onion; old files convert on load)
  Material mat;
  std::string code;        // custom SDF (prim == "custom"): GLSL defining float sdf(vec4 p)
  // camera: position = eye, rotation = view frame (right, up, forward, slider direction);
  // attached to the world surface (anchor) = a Geodesic camera
  double camFov = 60;      // degrees
  int camSlider = 3;       // Full 4D: world axis on the slice slider
  double camAperture = 0;  // depth of field: lens radius (0 = everything sharp)
  double camFocus = 5;     // distance in focus
  double codeBound = 1.5;  // custom SDF: half size of its bounding box (used for picking / collisions)
  bool isCustom() const { return kind == NodeKind::Leaf && prim == "custom"; }
  // regular polyhedron with flat (geodesic) faces / regular honeycomb (see polytope.hpp)
  PolyParams poly;
  bool isPoly() const { return kind == NodeKind::Leaf && prim == "polyhedron"; }
  bool isHoneycomb() const { return kind == NodeKind::Leaf && prim == "honeycomb"; }
  bool isPolytope() const { return isPoly() || isHoneycomb(); }
  // group
  BoolOp op = BoolOp::Union;
  double k = 0.3;
  std::vector<Node> children;

  bool isLeaf() const { return kind == NodeKind::Leaf; }
  bool isGroup() const { return kind == NodeKind::Group; }
  bool isCamera() const { return kind == NodeKind::Camera; }
};

struct CameraState {
  V4 target{0, 0.5, 0, 0};
  double distance = 8, yaw = 0.6, pitch = -0.35;
  M4 rot4 = identity();
  int slider = 3;  // world axis on the slice slider (the other three are shown)
};

/** What a frame draws and how hard it works. Edit mode builds one from the main settings; the
 *  fast profile is used in Explore (and, if chosen, while moving in Edit mode). */
struct RenderProfile {
  float resolution = 0.5f;  // x the window resolution
  int maxSteps = 100;       // march steps per ray
  int bounces = 0;          // reflections / refractions followed per pixel (mirrors, glass)
  bool textures = false;    // procedural textures
  bool specular = true;     // highlights
  bool pointLights = true;  // point lights (incl. emissive objects)
  bool shadows = true;      // sun shadows
  bool pointShadows = false;
  bool ao = false;          // ambient occlusion
  int aoSamples = 3;        // 1..5
  int shadowSteps = 24;     // max steps of a shadow ray
  int upscale = 1;          // below full resolution: 0 = bilinear, 1 = sharp (bicubic + sharpening)
  // dynamic resolution (the fast profile): lowers / raises the resolution to hold a frame rate
  bool dynamicRes = false;
  int targetFps = 60;
  float minRes = 0.3f;      // lowest resolution it may go to (x the window)
  enum Preset { Fast, Balanced, Full };
  static RenderProfile preset(Preset p) {
    RenderProfile r;  // Fast
    if (p == Balanced) { r.resolution = 0.75f; r.maxSteps = 128; r.bounces = 1; r.textures = true; r.ao = true; r.shadowSteps = 40; }
    if (p == Full) { r.resolution = 1.0f; r.maxSteps = 160; r.bounces = 3; r.textures = true; r.pointShadows = true; r.ao = true; r.aoSamples = 5; r.shadowSteps = 64; }
    return r;
  }
};

/** Ray-marching / rendering settings. */
struct QualitySettings {
  float resolutionScale = 1.0f;   // when idle
  float motionScale = 0.5f;       // while the camera / objects move
  int maxSteps = 160;
  float maxDistFull = 120;        // Full 4D: rays longer than this show the sky
  float maxDistGeo = 60;          // Geodesic: same, measured along the surface
  float eps = 0.0015f;            // surface hit precision
  float stepScale = 1.0f;         // < 1 = safer marching for inexact SDFs (custom code, repeats)
  bool shadows = true;
  float shadowK = 10;             // shadow sharpness
  float shadowMax = 12;           // shadow ray length
  bool ao = true;
  float aoStrength = 1.5f;
  float fog = 1.0f;
  float ambient = 0.18f;
  float lightYaw = 2.18f, lightPitch = 0.93f, lightW = 0.25f;  // light direction
  std::array<float, 3> skyTop{0.32f, 0.38f, 0.5f}, skyBottom{0.16f, 0.17f, 0.2f};
  bool grid = true;               // ground grid in Edit mode
  float sunIntensity = 1.0f;      // directional light
  bool pointShadows = true;       // point lights cast shadows (each one costs a shadow ray)
  bool emissiveLights = true;     // emissive objects light their surroundings (as point lights)
  int transparentLayers = 3;      // reflections / refractions followed per pixel (0 = mirrors and glass off)
  bool relaxedMarching = true;    // over-relaxed sphere tracing (fewer steps)
  // stochastic sampling (random per sample, averaged): viewport refinement when idle, renders
  bool stochastic = true;         // refine the viewport while nothing changes
  int viewportSamples = 64;       // samples per pixel the viewport refines to
  float sunSize = 0.04f;          // radians: soft sun shadows
  float lightRadius = 0.15f;      // point lights are balls of this radius: soft shadows
  // fast mode: used in Explore (keep it smooth) and optionally while moving in Edit mode
  RenderProfile fast = RenderProfile::preset(RenderProfile::Fast);
  bool fastWhileMovingInEdit = false;
  int frameLimit = 0;             // frames per second at most (0 = the display's rate); lower = less GPU load
  float geoMaxStep = 0.25f;
  int geoProjectIters = 1;
  // lighting model: 0 = simple (sun, point lights, ambient light; emissive objects shine as point
  // lights), 1 = path traced (physically based: light bounces, glowing objects are area lights,
  // no ambient light). Path tracing is used where images are refined (idle viewport, renders);
  // moving / fast views use the simple model.
  int lighting = 0;
  float skyIntensity = 1.0f;      // path traced: brightness of the sky's light (0 = black)
  int debugView = 0;              // the viewport shows one material value (VIEW_NAMES); not saved
  float costScale = 512;          // the Cost view: evaluations per pixel shown as red
  int upscale = 1;                // below full resolution: 0 = bilinear, 1 = sharp
  // path traced images: display (applied after averaging the samples)
  float exposure = 0.0f;          // stops (x 2^exposure)
  int tonemap = 1;                // 0 = clip, 1 = filmic (ACES): bright light rolls off instead of burning out
  int denoise = 1;                // DENOISE_NAMES: 0 off, 1 edge-aware, 2 bilateral, 3 median, 4 median + edge-aware
  float denoiseStrength = 1.0f;
  // Full 4D: light from a glowing object falls off with distance^3 (physical, 4D) or like in 3D
  // (distance^2: brightness behaves as you would expect from the real world)
  int light4D = 1;                // 0 = physical (4D), 1 = like 3D
};

std::string newId(const char* prefix = "n");
Node makeLeaf(const std::string& prim, const std::string& name, V4 pos = {0, 0, 0, 0}, std::array<double, 3> color = {0.8, 0.5, 0.3});
Node makeGroup(BoolOp op, std::vector<Node> children, const std::string& name = "");
Node makeCustom(const std::string& name, const std::string& code = DEFAULT_CUSTOM_SDF);
/** A camera object at eye looking along the frame's third column (columns: right, up, forward, slider). */
Node makeCamera(const std::string& name, const V4& eye, const M4& frame, double fov, int slider);

/** Full rotation of a node: anchor basis (if any) followed by its own rotation. */
M4 nodeRotation(const Node& n);

void walk(Node& n, const std::function<void(Node&, Node* parent, int depth)>& fn, Node* parent = nullptr, int depth = 0);
void walkc(const Node& n, const std::function<void(const Node&, const Node* parent, int depth)>& fn, const Node* parent = nullptr, int depth = 0);
Node* findNode(Node& root, const std::string& id);
Node* findParent(Node& root, const std::string& id);
std::vector<Node*> pathTo(Node& root, const std::string& id);
bool isAncestor(Node& root, const std::string& anc, const std::string& id);
std::optional<Node> removeNode(Node& root, const std::string& id);
Node cloneFresh(const Node& n);

// ---- transforms between frames (uniform scale: x_parent = pos + scale * rot * x_local) ----
/** The transform of `child` (given in `parent`'s local frame) expressed in the frame `parent` lives in. */
Transform composeXf(const Transform& parent, const Transform& child);
/** Inverse of composeXf: `world` expressed in the local frame of `parentWorld`. */
Transform relativeXf(const Transform& parentWorld, const Transform& world);
/** World transform of a node, composed along its path. nullopt if the node is missing or an
 *  anchored node lies on the path (anchors live in surface tangent frames: no plain conversion). */
std::optional<Transform> worldXf(Node& root, const std::string& id);
/** Moves a group's origin to its first child's position without moving anything in the world
 *  (the first child ends up at 0,0,0,0). Returns false if nothing could be done (no children, anchors). */
bool centreGroupOnFirst(Node& g);
/** Removes (recursively, bottom-up) every group below the root that has no children. */
void pruneEmptyGroups(Node& root);

// ---- editing several objects at once ----
/** Structural change of a modifier stack done in the Properties panel. */
struct ModStackOp {
  enum Kind { None, Add, Remove, MoveUp } kind = None;
  int index = -1;
  Modifier mod;  // Add
};
/** Same modifier types in the same order. */
bool sameModifierTypes(const Node& a, const Node& b);
/** Applies to `target` what changed between `before` and `after` (one frame of edits on the
 *  primary object): changed fields are copied, position / rotation / scale changes are applied
 *  relatively (offset, rotation about the target's own centre, scale ratio). Leaf-only fields
 *  only reach leaves and group-only fields only groups. Modifiers are edited only when
 *  `stacksMatch` (all selected stacks have the same types). */
void propagateEdit(const Node& before, const Node& after, Node& target, bool stacksMatch, const ModStackOp& op = {});

class PrimitiveRegistry {
 public:
  PrimitiveRegistry();
  const PrimitiveDef* get(const std::string& id) const;
  void set(const PrimitiveDef& p);
  void erase(const std::string& id);
  std::vector<const PrimitiveDef*> all() const;
  std::vector<PrimitiveDef> custom() const;

 private:
  std::vector<PrimitiveDef> list_;
};

/** Base dimension of a node before its own modifiers (leaf: primitive dim, group: max child dim). */
int nodeBaseDim(const Node& n, const PrimitiveRegistry& reg);
/** Per-modifier resolved axes: invalid raises (beyond 4D, revolve of a point) are ignored. */
struct ModAxis { bool valid = true; int newAxis = -1; int srcAxis = -1; };
std::vector<ModAxis> modifierAxes(const Node& n, const PrimitiveRegistry& reg);
/** A point leaf shaped like the named built-in primitive (cube, sphere, ...). */
Node makeShape(const std::string& builtinId, const std::string& name, V4 pos = {0, 0, 0, 0}, std::array<double, 3> color = {0.8, 0.5, 0.3});
/** Dimension after the node's modifiers (capped at 4). */
int nodeDim(const Node& n, const PrimitiveRegistry& reg);
/** Problems in a node's modifier stack (e.g. more than 4 dimensions). Index -> message. */
std::vector<std::pair<int, std::string>> modifierProblems(const Node& n, const PrimitiveRegistry& reg);
/** Convert a primitive recipe into modifiers on a point leaf. */
std::vector<Modifier> stepsToModifiers(const std::vector<Step>& steps);
Node nodeFromPrimitive(const PrimitiveDef& p, const std::string& name);

/** A point light (a 4D position; light falls off to 0 at `range`). */
struct PointLight {
  std::string id;  // stable id (animation tracks refer to it); given when created / loaded
  std::string name = "Light";
  V4 pos{0, 3, 0, 0};
  std::array<double, 3> color{1, 0.95, 0.85};
  double intensity = 1.5;
  double range = 10;
  bool shadows = true;
  bool enabled = true;
};

/** A saved camera position (Full 4D camera, plus the Geodesic camera if it was used). */
struct CameraBookmark {
  std::string name;
  CameraState cam;
  bool geo = false;
  V4 geoPivot{0, 0, 0, 0};
  M4 geoFrame = identity();
};

/** Editor camera of a curved scene: its isometry (columns: right, up, forward at the eye; eye)
 *  and the distance to the point it orbits. */
struct CurvedCamState {
  M4 iso = identity();
  double distance = 8;
};

// The scene's look: sun, ambient light, fog and sky. Saved with the scene (Settings > Scene) and
// undoable; while editing, the values live in QualitySettings (what the renderer reads).
struct SceneLook {
  float lightYaw = 2.18f, lightPitch = 0.93f, lightW = 0.25f;  // sun direction
  float sunIntensity = 1.0f, sunSize = 0.04f;
  float ambient = 0.18f, fog = 1.0f;
  std::array<float, 3> skyTop{0.32f, 0.38f, 0.5f}, skyBottom{0.16f, 0.17f, 0.2f};
  float skyIntensity = 1.0f;
};
SceneLook lookOf(const QualitySettings& q);
void applyLook(QualitySettings& q, const SceneLook& l);
std::string lookToJson(const SceneLook& l);
SceneLook lookFromJson(const std::string& text);  // (defaults for what is missing / bad data)

struct SceneFile {
  Space space;                             // Flat 4D (default), Hyperbolic or Spherical 3D
  std::optional<CurvedCamState> curvedCam;
  std::vector<CameraBookmark> bookmarks;  // old files only: converted to camera objects when loaded
  std::string activeCamera;               // id of the camera used for rendering
  std::string animation;                  // animation (JSON, see animation.hpp); empty = none
  std::vector<PointLight> lights;
  Node root;
  std::vector<PrimitiveDef> primitives;
  std::string world;  // empty = none
  std::optional<CameraState> camera;
  std::optional<SceneLook> look;  // older files: none (the defaults)
};

struct LibraryObject {
  std::string id, name;
  Node node;
};

struct LibraryFolder {
  std::string id, name;
  bool builtin = false;  // read-only built-in folders
  std::vector<LibraryObject> objects;
};

/** Built-in folders (basic 0D-2D, 3D, 4D shapes made of a point + modifiers). */
std::vector<LibraryFolder> builtinFolders();

// JSON text (de)serialisation; parse functions throw std::runtime_error.
std::string sceneToJson(const Node& root, const PrimitiveRegistry& reg, const std::string& world, const CameraState* cam,
                        const std::vector<CameraBookmark>* bookmarks = nullptr, const std::vector<PointLight>* lights = nullptr,
                        const std::string& activeCamera = "", const std::string* animationJson = nullptr,
                        const Space* space = nullptr, const CurvedCamState* curvedCam = nullptr, const SceneLook* look = nullptr);
std::string lightsToJson(const std::vector<PointLight>& l);
std::vector<PointLight> lightsFromJson(const std::string& text);
std::string bookmarksToJson(const std::vector<CameraBookmark>& b);
std::vector<CameraBookmark> bookmarksFromJson(const std::string& text);

// ---- linked duplicates ----
/** The shared part of a linked node (shape, material, modifiers, children), without ids,
 *  the node's own transform, name, visibility, lock or anchor. Equal strings = same definition. */
std::string linkDefinition(const Node& n);
/** Copies src's definition onto dst, keeping dst's ids where the structure matches. */
void copyDefinition(Node& dst, const Node& src);
/** Makes every linked group consistent: for each linkId, the member whose definition changed
 *  since `defs` (last known definitions) is copied to the others. Returns true if anything changed. */
bool syncLinks(Node& root, std::map<std::string, std::string>& defs);
/** Records the current definitions (after loading / undo). */
void recordLinks(Node& root, std::map<std::string, std::string>& defs);
SceneFile sceneFromJson(const std::string& text);
std::string nodeToJson(const Node& n);  // compact snapshot (undo)
Node nodeFromJson(const std::string& text);
LibraryObject makeLibraryObject(const Node& n, const PrimitiveRegistry& reg, const std::string& name);
std::string libraryObjectToJson(const LibraryObject& o);

struct LibraryImport {
  std::vector<LibraryFolder> folders;   // full library files
  std::vector<LibraryObject> objects;   // single objects / old primitives
  std::string description;
};
LibraryImport importLibraryJson(const std::string& text);
std::string libraryToJson(const std::vector<LibraryFolder>& folders);

}  // namespace fd
