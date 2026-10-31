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

int nodeBaseDim(const Node& n, const PrimitiveRegistry& reg) {
  if (n.isLeaf()) { auto* p = reg.get(n.prim); return p ? primitiveDim(*p) : 0; }
  int d = 0;
  for (auto& c : n.children) d = std::max(d, nodeDim(c, reg));
  return d;
}

std::vector<ModAxis> modifierAxes(const Node& n, const PrimitiveRegistry& reg) {
  std::vector<ModAxis> out;
  int dim = nodeBaseDim(n, reg);
  for (auto& m : n.mods) {
    ModAxis a;
    if (m.raises()) {
      if (dim >= 4 || (m.type == MOD_REVOLVE && dim == 0)) a.valid = false;
      else {
        a.newAxis = dim;
        if (m.type == MOD_REVOLVE) a.srcAxis = (m.axis < 0 || m.axis >= dim) ? dim - 1 : m.axis;
        dim++;
      }
    } else if (m.type == MOD_ROTATE) a.valid = m.i != m.j;
    out.push_back(a);
  }
  return out;
}

int nodeDim(const Node& n, const PrimitiveRegistry& reg) {
  int d = nodeBaseDim(n, reg);
  for (auto& a : modifierAxes(n, reg)) if (a.newAxis >= 0) d++;
  return std::min(d, 4);
}

Node makeShape(const std::string& builtinId, const std::string& name, V4 pos, std::array<double, 3> color) {
  for (auto& p : builtinPrimitives())
    if (p.id == builtinId) { Node n = nodeFromPrimitive(p, name); n.xf.pos = pos; n.color = color; return n; }
  return makeLeaf("point", name, pos, color);
}

std::vector<std::pair<int, std::string>> modifierProblems(const Node& n, const PrimitiveRegistry& reg) {
  std::vector<std::pair<int, std::string>> out;
  int dim = nodeBaseDim(n, reg);
  auto axes = modifierAxes(n, reg);
  for (size_t k = 0; k < n.mods.size(); k++) {
    auto& m = n.mods[k];
    if (m.raises()) {
      if (!axes[k].valid) out.push_back({(int)k, dim >= 4 ? "already 4D: ignored" : "needs at least a 1D shape: ignored"});
      else { if (m.type == MOD_REVOLVE && m.axis >= dim) out.push_back({(int)k, "axis does not exist yet: newest used"}); dim++; }
    } else if (!axes[k].valid) out.push_back({(int)k, "plane needs two different axes"});
  }
  return out;
}

std::vector<Modifier> stepsToModifiers(const std::vector<Step>& steps) {
  std::vector<Modifier> mods;
  for (auto& s : steps) {
    Modifier m;
    if (s.op == StepOp::Extrude) { m.type = MOD_EXTRUDE; m.mode = s.mode; m.length = s.length; }
    else if (s.op == StepOp::Revolve) { m.type = MOD_REVOLVE; m.radius = s.radius; m.axis = s.axis; }
    else { m.type = MOD_ROTATE; m.i = s.i; m.j = s.j; m.angle = s.angle; }
    mods.push_back(m);
  }
  return mods;
}

Node nodeFromPrimitive(const PrimitiveDef& p, const std::string& name) {
  Node n = makeLeaf("point", name);
  n.mods = stepsToModifiers(p.steps);
  if (p.round != 0) n.round = p.round;
  if (p.shell != 0) n.shell = p.shell;
  return n;
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

static const char* modeName(ExtrudeMode m) { return m == ExtrudeMode::Finite ? "finite" : m == ExtrudeMode::Infinite ? "infinite" : "half"; }
static ExtrudeMode modeFrom(const std::string& s) { return s == "infinite" ? ExtrudeMode::Infinite : s == "half" ? ExtrudeMode::Half : ExtrudeMode::Finite; }

// old (v1/v2) primitive recipes, only read for conversion
static PrimitiveDef jPrim(const json& j) {
  PrimitiveDef p;
  p.id = j.value("id", newId("p"));
  p.name = j.value("name", std::string("Primitive"));
  p.round = j.value("round", 0.0);
  p.shell = j.value("shell", 0.0);
  for (auto& sj : j.value("steps", json::array())) {
    Step s;
    std::string op = sj.value("op", std::string("extrude"));
    if (op == "extrude") { s.op = StepOp::Extrude; s.mode = modeFrom(sj.value("mode", std::string("finite"))); s.length = sj.value("length", 2.0); }
    else if (op == "revolve") { s.op = StepOp::Revolve; s.radius = sj.value("radius", 0.0); s.axis = sj.value("axis", -1); }
    else { s.op = StepOp::Rotate; s.i = sj.value("i", 0); s.j = sj.value("j", 1); s.angle = sj.value("angle", 0.0); }
    p.steps.push_back(s);
  }
  return p;
}

static json modJ(const Modifier& m) {
  switch (m.type) {
    case MOD_MIRROR: return {{"type", "mirror"}, {"axis", m.axis}};
    case MOD_REPEAT: return {{"type", "repeat"}, {"axis", m.axis}, {"spacing", m.spacing}, {"count", m.count}};
    case MOD_EXTRUDE: return {{"type", "extrude"}, {"mode", modeName(m.mode)}, {"length", m.length}};
    case MOD_REVOLVE: return {{"type", "revolve"}, {"radius", m.radius}, {"axis", m.axis}};
    default: return {{"type", "rotate"}, {"i", m.i}, {"j", m.j}, {"angle", m.angle}};
  }
}
static Modifier jMod(const json& mj) {
  Modifier m;
  std::string t = mj.value("type", std::string("mirror"));
  m.type = t == "repeat" ? MOD_REPEAT : t == "extrude" ? MOD_EXTRUDE : t == "revolve" ? MOD_REVOLVE : t == "rotate" ? MOD_ROTATE : MOD_MIRROR;
  m.axis = std::clamp(mj.value("axis", m.type == MOD_REVOLVE ? -1 : 0), -1, 3);
  if (m.type != MOD_REVOLVE && m.axis < 0) m.axis = 0;
  m.spacing = mj.value("spacing", 3.0);
  m.count = mj.value("count", 2);
  m.mode = modeFrom(mj.value("mode", std::string("finite")));
  m.length = mj.value("length", 2.0);
  m.radius = mj.value("radius", 0.0);
  m.i = std::clamp(mj.value("i", 0), 0, 3);
  m.j = std::clamp(mj.value("j", 1), 0, 3);
  m.angle = mj.value("angle", 45.0);
  return m;
}

static json nodeJ(const Node& n) {
  json j = {{"id", n.id}, {"name", n.name}, {"kind", n.isLeaf() ? "leaf" : "group"},
            {"xf", {{"pos", v4j(n.xf.pos)}, {"rotm", m4j(n.xf.rot)}, {"scale", n.xf.scale}}}};
  json mods = json::array();
  for (auto& m : n.mods) mods.push_back(modJ(m));
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

/** Context for reading older files: their primitive recipes are turned into modifiers. */
struct ReadCtx {
  int version = 3;
  std::vector<PrimitiveDef> filePrims;
};

static Node jNode(const json& j, const ReadCtx& ctx) {
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
  for (auto& mj : j.value("modifiers", json::array())) n.mods.push_back(jMod(mj));
  // before v3, mirror/repeat were listed outermost-first: reverse to innermost-first
  if (ctx.version < 3) std::reverse(n.mods.begin(), n.mods.end());
  n.hidden = j.value("hidden", false);
  if (j.contains("anchor") && j["anchor"].is_object()) {
    Anchor a;
    a.world = j["anchor"].value("world", std::string());
    a.basis = jm4(j["anchor"].value("basis", json::array()));
    a.reach = j["anchor"].value("reach", 1e4);
    n.anchor = a;
  }
  if (n.isLeaf()) {
    n.prim = j.value("prim", std::string("point"));
    n.name = j.value("name", n.prim);
    if (j.contains("color")) for (int i = 0; i < 3; i++) n.color[i] = j["color"][i].get<double>();
    n.thickness = j.value("thickness", 0.1);
    if (j.contains("round") && j["round"].is_number()) n.round = j["round"].get<double>();
    if (j.contains("shell") && j["shell"].is_number()) n.shell = j["shell"].get<double>();
    if (n.prim != "point") {
      // old primitive types become a point with the recipe as innermost modifiers
      const PrimitiveDef* def = nullptr;
      for (auto& p : builtinPrimitives()) if (p.id == n.prim) def = &p;
      for (auto& p : ctx.filePrims) if (p.id == n.prim) def = &p;
      if (!def) throw std::runtime_error("Unknown primitive: " + n.prim);
      auto steps = stepsToModifiers(def->steps);
      n.mods.insert(n.mods.begin(), steps.begin(), steps.end());
      if (!n.round && def->round != 0) n.round = def->round;
      if (!n.shell && def->shell != 0) n.shell = def->shell;
      n.prim = "point";
    }
  } else {
    std::string op = j.value("op", std::string("union"));
    for (int i = 0; i < 6; i++) if (op == BOOL_OP_NAMES[i]) n.op = (BoolOp)i;
    n.k = j.value("k", 0.3);
    n.name = j.value("name", op);
    for (auto& c : j.value("children", json::array())) n.children.push_back(jNode(c, ctx));
  }
  return n;
}

std::string sceneToJson(const Node& root, const PrimitiveRegistry&, const std::string& world, const CameraState* cam) {
  json j = {{"format", "4dcad-scene"}, {"version", 3}, {"root", nodeJ(root)}, {"primitives", json::array()}};
  j["world"] = world.empty() ? json(nullptr) : json(world);
  if (cam) j["camera"] = {{"target", v4j(cam->target)}, {"distance", cam->distance}, {"yaw", cam->yaw}, {"pitch", cam->pitch}, {"rot4", m4j(cam->rot4)}};
  return j.dump(2);
}

SceneFile sceneFromJson(const std::string& text) {
  json j;
  try { j = json::parse(text); } catch (const std::exception& e) { throw std::runtime_error(std::string("JSON error: ") + e.what()); }
  if (j.value("format", std::string()) != "4dcad-scene") throw std::runtime_error("Not a 4DCAD scene file (missing format: \"4dcad-scene\")");
  ReadCtx ctx;
  ctx.version = j.value("version", 1);
  for (auto& p : j.value("primitives", json::array())) ctx.filePrims.push_back(jPrim(p));
  SceneFile f;
  f.root = jNode(j.at("root"), ctx);
  if (f.root.isLeaf()) throw std::runtime_error("Scene root must be a group");
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
Node nodeFromJson(const std::string& text) { return jNode(json::parse(text), ReadCtx{}); }

LibraryObject makeLibraryObject(const Node& n, const PrimitiveRegistry&, const std::string& name) {
  LibraryObject o;
  o.id = newId("o");
  o.name = name;
  o.node = n;
  o.node.anchor.reset();
  o.node.xf.pos = {0, 0, 0, 0};
  return o;
}

static json objJ(const LibraryObject& o) {
  return {{"format", "4dcad-object"}, {"version", 3}, {"id", o.id}, {"name", o.name}, {"node", nodeJ(o.node)}};
}
static LibraryObject jObj(const json& j, const ReadCtx& outer) {
  ReadCtx ctx = outer;
  if (j.contains("version")) ctx.version = j["version"].get<int>();
  for (auto& p : j.value("primitives", json::array())) ctx.filePrims.push_back(jPrim(p));
  LibraryObject o;
  o.id = j.value("id", newId("o"));
  o.name = j.value("name", std::string("Object"));
  o.node = jNode(j.at("node"), ctx);
  return o;
}
std::string libraryObjectToJson(const LibraryObject& o) { return objJ(o).dump(2); }

std::string libraryToJson(const std::vector<LibraryFolder>& folders) {
  json fs = json::array();
  for (auto& f : folders) {
    if (f.builtin) continue;
    json objs = json::array();
    for (auto& o : f.objects) objs.push_back(objJ(o));
    fs.push_back({{"id", f.id}, {"name", f.name}, {"objects", objs}});
  }
  return json({{"format", "4dcad-library"}, {"version", 3}, {"folders", fs}}).dump(2);
}

LibraryImport importLibraryJson(const std::string& text) {
  json j = json::parse(text);
  std::string fmt = j.value("format", std::string());
  ReadCtx ctx;
  ctx.version = j.value("version", 1);
  LibraryImport r;
  auto primObj = [](const PrimitiveDef& p) {
    LibraryObject o;
    o.id = newId("o");
    o.name = p.name;
    o.node = nodeFromPrimitive(p, p.name);
    return o;
  };
  if (fmt == "4dcad-primitive") {
    r.objects.push_back(primObj(jPrim(j.at("primitive"))));
    r.description = "\"" + r.objects[0].name + "\"";
  } else if (fmt == "4dcad-object") {
    r.objects.push_back(jObj(j, ctx));
    r.description = "\"" + r.objects[0].name + "\"";
  } else if (fmt == "4dcad-library") {
    if (j.contains("folders")) {
      for (auto& fj : j["folders"]) {
        LibraryFolder f;
        f.id = fj.value("id", newId("f"));
        f.name = fj.value("name", std::string("Folder"));
        for (auto& o : fj.value("objects", json::array())) f.objects.push_back(jObj(o, ctx));
        r.folders.push_back(f);
      }
    } else {  // v1: flat primitives + objects
      for (auto& p : j.value("primitives", json::array())) r.objects.push_back(primObj(jPrim(p)));
      for (auto& o : j.value("objects", json::array())) r.objects.push_back(jObj(o, ctx));
    }
    size_t n = r.objects.size();
    for (auto& f : r.folders) n += f.objects.size();
    r.description = std::to_string(n) + " objects";
  } else if (fmt == "4dcad-scene") {
    for (auto& p : j.value("primitives", json::array())) r.objects.push_back(primObj(jPrim(p)));
    r.description = std::to_string(r.objects.size()) + " primitives from scene";
  } else throw std::runtime_error("Unknown file format");
  return r;
}

std::vector<LibraryFolder> builtinFolders() {
  std::vector<LibraryFolder> out = {{"builtin-basic", "Basic (0D-2D)", true, {}}, {"builtin-3d", "3D shapes", true, {}}, {"builtin-4d", "4D shapes", true, {}}};
  for (auto& p : builtinPrimitives()) {
    int d = primitiveDim(p);
    LibraryObject o;
    o.id = "builtin-" + p.id;
    o.name = p.name;
    o.node = nodeFromPrimitive(p, p.name);
    out[d <= 2 ? 0 : d == 3 ? 1 : 2].objects.push_back(o);
  }
  return out;
}

}  // namespace fd
