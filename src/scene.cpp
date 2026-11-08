#include "scene.hpp"
#include "version.hpp"

#include <set>

#include "defs.hpp"

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

Node makeCustom(const std::string& name, const std::string& code) {
  Node n = makeLeaf("custom", name);
  n.code = code;
  return n;
}

Node makeCamera(const std::string& name, const V4& eye, const M4& frame, double fov, int slider) {
  Node n;
  n.id = newId("c");
  n.kind = NodeKind::Camera;
  n.name = name;
  n.xf.pos = eye;
  n.xf.rot = orthonormalize(frame);
  n.camFov = fov;
  n.camSlider = slider;
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
  walk(c, [](Node& m, Node*, int) { m.id = newId(m.isLeaf() ? "n" : m.isCamera() ? "c" : "g"); });
  return c;
}

// ---------------------------------------------------------------- registry
// ============================================================ frames
Transform composeXf(const Transform& P, const Transform& C) {
  Transform r;
  r.pos = P.pos + mul(P.rot, C.pos) * P.scale;
  r.rot = orthonormalize(mul(P.rot, C.rot));
  r.scale = P.scale * C.scale;
  return r;
}

Transform relativeXf(const Transform& P, const Transform& W) {
  Transform r;
  M4 inv = transpose(P.rot);
  double s = P.scale != 0 ? P.scale : 1;
  r.pos = mul(inv, W.pos - P.pos) * (1 / s);
  r.rot = orthonormalize(mul(inv, W.rot));
  r.scale = W.scale / s;
  return r;
}

std::optional<Transform> worldXf(Node& root, const std::string& id) {
  auto path = pathTo(root, id);
  if (path.empty()) return std::nullopt;
  Transform acc;
  for (Node* n : path) {
    if (n->anchor) return std::nullopt;
    acc = composeXf(acc, n->xf);
  }
  return acc;
}

bool centreGroupOnFirst(Node& g) {
  if (g.isLeaf() || g.children.empty() || g.anchor) return false;
  for (auto& c : g.children) if (c.anchor) return false;
  V4 o = g.children[0].xf.pos;
  g.xf.pos = g.xf.pos + mul(g.xf.rot, o) * g.xf.scale;
  for (auto& c : g.children) c.xf.pos = c.xf.pos - o;
  return true;
}

void pruneEmptyGroups(Node& root) {
  std::function<void(Node&)> rec = [&](Node& n) {
    for (auto& c : n.children) rec(c);
    n.children.erase(std::remove_if(n.children.begin(), n.children.end(), [](const Node& c) { return c.isGroup() && c.children.empty(); }),
                     n.children.end());
  };
  rec(root);
}

// ============================================================ multi-object editing
bool sameModifierTypes(const Node& a, const Node& b) {
  if (a.mods.size() != b.mods.size()) return false;
  for (size_t i = 0; i < a.mods.size(); i++) if (a.mods[i].type != b.mods[i].type || a.mods[i].def != b.mods[i].def) return false;
  return true;
}

void propagateEdit(const Node& B, const Node& A, Node& T, bool stacksMatch, const ModStackOp& op) {
  auto ch = [](auto& b, auto& a, auto& t) { if (!(b == a)) t = a; };
  if (B.isLeaf() == T.isLeaf()) {
    if (T.isLeaf()) {
      ch(B.color, A.color, T.color);
      ch(B.thickness, A.thickness, T.thickness);
      ch(B.mat.emissive, A.mat.emissive, T.mat.emissive);
      for (int c = 0; c < CH_COUNT; c++) ch(B.mat.tex[c], A.mat.tex[c], T.mat.tex[c]);
      ch(B.mat.color2, A.mat.color2, T.mat.color2);
      ch(B.mat.roughness, A.mat.roughness, T.mat.roughness);
      ch(B.mat.specular, A.mat.specular, T.mat.specular);
      ch(B.mat.transparency, A.mat.transparency, T.mat.transparency);
      ch(B.mat.ior, A.mat.ior, T.mat.ior);
      ch(B.mat.metallic, A.mat.metallic, T.mat.metallic);
      ch(B.mat.emissionStrength, A.mat.emissionStrength, T.mat.emissionStrength);
      if (B.isCustom() && T.isCustom()) { ch(B.code, A.code, T.code); ch(B.codeBound, A.codeBound, T.codeBound); }
    } else {
      ch(B.op, A.op, T.op);
      ch(B.k, A.k, T.k);
    }
  }
  // transforms: relative
  if (!T.anchor) {
    V4 d = A.xf.pos - B.xf.pos;
    if (length(d) > 0) T.xf.pos = T.xf.pos + d;
    if (A.xf.rot != B.xf.rot) T.xf.rot = orthonormalize(mul(mul(A.xf.rot, transpose(B.xf.rot)), T.xf.rot));
  } else if (A.xf.rot != B.xf.rot) {
    T.xf.rot = orthonormalize(mul(mul(A.xf.rot, transpose(B.xf.rot)), T.xf.rot));  // anchored: rotation within the tangent frame
  }
  if (A.xf.scale != B.xf.scale && B.xf.scale > 0) T.xf.scale = std::max(0.01, T.xf.scale * A.xf.scale / B.xf.scale);
  // modifiers
  if (!stacksMatch) return;
  if (op.kind == ModStackOp::Add) { T.mods.push_back(op.mod); return; }
  if (op.kind == ModStackOp::Remove) { if (op.index >= 0 && op.index < (int)T.mods.size()) T.mods.erase(T.mods.begin() + op.index); return; }
  if (op.kind == ModStackOp::MoveUp) { if (op.index > 0 && op.index < (int)T.mods.size()) std::swap(T.mods[op.index], T.mods[op.index - 1]); return; }
  if (!sameModifierTypes(B, A) || !sameModifierTypes(B, T)) return;
  for (size_t i = 0; i < A.mods.size(); i++) {
    const Modifier &b = B.mods[i], &a = A.mods[i];
    Modifier& t = T.mods[i];
    ch(b.axis, a.axis, t.axis);
    ch(b.spacing, a.spacing, t.spacing);
    ch(b.count, a.count, t.count);
    ch(b.mode, a.mode, t.mode);
    ch(b.length, a.length, t.length);
    ch(b.radius, a.radius, t.radius);
    ch(b.i, a.i, t.i);
    ch(b.j, a.j, t.j);
    ch(b.angle, a.angle, t.angle);
    ch(b.offset, a.offset, t.offset);
    ch(b.startAngle, a.startAngle, t.startAngle);
    ch(b.params, a.params, t.params);
  }
}

PrimitiveRegistry::PrimitiveRegistry() {
  list_ = builtinPrimitives();
  PrimitiveDef custom;  // placeholder for GLSL custom SDF leaves (base dimension 4)
  custom.id = "custom";
  custom.name = "Custom SDF";
  custom.builtin = true;
  list_.push_back(custom);
}
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
  if (n.isCustom()) return 4;
  if (n.isPolytope()) return n.isPoly() && polyIs4D(n.poly.type) ? 4 : 3;
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
        else if (m.type == MOD_DEF) a.srcAxis = dim - 1;  // (a data-driven one: the newest existing axis, -1 for a point)
        dim++;
      }
    } else if (m.type == MOD_RADIAL) a.valid = m.i != m.j;
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
    if (m.type == MOD_DEF && !modifierDefOf(m)) out.push_back({(int)k, "unknown modifier definition \"" + m.def + "\": no effect"});
  }
  return out;
}

std::vector<Modifier> stepsToModifiers(const std::vector<Step>& steps) {
  std::vector<Modifier> mods;
  for (auto& s : steps) {
    Modifier m;
    if (s.op == StepOp::Extrude) { m.type = MOD_EXTRUDE; m.mode = s.mode; m.length = s.length; }
    else if (s.op == StepOp::Revolve) { m.type = MOD_REVOLVE; m.radius = s.radius; m.axis = s.axis; }
    else { m.type = MOD_RADIAL; m.i = s.i; m.j = s.j; m.count = 1; m.radius = 0; m.startAngle = s.angle; }  // old rotate step
    mods.push_back(m);
  }
  return mods;
}

Node nodeFromPrimitive(const PrimitiveDef& p, const std::string& name) {
  Node n = makeLeaf("point", name);
  n.mods = stepsToModifiers(p.steps);
  appendRoundShell(n.mods, p.round, p.shell);
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

// ---------------------------------------------------------------- texture slots
int texMappingChannel(const Material& m, int ch) {
  int c = ch;
  for (int hops = 0; hops < CH_COUNT; hops++) {
    int f = m.tex[c].mapFrom;
    if (f < 0 || f >= CH_COUNT || f == c || !m.tex[f].used()) return c;
    c = f;
  }
  return ch;  // a cycle: its own mapping
}

TexSlot legacyColorSlot(int type, double scale, double strength, const std::array<double, 3>& color2) {
  TexSlot s;
  s.def = legacyTextureId(type);
  if (auto d = defs().texture(s.def)) conformValues(s.params, d->params);
  s.map.scale = scale;
  s.strength = strength;
  s.color2 = color2;
  return s;
}

static json texSlotJ(const TexSlot& t) {
  json j = {{"def", t.def}, {"strength", t.strength}, {"color2", {t.color2[0], t.color2[1], t.color2[2]}}, {"value2", t.value2}};
  if (auto d = defs().texture(t.def)) j["params"] = valuesToJson(t.params, d->params);
  if (t.low != 0 || t.high != 1) { j["low"] = t.low; j["high"] = t.high; }
  if (t.contrast != 0) j["contrast"] = t.contrast;
  if (t.invert) j["invert"] = true;
  if (!t.curve.empty()) {
    json c = json::array();
    for (auto& pt : t.curve) c.push_back({pt[0], pt[1]});
    j["curve"] = c;
  }
  const TexMapping& m = t.map;
  j["map"] = {{"space", TEX_SPACE_NAMES[std::clamp(m.space, 0, TS_COUNT - 1)]}, {"offset", m.offset}, {"rot", m.rot}, {"scale", m.scale}, {"axisScale", m.axisScale}};
  if (t.mapFrom >= 0 && t.mapFrom < CH_COUNT) j["mapFrom"] = CHANNEL_IDS[t.mapFrom];
  return j;
}
static TexSlot jTexSlot(const json& j) {
  TexSlot t;
  if (!j.is_object()) return t;
  t.def = j.value("def", std::string());
  t.strength = j.value("strength", 1.0);
  if (j.contains("color2") && j["color2"].is_array() && j["color2"].size() == 3) for (int k = 0; k < 3; k++) t.color2[k] = j["color2"][k].get<double>();
  t.value2 = j.value("value2", 1.0);
  if (auto d = defs().texture(t.def)) valuesFromJson(t.params, d->params, j.value("params", json::object()));
  t.low = j.value("low", 0.0);
  t.high = j.value("high", 1.0);
  t.contrast = std::clamp(j.value("contrast", 0.0), -1.0, 1.0);
  t.invert = j.value("invert", false);
  for (auto& pt : j.value("curve", json::array()))
    if (pt.is_array() && pt.size() == 2 && (int)t.curve.size() < TEX_CURVE_MAX) t.curve.push_back({pt[0].get<double>(), pt[1].get<double>()});
  if (j.contains("map") && j["map"].is_object()) {
    auto& mj = j["map"];
    std::string sp = mj.value("space", std::string("Object"));
    for (int k = 0; k < TS_COUNT; k++) if (sp == TEX_SPACE_NAMES[k]) t.map.space = k;
    auto arr = [&](const char* key, auto& out) {
      if (mj.contains(key) && mj[key].is_array() && mj[key].size() == out.size()) for (size_t k = 0; k < out.size(); k++) out[k] = mj[key][k].get<double>();
    };
    arr("offset", t.map.offset);
    arr("rot", t.map.rot);
    arr("axisScale", t.map.axisScale);
    t.map.scale = mj.value("scale", 1.0);
  }
  std::string mf = j.value("mapFrom", std::string());
  for (int k = 0; k < CH_COUNT; k++) if (mf == CHANNEL_IDS[k]) t.mapFrom = k;
  return t;
}

// custom (non-built-in) modifier / texture definitions a tree uses / registering those a file carries
static json usedModifierDefs(const Node& root) {
  std::set<std::string> ids;
  walkc(root, [&](const Node& n, const Node*, int) { for (auto& m : n.mods) if (m.type == MOD_DEF) ids.insert(m.def); });
  json a = json::array();
  for (auto& id : ids)
    if (auto d = defs().modifier(id); d && !d->builtin) a.push_back(modifierDefToJson(*d));
  return a;
}
static json usedTextureDefs(const Node& root) {
  std::set<std::string> ids;
  walkc(root, [&](const Node& n, const Node*, int) { for (auto& t : n.mat.tex) if (t.used()) ids.insert(t.def); });
  json a = json::array();
  for (auto& id : ids)
    if (auto d = defs().texture(id); d && !d->builtin) a.push_back(textureDefToJson(*d));
  return a;
}
static void registerModifierDefs(const json& j) {
  for (auto& dj : j.value("modifierDefs", json::array())) {
    try { defs().setModifier(modifierDefFromJson(dj)); } catch (...) {}
  }
  for (auto& dj : j.value("textureDefs", json::array())) {
    try { defs().setTexture(textureDefFromJson(dj)); } catch (...) {}
  }
}

static json modJ(const Modifier& m) {
  switch (m.type) {
    case MOD_MIRROR: return {{"type", "mirror"}, {"axis", m.axis}, {"offset", m.offset}};
    case MOD_REPEAT: return {{"type", "repeat"}, {"axis", m.axis}, {"spacing", m.spacing}, {"count", m.count}};
    case MOD_EXTRUDE: return {{"type", "extrude"}, {"mode", modeName(m.mode)}, {"length", m.length}};
    case MOD_REVOLVE: return {{"type", "revolve"}, {"radius", m.radius}, {"axis", m.axis}};
    case MOD_DEF: {
      json j = {{"type", "def"}, {"def", m.def}};
      if (auto d = defs().modifier(m.def)) j["params"] = modParamsToJson(m, *d);
      else j["values"] = m.params;  // (unknown definition: kept as they are)
      return j;
    }
    default: return {{"type", "radial"}, {"i", m.i}, {"j", m.j}, {"radius", m.radius}, {"count", m.count}, {"angle", m.angle}, {"startAngle", m.startAngle}};
  }
}
static Modifier jMod(const json& mj) {
  Modifier m;
  std::string t = mj.value("type", std::string("mirror"));
  if (t == "def") {
    m.type = MOD_DEF;
    m.def = mj.value("def", std::string());
    if (auto d = defs().modifier(m.def)) modParamsFromJson(m, *d, mj.value("params", json::object()));
    else m.params = mj.value("values", std::vector<double>());
    return m;
  }
  m.type = t == "repeat" ? MOD_REPEAT : t == "extrude" ? MOD_EXTRUDE : t == "revolve" ? MOD_REVOLVE : (t == "radial" || t == "rotate") ? MOD_RADIAL : MOD_MIRROR;
  m.axis = std::clamp(mj.value("axis", m.type == MOD_REVOLVE ? -1 : 0), -1, 3);
  if (m.type != MOD_REVOLVE && m.axis < 0) m.axis = 0;
  m.spacing = mj.value("spacing", 3.0);
  m.count = mj.value("count", 2);
  m.mode = modeFrom(mj.value("mode", std::string("finite")));
  m.length = mj.value("length", 2.0);
  m.radius = mj.value("radius", 0.0);
  m.i = std::clamp(mj.value("i", 0), 0, 3);
  m.j = std::clamp(mj.value("j", 1), 0, 3);
  m.angle = mj.value("angle", 60.0);
  m.offset = std::max(0.0, mj.value("offset", 0.0));
  m.startAngle = mj.value("startAngle", 0.0);
  if (t == "rotate") {  // the removed Rotate modifier: one radial copy at its angle (same shape)
    m.count = 1;
    m.radius = 0;
    m.startAngle = mj.value("angle", 45.0);
    m.angle = 60;
  }
  if (m.type == MOD_RADIAL) m.count = std::max(1, m.count);
  return m;
}

static json nodeJ(const Node& n) {
  json j = {{"id", n.id}, {"name", n.name}, {"kind", n.isLeaf() ? "leaf" : n.isCamera() ? "camera" : "group"},
            {"xf", {{"pos", v4j(n.xf.pos)}, {"rotm", m4j(n.xf.rot)}, {"scale", n.xf.scale}}}};
  json mods = json::array();
  for (auto& m : n.mods) mods.push_back(modJ(m));
  j["modifiers"] = mods;
  if (n.hidden) j["hidden"] = true;
  if (n.locked) j["locked"] = true;
  if (!n.linkId.empty()) j["link"] = n.linkId;
  if (n.anchor) {
    const Anchor& a = *n.anchor;
    j["anchor"] = {{"world", a.world}, {"basis", m4j(a.basis)}, {"reach", a.reach}};
    if (a.follow) j["anchor"]["follow"] = true;
    if (a.hasRest)
      j["anchor"]["rest"] = {{"pos", {a.restPos[0], a.restPos[1], a.restPos[2], a.restPos[3]}}, {"frame", m4j(a.restFrame)},
                             {"travel", a.travel}, {"heading", a.heading}, {"spin", a.spin}};
  }
  if (n.isLeaf()) {
    j["prim"] = n.prim;
    j["color"] = {n.color[0], n.color[1], n.color[2]};
    j["thickness"] = n.thickness;
    const Material& m = n.mat;
    j["material"] = {{"emissive", m.emissive}, {"color2", {m.color2[0], m.color2[1], m.color2[2]}},
                     {"roughness", m.roughness}, {"specular", m.specular}, {"transparency", m.transparency}, {"ior", m.ior},
                     {"metallic", m.metallic}, {"emissionStrength", m.emissionStrength}};
    if (m.textured()) {
      json tj = json::object();
      for (int c = 0; c < CH_COUNT; c++) if (m.tex[c].used()) tj[CHANNEL_IDS[c]] = texSlotJ(m.tex[c]);
      j["material"]["textures"] = tj;
    }
    if (n.isCustom()) { j["code"] = n.code; j["codeBound"] = n.codeBound; }
    if (n.isPolytope()) {
      const PolyParams& p = n.poly;
      j["poly"] = {{"type", POLY_NAMES[std::clamp(p.type, 0, POLY_COUNT - 1)]}, {"size", p.size}, {"style", p.style ? "cells" : "edges"},
                   {"width", p.width}, {"alternate", p.alternate}};
      if (n.isHoneycomb() && p.honey >= 0 && p.honey < (int)honeycombs().size()) j["poly"]["honeycomb"] = honeycombs()[p.honey].schlafli;
    }
  } else if (n.isCamera()) {
    j["camera"] = {{"fov", n.camFov}, {"slider", n.camSlider}, {"aperture", n.camAperture}, {"focus", n.camFocus}};
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
  if (kind != "leaf" && kind != "group" && kind != "camera") throw std::runtime_error("Unknown node kind: " + kind);
  n.kind = kind == "leaf" ? NodeKind::Leaf : kind == "camera" ? NodeKind::Camera : NodeKind::Group;
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
  n.locked = j.value("locked", false);
  n.linkId = j.value("link", std::string());
  if (j.contains("anchor") && j["anchor"].is_object()) {
    Anchor a;
    a.world = j["anchor"].value("world", std::string());
    a.basis = jm4(j["anchor"].value("basis", json::array()));
    a.reach = j["anchor"].value("reach", 1e4);
    a.follow = j["anchor"].value("follow", false);
    if (j["anchor"].contains("rest") && j["anchor"]["rest"].is_object()) {
      auto& r = j["anchor"]["rest"];
      a.hasRest = true;
      a.restPos = jv4(r.value("pos", json::array()));
      a.restFrame = jm4(r.value("frame", json::array()));
      a.travel = r.value("travel", 0.0);
      a.heading = r.value("heading", 0.0);
      a.spin = r.value("spin", 0.0);
    }
    n.anchor = a;
  }
  if (n.isLeaf()) {
    n.prim = j.value("prim", std::string("point"));
    n.name = j.value("name", n.prim);
    if (j.contains("color")) for (int i = 0; i < 3; i++) n.color[i] = j["color"][i].get<double>();
    n.thickness = j.value("thickness", 0.1);
    // old files: rounding / shell were fields (now the Onion / Round modifiers, at the end of the stack)
    appendRoundShell(n.mods, j.contains("round") && j["round"].is_number() ? j["round"].get<double>() : 0.0,
                     j.contains("shell") && j["shell"].is_number() ? j["shell"].get<double>() : 0.0);
    if (j.contains("material") && j["material"].is_object()) {
      auto& mj = j["material"];
      n.mat.emissive = std::clamp(mj.value("emissive", 0.0), 0.0, 1.0);
      if (mj.contains("color2")) for (int i = 0; i < 3; i++) n.mat.color2[i] = mj["color2"][i].get<double>();
      // old files: one colour texture (a type, scale, strength and the second colour)
      static const char* oldNames[TEX_COUNT] = {"None", "Checker", "Stripes", "Grid", "Noise", "Rings", "Dots", "Waves", "Marble", "Wood", "Cells", "Bricks", "Gradient"};
      std::string t = mj.value("texture", std::string("None"));
      for (int i = 1; i < TEX_COUNT; i++)
        if (t == oldNames[i]) n.mat.tex[CH_COLOR] = legacyColorSlot(i, mj.value("texScale", 1.0), mj.value("texStrength", 0.7), n.mat.color2);
      if (mj.contains("textures") && mj["textures"].is_object())
        for (int c = 0; c < CH_COUNT; c++)
          if (mj["textures"].contains(CHANNEL_IDS[c])) n.mat.tex[c] = jTexSlot(mj["textures"][CHANNEL_IDS[c]]);
      n.mat.roughness = std::clamp(mj.value("roughness", 0.75), 0.0, 1.0);
      n.mat.specular = std::clamp(mj.value("specular", 0.35), 0.0, 2.0);
      n.mat.transparency = std::clamp(mj.value("transparency", 0.0), 0.0, 1.0);
      n.mat.ior = std::clamp(mj.value("ior", 1.45), 1.0, 3.0);
      n.mat.metallic = std::clamp(mj.value("metallic", 0.0), 0.0, 1.0);
      n.mat.emissionStrength = std::clamp(mj.value("emissionStrength", 4.0), 0.0, 1000.0);
    }
    if (n.prim == "custom") {
      n.code = j.value("code", std::string(DEFAULT_CUSTOM_SDF));
      n.codeBound = j.value("codeBound", 1.5);
    } else if (n.prim == "polyhedron" || n.prim == "honeycomb") {
      if (j.contains("poly") && j["poly"].is_object()) {
        auto& pj = j["poly"];
        std::string t = pj.value("type", std::string("Cube"));
        for (int i = 0; i < POLY_COUNT; i++) if (t == POLY_NAMES[i]) n.poly.type = i;
        n.poly.size = std::max(1e-3, pj.value("size", 1.0));
        n.poly.style = pj.value("style", std::string("edges")) == "cells" ? 1 : 0;
        n.poly.width = std::max(0.0, pj.value("width", 0.05));
        n.poly.alternate = pj.value("alternate", true);
        std::string h = pj.value("honeycomb", std::string());
        for (size_t i = 0; i < honeycombs().size(); i++) if (h == honeycombs()[i].schlafli) n.poly.honey = (int)i;
      }
    } else if (n.prim != "point") {
      // old primitive types become a point with the recipe as innermost modifiers
      const PrimitiveDef* def = nullptr;
      for (auto& p : builtinPrimitives()) if (p.id == n.prim) def = &p;
      for (auto& p : ctx.filePrims) if (p.id == n.prim) def = &p;
      if (!def) throw std::runtime_error("Unknown primitive: " + n.prim);
      auto steps = stepsToModifiers(def->steps);
      n.mods.insert(n.mods.begin(), steps.begin(), steps.end());
      bool hasRS = false;
      for (auto& m : n.mods) if (m.type == MOD_DEF && (m.def == "round" || m.def == "onion")) hasRS = true;
      if (!hasRS) appendRoundShell(n.mods, def->round, def->shell);
      n.prim = "point";
    }
  } else if (n.isCamera()) {
    n.name = j.value("name", std::string("Camera"));
    auto c = j.value("camera", json::object());
    n.camFov = std::clamp(c.value("fov", 60.0), 5.0, 170.0);
    n.camSlider = std::clamp(c.value("slider", 3), 0, 3);
    n.camAperture = std::max(0.0, c.value("aperture", 0.0));
    n.camFocus = std::max(0.01, c.value("focus", 5.0));
  } else {
    std::string op = j.value("op", std::string("union"));
    for (int i = 0; i < 6; i++) if (op == BOOL_OP_NAMES[i]) n.op = (BoolOp)i;
    n.k = j.value("k", 0.3);
    n.name = j.value("name", op);
    for (auto& c : j.value("children", json::array())) n.children.push_back(jNode(c, ctx));
  }
  return n;
}

static json camJ(const CameraState& c) {
  return {{"target", v4j(c.target)}, {"distance", c.distance}, {"yaw", c.yaw}, {"pitch", c.pitch}, {"rot4", m4j(c.rot4)}, {"slider", c.slider}};
}
static CameraState jCam(const json& c) {
  CameraState cs;
  cs.target = jv4(c.value("target", json::array()));
  cs.distance = c.value("distance", 8.0);
  cs.yaw = c.value("yaw", 0.6);
  cs.pitch = c.value("pitch", -0.35);
  cs.rot4 = orthonormalize(jm4(c.value("rot4", json::array())));
  cs.slider = std::clamp(c.value("slider", 3), 0, 3);
  return cs;
}
static json marksJ(const std::vector<CameraBookmark>& b) {
  json a = json::array();
  for (auto& m : b) {
    json o = {{"name", m.name}, {"camera", camJ(m.cam)}};
    if (m.geo) o["geo"] = {{"pivot", v4j(m.geoPivot)}, {"frame", m4j(m.geoFrame)}};
    a.push_back(o);
  }
  return a;
}
static std::vector<CameraBookmark> jMarks(const json& a) {
  std::vector<CameraBookmark> out;
  if (!a.is_array()) return out;
  for (auto& o : a) {
    if (!o.is_object()) continue;
    CameraBookmark m;
    m.name = o.value("name", std::string("View"));
    if (o.contains("camera") && o["camera"].is_object()) m.cam = jCam(o["camera"]);
    if (o.contains("geo") && o["geo"].is_object()) {
      m.geo = true;
      m.geoPivot = jv4(o["geo"].value("pivot", json::array()));
      m.geoFrame = orthonormalize(jm4(o["geo"].value("frame", json::array())));
    }
    out.push_back(m);
  }
  return out;
}
std::string bookmarksToJson(const std::vector<CameraBookmark>& b) { return marksJ(b).dump(); }
std::vector<CameraBookmark> bookmarksFromJson(const std::string& text) {
  try { return jMarks(json::parse(text)); } catch (...) { return {}; }
}

static json lightsJ(const std::vector<PointLight>& l) {
  json a = json::array();
  for (auto& x : l)
    a.push_back({{"id", x.id}, {"name", x.name}, {"pos", v4j(x.pos)}, {"color", {x.color[0], x.color[1], x.color[2]}}, {"intensity", x.intensity},
                 {"range", x.range}, {"shadows", x.shadows}, {"enabled", x.enabled}});
  return a;
}
static std::vector<PointLight> jLights(const json& a) {
  std::vector<PointLight> out;
  if (!a.is_array()) return out;
  for (auto& o : a) {
    if (!o.is_object()) continue;
    PointLight l;
    l.id = o.value("id", std::string());
    if (l.id.empty()) l.id = newId("l");
    l.name = o.value("name", std::string("Light"));
    l.pos = jv4(o.value("pos", json::array()));
    if (o.contains("color") && o["color"].is_array() && o["color"].size() == 3) for (int i = 0; i < 3; i++) l.color[i] = o["color"][i].get<double>();
    l.intensity = std::max(0.0, o.value("intensity", 1.5));
    l.range = std::max(0.01, o.value("range", 10.0));
    l.shadows = o.value("shadows", true);
    l.enabled = o.value("enabled", true);
    out.push_back(l);
  }
  return out;
}
std::string lightsToJson(const std::vector<PointLight>& l) { return lightsJ(l).dump(); }
std::vector<PointLight> lightsFromJson(const std::string& text) {
  try { return jLights(json::parse(text)); } catch (...) { return {}; }
}

std::string sceneToJson(const Node& root, const PrimitiveRegistry&, const std::string& world, const CameraState* cam,
                        const std::vector<CameraBookmark>* bookmarks, const std::vector<PointLight>* lights,
                        const std::string& activeCamera, const std::string* animationJson, const Space* space,
                        const CurvedCamState* curvedCam) {
  json j = {{"format", "4dcad-scene"}, {"version", 3}, {"appVersion", FD_VERSION}, {"root", nodeJ(root)}, {"primitives", json::array()}};
  if (json md = usedModifierDefs(root); !md.empty()) j["modifierDefs"] = md;  // (custom definitions the scene uses)
  if (json td = usedTextureDefs(root); !td.empty()) j["textureDefs"] = td;
  j["world"] = world.empty() ? json(nullptr) : json(world);
  if (cam) j["camera"] = camJ(*cam);
  if (bookmarks && !bookmarks->empty()) j["bookmarks"] = marksJ(*bookmarks);
  if (!activeCamera.empty()) j["activeCamera"] = activeCamera;
  if (animationJson && !animationJson->empty()) {
    try { j["animation"] = json::parse(*animationJson); } catch (...) {}
  }
  if (lights && !lights->empty()) j["lights"] = lightsJ(*lights);
  if (space && space->curved()) j["space"] = {{"type", SPACE_IDS[(int)space->kind]}, {"radius", space->radius}};
  if (space && space->curved() && curvedCam) j["curvedCamera"] = {{"iso", std::vector<double>(curvedCam->iso.begin(), curvedCam->iso.end())}, {"distance", curvedCam->distance}};
  return j.dump(2);
}

// ============================================================ linked duplicates
static void stripForDefinition(json& j, bool top) {
  j.erase("id");
  j.erase("link");
  if (top) { j.erase("xf"); j.erase("name"); j.erase("hidden"); j.erase("locked"); j.erase("anchor"); }
  if (j.contains("children")) for (auto& c : j["children"]) stripForDefinition(c, false);
}
std::string linkDefinition(const Node& n) {
  json j = nodeJ(n);
  stripForDefinition(j, true);
  return j.dump();
}

static void copyDef(Node& d, const Node& s, bool top) {
  if (!top) { d.xf = s.xf; d.name = s.name; d.hidden = s.hidden; d.locked = s.locked; d.anchor = s.anchor; }
  d.kind = s.kind;
  d.mods = s.mods;
  d.prim = s.prim; d.color = s.color; d.thickness = s.thickness;
  d.mat = s.mat; d.code = s.code; d.codeBound = s.codeBound; d.poly = s.poly;
  d.op = s.op; d.k = s.k;
  bool same = d.children.size() == s.children.size();
  for (size_t i = 0; same && i < s.children.size(); i++) same = d.children[i].kind == s.children[i].kind;
  if (same) for (size_t i = 0; i < s.children.size(); i++) copyDef(d.children[i], s.children[i], false);
  else {  // structure changed: fresh copies (new ids, but nested links are kept)
    d.children.clear();
    for (auto& c : s.children) d.children.push_back(cloneFresh(c));
  }
}
void copyDefinition(Node& dst, const Node& src) { copyDef(dst, src, true); }

static std::map<std::string, std::vector<Node*>> linkGroups(Node& root) {
  std::map<std::string, std::vector<Node*>> g;
  walk(root, [&](Node& n, Node*, int) { if (!n.linkId.empty()) g[n.linkId].push_back(&n); });
  return g;
}

void recordLinks(Node& root, std::map<std::string, std::string>& defs) {
  defs.clear();
  // inconsistent files (edited by hand): the first member defines the shared part
  for (int pass = 0; pass < 8; pass++) {
    bool changed = false;
    for (auto& [id, members] : linkGroups(root)) {
      std::string d0 = linkDefinition(*members[0]);
      for (size_t i = 1; i < members.size() && !changed; i++)
        if (!isAncestor(root, members[0]->id, members[i]->id) && !isAncestor(root, members[i]->id, members[0]->id) &&
            linkDefinition(*members[i]) != d0) { copyDefinition(*members[i], *members[0]); changed = true; }
      if (changed) break;
    }
    if (!changed) break;
  }
  for (auto& [id, members] : linkGroups(root)) defs[id] = linkDefinition(*members[0]);
}

bool syncLinks(Node& root, std::map<std::string, std::string>& defs) {
  bool changed = false;
  // a definition change can resize a parent's children (and move nodes), so re-scan after each copy
  for (int pass = 0; pass < 8; pass++) {
    bool again = false;
    for (auto& [id, members] : linkGroups(root)) {
      // a member inside another member of the same link would make the definition recursive: unlink it
      bool nested = false;
      for (Node* a : members)
        for (Node* b : members)
          if (a != b && isAncestor(root, a->id, b->id)) { b->linkId.clear(); nested = true; }
      if (nested) { changed = true; again = true; break; }
      if (members.size() < 2) { defs[id] = linkDefinition(*members[0]); continue; }
      auto it = defs.find(id);
      Node* src = nullptr;
      std::string srcDef;
      for (Node* m : members) {
        std::string d = linkDefinition(*m);
        if (it == defs.end() || d != it->second) { src = m; srcDef = d; break; }
      }
      if (!src) continue;
      for (Node* m : members) if (m != src && linkDefinition(*m) != srcDef) { copyDefinition(*m, *src); changed = true; again = true; }
      defs[id] = srcDef;
      if (again) break;  // pointers may be stale now
    }
    if (!again) break;
  }
  return changed;
}

SceneFile sceneFromJson(const std::string& text) {
  json j;
  try { j = json::parse(text); } catch (const std::exception& e) { throw std::runtime_error(std::string("JSON error: ") + e.what()); }
  if (j.value("format", std::string()) != "4dcad-scene") throw std::runtime_error("Not a 4DCAD scene file (missing format: \"4dcad-scene\")");
  ReadCtx ctx;
  ctx.version = j.value("version", 1);
  for (auto& p : j.value("primitives", json::array())) ctx.filePrims.push_back(jPrim(p));
  registerModifierDefs(j);
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
    cs.slider = std::clamp(c.value("slider", 3), 0, 3);
    f.camera = cs;
  }
  if (j.contains("bookmarks")) f.bookmarks = jMarks(j["bookmarks"]);
  f.activeCamera = j.value("activeCamera", std::string());
  if (j.contains("animation") && j["animation"].is_object()) f.animation = j["animation"].dump();
  if (j.contains("lights")) f.lights = jLights(j["lights"]);
  if (j.contains("space") && j["space"].is_object()) {
    std::string t = j["space"].value("type", std::string("flat"));
    for (int i = 0; i < 3; i++) if (t == SPACE_IDS[i]) f.space.kind = (SpaceKind)i;
    f.space.radius = std::max(0.05, j["space"].value("radius", 8.0));
  }
  if (f.space.curved() && j.contains("curvedCamera") && j["curvedCamera"].is_object()) {
    CurvedCamState c;
    auto& o = j["curvedCamera"];
    c.iso = jm4(o.value("iso", json::array()));
    c.distance = o.value("distance", 8.0);
    c.iso = cs::renorm(f.space.k(), c.iso);
    f.curvedCam = c;
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
  json j = {{"format", "4dcad-object"}, {"version", 3}, {"id", o.id}, {"name", o.name}, {"node", nodeJ(o.node)}};
  if (json md = usedModifierDefs(o.node); !md.empty()) j["modifierDefs"] = md;
  if (json td = usedTextureDefs(o.node); !td.empty()) j["textureDefs"] = td;
  return j;
}
static LibraryObject jObj(const json& j, const ReadCtx& outer) {
  ReadCtx ctx = outer;
  if (j.contains("version")) ctx.version = j["version"].get<int>();
  for (auto& p : j.value("primitives", json::array())) ctx.filePrims.push_back(jPrim(p));
  registerModifierDefs(j);
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
  {
    LibraryObject o;
    o.id = "builtin-custom";
    o.name = "Custom SDF (GLSL)";
    o.node = makeCustom("Custom SDF");
    out[2].objects.push_back(o);
  }
  for (auto& p : builtinPrimitives()) {
    int d = primitiveDim(p);
    LibraryObject o;
    o.id = "builtin-" + p.id;
    o.name = p.name;
    o.node = nodeFromPrimitive(p, p.name);
    out[d <= 2 ? 0 : d == 3 ? 1 : 2].objects.push_back(o);
  }
  // regular polyhedra with flat faces, the regular 4D polytopes (flat 4D), and the regular
  // honeycombs (tilings of the whole space)
  LibraryFolder poly{"builtin-poly", "Polyhedra & tilings", true, {}};
  for (int t = 0; t < POLY_COUNT; t++) {
    LibraryObject o;
    o.id = std::string("builtin-poly-") + POLY_NAMES[t];
    o.name = POLY_NAMES[t];
    o.node = makeLeaf("polyhedron", POLY_NAMES[t]);
    o.node.poly.type = t;
    o.node.poly.size = 0.7;
    poly.objects.push_back(o);
  }
  {
    LibraryObject o;
    o.id = "builtin-honeycomb";
    o.name = "Honeycomb (tiling)";
    o.node = makeLeaf("honeycomb", "Honeycomb");
    o.node.poly.honey = 0;
    o.node.poly.size = 1.5;
    o.node.color = {0.85, 0.75, 0.55};
    o.node.mat.color2 = {0.35, 0.55, 0.85};
    poly.objects.push_back(o);
  }
  out.push_back(poly);
  return out;
}

}  // namespace fd
