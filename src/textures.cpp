#include "textures.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "animation.hpp"
#include "codegen.hpp"

namespace fd {

void texMapMatrix(const Material& m, int ch, int dim, M4& A, V4& b) {
  const TexMapping& mp = m.tex[texMappingChannel(m, ch)].map;
  // tex = S * R^T * (x - offset): the pattern moves by the offset, turns by the rotation, cells / unit = scale
  M4 Rt = transpose(rotationFromAngles(mp.rot.data()));
  A = Rt;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) A[c * 4 + r] *= mp.scale * mp.axisScale[r];
  V4 o{mp.offset[0], mp.offset[1], mp.offset[2], mp.offset[3]};
  b = -mul(A, o);
  // thin objects: along the axes they do not use they sit on a cell boundary (coordinate 0), which
  // would make grids solid and checkers flicker, so they are put in the middle of a cell
  if (mp.space == TS_OBJECT)
    for (int a = std::max(dim, 0); a < 4; a++) b[a] += 0.5;
}

static double smoothstep(double e0, double e1, double x) {
  double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}
// the curve's points as the shader gets them: sorted, inside 0..1, strictly increasing inputs
static std::vector<std::array<double, 2>> curvePoints(const TexSlot& s) {
  auto c = s.curve;
  if ((int)c.size() > TEX_CURVE_MAX) c.resize(TEX_CURVE_MAX);
  std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a[0] < b[0]; });
  for (size_t k = 0; k < c.size(); k++) {
    c[k][0] = std::clamp(c[k][0], 0.0, 1.0);
    c[k][1] = std::clamp(c[k][1], 0.0, 1.0);
    if (k > 0 && c[k][0] <= c[k - 1][0]) c[k][0] = c[k - 1][0] + 1e-4;
  }
  return c;
}
static double contrastExp(const TexSlot& s) { return std::pow(2.0, 2.0 * std::clamp(s.contrast, -1.0, 1.0)); }
static double invRange(const TexSlot& s) {
  double r = s.high - s.low;
  if (std::abs(r) < 1e-6) r = r < 0 ? -1e-6 : 1e-6;
  return 1.0 / r;
}

double texRemap(const TexSlot& s, double m) {
  // (float steps as the shader's texRemap)
  m = std::clamp((m - s.low) * invRange(s), 0.0, 1.0);
  double g = contrastExp(s);
  if (g != 1) m = m < 0.5 ? 0.5 * std::pow(2 * m, g) : 1 - 0.5 * std::pow(2 - 2 * m, g);
  if (s.invert) m = 1 - m;
  auto c = curvePoints(s);
  if (c.size() >= 2) {
    double res = c[0][1];
    for (size_t i = 1; i < c.size(); i++) {
      if (m <= c[i][0]) { res = m <= c[i - 1][0] ? c[i - 1][1] : c[i - 1][1] + (c[i][1] - c[i - 1][1]) * smoothstep(c[i - 1][0], c[i][0], m); break; }
      res = c[i][1];
    }
    m = res;
  }
  return m;
}

double texMaskOf(TexKind kind, const double v[4]) {
  if (kind == TEXK_MASK) return v[0];
  if (kind == TEXK_RGBA) return v[3];
  return 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2];
}

int texSlotVec4s(const TexSlot& s) {
  auto d = defs().texture(s.def);
  return TEX_SLOT_HEADER + (d ? paramsVec4s(d->params) : 1);
}
int texOffset(const Material& m, int ch) {
  if (ch < 0 || ch >= CH_COUNT || !m.tex[ch].used()) return -1;
  int o = TEX_FRAME_VEC4S;
  for (int c = 0; c < ch; c++) if (m.tex[c].used()) o += texSlotVec4s(m.tex[c]);
  return o;
}
int texTotalVec4s(const Material& m) {
  int o = 0;
  for (auto& t : m.tex) if (t.used()) o += texSlotVec4s(t);
  return o ? o + TEX_FRAME_VEC4S : 0;
}

std::vector<float> packTexSlot(const Material& m, int ch, int dim) {
  const TexSlot& s = m.tex[ch];
  std::vector<float> v((size_t)texSlotVec4s(s) * 4, 0.f);
  M4 A;
  V4 b;
  texMapMatrix(m, ch, dim, A, b);
  for (int k = 0; k < 16; k++) v[k] = (float)A[k];
  for (int k = 0; k < 4; k++) v[16 + k] = (float)b[k];
  v[20] = (float)s.strength;
  v[21] = (float)s.low;
  v[22] = (float)invRange(s);
  v[23] = (float)contrastExp(s);
  auto c = curvePoints(s);
  v[24] = s.invert ? 1.f : 0.f;
  v[25] = (float)c.size();
  v[26] = (float)s.value2;
  for (int k = 0; k < 3; k++) v[28 + k] = (float)s.color2[k];
  for (size_t k = 0; k < c.size(); k++) { v[32 + k * 2] = (float)c[k][0]; v[33 + k * 2] = (float)c[k][1]; }
  if (auto d = defs().texture(s.def)) {
    auto P = packValues(s.params, d->params);
    for (size_t k = 0; k < P.size() && 48 + k < v.size(); k++) v[48 + k] = (float)P[k];
  }
  return v;
}

bool sampleTexSlot(const Material& m, int ch, int dim, const V4& x, double out[4], TexKind* kind) {
  const TexSlot& s = m.tex[ch];
  auto d = defs().texture(s.def);
  if (!d) return false;
  M4 A;
  V4 b;
  texMapMatrix(m, ch, dim, A, b);
  V4 xx = x;
  if (m.tex[texMappingChannel(m, ch)].map.space == TS_OBJECT)
    for (int a = std::max(dim, 0); a < 4; a++) xx[a] = 0;
  V4 tq = mul(A, xx) + b;
  if (kind) *kind = textureKind(*d);
  return runTexture(*d, packValues(s.params, d->params), tq.data(), out);
}

static const V4& spacePoint(const Material& m, int ch, const V4& local, const V4& world, const V4& parent) {
  int sp = m.tex[texMappingChannel(m, ch)].map.space;
  return sp == TS_WORLD ? world : sp == TS_PARENT ? parent : local;
}

std::array<double, 3> texturedColor(const Node& leaf, int dim, const V4& local, const V4& world, const V4& parent) {
  std::array<double, 3> col = leaf.color;
  const TexSlot& s = leaf.mat.tex[CH_COLOR];
  double v[4];
  TexKind kind;
  if (!s.used() || !sampleTexSlot(leaf.mat, CH_COLOR, dim, spacePoint(leaf.mat, CH_COLOR, local, world, parent), v, &kind)) return col;
  std::array<double, 3> target = s.color2;
  double w;
  if (kind == TEXK_MASK) w = texRemap(s, v[0]);
  else { target = {v[0], v[1], v[2]}; w = kind == TEXK_RGBA ? texRemap(s, v[3]) : 1.0; }
  w *= s.strength;
  for (int k = 0; k < 3; k++) col[k] += (target[k] - col[k]) * w;
  return col;
}

double texturedChannel(const Node& leaf, int ch, int dim, const V4& local, const V4& world, const V4& parent) {
  const Material& m = leaf.mat;
  double base = ch == CH_ROUGHNESS ? m.roughness : ch == CH_METALLIC ? m.metallic : ch == CH_SPECULAR ? m.specular
              : ch == CH_EMISSION ? m.emissive : ch == CH_TRANSPARENCY ? m.transparency : 0.0;
  if (ch <= CH_COLOR || ch >= CH_COUNT) return base;
  const TexSlot& s = m.tex[ch];
  double v[4];
  TexKind kind;
  if (!s.used() || !sampleTexSlot(m, ch, dim, spacePoint(m, ch, local, world, parent), v, &kind)) return base;
  double w = texRemap(s, texMaskOf(kind, v)) * s.strength;
  if (ch == CH_BUMP) return s.value2 * w;
  return base + (s.value2 - base) * w;
}

// ================================================================ textures made of shapes
using nlohmann::json;

std::string shapeTextureToJson(const ShapeTexture& st) {
  json ex = json::array();
  for (auto& e : st.exposed) ex.push_back({{"node", e.node}, {"path", e.path}, {"name", e.name}, {"min", e.min}, {"max", e.max}});
  json j = {{"root", json::parse(nodeToJson(st.root))}, {"objectColors", st.objectColors}, {"mergeColors", st.mergeColors},
            {"coverage", COVERAGE_NAMES[std::clamp(st.coverage, 0, COV_COUNT - 1)]}, {"softness", st.softness}, {"width", st.width},
            {"spacing", st.spacing}, {"range", st.range}, {"tile", st.tile}, {"cell", st.cell}, {"exposed", ex}};
  if (!st.sourceId.empty()) j["sourceId"] = st.sourceId;
  return j.dump();
}

ShapeTexture shapeTextureFromJson(const std::string& text) {
  json j = json::parse(text);
  ShapeTexture st;
  st.root = nodeFromJson(j.at("root").dump());
  st.objectColors = j.value("objectColors", true);
  st.mergeColors = j.value("mergeColors", true);
  std::string cov = j.value("coverage", std::string("Inside"));
  for (int k = 0; k < COV_COUNT; k++) if (cov == COVERAGE_NAMES[k]) st.coverage = k;
  st.softness = j.value("softness", 0.02);
  st.width = j.value("width", 0.05);
  st.spacing = j.value("spacing", 0.25);
  st.range = j.value("range", 1.0);
  st.tile = j.value("tile", false);
  if (j.contains("cell") && j["cell"].is_array() && j["cell"].size() == 4) for (int k = 0; k < 4; k++) st.cell[k] = j["cell"][k].get<double>();
  for (auto& e : j.value("exposed", json::array()))
    st.exposed.push_back({e.value("node", std::string()), e.value("path", std::string()), e.value("name", std::string("value")), e.value("min", 0.0), e.value("max", 1.0)});
  st.sourceId = j.value("sourceId", std::string());
  return st;
}

// the modifier parameter a path names (mods.<i>.p.<name>), nullptr if none
static const ParamDef* modParamOf(const Node& n, const std::string& path, int* modIndex = nullptr) {
  if (path.rfind("mods.", 0) != 0) return nullptr;
  size_t dot = path.find('.', 5);
  if (dot == std::string::npos || path.compare(dot, 3, ".p.") != 0) return nullptr;
  int k = std::atoi(path.substr(5, dot - 5).c_str());
  if (k < 0 || k >= (int)n.mods.size() || n.mods[k].type != MOD_DEF) return nullptr;
  auto d = defs().modifier(n.mods[k].def);
  if (!d) return nullptr;
  std::string name = path.substr(dot + 3);
  if (modIndex) *modIndex = k;
  for (auto& p : d->params) if (p.name == name) return &p;
  return nullptr;
}
static const ParamDef* fillParamOf(const Node& n, const std::string& path) {
  if (path.rfind("tex.color.p.", 0) != 0 || !n.mat.tex[CH_COLOR].used()) return nullptr;
  auto d = defs().texture(n.mat.tex[CH_COLOR].def);
  if (!d) return nullptr;
  std::string name = path.substr(12);
  for (auto& p : d->params) if (p.name == name && p.type != P_COLOR) return &p;
  return nullptr;
}

bool exposablePath(const Node& n, const std::string& path) {
  if (path == "pos.x" || path == "pos.y" || path == "pos.z" || path == "pos.w" || path == "scale") return true;
  if (path == "k") return n.isGroup() && isSmooth(n.op);
  if (path == "color") return n.isLeaf();
  if (const ParamDef* p = modParamOf(n, path)) return p != nullptr;
  if (fillParamOf(n, path)) return true;
  return false;
}

static std::string ident(const std::string& name, const std::string& prefix) {
  std::string s = prefix;
  for (char c : name) s += std::isalnum((unsigned char)c) ? c : '_';
  return s;
}
static std::string vec3c(const std::array<double, 3>& c) { return "vec3(" + glslFloat(c[0]) + ", " + glslFloat(c[1]) + ", " + glslFloat(c[2]) + ")"; }

TextureDef compileShapeTexture(const ShapeTexture& st, const PrimitiveRegistry& reg, const std::string& id, const std::string& name) {
  TextureDef d;
  d.id = id;
  d.name = name;
  d.category = "Shapes";
  d.description = "Made of shapes (texture editing mode)";
  d.source = "shapes";
  d.shapes = shapeTextureToJson(st);
  auto pf = [](const char* n, const char* label, double def, double lo, double hi) {
    ParamDef p;
    p.name = n; p.label = label; p.def = def; p.min = lo; p.max = hi;
    return p;
  };
  // coverage / tiling: parameters (switching them never recompiles)
  ParamDef cov = pf("coverage", "Coverage", st.coverage, 0, COV_COUNT - 1);
  cov.type = P_ENUM;
  for (auto* n : COVERAGE_NAMES) cov.options.push_back(n);
  d.params = {cov, pf("softness", "Softness", st.softness, 0.0005, 1), pf("width", "Outline width", st.width, 0.0005, 2),
              pf("spacing", "Band spacing", st.spacing, 0.01, 5), pf("range", "Falloff range", st.range, 0.01, 20)};
  ParamDef tile = pf("tiling", "Tiling", st.tile ? 1 : 0, 0, 1);
  tile.type = P_BOOL;
  d.params.push_back(tile);
  const char* cellN[4] = {"cellX", "cellY", "cellZ", "cellW"};
  const char* cellL[4] = {"Cell X", "Cell Y", "Cell Z", "Cell W"};
  for (int k = 0; k < 4; k++) d.params.push_back(pf(cellN[k], cellL[k], st.cell[k], 0.01, 100));
  std::set<std::string> used;
  for (auto& p : d.params) used.insert(p.name);
  auto unique = [&](std::string n) {
    std::string b = n;
    for (int k = 2; used.count(n); k++) n = b + std::to_string(k);
    used.insert(n);
    return n;
  };

  StandaloneOptions opt;
  opt.color = true;
  // exposed values
  std::map<std::string, const Node*> byId;
  walkc(st.root, [&](const Node& n, const Node*, int) { byId[n.id] = &n; });
  std::set<std::string> exposedColor;
  std::map<std::pair<std::string, std::string>, std::string> fillExpr;  // (leaf, fill parameter) -> variable
  for (auto& e : st.exposed) {
    auto it = byId.find(e.node);
    if (it == byId.end() || !exposablePath(*it->second, e.path)) continue;
    const Node& n = *it->second;
    ParamDef p = pf("", "", 0, e.min, e.max);
    p.label = e.name;
    p.name = unique(ident(e.name, "x_"));
    double v = 0;
    if (e.path == "color") {
      p.type = P_COLOR;
      p.defColor = n.color;
      opt.colorExpr[n.id] = p.name;
      exposedColor.insert(n.id);
    } else if (const ParamDef* mp = modParamOf(n, e.path)) {
      p.type = mp->type;
      p.options = mp->options;
      if (mp->type == P_COLOR) {
        for (int c = 0; c < 3; c++) { getValue(n, e.path + "." + "rgb"[c], v); p.defColor[c] = v; }
        for (int c = 0; c < 3; c++) opt.expr[{n.id, e.path + "." + "rgb"[c]}] = p.name + "." + "rgb"[c];
      } else {
        getValue(n, e.path, v);
        p.def = v;
        opt.expr[{n.id, e.path}] = mp->type == P_FLOAT ? p.name : mp->type == P_BOOL ? "(" + p.name + " ? 1.0 : 0.0)" : "float(" + p.name + ")";
      }
    } else if (const ParamDef* fp = fillParamOf(n, e.path)) {
      p.type = fp->type;
      p.options = fp->options;
      getValue(n, e.path, v);
      p.def = v;
      fillExpr[{n.id, fp->name}] = fp->type == P_FLOAT ? p.name : fp->type == P_BOOL ? "(" + p.name + " ? 1.0 : 0.0)" : "float(" + p.name + ")";
    } else {
      getValue(n, e.path, v);
      p.def = v;
      opt.expr[{n.id, e.path}] = p.name;
    }
    d.params.push_back(p);
  }
  // part colours: colour parameters (object colours) or constants
  std::vector<std::pair<std::array<double, 3>, std::string>> colorParams;
  int fillN = 0;
  std::ostringstream fills;
  walkc(st.root, [&](const Node& n, const Node*, int) {
    if (!n.isLeaf() || n.isCamera()) return;
    if (st.objectColors && !exposedColor.count(n.id)) {
      std::string var;
      if (st.mergeColors)
        for (auto& cp : colorParams) if (cp.first == n.color) var = cp.second;
      if (var.empty()) {
        ParamDef p = pf("", "", 0, 0, 1);
        p.type = P_COLOR;
        p.defColor = n.color;
        p.label = "Colour: " + n.name;
        p.name = unique(ident(n.name, "c_"));
        d.params.push_back(p);
        colorParams.push_back({n.color, p.name});
        var = p.name;
      }
      opt.colorExpr[n.id] = var;
    }
    // the fill: the object's colour slot (a code texture inside the shape), as a material applies it
    const TexSlot& t = n.mat.tex[CH_COLOR];
    auto fd = t.used() ? defs().texture(t.def) : nullptr;
    if (!fd || fd->id == id) return;
    std::string sfx = "_f" + std::to_string(fillN++);
    fills << "// fill of " << n.name << ": " << fd->name << "\n" << transformedTextureCode(*fd, sfx) << "\n";
    // its remap with constants (as texRemap)
    TexSlot probe = t;
    fills << "float remap" << sfx << "(float m) {\n";
    double inv = 1.0 / ((std::abs(t.high - t.low) < 1e-6) ? 1e-6 : (t.high - t.low));
    double g = std::pow(2.0, 2.0 * std::clamp(t.contrast, -1.0, 1.0));
    fills << "  m = clamp((m - " << glslFloat(t.low) << ") * " << glslFloat(inv) << ", 0.0, 1.0);\n";
    if (g != 1) fills << "  m = m < 0.5 ? 0.5 * pow(2.0 * m, " << glslFloat(g) << ") : 1.0 - 0.5 * pow(2.0 - 2.0 * m, " << glslFloat(g) << ");\n";
    if (t.invert) fills << "  m = 1.0 - m;\n";
    auto c = t.curve;
    std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a[0] < b[0]; });
    if (c.size() >= 2) {
      fills << "  if (m <= " << glslFloat(c[0][0]) << ") return " << glslFloat(c[0][1]) << ";\n";
      for (size_t i = 1; i < c.size(); i++)
        fills << "  if (m <= " << glslFloat(c[i][0]) << ") return mix(" << glslFloat(c[i - 1][1]) << ", " << glslFloat(c[i][1]) << ", smoothstep("
              << glslFloat(c[i - 1][0]) << ", " << glslFloat(std::max(c[i][0], c[i - 1][0] + 1e-4)) << ", m));\n";
      fills << "  return " << glslFloat(c.back()[1]) << ";\n";
    } else fills << "  return m;\n";
    fills << "}\n";
    (void)probe;
    M4 A;
    V4 b;
    Material m1;
    m1.tex[CH_COLOR] = t;
    m1.tex[CH_COLOR].mapFrom = -1;
    int dim = nodeDim(n, reg);
    texMapMatrix(m1, CH_COLOR, dim, A, b);
    std::ostringstream f;
    f << "    vec4 tx = %Q;";
    for (int a = std::max(dim, 0); a < 4; a++) f << " tx." << "xyzw"[a] << " = 0.0;";
    f << "\n    vec4 tq = mat4(";
    for (int k = 0; k < 16; k++) f << (k ? ", " : "") << glslFloat(A[k]);
    f << ") * tx + vec4(" << glslFloat(b[0]) << ", " << glslFloat(b[1]) << ", " << glslFloat(b[2]) << ", " << glslFloat(b[3]) << ");\n";
    auto P = packValues(t.params, fd->params);
    std::vector<std::string> pv(P.size());
    for (size_t k = 0; k < P.size(); k++) pv[k] = glslFloat(P[k]);
    for (int i = 0; i < (int)fd->params.size(); i++) {
      auto it = fillExpr.find({n.id, fd->params[i].name});
      if (it != fillExpr.end()) pv[paramOffset(fd->params, i)] = it->second;
    }
    std::string call = "tex" + sfx + "(tq";
    for (size_t k = 0; k + 3 < pv.size(); k += 4) call += ", vec4(" + pv[k] + ", " + pv[k + 1] + ", " + pv[k + 2] + ", " + pv[k + 3] + ")";
    call += ")";
    TexKind kind = textureKind(*fd);
    std::string strength = glslFloat(t.strength);
    if (kind == TEXK_MASK) f << "    %C = mix(%C, " << vec3c(t.color2) << ", remap" << sfx << "(" << call << ") * " << strength << ");\n";
    else if (kind == TEXK_RGB) f << "    %C = mix(%C, " << call << ", " << strength << ");\n";
    else f << "    vec4 tv = " << call << ";\n    %C = mix(%C, tv.rgb, remap" << sfx << "(tv.a) * " << strength << ");\n";
    opt.fill[n.id] = f.str();
  });
  StandaloneCode sc = standaloneParts(st.root, reg, opt);
  std::ostringstream o;
  o << "// Generated from shapes (texture editing mode): edit it there. Editing this code by hand\n"
    << "// turns it into a code texture.\n" << fills.str() << sc.pre;
  o << (st.objectColors ? "vec4" : "float") << " tex(vec4 p) {\n";
  o << "  if (tiling) { vec4 cell = max(vec4(cellX, cellY, cellZ, cellW), vec4(0.01)); p = p - cell * floor(p / cell + 0.5); }\n";
  o << sc.body;
  o << "  float dd = " << sc.d << ";\n  vec3 col = " << sc.c << ";\n";
  o << "  float a = 0.0;\n"
    << "  if (coverage == 0) a = 1.0 - smoothstep(-softness, softness, dd);\n"
    << "  else if (coverage == 1) a = 1.0 - smoothstep(0.0, width, abs(dd));\n"
    << "  else if (coverage == 2) a = smoothstep(0.5 - softness / spacing, 0.5 + softness / spacing, abs(2.0 * fract(dd / spacing) - 1.0));\n"
    << "  else a = clamp(1.0 - dd / range, 0.0, 1.0);\n";
  if (st.objectColors) o << "  return vec4(col, a);\n}\n";
  else o << "  return a * dot(col, vec3(0.299, 0.587, 0.114));\n}\n";
  d.code = o.str();
  return d;
}

bool textureCycle(const std::string& id, const ShapeTexture& st) {
  std::set<std::string> seen;
  std::function<bool(const Node&)> walkNode = [&](const Node& root) {
    bool found = false;
    walkc(root, [&](const Node& n, const Node*, int) {
      if (found || !n.isLeaf() || !n.mat.tex[CH_COLOR].used()) return;
      const std::string& f = n.mat.tex[CH_COLOR].def;
      if (f == id) { found = true; return; }
      if (!seen.insert(f).second) return;
      auto d = defs().texture(f);
      if (d && d->source == "shapes") {
        try { found = walkNode(shapeTextureFromJson(d->shapes).root); } catch (...) {}
      }
    });
    return found;
  };
  return walkNode(st.root);
}

}  // namespace fd
