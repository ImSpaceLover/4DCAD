#include "defs.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <regex>
#include <set>

#include <nlohmann/json.hpp>

#include "codegen.hpp"
#include "scene.hpp"
#include "shaderlang.hpp"

namespace fd {

// ================================================================ parameters
int paramScalars(const ParamDef& p) { return p.type == P_COLOR ? 3 : 1; }
int paramsScalars(const std::vector<ParamDef>& ps) {
  int n = 0;
  for (auto& p : ps) n += paramScalars(p);
  return n;
}
int paramsVec4s(const std::vector<ParamDef>& ps) { return std::max(1, (paramsScalars(ps) + 3) / 4); }
int paramOffset(const std::vector<ParamDef>& ps, int i) {
  int o = 0;
  for (int k = 0; k < i && k < (int)ps.size(); k++) o += paramScalars(ps[k]);
  return o;
}

uint64_t ModifierDef::hash() const {
  uint64_t h = 1469598103934665603ull;
  auto add = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xff; h *= 1099511628211ull; };
  add(code);
  for (auto& p : params) { add(p.name); add(std::to_string((int)p.type)); }
  add(std::to_string(raisesAxis) + ":" + std::to_string(safety) + ":" + std::to_string(role));
  return h;
}

// ================================================================ built-in definitions
static ParamDef pf(const char* name, const char* label, double def, double lo, double hi, const char* field = "", const char* tip = "") {
  ParamDef p;
  p.name = name; p.label = label; p.def = def; p.min = lo; p.max = hi; p.field = field; p.tip = tip;
  return p;
}
static ParamDef pi(const char* name, const char* label, double def, double lo, double hi, const char* field = "", const char* tip = "") {
  ParamDef p = pf(name, label, def, lo, hi, field, tip);
  p.type = P_INT;
  return p;
}
static ParamDef pa(const char* name, const char* label, double def, const char* field = "", const char* tip = "") {
  ParamDef p = pf(name, label, def, 0, 3, field, tip);
  p.type = P_AXIS;
  return p;
}
static ParamDef pe(const char* name, const char* label, double def, std::vector<std::string> opts, const char* field = "", const char* tip = "") {
  ParamDef p = pf(name, label, def, 0, (double)opts.size() - 1, field, tip);
  p.type = P_ENUM;
  p.options = std::move(opts);
  return p;
}

const std::vector<ModifierDef>& builtinModifierDefs() {
  static const std::vector<ModifierDef> defs = [] {
    std::vector<ModifierDef> v;
    auto add = [&](ModifierDef d) { d.builtin = true; v.push_back(std::move(d)); };
    {
      ModifierDef d;
      d.id = "mirror"; d.name = "Mirror"; d.category = "Basic"; d.role = MOD_MIRROR;
      d.description = "Reflects the shape across the plane of an axis (both sides show it), optionally moved apart";
      d.params = {pa("axis", "Axis", 0, "axis"), pf("offset", "Offset", 0, 0, 5, "offset", "The shape and its mirror image sit this far from the mirror plane")};
      d.code = "vec4 warp(vec4 p) { return axisSet(p, axis, abs(axisGet(p, axis)) - offset); }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "repeat"; d.name = "Repeat"; d.category = "Basic"; d.role = MOD_REPEAT;
      d.description = "Copies along an axis at a spacing: a number on each side, or endlessly";
      d.params = {pa("axis", "Axis", 0, "axis"), pf("spacing", "Spacing", 3, 0, 20, "spacing"),
                  pi("count", "Copies each side", 2, 0, 50, "count", "0 = endless")};
      d.code =
          "vec4 warp(vec4 p) {\n"
          "  if (spacing <= 0.0) return p;\n"
          "  float x = axisGet(p, axis);\n"
          "  float r = floor(x / spacing + 0.5);\n"
          "  if (count > 0) r = clamp(r, -float(count), float(count));\n"
          "  return axisSet(p, axis, x - spacing * r);\n"
          "}\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "extrude"; d.name = "Extrude"; d.category = "Dimension"; d.role = MOD_EXTRUDE; d.raisesAxis = true;
      d.description = "Stretches the shape into the next dimension (a square becomes a cube, a cube a tesseract)";
      d.params = {pe("mode", "Mode", 0, {"Finite", "Half (one side)", "Infinite"}, "mode"), pf("len", "Length", 2, 0, 20, "length")};
      d.code =
          "vec4 warp(vec4 p) { return axisSet(p, newAxis, 0.0); }\n"
          "float shape(float d, vec4 p) {\n"
          "  float t = axisGet(p, newAxis);\n"
          "  if (mode == 2) return d;\n"
          "  return opExtrude(d, mode == 1 ? t : abs(t) - len * 0.5);\n"
          "}\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "revolve"; d.name = "Revolve"; d.category = "Dimension"; d.role = MOD_REVOLVE; d.raisesAxis = true;
      d.description = "Spins the shape around, into the next dimension (a disc offset from the axis becomes a torus)";
      d.params = {pe("spin", "Spun axis", 0, {"Newest", "X", "Y", "Z"}, "revaxis", "The existing axis spun into the new one"),
                  pf("radius", "Radius", 0, 0, 20, "radius", "Distance of the shape from the spin axis")};
      d.code =
          "vec4 warp(vec4 p) {\n"
          "  float r = length(vec2(axisGet(p, srcAxis), axisGet(p, newAxis))) - radius;\n"
          "  return axisSet(axisSet(p, srcAxis, r), newAxis, 0.0);\n"
          "}\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "radial"; d.name = "Radial repeat"; d.category = "Basic"; d.role = MOD_RADIAL;
      d.description = "Copies around an axis in a plane, at a distance and an angle step";
      d.params = {pa("from", "Plane from", 0, "i"), pa("to", "Plane to", 2, "j"), pf("radius", "Radius", 2, 0, 20, "radius"),
                  pi("count", "Copies", 6, 1, 64, "count"), pf("angleStep", "Angle step", 60, -360, 360, "angle", "Degrees between copies"),
                  pf("startAngle", "Start angle", 0, -360, 360, "startAngle", "Angle of the first copy (degrees)")};
      d.code =
          "vec4 warp(vec4 p) {\n"
          "  if (from == to) return p;\n"
          "  vec2 v = opRadial(vec2(axisGet(p, from), axisGet(p, to)), radius, float(max(count, 1)), radians(startAngle), radians(angleStep));\n"
          "  return axisSet(axisSet(p, from, v.x), to, v.y);\n"
          "}\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "round"; d.name = "Round"; d.category = "Surface";
      d.description = "Inflates the shape, rounding its edges and corners";
      d.params = {pf("radius", "Radius", 0.1, -1, 2)};
      d.code =
          "float shape(float d, vec4 p) { return d - radius; }\n"
          "float bound(float r) { return r + max(radius, 0.0); }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "onion"; d.name = "Onion"; d.category = "Surface";
      d.description = "Hollows the shape into a skin of this half-thickness. Add it again for more, thinner layers (shells within shells)";
      d.params = {pf("thickness", "Thickness", 0.05, 0, 2)};
      d.code =
          "float shape(float d, vec4 p) { return abs(d) - thickness; }\n"
          "float bound(float r) { return r + max(thickness, 0.0); }\n";
      add(d);
    }
    // ---- after Inigo Quilez's distance-function operators (iquilezles.org), in 4D ----
    {
      ModifierDef d;
      d.id = "elongate"; d.name = "Elongate"; d.category = "Deform";
      d.description = "Stretches the shape by cutting it in the middle and inserting straight parts (per axis): a sphere becomes a capsule / rounded box";
      d.params = {pf("hx", "X", 0.5, 0, 10), pf("hy", "Y", 0, 0, 10), pf("hz", "Z", 0, 0, 10), pf("hw", "W", 0, 0, 10)};
      d.code =
          "vec4 warp(vec4 p) { vec4 h = vec4(hx, hy, hz, hw); return p - clamp(p, -h, h); }\n"
          "float bound(float r) { return r + length(vec4(hx, hy, hz, hw)); }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "twist"; d.name = "Twist"; d.category = "Deform"; d.safety = 0.6;
      d.description = "Turns the shape's cross-sections in a plane, more the further along an axis";
      d.params = {pa("from", "Plane from", 0), pa("to", "Plane to", 2), pa("along", "Along", 1),
                  pf("rate", "Rate", 45, -720, 720, "", "Degrees of turn per unit along the axis")};
      d.code =
          "vec4 warp(vec4 p) {\n"
          "  if (from == to) return p;\n"
          "  vec2 v = rot2(vec2(axisGet(p, from), axisGet(p, to)), -radians(rate) * axisGet(p, along));\n"
          "  return axisSet(axisSet(p, from, v.x), to, v.y);\n"
          "}\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "bend"; d.name = "Bend"; d.category = "Deform"; d.safety = 0.6;
      d.description = "Bends the shape in a plane: the turn grows along the plane's first axis (Inigo Quilez's cheap bend)";
      d.params = {pa("from", "Along", 0), pa("to", "Towards", 1), pf("rate", "Amount", 20, -360, 360, "", "Degrees of bend per unit")};
      d.code =
          "vec4 warp(vec4 p) {\n"
          "  if (from == to) return p;\n"
          "  vec2 v = rot2(vec2(axisGet(p, from), axisGet(p, to)), -radians(rate) * axisGet(p, from));\n"
          "  return axisSet(axisSet(p, from, v.x), to, v.y);\n"
          "}\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "displace"; d.name = "Displace"; d.category = "Surface";
      d.description = "Ripples the surface with a sine pattern (a 3D or 4D grid of bumps)";
      d.params = {pf("amp", "Amplitude", 0.05, -1, 1), pf("freq", "Frequency", 8, 0, 50, "", "Bumps per 2 pi units"),
                  ParamDef{"useW", "Along W too", "Also ripples along the 4th axis (4D objects)", P_BOOL, 0, 1, 0}};
      d.code =
          "float shape(float d, vec4 p) {\n"
          "  vec4 s = sin(freq * p);\n"
          "  float b = s.x * s.y * s.z * (useW ? s.w : 1.0);\n"
          "  return (d + amp * b) / (1.0 + abs(amp) * freq);  // (scaled: stays a safe distance)\n"
          "}\n"
          "float bound(float r) { return r + abs(amp); }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "noisedisplace"; d.name = "Noise displace"; d.category = "Surface";
      d.description = "Roughens the surface with smooth 4D noise (fbm): rocks, blobs, wobbly shapes";
      d.params = {pf("amp", "Amplitude", 0.15, -2, 2), pf("scale", "Scale", 2, 0.01, 30, "", "Noise features per unit")};
      d.code =
          "float shape(float d, vec4 p) {\n"
          "  float n = (fbm(p * scale) - 0.4375) * 2.0;\n"
          "  return (d + amp * n) / (1.0 + 2.0 * abs(amp) * scale);  // (scaled: stays a safe distance)\n"
          "}\n"
          "float bound(float r) { return r + abs(amp); }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "taper"; d.name = "Taper"; d.category = "Deform"; d.safety = 0.8;
      d.description = "Shrinks or widens the cross-sections steadily along an axis (a cone from a cylinder, a frustum from a box)";
      d.params = {pa("axis", "Axis", 1), pf("rate", "Rate", -0.3, -2, 2, "", "Change of size per unit along the axis (negative: narrower upwards)")};
      d.code =
          "vec4 warp(vec4 p) {\n"
          "  float x = axisGet(p, axis);\n"
          "  float s = max(1.0 + rate * x, 0.05);\n"
          "  return axisSet(p / s, axis, x);\n"
          "}\n"
          "float shape(float d, vec4 p) { return d * min(max(1.0 + rate * axisGet(p, axis), 0.05), 1.0); }\n"
          "float bound(float r) { return r * (1.0 + abs(rate) * r); }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "softmirror"; d.name = "Soft mirror"; d.category = "Basic";
      d.description = "Mirror with a smooth seam: the shape and its image melt together where they meet";
      d.params = {pa("axis", "Axis", 0), pf("softness", "Smoothness", 0.3, 0.001, 5), pf("offset", "Offset", 0.5, 0, 10)};
      d.code =
          "vec4 warp(vec4 p) { float x = axisGet(p, axis); return axisSet(p, axis, sqrt(x * x + softness * softness) - offset); }\n"
          "float bound(float r) { return r + offset; }\n";
      add(d);
    }
    {
      ModifierDef d;
      d.id = "stretch"; d.name = "Stretch"; d.category = "Deform";
      d.description = "Scales the shape differently along each axis (distances stay safe)";
      d.params = {pf("sx", "X", 1.5, 0.05, 10), pf("sy", "Y", 1, 0.05, 10), pf("sz", "Z", 1, 0.05, 10), pf("sw", "W", 1, 0.05, 10)};
      d.code =
          "vec4 warp(vec4 p) { return p / vec4(sx, sy, sz, sw); }\n"
          "float shape(float d, vec4 p) { return d * min(min(sx, sy), min(sz, sw)); }\n"
          "float bound(float r) { return r * max(max(sx, sy), max(sz, sw)); }\n";
      add(d);
    }
    return v;
  }();
  return defs;
}

// ================================================================ registry
struct DefRegistry::Impl {
  mutable std::mutex mu;
  std::map<std::string, std::shared_ptr<const ModifierDef>> mods;
  std::vector<std::string> order;
  std::map<std::string, std::shared_ptr<const TextureDef>> texs;
  std::vector<std::string> texOrder;
};

DefRegistry::DefRegistry() : impl_(std::make_shared<Impl>()) {
  for (auto& d : builtinModifierDefs()) {
    impl_->mods[d.id] = std::make_shared<const ModifierDef>(d);
    impl_->order.push_back(d.id);
  }
  for (auto& d : builtinTextureDefs()) {
    impl_->texs[d.id] = std::make_shared<const TextureDef>(d);
    impl_->texOrder.push_back(d.id);
  }
}
std::shared_ptr<const TextureDef> DefRegistry::texture(const std::string& id) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->texs.find(id);
  return it == impl_->texs.end() ? nullptr : it->second;
}
std::vector<std::shared_ptr<const TextureDef>> DefRegistry::textures() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  std::vector<std::shared_ptr<const TextureDef>> r;
  for (auto& id : impl_->texOrder) r.push_back(impl_->texs.at(id));
  return r;
}
void DefRegistry::setTexture(const TextureDef& d) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->texs.find(d.id);
  if (it != impl_->texs.end() && it->second->builtin) return;
  if (it == impl_->texs.end()) impl_->texOrder.push_back(d.id);
  TextureDef c = d;
  c.builtin = false;
  impl_->texs[d.id] = std::make_shared<const TextureDef>(c);
}
void DefRegistry::eraseTexture(const std::string& id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->texs.find(id);
  if (it == impl_->texs.end() || it->second->builtin) return;
  impl_->texs.erase(it);
  impl_->texOrder.erase(std::remove(impl_->texOrder.begin(), impl_->texOrder.end(), id), impl_->texOrder.end());
}
std::shared_ptr<const ModifierDef> DefRegistry::modifier(const std::string& id) const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->mods.find(id);
  return it == impl_->mods.end() ? nullptr : it->second;
}
std::vector<std::shared_ptr<const ModifierDef>> DefRegistry::modifiers() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  std::vector<std::shared_ptr<const ModifierDef>> r;
  for (auto& id : impl_->order) r.push_back(impl_->mods.at(id));
  return r;
}
void DefRegistry::setModifier(const ModifierDef& d) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->mods.find(d.id);
  if (it != impl_->mods.end() && it->second->builtin) return;
  if (it == impl_->mods.end()) impl_->order.push_back(d.id);
  ModifierDef c = d;
  c.builtin = false;
  c.role = 0;  // (engine roles belong to the built-in definitions)
  impl_->mods[d.id] = std::make_shared<const ModifierDef>(c);
}
void DefRegistry::eraseModifier(const std::string& id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->mods.find(id);
  if (it == impl_->mods.end() || it->second->builtin) return;
  impl_->mods.erase(it);
  impl_->order.erase(std::remove(impl_->order.begin(), impl_->order.end(), id), impl_->order.end());
}
DefRegistry& defs() {
  static DefRegistry r;
  return r;
}

static const char* roleId(int type) {
  switch (type) {
    case MOD_MIRROR: return "mirror";
    case MOD_REPEAT: return "repeat";
    case MOD_EXTRUDE: return "extrude";
    case MOD_REVOLVE: return "revolve";
    case MOD_RADIAL: return "radial";
    default: return "";
  }
}
std::shared_ptr<const ModifierDef> modifierDefOf(const Modifier& m) { return defs().modifier(m.type == MOD_DEF ? m.def : roleId(m.type)); }

bool Modifier::raises() const {
  if (type == MOD_EXTRUDE || type == MOD_REVOLVE) return true;
  if (type != MOD_DEF) return false;
  auto d = defs().modifier(def);
  return d && d->raisesAxis;
}

// ================================================================ parameter values
static double fieldGet(const Modifier& m, const std::string& f) {
  if (f == "axis") return m.axis;
  if (f == "offset") return m.offset;
  if (f == "spacing") return m.spacing;
  if (f == "count") return m.count;
  if (f == "mode") return (double)(int)m.mode;
  if (f == "length") return m.length;
  if (f == "radius") return m.radius;
  if (f == "i") return m.i;
  if (f == "j") return m.j;
  if (f == "angle") return m.angle;
  if (f == "startAngle") return m.startAngle;
  if (f == "revaxis") return m.axis + 1;
  return 0;
}
static void fieldSet(Modifier& m, const std::string& f, double v) {
  int iv = (int)std::lround(v);
  if (f == "axis") m.axis = std::clamp(iv, 0, 3);
  else if (f == "offset") m.offset = std::max(0.0, v);
  else if (f == "spacing") m.spacing = v;
  else if (f == "count") m.count = std::max(m.type == MOD_RADIAL ? 1 : 0, iv);
  else if (f == "mode") m.mode = (ExtrudeMode)std::clamp(iv, 0, 2);
  else if (f == "length") m.length = std::max(0.0, v);
  else if (f == "radius") m.radius = v;
  else if (f == "i") m.i = std::clamp(iv, 0, 3);
  else if (f == "j") m.j = std::clamp(iv, 0, 3);
  else if (f == "angle") m.angle = v;
  else if (f == "startAngle") m.startAngle = v;
  else if (f == "revaxis") m.axis = std::clamp(iv - 1, -1, 3);
}

static double paramDefault(const ParamDef& p, int comp) { return p.type == P_COLOR ? p.defColor[std::clamp(comp, 0, 2)] : p.def; }

double paramValue(const std::vector<double>& vals, const std::vector<ParamDef>& ps, int i, int comp) {
  if (i < 0 || i >= (int)ps.size()) return 0;
  int o = paramOffset(ps, i) + comp;
  return o < (int)vals.size() ? vals[o] : paramDefault(ps[i], comp);
}
static double sanitize(const ParamDef& p, double v) {
  if (p.type == P_INT || p.type == P_ENUM || p.type == P_AXIS || p.type == P_BOOL) v = std::round(v);
  if (p.type == P_ENUM) v = std::clamp(v, 0.0, std::max(0.0, (double)p.options.size() - 1));
  if (p.type == P_AXIS) v = std::clamp(v, 0.0, 3.0);
  if (p.type == P_BOOL) v = v != 0 ? 1 : 0;
  return v;
}
void setParamValue(std::vector<double>& vals, const std::vector<ParamDef>& ps, int i, double v, int comp) {
  if (i < 0 || i >= (int)ps.size()) return;
  conformValues(vals, ps);
  int o = paramOffset(ps, i) + comp;
  if (o < (int)vals.size()) vals[o] = sanitize(ps[i], v);
}
void conformValues(std::vector<double>& vals, const std::vector<ParamDef>& ps) {
  int n = paramsScalars(ps);
  if ((int)vals.size() == n) return;
  std::vector<double> v(n);
  int o = 0;
  for (auto& p : ps)
    for (int c = 0; c < paramScalars(p); c++, o++) v[o] = o < (int)vals.size() ? vals[o] : paramDefault(p, c);
  vals = v;
}
std::vector<double> packValues(const std::vector<double>& vals, const std::vector<ParamDef>& ps) {
  std::vector<double> v((size_t)paramsVec4s(ps) * 4, 0.0);
  int o = 0;
  for (int i = 0; i < (int)ps.size(); i++)
    for (int c = 0; c < paramScalars(ps[i]); c++, o++) v[o] = paramValue(vals, ps, i, c);
  return v;
}

double modParam(const Modifier& m, const ModifierDef& d, int i, int comp) {
  if (i < 0 || i >= (int)d.params.size()) return 0;
  const ParamDef& p = d.params[i];
  if (d.role && !p.field.empty()) return fieldGet(m, p.field);
  return paramValue(m.params, d.params, i, comp);
}

void setModParam(Modifier& m, const ModifierDef& d, int i, double v, int comp) {
  if (i < 0 || i >= (int)d.params.size()) return;
  const ParamDef& p = d.params[i];
  if (d.role && !p.field.empty()) { fieldSet(m, p.field, sanitize(p, v)); return; }
  setParamValue(m.params, d.params, i, v, comp);
}

void conformParams(Modifier& m, const ModifierDef& d) {
  if (d.role) return;
  conformValues(m.params, d.params);
}

Modifier makeModifier(const ModifierDef& d) {
  Modifier m;
  if (d.role) {
    m.type = d.role;
    if (d.role == MOD_REVOLVE) { m.axis = -1; m.radius = 0; }
    for (int i = 0; i < (int)d.params.size(); i++) setModParam(m, d, i, d.params[i].def);
    return m;
  }
  m.type = MOD_DEF;
  m.def = d.id;
  conformParams(m, d);
  return m;
}

std::vector<double> packedParams(const Modifier& m, const ModifierDef& d) {
  std::vector<double> v((size_t)paramsVec4s(d.params) * 4, 0.0);
  int o = 0;
  for (int i = 0; i < (int)d.params.size(); i++)
    for (int c = 0; c < paramScalars(d.params[i]); c++, o++) v[o] = modParam(m, d, i, c);
  return v;
}

// ================================================================ code
std::string paramDeclsGLSL(const std::vector<ParamDef>& ps) {
  std::string decls;
  const char* C = "xyzw";
  auto comp = [&](int o) { return "P_" + std::to_string(o / 4) + "." + C[o % 4]; };
  int o = 0;
  for (auto& p : ps) {
    switch (p.type) {
      case P_FLOAT: decls += "  float " + p.name + " = " + comp(o) + ";\n"; break;
      case P_BOOL: decls += "  bool " + p.name + " = " + comp(o) + " > 0.5;\n"; break;
      case P_COLOR: decls += "  vec3 " + p.name + " = vec3(" + comp(o) + ", " + comp(o + 1) + ", " + comp(o + 2) + ");\n"; break;
      default: decls += "  int " + p.name + " = int(floor(" + comp(o) + " + 0.5));\n"; break;
    }
    o += paramScalars(p);
  }
  return decls;
}

std::string transformedModifierCode(const ModifierDef& d, const std::string& suffix, HookInfo* hooks) {
  std::string code = renameCustomCode(d.code, suffix);
  int nv = paramsVec4s(d.params);
  std::string extra;
  for (int k = 0; k < nv; k++) extra += ", vec4 P_" + std::to_string(k);
  extra += ", int newAxis, int srcAxis";
  std::string decls = paramDeclsGLSL(d.params);  // the parameter variables, read from the packed vec4s
  for (auto& c : decls) if (c == '\n') c = ' ';
  HookInfo info;
  for (const char* h : {"warp", "shape", "bound"}) {
    std::regex re(std::string(R"(\b(vec4|float)(\s+)()") + h + suffix + R"()\s*\(([^)]*)\)\s*\{)");
    std::smatch m;
    if (!std::regex_search(code, m, re)) continue;
    std::string args = m[4].str();
    std::string rep = m[1].str() + " " + h + suffix + "(" + args + extra + ") {" + decls;  // (one line: errors keep the user's line numbers)
    code = code.substr(0, m.position(0)) + rep + code.substr(m.position(0) + m.length(0));
    if (std::string(h) == "warp") info.warp = true;
    if (std::string(h) == "shape") info.shape = true;
    if (std::string(h) == "bound") info.bound = true;
  }
  if (hooks) *hooks = info;
  return code;
}

static bool validIdent(const std::string& s) {
  if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
  for (char c : s) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
  return true;
}

std::string checkParams(const std::vector<ParamDef>& ps) {
  static const std::set<std::string> reserved = {
      "float", "int", "bool", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "void", "if", "else", "for", "while", "do", "return", "break",
      "continue", "true", "false", "const", "in", "out", "inout", "uniform", "p", "d", "r", "newAxis", "srcAxis", "length", "step", "mix",
      "min", "max", "abs", "sign", "floor", "ceil", "fract", "mod", "clamp", "smoothstep", "sqrt", "pow", "exp", "log", "sin", "cos", "tan",
      "dot", "cross", "normalize", "distance", "radians", "degrees", "PI", "INF",
      // GLSL keywords / reserved words that are not types or statements
      "smooth", "flat", "noperspective", "centroid", "sample", "patch", "layout", "attribute", "varying", "precision", "highp", "mediump",
      "lowp", "invariant", "discard", "struct", "switch", "case", "default", "buffer", "shared", "coherent", "volatile", "restrict",
      "readonly", "writeonly", "subroutine", "uint", "double", "input", "output", "filter", "common", "partition", "active", "asm",
      "class", "union", "enum", "typedef", "template", "this", "goto", "inline", "noinline", "public", "static", "extern", "external",
      "interface", "long", "short", "half", "fixed", "unsigned", "superp", "namespace", "using", "sizeof", "cast", "texture", "atan",
      "sinh", "cosh", "tanh", "trunc", "round", "transpose", "inversesqrt", "exp2", "log2", "asin", "acos"};
  std::set<std::string> seen;
  for (auto& p : ps) {
    if (!validIdent(p.name)) return "parameter name '" + p.name + "' is not a valid name (letters, digits, _)";
    if (reserved.count(p.name) || p.name.rfind("P_", 0) == 0) return "parameter name '" + p.name + "' is reserved";
    if (!seen.insert(p.name).second) return "parameter '" + p.name + "' is listed twice";
    if (p.type == P_ENUM && p.options.empty()) return "parameter '" + p.name + "' (enum) has no options";
  }
  return "";
}

// compile errors: line numbers of the user's code (the prelude comes first)
static std::string userLineError(std::string err) {
  std::string pre = sdfHelpersGLSL();
  int preludeLines = (int)std::count(pre.begin(), pre.end(), '\n') + 1;
  int line = std::atoi(err.c_str());
  if (line > preludeLines && err.find(':') != std::string::npos) err = std::to_string(line - preludeLines) + err.substr(err.find(':'));
  return err;
}

std::string checkModifierDef(const ModifierDef& d) {
  if (auto e = checkParams(d.params); !e.empty()) return e;
  HookInfo h;
  std::string code = transformedModifierCode(d, "_d", &h);
  std::string err;
  auto prog = sl::compile(std::string(sdfHelpersGLSL()) + "\n" + guardLoops(code), err);
  if (!prog) return userLineError(err);
  if (!h.warp && !h.shape) return "define vec4 warp(vec4 p) and / or float shape(float d, vec4 p)";
  int nv = paramsVec4s(d.params);
  std::vector<sl::Type> ps(nv, sl::T_VEC4);
  auto sig = [&](std::vector<sl::Type> head) {
    head.insert(head.end(), ps.begin(), ps.end());
    head.push_back(sl::T_INT);
    head.push_back(sl::T_INT);
    return head;
  };
  sl::Type rt;
  if (h.warp && !(sl::hasFunction(*prog, "warp_d", sig({sl::T_VEC4}), &rt) && rt == sl::T_VEC4)) return "warp must be: vec4 warp(vec4 p)";
  if (h.shape && !(sl::hasFunction(*prog, "shape_d", sig({sl::T_FLOAT, sl::T_VEC4}), &rt) && rt == sl::T_FLOAT))
    return "shape must be: float shape(float d, vec4 p)";
  if (h.bound && !(sl::hasFunction(*prog, "bound_d", sig({sl::T_FLOAT}), &rt) && rt == sl::T_FLOAT)) return "bound must be: float bound(float r)";
  return "";
}

// ================================================================ files
using nlohmann::json;

static json paramsToJson(const std::vector<ParamDef>& params) {
  json ps = json::array();
  for (auto& p : params) {
    json pj = {{"name", p.name}, {"type", PARAM_TYPE_NAMES[p.type]}};
    if (!p.label.empty()) pj["label"] = p.label;
    if (!p.tip.empty()) pj["tip"] = p.tip;
    if (p.type == P_COLOR) pj["default"] = {p.defColor[0], p.defColor[1], p.defColor[2]};
    else { pj["default"] = p.def; pj["min"] = p.min; pj["max"] = p.max; }
    if (p.type == P_ENUM) pj["options"] = p.options;
    ps.push_back(pj);
  }
  return ps;
}
static std::vector<ParamDef> paramsFromJson(const json& arr) {
  std::vector<ParamDef> out;
  for (auto& pj : arr) {
    ParamDef p;
    p.name = pj.value("name", std::string("value"));
    p.label = pj.value("label", std::string());
    p.tip = pj.value("tip", std::string());
    std::string t = pj.value("type", std::string("float"));
    for (int k = 0; k < P_TYPE_COUNT; k++) if (t == PARAM_TYPE_NAMES[k]) p.type = (ParamType)k;
    if (p.type == P_COLOR) {
      auto c = pj.value("default", json::array({1, 1, 1}));
      for (int k = 0; k < 3 && k < (int)c.size(); k++) p.defColor[k] = c[k].get<double>();
    } else {
      p.def = pj.value("default", 0.0);
      p.min = pj.value("min", 0.0);
      p.max = pj.value("max", 1.0);
    }
    if (p.type == P_ENUM) p.options = pj.value("options", std::vector<std::string>{"A", "B"});
    if (p.type == P_AXIS) { p.min = 0; p.max = 3; }
    out.push_back(p);
  }
  return out;
}

json modifierDefToJson(const ModifierDef& d) {
  json j = {{"format", "4dcad-modifier"}, {"version", 1}, {"id", d.id}, {"name", d.name}, {"code", d.code}, {"params", paramsToJson(d.params)}};
  if (!d.description.empty()) j["description"] = d.description;
  if (!d.category.empty()) j["category"] = d.category;
  if (d.raisesAxis) j["raisesAxis"] = true;
  if (d.safety != 1) j["safety"] = d.safety;
  return j;
}

ModifierDef modifierDefFromJson(const json& j) {
  ModifierDef d;
  d.id = j.at("id").get<std::string>();
  d.name = j.value("name", d.id);
  d.description = j.value("description", std::string());
  d.category = j.value("category", std::string("Custom"));
  d.code = j.value("code", std::string());
  d.raisesAxis = j.value("raisesAxis", false);
  d.safety = std::clamp(j.value("safety", 1.0), 0.05, 1.0);
  d.params = paramsFromJson(j.value("params", json::array()));
  return d;
}

json valuesToJson(const std::vector<double>& vals, const std::vector<ParamDef>& ps) {
  json j = json::object();
  for (int i = 0; i < (int)ps.size(); i++) {
    const ParamDef& p = ps[i];
    if (p.type == P_COLOR) j[p.name] = {paramValue(vals, ps, i, 0), paramValue(vals, ps, i, 1), paramValue(vals, ps, i, 2)};
    else j[p.name] = paramValue(vals, ps, i);
  }
  return j;
}
void valuesFromJson(std::vector<double>& vals, const std::vector<ParamDef>& ps, const json& j) {
  conformValues(vals, ps);
  if (!j.is_object()) return;
  for (int i = 0; i < (int)ps.size(); i++) {
    const ParamDef& p = ps[i];
    if (!j.contains(p.name)) continue;
    const json& v = j[p.name];
    if (p.type == P_COLOR && v.is_array()) { for (int c = 0; c < 3 && c < (int)v.size(); c++) setParamValue(vals, ps, i, v[c].get<double>(), c); }
    else if (v.is_number()) setParamValue(vals, ps, i, v.get<double>());
    else if (v.is_boolean()) setParamValue(vals, ps, i, v.get<bool>() ? 1 : 0);
  }
}

json modParamsToJson(const Modifier& m, const ModifierDef& d) {
  json j = json::object();
  for (int i = 0; i < (int)d.params.size(); i++) {
    const ParamDef& p = d.params[i];
    if (p.type == P_COLOR) j[p.name] = {modParam(m, d, i, 0), modParam(m, d, i, 1), modParam(m, d, i, 2)};
    else j[p.name] = modParam(m, d, i);
  }
  return j;
}

void modParamsFromJson(Modifier& m, const ModifierDef& d, const json& j) {
  conformParams(m, d);
  if (!j.is_object()) return;
  for (int i = 0; i < (int)d.params.size(); i++) {
    const ParamDef& p = d.params[i];
    if (!j.contains(p.name)) continue;
    const json& v = j[p.name];
    if (p.type == P_COLOR && v.is_array()) { for (int c = 0; c < 3 && c < (int)v.size(); c++) setModParam(m, d, i, v[c].get<double>(), c); }
    else if (v.is_number()) setModParam(m, d, i, v.get<double>());
    else if (v.is_boolean()) setModParam(m, d, i, v.get<bool>() ? 1 : 0);
  }
}

void appendRoundShell(std::vector<Modifier>& mods, double round, double shell) {
  // (the old fields' order: shell, then rounding)
  if (shell > 0) {
    auto d = defs().modifier("onion");
    Modifier m = makeModifier(*d);
    setModParam(m, *d, 0, shell);
    mods.push_back(m);
  }
  if (round != 0) {
    auto d = defs().modifier("round");
    Modifier m = makeModifier(*d);
    setModParam(m, *d, 0, round);
    mods.push_back(m);
  }
}

std::shared_ptr<const ModRuntime> modRuntime(const ModifierDef& d) {
  static std::mutex mu;
  static std::map<uint64_t, std::shared_ptr<const ModRuntime>> cache;
  uint64_t h = d.hash();
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
  }
  auto rt = std::make_shared<ModRuntime>();
  std::string code = transformedModifierCode(d, "_d", &rt->hooks);
  std::string err;
  rt->prog = sl::compile(std::string(sdfHelpersGLSL()) + "\n" + guardLoops(code), err);
  rt->error = err;
  rt->vec4s = paramsVec4s(d.params);
  std::lock_guard<std::mutex> lk(mu);
  if (cache.size() > 256) cache.clear();
  cache[h] = rt;
  return rt;
}

// ================================================================ textures
uint64_t TextureDef::hash() const {
  uint64_t h = 1099511628211ull ^ 0x7e57u;
  auto add = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xff; h *= 1099511628211ull; };
  add(code);
  for (auto& p : params) { add(p.name); add(std::to_string((int)p.type)); }
  return h;
}

TexKind textureKind(const TextureDef& d) {
  static const std::regex re(R"(\b(float|vec3|vec4)\s+tex\s*\()");
  std::smatch m;
  if (!std::regex_search(d.code, m, re)) return TEXK_MASK;
  return m[1] == "vec4" ? TEXK_RGBA : m[1] == "vec3" ? TEXK_RGB : TEXK_MASK;
}

const char* legacyTextureId(int type) {
  static const char* ids[13] = {"", "checker", "stripes", "grid", "noise", "rings", "dots", "waves", "marble", "wood", "cells", "bricks", "gradient"};
  return type > 0 && type < 13 ? ids[type] : "";
}

static ParamDef pcol(const char* name, const char* label, double r, double g, double b) {
  ParamDef p = pf(name, label, 0, 0, 1);
  p.type = P_COLOR;
  p.defColor = {r, g, b};
  return p;
}

const std::vector<TextureDef>& builtinTextureDefs() {
  static const std::vector<TextureDef> defs = [] {
    std::vector<TextureDef> v;
    auto T = [&](const char* id, const char* name, const char* cat, const char* desc, std::vector<ParamDef> ps, const char* code) {
      TextureDef d;
      d.id = id; d.name = name; d.category = cat; d.description = desc; d.builtin = true; d.params = std::move(ps); d.code = code;
      v.push_back(d);
    };
    // (the defaults reproduce the textures of older versions exactly)
    T("checker", "Checker", "Patterns", "Alternating 4D cubes.", {},
      "float tex(vec4 p) {\n  vec4 f = floor(p);\n  return mod(f.x + f.y + f.z + f.w, 2.0);\n}\n");
    T("stripes", "Stripes", "Patterns", "Parallel stripes across an axis.",
      {pa("axis", "Across", 0), pf("width", "Width", 0.5, 0, 1), pf("slant", "W slant", 0.5, -2, 2, "", "How much the stripes lean through W")},
      "float tex(vec4 p) {\n  return step(1.0 - width, fract(axisGet(p, axis) + slant * p.w));\n}\n");
    T("grid", "Grid", "Patterns", "Lines along every cell face.", {pf("width", "Line width", 0.05, 0, 0.5)},
      "float tex(vec4 p) {\n  vec4 e = 0.5 - abs(fract(p) - 0.5);\n  return 1.0 - step(width, min(min(e.x, e.y), min(e.z, e.w)));\n}\n");
    T("noise", "Noise", "Patterns", "Smooth wobbly noise.", {pf("amount", "Contrast", 0.2, 0, 1)},
      "float tex(vec4 p) {\n"
      "  float n = sin(p.x * 1.7 + sin(p.y * 2.3 + p.w)) + sin(p.z * 1.9 + sin(p.w * 2.9 + p.x)) + 0.5 * sin(3.1 * p.y + 2.0 * sin(1.3 * p.z));\n"
      "  return clamp(0.5 + amount * n, 0.0, 1.0);\n}\n");
    T("rings", "Rings", "Patterns", "Concentric hyperspherical shells.", {pf("width", "Width", 0.5, 0, 1)},
      "float tex(vec4 p) {\n  return step(1.0 - width, fract(length(p)));\n}\n");
    T("dots", "Dots", "Patterns", "A ball in every cell.", {pf("radius", "Radius", 0.32, 0, 1)},
      "float tex(vec4 p) {\n  return step(length(fract(p) - 0.5), radius);\n}\n");
    T("waves", "Waves", "Patterns", "Soft wavy bands.", {pf("wobble", "Wobble", 0.35, 0, 2)},
      "float tex(vec4 p) {\n  return 0.5 + 0.5 * sin(6.2832 * (p.x + wobble * sin(2.5 * p.y + p.w) + 0.25 * p.z));\n}\n");
    T("marble", "Marble", "Natural", "Veins bent by noise.", {pf("turbulence", "Turbulence", 6, 0, 20)},
      "float tex(vec4 p) {\n  return 0.5 + 0.5 * sin(3.0 * p.x + turbulence * fbm(p));\n}\n");
    T("wood", "Wood", "Natural", "Growth rings around the Y axis.", {pf("rings", "Rings", 3, 0.1, 20), pf("grain", "Grain", 1.5, 0, 5)},
      "float tex(vec4 p) {\n  return smoothstep(0.35, 0.65, fract(length(p.xz) * rings + grain * fbm(p * 0.8)));\n}\n");
    T("cells", "Cells", "Natural", "Edges of 4D Voronoi cells.", {pf("edge", "Edge", 0.03, 0, 0.5), pf("softness", "Softness", 0.05, 0.001, 0.5)},
      "float tex(vec4 p) {  // feature points of the 16 nearest lattice cells\n"
      "  vec4 base = floor(p) + step(0.5, fract(p)) - 1.0;\n"
      "  float d1 = 9.0, d2 = 9.0;\n"
      "  for (int k = 0; k < 16; k++) {\n"
      "    vec4 c = base + corner(k);\n"
      "    float d = length(p - c - vec4(h4(c), h4(c + 11.3), h4(c + 23.7), h4(c + 37.1)));\n"
      "    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;\n"
      "  }\n"
      "  return 1.0 - smoothstep(edge, edge + softness, d2 - d1);\n}\n");
    T("bricks", "Bricks", "Patterns", "Mortar lines of offset bricks.", {pf("mortar", "Mortar", 0.06, 0, 0.5)},
      "float tex(vec4 p) {\n"
      "  vec4 q = p * vec4(1.0, 2.0, 1.0, 1.0);\n"
      "  float shift = 0.5 * mod(floor(q.y), 2.0);  // every other row offset by half a brick\n"
      "  q.x += shift; q.z += shift;\n"
      "  vec4 e = 0.5 - abs(fract(q) - 0.5);\n"
      "  return 1.0 - step(mortar, min(min(e.x, e.y), e.z));\n}\n");
    T("gradient", "Gradient", "Patterns", "A ramp along an axis (0 at -1, 1 at +1).", {pa("axis", "Along", 1)},
      "float tex(vec4 p) {\n  return clamp(0.5 + 0.5 * axisGet(p, axis), 0.0, 1.0);\n}\n");
    T("palette", "Cosine palette", "Colour", "Inigo Quilez's palette a + b cos(2 pi (c t + d)) along an axis.",
      {pa("axis", "Along", 0), pcol("a", "Offset (a)", 0.5, 0.5, 0.5), pcol("b", "Amplitude (b)", 0.5, 0.5, 0.5), pcol("c", "Frequency (c)", 1, 1, 1),
       pcol("ph", "Phase (d)", 0, 0.33, 0.67)},
      "vec3 tex(vec4 p) {\n  float t = axisGet(p, axis);\n  return clamp(a + b * cos(6.28318 * (c * t + ph)), 0.0, 1.0);\n}\n");
    T("colorgradient", "Colour gradient", "Colour", "Two colours along an axis.",
      {pa("axis", "Along", 1), pcol("c0", "From", 0.1, 0.2, 0.8), pcol("c1", "To", 1.0, 0.6, 0.1)},
      "vec3 tex(vec4 p) {\n  return mix(c0, c1, clamp(0.5 + 0.5 * axisGet(p, axis), 0.0, 1.0));\n}\n");
    return v;
  }();
  return defs;
}

static std::string oneLine(std::string s) {
  for (auto& c : s) if (c == '\n') c = ' ';
  return s;
}

std::string transformedTextureCode(const TextureDef& d, const std::string& suffix) {
  std::string code = renameCustomCode(d.code, suffix);
  std::string extra;
  for (int k = 0; k < paramsVec4s(d.params); k++) extra += ", vec4 P_" + std::to_string(k);
  std::regex re(std::string(R"(\b(float|vec3|vec4)(\s+)tex)") + suffix + R"(\s*\(([^)]*)\)\s*\{)");
  std::smatch m;
  if (std::regex_search(code, m, re))
    code = code.substr(0, m.position(0)) + m[1].str() + " tex" + suffix + "(" + m[3].str() + extra + ") {" + oneLine(paramDeclsGLSL(d.params)) +
           code.substr(m.position(0) + m.length(0));
  return code;
}

std::string checkTextureDef(const TextureDef& d) {
  if (auto e = checkParams(d.params); !e.empty()) return e;
  std::string err;
  auto prog = sl::compile(std::string(sdfHelpersGLSL()) + "\n" + guardLoops(transformedTextureCode(d, "_d")), err);
  if (!prog) return userLineError(err);
  std::vector<sl::Type> sig{sl::T_VEC4};
  for (int k = 0; k < paramsVec4s(d.params); k++) sig.push_back(sl::T_VEC4);
  sl::Type rt;
  if (!sl::hasFunction(*prog, "tex_d", sig, &rt) || !(rt == sl::T_FLOAT || rt == sl::T_VEC3 || rt == sl::T_VEC4))
    return "define float tex(vec4 p) (a mask), vec3 tex(vec4 p) (a colour) or vec4 tex(vec4 p) (colour + coverage)";
  return "";
}

std::shared_ptr<const TexRuntime> texRuntime(const TextureDef& d) {
  static std::mutex mu;
  static std::map<uint64_t, std::shared_ptr<const TexRuntime>> cache;
  uint64_t h = d.hash();
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
  }
  auto rt = std::make_shared<TexRuntime>();
  std::string err;
  rt->prog = sl::compile(std::string(sdfHelpersGLSL()) + "\n" + guardLoops(transformedTextureCode(d, "_d")), err);
  rt->error = err;
  rt->kind = textureKind(d);
  rt->vec4s = paramsVec4s(d.params);
  std::lock_guard<std::mutex> lk(mu);
  if (cache.size() > 256) cache.clear();
  cache[h] = rt;
  return rt;
}

bool runTexture(const TextureDef& d, const std::vector<double>& packed, const double p[4], double out[4]) {
  auto rt = texRuntime(d);
  out[0] = out[1] = out[2] = 0; out[3] = 1;
  if (!rt->prog) return false;
  std::vector<sl::Value> args;
  args.push_back(sl::Value::vec(4, p));
  for (int k = 0; k < rt->vec4s; k++) {
    double v[4] = {0, 0, 0, 0};
    for (int c = 0; c < 4 && k * 4 + c < (int)packed.size(); c++) v[c] = packed[k * 4 + c];
    args.push_back(sl::Value::vec(4, v));
  }
  sl::Value r;
  if (!sl::call(*rt->prog, "tex_d", args, r)) return false;
  if (r.t == sl::T_FLOAT) out[0] = r.v[0];
  else for (int c = 0; c < (r.t == sl::T_VEC4 ? 4 : 3); c++) out[c] = r.v[c];
  return true;
}

json textureDefToJson(const TextureDef& d) {
  json j = {{"format", "4dcad-texture"}, {"version", 1}, {"id", d.id}, {"name", d.name}, {"code", d.code}, {"params", paramsToJson(d.params)}};
  if (!d.description.empty()) j["description"] = d.description;
  if (!d.category.empty()) j["category"] = d.category;
  return j;
}
TextureDef textureDefFromJson(const json& j) {
  TextureDef d;
  d.id = j.at("id").get<std::string>();
  d.name = j.value("name", d.id);
  d.description = j.value("description", std::string());
  d.category = j.value("category", std::string("Custom"));
  d.code = j.value("code", std::string());
  d.params = paramsFromJson(j.value("params", json::array()));
  return d;
}

}  // namespace fd
