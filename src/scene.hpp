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

struct Modifier {
  int type = 1;  // 1 mirror, 2 repeat
  int axis = 0;
  double spacing = 3;
  int count = 2;  // repeat: copies on each side, 0 = infinite
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

struct QualitySettings {
  float resolutionScale = 1.0f;   // when idle
  float motionScale = 0.5f;       // while the camera / objects move
  int maxSteps = 160;
  bool shadows = true;
  bool ao = true;
  float geoMaxStep = 0.25f;
  int geoProjectIters = 1;
};

std::string newId(const char* prefix = "n");
Node makeLeaf(const std::string& prim, const std::string& name, V4 pos = {0, 0, 0, 0}, std::array<double, 3> color = {0.8, 0.5, 0.3});
Node makeGroup(BoolOp op, std::vector<Node> children, const std::string& name = "");

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

int nodeDim(const Node& n, const PrimitiveRegistry& reg);

struct SceneFile {
  Node root;
  std::vector<PrimitiveDef> primitives;
  std::string world;  // empty = none
  std::optional<CameraState> camera;
};

struct LibraryObject {
  std::string id, name;
  Node node;
  std::vector<PrimitiveDef> primitives;
};

// JSON text (de)serialisation; parse functions throw std::runtime_error.
std::string sceneToJson(const Node& root, const PrimitiveRegistry& reg, const std::string& world, const CameraState* cam);
SceneFile sceneFromJson(const std::string& text);
std::string nodeToJson(const Node& n);  // compact snapshot (undo)
Node nodeFromJson(const std::string& text);
std::string primitiveToJson(const PrimitiveDef& p, bool asFile);
LibraryObject makeLibraryObject(const Node& n, const PrimitiveRegistry& reg, const std::string& name);
std::string libraryObjectToJson(const LibraryObject& o);

struct LibraryImport {
  std::vector<PrimitiveDef> primitives;
  std::vector<LibraryObject> objects;
  std::string description;
};
LibraryImport importLibraryJson(const std::string& text);
std::string libraryToJson(const std::vector<PrimitiveDef>& prims, const std::vector<LibraryObject>& objs);

}  // namespace fd
