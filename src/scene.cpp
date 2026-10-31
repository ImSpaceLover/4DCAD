#include "scene.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace fd {

using json = nlohmann::json;

std::string newId(const char* prefix) {
  static std::atomic<unsigned> counter{0};
  static std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s%x%x", prefix, (unsigned)++counter, (unsigned)(rng() & 0xfffff));
  return buf;
}

Node makeLeaf(const std::string& prim, const std::string& name, V4 pos, std::array<double, 3> color) {
  Node n;
  n.id = newId("n"); n.name = name; n.kind = NodeKind::Leaf; n.prim = prim; n.color = color; n.xf.pos = pos;
  return n;
}

Node makeGroup(BoolOp op, std::vector<Node> children, const std::string& name) {
  Node n;
  n.id = newId("g"); n.kind = NodeKind::Group; n.op = op; n.children = std::move(children);
  n.name = name.empty() ? BOOL_OP_NAMES[(int)op] : name;
  return n;
}

M4 nodeRotation(const Node& n) { return n.anchor ? mul(n.anchor->basis, n.xf.rot) : n.xf.rot; }

void walk(Node& n, const std::function<void(Node&, Node*, int)>& fn, Node* parent, int depth) {
  fn(n, parent, depth);
  if (n.kind == NodeKind::Group)
    for (auto& c : n.children) walk(c, fn, &n, depth + 1);
}
void walkc(const Node& n, const std::function<void(const Node&, const Node*, int)>& fn, const Node* parent, int depth) {
  fn(n, parent, depth);
  if (n.kind == NodeKind::Group)
    for (auto& c : n.children) walkc(c, fn, &n, depth + 1);
}
Node* findNode(Node& root, const std::string& id) {
  if (root.id == id) return &root;
  for (auto& c : root.children) if (Node* r = findNode(c, id)) return r;
  return nullptr;
}
Node* findParent(Node& root, const std::string& id) {
  for (auto& c : root.children) {
    if (c.id == id) return &root;
    if (Node* r = findParent(c, id)) return r;
  }
  return nullptr;
}
std::vector<Node*> pathTo(Node& root, const std::string& id) {
  if (root.id == id) return {&root};
  for (auto& c : root.children) {
    auto p = pathTo(c, id);
    if (!p.empty()) { p.insert(p.begin(), &root); return p; }
  }
  return {};
}
bool isAncestor(Node& root, const std::string& anc, const std::string& id) {
  for (Node* n : pathTo(root, id)) if (n->id == anc) return true;
  return false;
}
std::optional<Node> removeNode(Node& root, const std::string& id) {
  Node* p = findParent(root, id);
  if (!p) return std::nullopt;
  for (size_t i = 0; i < p->children.size(); i++)
    if (p->children[i].id == id) {
      Node n = std::move(p->children[i]);
      p->children.erase(p->children.begin() + i);
      return n;
    }
  return std::nullopt;
}
Node cloneFresh(const Node& n) {
  Node c = n;
  walk(c, [](Node& m, Node*, int) { m.id = newId(m.isLeaf() ? "n" : "g"); });
  return c;
}

// ---------------------------------------------------------------- registry
PrimitiveRegistry::PrimitiveRegistry() { list_ = builtinPrimitives(); }
const PrimitiveDef* PrimitiveRegistry::get(const std::string& id) const {
  for (auto& p : list_) if (p.id == id) return &p;
  return nullptr;
}
void PrimitiveRegistry::set(const PrimitiveDef& p) {
  for (auto& q : list_) if (q.id == p.id) { if (!q.builtin) q = p; return; }
  list_.push_back(p);
}
void PrimitiveRegistry::erase(const std::string& id) {
  for (size_t i = 0; i < list_.size(); i++)
    if (list_[i].id == id && !list_[i].builtin) { list_.erase(list_.begin() + i); return; }
}
std::vector<const PrimitiveDef*> PrimitiveRegistry::all() const {
  std::vector<const PrimitiveDef*> r;
  for (auto& p : list_) r.push_back(&p);
  return r;
}
std::vector<PrimitiveDef> PrimitiveRegistry::custom() const {
  std::vector<PrimitiveDef> r;
  for (auto& p : list_) if (!p.builtin) r.push_back(p);
  return r;
}

int nodeDim(const Node& n, const PrimitiveRegistry& reg) {
  int d = 0;
  walkc(n, [&](const Node& m, const Node*, int) {
    if (m.isLeaf()) if (auto* p = reg.get(m.prim)) d = std::max(d, primitiveDim(*p));
  });
  return d;
}

// ---------------------------------------------------------------- JSON
static json v4j(const V4& v) { return json::array({v[0], v[1], v[2], v[3]}); }
static V4 jv4(const json& j) {
  V4 v{0, 0, 0, 0};
  if (j.is_array()) for (size_t i = 0; i < 4 && i < j.size(); i++) v[i] = j[i].get<double>();
  return v;
}
static json m4j(const M4& m) { json a = json::array(); for (double x : m) a.push_back(x); return a; }
static M4 jm4(const json& j) {
  M4 m = identity();
  if (j.is_array() && j.size() == 16) for (size_t i = 0; i < 16; i++) m[i] = j[i].get<double>();
  return m;
}

static json primJ(const PrimitiveDef& p) {
  json steps = json::array();
  for (auto& s : p.steps) {
    if (s.op == StepOp::Extrude)
      steps.push_back({{"op", "extrude"}, {"mode", s.mode == ExtrudeMode::Finite ? "finite" : s.mode == ExtrudeMode::Infinite ? "infinite" : "half"}, {"length", s.length}});
    else if (s.op == StepOp::Revolve) {
      json j = {{"op", "revolve"}, {"radius", s.radius}};
      if (s.axis >= 0) j["axis"] = s.axis;
      steps.push_back(j);
    } else steps.push_back({{"op", "rotate"}, {"i", s.i}, {"j", s.j}, {"angle", s.angle}});
  }
  return {{"id", p.id}, {"name", p.name}, {"description", p.description}, {"steps", steps}, {"round", p.round}, {"shell", p.shell}};
}
static PrimitiveDef jPrim(const json& j) {
  PrimitiveDef p;
  p.id = j.value("id", newId("p"));
  p.name = j.value("name", std::string("Primitive"));
  p.description = j.value("description", std::string());
  p.round = j.value("round", 0.0);
  p.shell = j.value("shell", 0.0);
  for (auto& sj : j.value("steps", json::array())) {
    Step s;
    std::string op = sj.value("op", std::string("extrude"));
    if (op == "extrude") {
      s.op = StepOp::Extrude;
      std::string m = sj.value("mode", std::string("finite"));
      s.mode = m == "infinite" ? ExtrudeMode::Infinite : m == "half" ? ExtrudeMode::Half : ExtrudeMode::Finite;
      s.length = sj.value("length", 2.0);
    } else if (op == "revolve") {
      s.op = StepOp::Revolve; s.radius = sj.value("radius", 0.0); s.axis = sj.value("axis", -1);
    } else {
      s.op = StepOp::Rotate; s.i = sj.value("i", 0); s.j = sj.value("j", 1); s.angle = sj.value("angle", 0.0);
    }
    p.steps.push_back(s);
  }
  auto errs = validatePrimitive(p);
  if (!errs.empty()) throw std::runtime_error("Invalid primitive \"" + p.name + "\": " + errs[0]);
  return p;
}

static json nodeJ(const Node& n) {
  json j = {{"id", n.id}, {"name", n.name}, {"kind", n.isLeaf() ? "leaf" : "group"},
            {"xf", {{"pos", v4j(n.xf.pos)}, {"rotm", m4j(n.xf.rot)}, {"scale", n.xf.scale}}}};
  json mods = json::array();
  for (auto& m : n.mods) {
    if (m.type == 1) mods.push_back({{"type", "mirror"}, {"axis", m.axis}});
    else mods.push_back({{"type", "repeat"}, {"axis", m.axis}, {"spacing", m.spacing}, {"count", m.count}});
  }
  j["modifiers"] = mods;
  if (n.hidden) j["hidden"] = true;
  if (n.anchor) j["anchor"] = {{"world", n.anchor->world}, {"basis", m4j(n.anchor->basis)}, {"reach", n.anchor->reach}};
  if (n.isLeaf()) {
    j["prim"] = n.prim;
    j["color"] = {n.color[0], n.color[1], n.color[2]};
    j["thickness"] = n.thickness;
    if (n.round) j["round"] = *n.round;
    if (n.shell) j["shell"] = *n.shell;
  } else {
    j["op"] = BOOL_OP_NAMES[(int)n.op];
    j["k"] = n.k;
    json ch = json::array();
    for (auto& c : n.children) ch.push_back(nodeJ(c));
    j["children"] = ch;
  }
  return j;
}

static Node jNode(const json& j) {
  if (!j.is_object()) throw std::runtime_error("Invalid node");
  Node n;
  std::string kind = j.value("kind", std::string("leaf"));
  if (kind != "leaf" && kind != "group") throw std::runtime_error("Unknown node kind: " + kind);
  n.kind = kind == "leaf" ? NodeKind::Leaf : NodeKind::Group;
  n.id = j.value("id", newId());
  if (j.contains("xf")) {
    auto& x = j["xf"];
    n.xf.pos = jv4(x.value("pos", json::array()));
    if (x.contains("rotm")) n.xf.rot = jm4(x["rotm"]);
    else if (x.contains("rot")) {  // legacy: 6 plane angles in degrees
      double a[6] = {0, 0, 0, 0, 0, 0};
      for (size_t i = 0; i < 6 && i < x["rot"].size(); i++) a[i] = x["rot"][i].get<double>();
      n.xf.rot = rotationFromAngles(a);
    }
    n.xf.scale = x.value("scale", 1.0);
    if (!(n.xf.scale > 0)) n.xf.scale = 1;
  }
  for (auto& mj : j.value("modifiers", json::array())) {
    Modifier m;
    m.type = mj.value("type", std::string("mirror")) == "mirror" ? 1 : 2;
    m.axis = std::clamp(mj.value("axis", 0), 0, 3);
    m.spacing = mj.value("spacing", 3.0);
    m.count = mj.value("count", 2);
    n.mods.push_back(m);
  }
  n.hidden = j.value("hidden", false);
  if (j.contains("anchor") && j["anchor"].is_object()) {
    Anchor a;
    a.world = j["anchor"].value("world", std::string());
    a.basis = jm4(j["anchor"].value("basis", json::array()));
    a.reach = j["anchor"].value("reach", 1e4);
    n.anchor = a;
  }
  if (n.isLeaf()) {
    if (!j.contains("prim")) throw std::runtime_error("Leaf " + n.id + " has no primitive");
    n.prim = j["prim"].get<std::string>();
    n.name = j.value("name", n.prim);
    if (j.contains("color")) for (int i = 0; i < 3; i++) n.color[i] = j["color"][i].get<double>();
    n.thickness = j.value("thickness", 0.1);
    if (j.contains("round") && j["round"].is_number()) n.round = j["round"].get<double>();
    if (j.contains("shell") && j["shell"].is_number()) n.shell = j["shell"].get<double>();
  } else {
    std::string op = j.value("op", std::string("union"));
    for (int i = 0; i < 6; i++) if (op == BOOL_OP_NAMES[i]) n.op = (BoolOp)i;
    n.k = j.value("k", 0.3);
    n.name = j.value("name", op);
    for (auto& c : j.value("children", json::array())) n.children.push_back(jNode(c));
  }
  return n;
}

static std::vector<PrimitiveDef> usedCustom(const Node& root, const PrimitiveRegistry& reg) {
  std::vector<PrimitiveDef> out;
  walkc(root, [&](const Node& n, const Node*, int) {
    if (!n.isLeaf()) return;
    auto* p = reg.get(n.prim);
    if (!p || p->builtin) return;
    for (auto& q : out) if (q.id == p->id) return;
    out.push_back(*p);
  });
  return out;
}

std::string sceneToJson(const Node& root, const PrimitiveRegistry& reg, const std::string& world, const CameraState* cam) {
  json prims = json::array();
  for (auto& p : usedCustom(root, reg)) prims.push_back(primJ(p));
  json j = {{"format", "4dcad-scene"}, {"version", 2}, {"root", nodeJ(root)}, {"primitives", prims}};
  j["world"] = world.empty() ? json(nullptr) : json(world);
  if (cam) j["camera"] = {{"target", v4j(cam->target)}, {"distance", cam->distance}, {"yaw", cam->yaw}, {"pitch", cam->pitch}, {"rot4", m4j(cam->rot4)}};
  return j.dump(2);
}

SceneFile sceneFromJson(const std::string& text) {
  json j;
  try { j = json::parse(text); } catch (const std::exception& e) { throw std::runtime_error(std::string("JSON error: ") + e.what()); }
  if (j.value("format", std::string()) != "4dcad-scene") throw std::runtime_error("Not a 4DCAD scene file (missing format: \"4dcad-scene\")");
  SceneFile f;
  f.root = jNode(j.at("root"));
  if (f.root.isLeaf()) throw std::runtime_error("Scene root must be a group");
  for (auto& p : j.value("primitives", json::array())) f.primitives.push_back(jPrim(p));
  if (j.contains("world") && j["world"].is_string()) f.world = j["world"].get<std::string>();
  if (j.contains("camera") && j["camera"].is_object()) {
    auto& c = j["camera"];
    CameraState cs;
    cs.target = jv4(c.value("target", json::array()));
    cs.distance = c.value("distance", 8.0);
    cs.yaw = c.value("yaw", 0.6);
    cs.pitch = c.value("pitch", -0.35);
    cs.rot4 = orthonormalize(jm4(c.value("rot4", json::array())));
    f.camera = cs;
  }
  return f;
}

std::string nodeToJson(const Node& n) { return nodeJ(n).dump(); }
Node nodeFromJson(const std::string& text) { return jNode(json::parse(text)); }

std::string primitiveToJson(const PrimitiveDef& p, bool asFile) {
  if (!asFile) return primJ(p).dump();
  return json({{"format", "4dcad-primitive"}, {"version", 1}, {"primitive", primJ(p)}}).dump(2);
}

LibraryObject makeLibraryObject(const Node& n, const PrimitiveRegistry& reg, const std::string& name) {
  LibraryObject o;
  o.id = newId("o");
  o.name = name;
  o.node = n;
  o.node.anchor.reset();
  o.node.xf.pos = {0, 0, 0, 0};
  o.primitives = usedCustom(n, reg);
  return o;
}

static json objJ(const LibraryObject& o) {
  json prims = json::array();
  for (auto& p : o.primitives) prims.push_back(primJ(p));
  return {{"format", "4dcad-object"}, {"version", 1}, {"id", o.id}, {"name", o.name}, {"node", nodeJ(o.node)}, {"primitives", prims}};
}
static LibraryObject jObj(const json& j) {
  LibraryObject o;
  o.id = j.value("id", newId("o"));
  o.name = j.value("name", std::string("Object"));
  o.node = jNode(j.at("node"));
  for (auto& p : j.value("primitives", json::array())) o.primitives.push_back(jPrim(p));
  return o;
}
std::string libraryObjectToJson(const LibraryObject& o) { return objJ(o).dump(2); }

std::string libraryToJson(const std::vector<PrimitiveDef>& prims, const std::vector<LibraryObject>& objs) {
  json p = json::array(), o = json::array();
  for (auto& x : prims) p.push_back(primJ(x));
  for (auto& x : objs) o.push_back(objJ(x));
  return json({{"format", "4dcad-library"}, {"version", 1}, {"primitives", p}, {"objects", o}}).dump(2);
}

LibraryImport importLibraryJson(const std::string& text) {
  json j = json::parse(text);
  std::string fmt = j.value("format", std::string());
  LibraryImport r;
  if (fmt == "4dcad-primitive") {
    r.primitives.push_back(jPrim(j.at("primitive")));
    r.description = "primitive \"" + r.primitives[0].name + "\"";
  } else if (fmt == "4dcad-object") {
    r.objects.push_back(jObj(j));
    r.description = "object \"" + r.objects[0].name + "\"";
  } else if (fmt == "4dcad-library") {
    for (auto& p : j.value("primitives", json::array())) r.primitives.push_back(jPrim(p));
    for (auto& o : j.value("objects", json::array())) r.objects.push_back(jObj(o));
    r.description = std::to_string(r.primitives.size()) + " primitives, " + std::to_string(r.objects.size()) + " objects";
  } else if (fmt == "4dcad-scene") {
    for (auto& p : j.value("primitives", json::array())) r.primitives.push_back(jPrim(p));
    r.description = std::to_string(r.primitives.size()) + " primitives from scene";
  } else throw std::runtime_error("Unknown file format");
  return r;
}

}  // namespace fd
