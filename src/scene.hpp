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

namespace fd {

enum class NodeKind { Leaf, Group };
enum class BoolOp { Union, Intersection, Subtraction, SmoothUnion, SmoothIntersection, SmoothSubtraction };
inline const char* BOOL_OP_NAMES[6] = {"union", "intersection", "subtraction", "smoothUnion", "smoothIntersection", "smoothSubtraction"};
inline const char* BOOL_OP_LABELS[6] = {"Union", "Intersection", "Subtraction", "Smooth union", "Smooth intersection", "Smooth subtraction"};
inline bool isSmooth(BoolOp o) { return o >= BoolOp::SmoothUnion; }

// Modifier stack of a node, listed in the order they are applied to the shape
// (first = innermost). Dimension-raising modifiers (Extrude, Revolve) each add
// the next axis (X -> Y -> Z -> W) after the node's base dimension.
enum ModType { MOD_MIRROR = 1, MOD_REPEAT = 2, MOD_EXTRUDE = 3, MOD_REVOLVE = 4, MOD_ROTATE = 5 };
inline const char* MOD_NAMES[6] = {"", "Mirror", "Repeat", "Extrude", "Revolve", "Rotate"};

struct Modifier {
  int type = MOD_MIRROR;
  int axis = 0;          // mirror / repeat axis; revolve: existing axis spun into the new one (-1 = newest)
  double spacing = 3;    // repeat
  int count = 2;         // repeat: copies on each side, 0 = infinite
  ExtrudeMode mode = ExtrudeMode::Finite;  // extrude
  double length = 2;     // extrude
  double radius = 0;     // revolve
  int i = 0, j = 1;      // rotate plane
  double angle = 45;     // rotate (degrees)
  bool raises() const { return type == MOD_EXTRUDE || type == MOD_REVOLVE; }
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
};

// Surface material of a leaf. Textures are procedural 4D solid textures evaluated in
// the object's local coordinates, so they move and rotate with the object.
enum TextureType { TEX_NONE = 0, TEX_CHECKER, TEX_STRIPES, TEX_GRID, TEX_NOISE, TEX_RINGS, TEX_COUNT };
inline const char* TEXTURE_NAMES[TEX_COUNT] = {"None", "Checker", "Stripes", "Grid", "Noise", "Rings"};
struct Material {
  double emissive = 0;   // 0..1: always lit, casts no shadow, does not light others
  int texture = TEX_NONE;
  double texScale = 1;   // pattern cells per unit
  double texStrength = 0.7;
  std::array<double, 3> color2{0.12, 0.12, 0.14};  // second texture colour
};

/** Default code of a custom SDF leaf (GLSL). */
inline const char* DEFAULT_CUSTOM_SDF =
    "// Signed distance in the object's local 4D space.\n"
    "// Helpers: opExtrude(d, h), sU/sI/sS(a, b, k) smooth booleans, INF.\n"
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
  std::optional<Anchor> anchor;
  // leaf
  std::string prim;
  std::array<double, 3> color{0.8, 0.5, 0.3};
  double thickness = 0.1;  // extent along unused axes for objects with dim < 4
  std::optional<double> round, shell;
  Material mat;
  std::string code;        // custom SDF (prim == "custom"): GLSL defining float sdf(vec4 p)
  double codeBound = 1.5;  // custom SDF: half size of its bounding box (used for picking / collisions)
  bool isCustom() const { return kind == NodeKind::Leaf && prim == "custom"; }
  // group
  BoolOp op = BoolOp::Union;
  double k = 0.3;
  std::vector<Node> children;

  bool isLeaf() const { return kind == NodeKind::Leaf; }
};

struct CameraState {
  V4 target{0, 0.5, 0, 0};
  double distance = 8, yaw = 0.6, pitch = -0.35;
  M4 rot4 = identity();
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
  float geoMaxStep = 0.25f;
  int geoProjectIters = 1;
};

std::string newId(const char* prefix = "n");
Node makeLeaf(const std::string& prim, const std::string& name, V4 pos = {0, 0, 0, 0}, std::array<double, 3> color = {0.8, 0.5, 0.3});
Node makeGroup(BoolOp op, std::vector<Node> children, const std::string& name = "");
Node makeCustom(const std::string& name, const std::string& code = DEFAULT_CUSTOM_SDF);

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

struct SceneFile {
  Node root;
  std::vector<PrimitiveDef> primitives;
  std::string world;  // empty = none
  std::optional<CameraState> camera;
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
std::string sceneToJson(const Node& root, const PrimitiveRegistry& reg, const std::string& world, const CameraState* cam);
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
