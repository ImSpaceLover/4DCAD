#include "codegen.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <regex>
#include "math4.hpp"
#include <sstream>
#include <unordered_map>

namespace fd {

static const char* C = "xyzw";

// SDF helpers available to generated code and to custom SDFs.
static const char* SDF_HELPERS = R"(
#define INF 1e9
float opExtrude(float d, float wy) {
  vec2 w = vec2(d, wy);
  return min(max(w.x, w.y), 0.0) + length(max(w, 0.0));
}
// smooth booleans; an operand >= INF/2 means "empty" (avoids float cancellation in mix(1e9, a, 1))
float sU(float a, float b, float k) { if (b >= INF * 0.5) return a; if (a >= INF * 0.5) return b; float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); return mix(b, a, h) - k * h * (1.0 - h); }
float sI(float a, float b, float k) { if (a >= INF * 0.5 || b >= INF * 0.5) return INF; float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0); return mix(b, a, h) + k * h * (1.0 - h); }
float sS(float a, float b, float k) { if (b >= INF * 0.5 || a >= INF * 0.5) return a; float h = clamp(0.5 - 0.5 * (a + b) / k, 0.0, 1.0); return mix(a, -b, h) + k * h * (1.0 - h); }
)";

// Material = mat2x4: column 0 (rgb, selection highlight), column 1 (emissive, 0, 0, 0).
static const char* SCENE_HELPERS = R"(
bool g_exact = false;      // disable bounding-sphere shortcuts (shadow / AO samples need true distances)
bool g_noEmissive = false; // shadow / AO rays ignore emissive objects
#define MAT mat2x4
float opU(float a, inout MAT ma, float b, MAT mb) { if (b < a) { ma = mb; return b; } return a; }
float opI(float a, inout MAT ma, float b, MAT mb) { if (b > a) { ma = mb; return b; } return a; }
float opS(float a, inout MAT ma, float b, MAT mb) { if (-b > a) { ma = mb; return -b; } return a; }
float opSU(float a, inout MAT ma, float b, MAT mb, float k) {
  if (b >= INF * 0.5) return a;
  if (a >= INF * 0.5) { ma = mb; return b; }
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); ma = mb * (1.0 - h) + ma * h; return mix(b, a, h) - k * h * (1.0 - h);
}
float opSI(float a, inout MAT ma, float b, MAT mb, float k) {
  if (a >= INF * 0.5 || b >= INF * 0.5) return INF;
  float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0); ma = mb * (1.0 - h) + ma * h; return mix(b, a, h) + k * h * (1.0 - h);
}
float opSS(float a, inout MAT ma, float b, MAT mb, float k) {
  if (b >= INF * 0.5 || a >= INF * 0.5) return a;
  float h = clamp(0.5 - 0.5 * (a + b) / k, 0.0, 1.0); ma = ma * (1.0 - h) + mb * h; return mix(a, -b, h) + k * h * (1.0 - h);
}
// procedural 4D solid textures (0..1), evaluated in object-local coordinates
float texChecker(vec4 p) { vec4 f = floor(p); return mod(f.x + f.y + f.z + f.w, 2.0); }
float texStripes(vec4 p) { return step(0.5, fract(p.x + 0.5 * p.w)); }
float texGrid(vec4 p) { vec4 e = 0.5 - abs(fract(p) - 0.5); return 1.0 - step(0.05, min(min(e.x, e.y), min(e.z, e.w))); }
float texNoise(vec4 p) {
  float n = sin(p.x * 1.7 + sin(p.y * 2.3 + p.w)) + sin(p.z * 1.9 + sin(p.w * 2.9 + p.x)) + 0.5 * sin(3.1 * p.y + 2.0 * sin(1.3 * p.z));
  return clamp(0.5 + 0.2 * n, 0.0, 1.0);
}
float texRings(vec4 p) { return step(0.5, fract(length(p))); }
)";

const char* sdfHelpersGLSL() { return SDF_HELPERS; }

std::string renameCustomCode(const std::string& code, const std::string& suffix) {
  // rename every top-level function defined in the code (e.g. sdf -> sdf_c3) so that
  // several custom SDFs (and their helpers) can live in one shader
  static const std::regex def(R"(\b(?:float|vec[234]|mat[234]|int|bool|void)\s+([A-Za-z_]\w*)\s*\()");
  std::vector<std::string> names;
  for (auto it = std::sregex_iterator(code.begin(), code.end(), def); it != std::sregex_iterator(); ++it) {
    std::string n = (*it)[1];
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
  }
  std::string out = code;
  for (auto& n : names) out = std::regex_replace(out, std::regex("\\b" + n + "\\b"), n + suffix);
  return out;
}

static const char* texFn(int t) {
  switch (t) {
    case TEX_CHECKER: return "texChecker";
    case TEX_STRIPES: return "texStripes";
    case TEX_GRID: return "texGrid";
    case TEX_NOISE: return "texNoise";
    case TEX_RINGS: return "texRings";
    default: return nullptr;
  }
}

// Coordinate pass of a modifier stack (outermost first). Extrusions capture their
// coordinate in t<k>; the distance pass (extrudeGLSL) applies them afterwards.
static std::string modifierCoordsGLSL(const Node& n, const std::vector<ModAxis>& ax, const char* v, const std::string& tag) {
  std::ostringstream o;
  auto c = [&](int a) { return std::string(v) + "." + C[a]; };
  for (int k = (int)n.mods.size() - 1; k >= 0; k--) {
    const Modifier& m = n.mods[k];
    if (!ax[k].valid) continue;
    switch (m.type) {
      case MOD_MIRROR: o << "  " << c(m.axis) << " = abs(" << c(m.axis) << ");\n"; break;
      case MOD_REPEAT: {
        if (m.spacing <= 0) break;
        std::string a = c(m.axis), s = glslFloat(m.spacing);
        std::string r = m.count > 0 ? "clamp(floor(" + a + " / " + s + " + 0.5), " + glslFloat(-m.count) + ", " + glslFloat(m.count) + ")"
                                    : "floor(" + a + " / " + s + " + 0.5)";
        o << "  " << a << " -= " << s << " * " << r << ";\n";
        break;
      }
      case MOD_ROTATE: {
        double ang = -m.angle * PI / 180;
        o << "  " << v << "." << C[m.i] << C[m.j] << " = mat2(" << glslFloat(std::cos(ang)) << ", " << glslFloat(std::sin(ang)) << ", "
          << glslFloat(-std::sin(ang)) << ", " << glslFloat(std::cos(ang)) << ") * " << v << "." << C[m.i] << C[m.j] << ";\n";
        break;
      }
      case MOD_EXTRUDE:
        if (m.mode != ExtrudeMode::Infinite) o << "  float t" << tag << k << " = " << c(ax[k].newAxis) << ";\n";
        o << "  " << c(ax[k].newAxis) << " = 0.0;\n";
        break;
      case MOD_REVOLVE:
        o << "  " << c(ax[k].srcAxis) << " = length(vec2(" << c(ax[k].srcAxis) << ", " << c(ax[k].newAxis) << ")) - " << glslFloat(m.radius) << ";\n";
        o << "  " << c(ax[k].newAxis) << " = 0.0;\n";
        break;
    }
  }
  return o.str();
}

static std::string extrudeGLSL(const Node& n, const std::vector<ModAxis>& ax, const std::string& tag) {
  std::ostringstream o;
  for (size_t k = 0; k < n.mods.size(); k++) {
    const Modifier& m = n.mods[k];
    if (!ax[k].valid || m.type != MOD_EXTRUDE || m.mode == ExtrudeMode::Infinite) continue;
    if (m.mode == ExtrudeMode::Half) o << "  d = opExtrude(d, t" << tag << k << ");\n";
    else o << "  d = opExtrude(d, abs(t" << tag << k << ") - " << glslFloat(m.length / 2) << ");\n";
  }
  return o.str();
}

CompiledScene compileScene(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorldIn, bool geo,
                           const std::unordered_set<std::string>* badCode) {
  CompiledScene cs;
  std::unordered_map<std::string, int> index;
  walk(root, [&](Node& n, Node*, int) { index[n.id] = (int)cs.nodeIds.size(); cs.nodeIds.push_back(n.id); });
  std::string world = (!activeWorldIn.empty() && findNode(root, activeWorldIn)) ? activeWorldIn : "";
  cs.hasWorld = !world.empty();
  auto exists = [&](const std::string& id) { return index.count(id) > 0; };
  int N = (int)cs.nodeIds.size();
  EvalCtx main = geo ? EvalCtx::Geo : EvalCtx::Full;

  std::ostringstream o;
  o << "uniform mat4 u_rot[" << N << "];\nuniform vec4 u_pos[" << N << "];\nuniform vec4 u_par[" << N
    << "];\nuniform vec4 u_col[" << N << "];\nuniform vec4 u_bc[" << N << "];\nuniform float u_br[" << N << "];\n"
    << "uniform vec4 u_mat[" << N << "];\nuniform vec4 u_col2[" << N << "];\n" << SDF_HELPERS << SCENE_HELPERS;

  // primitives
  std::map<std::string, std::string> primFn;
  walk(root, [&](Node& n, Node*, int) {
    if (n.isCustom()) {  // user GLSL (functions renamed per node); broken code falls back to its bounding box
      std::string suffix = "_c" + std::to_string(index[n.id]);
      if (badCode && badCode->count(n.code))
        o << "float sdf" << suffix << "(vec4 p) { vec4 e = abs(p) - " << glslFloat(n.codeBound) << "; return length(max(e, 0.0)) + min(max(max(e.x, e.y), max(e.z, e.w)), 0.0); }\n";
      else o << "// custom SDF: " << n.name << "\n" << renameCustomCode(n.code, suffix) << "\n";
      return;
    }
    if (!n.isLeaf() || primFn.count(n.prim)) return;
    auto* def = reg.get(n.prim);
    if (!def) return;
    std::string name = "prim" + std::to_string(primFn.size());
    primFn[n.prim] = name;
    o << "// " << def->name << "\n" << primitiveGLSL(*def, name);
  });

  // world slots: active world + worlds used by 'hug' nodes in this context
  std::vector<std::string> worlds;
  if (!world.empty()) worlds.push_back(world);
  walk(root, [&](Node& n, Node*, int) {
    if (nodeRole(n, main, world, exists) == Role::Hug && std::find(worlds.begin(), worlds.end(), n.anchor->world) == worlds.end())
      worlds.push_back(n.anchor->world);
  });
  auto wslot = [&](const std::string& id) { return (int)(std::find(worlds.begin(), worlds.end(), id) - worlds.begin()); };
  for (size_t k = 0; k < worlds.size(); k++) o << "float worldSDF" << k << "(vec4 g);\nfloat g_world" << k << " = INF;\n";

  std::unordered_set<std::string> emitted;
  auto fnName = [&](const Node& n, EvalCtx ctx) { return "n" + std::to_string(index[n.id]) + (ctx == EvalCtx::World ? "_w" : "_m"); };

  std::function<void(Node&, EvalCtx)> emitNode = [&](Node& n, EvalCtx ctx) {
    std::string name = fnName(n, ctx);
    if (emitted.count(name)) return;
    emitted.insert(name);
    Role role = nodeRole(n, ctx, world, exists);
    if (role != Role::Empty && !n.isLeaf()) for (auto& c : n.children) emitNode(c, ctx);
    int i = index[n.id];
    std::ostringstream L;
    L << "float " << name << "(vec4 p, vec4 g, float sAcc, out MAT m) {\n";
    const PrimitiveDef* def = n.isLeaf() ? reg.get(n.prim) : nullptr;
    (void)def;
    if (role == Role::Empty || (n.isLeaf() && !def) || (!n.isLeaf() && n.children.empty())) {
      L << "  m = MAT(0.0); return INF;\n}\n";
      o << L.str();
      return;
    }
    if (n.isLeaf() && n.mat.emissive > 0) L << "  if (g_noEmissive && u_mat[" << i << "].x > 0.0) { m = MAT(0.0); return INF; }\n";
    // bounding-sphere early out (threshold covers the parent's smooth blend radius)
    if (n.id != root.id) L << "  float bd = length(p - u_bc[" << i << "]) - u_br[" << i << "];\n  if (bd > u_par[" << i << "].w && !g_exact) { m = MAT(0.0); return bd; }\n";
    L << "  float s = u_par[" << i << "].x;\n  vec4 q = (u_rot[" << i << "] * (p - u_pos[" << i << "])) / s;\n  vec4 q0 = q;\n";
    bool anchored = role == Role::Tangent || role == Role::Hug;
    if (anchored) L << "  float nd = q.w;\n";
    if (role == Role::Tangent) L << "  q.w = 0.0;\n";
    if (role == Role::Hug) L << "  q.w = g_world" << wslot(n.anchor->world) << " / (sAcc * s);\n";
    auto ax = modifierAxes(n, reg);
    L << modifierCoordsGLSL(n, ax, "q", "");
    std::string reachExpr = "u_col[" + std::to_string(i) + "].w";  // groups: reach stored in colour.w (unused)
    if (n.isLeaf()) {
      if (n.isCustom()) L << "  float d = sdf_c" << i << "(q);\n";
      else L << "  float d = " << primFn[n.prim] << "(q);\n";
      for (int a = nodeDim(n, reg); a < 4; a++) L << "  d = opExtrude(d, abs(q." << C[a] << ") - 0.5 * u_par[" << i << "].z);\n";
      L << extrudeGLSL(n, ax, "");
      double shell = n.shell.value_or(0.0), round = n.round.value_or(0.0);
      if (shell > 0) L << "  d = abs(d) - " << glslFloat(shell) << ";\n";
      if (round != 0) L << "  d -= " << glslFloat(round) << ";\n";
      L << "  vec3 col = u_col[" << i << "].rgb;\n";
      if (const char* tf = texFn(n.mat.texture))
        L << "  col = mix(col, u_col2[" << i << "].rgb, " << tf << "(q0 * u_mat[" << i << "].y) * u_mat[" << i << "].z);\n";
      L << "  m = MAT(vec4(col, u_col[" << i << "].w), vec4(u_mat[" << i << "].x, 0.0, 0.0, 0.0));\n";
      if (anchored) L << "  return max(d * s, -nd * s - u_par[" << i << "].y);\n}\n";
      else L << "  return d * s;\n}\n";
    } else {
      static const char* opf[6] = {"opU", "opI", "opS", "opSU", "opSI", "opSS"};
      L << "  float k = max(u_par[" << i << "].y, 1e-4);\n  MAT mb;\n";
      L << "  float d = " << fnName(n.children[0], ctx) << "(q, g, sAcc * s, m);\n";
      for (size_t c = 1; c < n.children.size(); c++)
        L << "  { float b = " << fnName(n.children[c], ctx) << "(q, g, sAcc * s, mb); d = " << opf[(int)n.op] << "(d, m, b, mb"
          << (isSmooth(n.op) ? ", k" : "") << "); }\n";
      std::string ex = extrudeGLSL(n, ax, "");
      if (!ex.empty()) L << "  if (d < INF * 0.5) {\n" << ex << "  }\n";
      if (anchored) L << "  return d >= INF * 0.5 ? d : max(d * s, -nd * s - " << reachExpr << ");\n}\n";
      else L << "  return d >= INF * 0.5 ? d : d * s;\n}\n";
    }
    o << L.str();
  };

  for (size_t k = 0; k < worlds.size(); k++) {
    auto path = pathTo(root, worlds[k]);
    Node& target = *path.back();
    emitNode(target, EvalCtx::World);
    o << "float worldSDF" << k << "(vec4 g) {\n  vec4 p = g; float s = 1.0; MAT m;\n";
    for (size_t a = 0; a + 1 < path.size(); a++) {
      int i = index[path[a]->id];
      o << "  p = (u_rot[" << i << "] * (p - u_pos[" << i << "])) / u_par[" << i << "].x; s *= u_par[" << i << "].x;\n";
      o << "  {\n" << modifierCoordsGLSL(*path[a], modifierAxes(*path[a], reg), "p", "w") << "  }\n";
    }
    o << "  return " << fnName(target, EvalCtx::World) << "(p, g, s, m) * s;\n}\n";
  }

  emitNode(root, main);
  std::ostringstream hug;
  walk(root, [&](Node& n, Node*, int) {
    if (nodeRole(n, main, world, exists) == Role::Hug) {
      std::string line = "g_world" + std::to_string(wslot(n.anchor->world)) + " = worldSDF" + std::to_string(wslot(n.anchor->world)) + "(p); ";
      if (hug.str().find(line) == std::string::npos) hug << line;
    }
  });
  o << "float mapScene(vec4 p, out MAT m) { " << hug.str() << "return " << fnName(root, main) << "(p, p, 1.0, m); }\n";
  if (!world.empty()) o << "float worldSDF(vec4 p) { return worldSDF0(p); }\n";
  else o << "float worldSDF(vec4 p) { return INF; }\n";
  cs.glsl = o.str();
  return cs;
}

PackedUniforms packUniforms(Node& root, const PrimitiveRegistry& reg, const CompiledScene& cs, const std::unordered_set<std::string>& highlight) {
  size_t N = cs.nodeIds.size();
  PackedUniforms u;
  u.rot.assign(N * 16, 0); u.pos.assign(N * 4, 0); u.par.assign(N * 4, 0); u.col.assign(N * 4, 0); u.bc.assign(N * 4, 0); u.br.assign(N, 1e9f);
  u.mat.assign(N * 4, 0); u.col2.assign(N * 4, 0);
  auto bounds = computeBounds(root, reg);
  std::unordered_map<std::string, Node*> byId;
  std::unordered_map<std::string, Node*> parentOf;
  walk(root, [&](Node& n, Node* p, int) { byId[n.id] = &n; parentOf[n.id] = p; });
  for (size_t i = 0; i < N; i++) {
    auto it = byId.find(cs.nodeIds[i]);
    if (it == byId.end()) continue;
    Node& n = *it->second;
    M4 inv = transpose(nodeRotation(n));
    for (int k = 0; k < 16; k++) u.rot[i * 16 + k] = (float)inv[k];
    for (int k = 0; k < 4; k++) u.pos[i * 4 + k] = (float)n.xf.pos[k];
    Node* parent = parentOf[n.id];
    double skip = 0.5 + (parent && isSmooth(parent->op) ? std::max(parent->k, 0.0) : 0.0);
    double reach = n.anchor ? n.anchor->reach : 1e4;
    // par = (scale, k | leaf: anchor reach, thickness, skip distance)
    u.par[i * 4 + 0] = (float)n.xf.scale;
    u.par[i * 4 + 1] = (float)(n.isLeaf() ? reach : n.k);
    u.par[i * 4 + 2] = (float)(n.isLeaf() ? n.thickness : 0);
    u.par[i * 4 + 3] = (float)skip;
    if (n.isLeaf()) {
      for (int k = 0; k < 3; k++) u.col[i * 4 + k] = (float)n.color[k];
      u.col[i * 4 + 3] = highlight.count(n.id) ? 1.f : 0.f;
      u.mat[i * 4 + 0] = (float)n.mat.emissive;
      u.mat[i * 4 + 1] = (float)n.mat.texScale;
      u.mat[i * 4 + 2] = (float)n.mat.texStrength;
      for (int k = 0; k < 3; k++) u.col2[i * 4 + k] = (float)n.mat.color2[k];
    } else u.col[i * 4 + 3] = (float)reach;  // groups: colour unused, store reach
    auto b = bounds.find(n.id);
    if (b != bounds.end()) {
      for (int k = 0; k < 4; k++) u.bc[i * 4 + k] = (float)b->second.c[k];
      u.br[i] = std::isfinite(b->second.r) ? (float)b->second.r : 1e9f;
    }
  }
  return u;
}

std::string structureKey(const Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo) {
  std::ostringstream o;
  o << world << "|" << geo << "|";
  walkc(root, [&](const Node& n, const Node*, int depth) {
    o << depth << n.id << (n.isLeaf() ? "L" + n.prim : "G" + std::to_string((int)n.op)) << (n.hidden ? "h" : "") << (n.anchor ? "a" + n.anchor->world : "");
    for (auto& m : n.mods) o << "m" << m.type << m.axis << m.spacing << ":" << m.count << ":" << (int)m.mode << m.length << ":" << m.radius << ":" << m.i << m.j << m.angle;
    if (n.round) o << "r" << *n.round;
    if (n.shell) o << "s" << *n.shell;
    if (n.isLeaf()) o << "t" << n.mat.texture << (n.mat.emissive > 0 ? "e" : "");
    if (n.isCustom()) o << "c" << std::hash<std::string>()(n.code) << ":" << n.codeBound;
    o << ";";
  });
  for (auto& p : reg.custom()) o << p.id << primitiveGLSL(p, "f");
  return o.str();
}

// ---------------------------------------------------------------- standalone SDF (code viewer)
static std::string mat4Const(const M4& m) {
  std::ostringstream o;
  o << "mat4(";
  for (int k = 0; k < 16; k++) o << (k ? ", " : "") << glslFloat(m[k]);
  o << ")";
  return o.str();
}
static std::string vec4Const(const V4& v) {
  return "vec4(" + glslFloat(v[0]) + ", " + glslFloat(v[1]) + ", " + glslFloat(v[2]) + ", " + glslFloat(v[3]) + ")";
}

std::string standaloneGLSL(const Node& top, const PrimitiveRegistry& reg) {
  std::ostringstream pre, body;
  int counter = 0;
  std::function<std::string(const Node&, const std::string&, bool)> emit = [&](const Node& n, const std::string& pin, bool isTop) -> std::string {
    int k = counter++;
    std::string q = "q" + std::to_string(k), d = "d" + std::to_string(k);
    if (n.hidden && !isTop) { body << "  float " << d << " = INF;  // hidden: " << n.name << "\n"; return d; }
    body << "  // " << n.name << "\n";
    if (isTop) body << "  vec4 " << q << " = " << pin << ";\n";
    else body << "  vec4 " << q << " = (" << mat4Const(transpose(nodeRotation(n))) << " * (" << pin << " - " << vec4Const(n.xf.pos) << ")) / "
              << glslFloat(n.xf.scale) << ";\n";
    auto ax = modifierAxes(n, reg);
    std::string tag = std::to_string(k) + "_";
    std::string coords = modifierCoordsGLSL(n, ax, q.c_str(), tag);
    body << coords;
    if (n.isLeaf()) {
      if (n.isCustom()) {
        std::string suffix = "_n" + std::to_string(k);
        pre << "// custom SDF: " << n.name << "\n" << renameCustomCode(n.code, suffix) << "\n";
        body << "  float " << d << " = sdf" << suffix << "(" << q << ");\n";
      } else {
        auto* def = reg.get(n.prim);
        std::string fn = "prim_" + std::to_string(k);
        if (def && !def->steps.empty()) { pre << primitiveGLSL(*def, fn); body << "  float " << d << " = " << fn << "(" << q << ");\n"; }
        else body << "  float " << d << " = -1e9;  // point\n";
        for (int a = nodeDim(n, reg); a < 4; a++)
          body << "  " << d << " = opExtrude(" << d << ", abs(" << q << "." << C[a] << ") - " << glslFloat(n.thickness / 2) << ");  // thin along " << AXIS[a] << "\n";
      }
    } else {
      static const char* opf[6] = {"min(%a, %b)", "max(%a, %b)", "max(%a, -%b)", "sU(%a, %b, %k)", "sI(%a, %b, %k)", "sS(%a, %b, %k)"};
      std::vector<std::string> kids;
      for (auto& c : n.children) kids.push_back(emit(c, q, false));
      body << "  float " << d << " = " << (kids.empty() ? std::string("INF") : kids[0]) << ";\n";
      for (size_t c = 1; c < kids.size(); c++) {
        std::string e = opf[(int)n.op];
        e = std::regex_replace(e, std::regex("%a"), d);
        e = std::regex_replace(e, std::regex("%b"), kids[c]);
        e = std::regex_replace(e, std::regex("%k"), glslFloat(std::max(n.k, 1e-4)));
        body << "  " << d << " = " << e << ";  // " << BOOL_OP_LABELS[(int)n.op] << "\n";
      }
    }
    std::string ex = extrudeGLSL(n, ax, tag);
    if (!ex.empty()) body << std::regex_replace(ex, std::regex("\\bd\\b"), d);
    if (n.isLeaf()) {
      if (n.shell.value_or(0) > 0) body << "  " << d << " = abs(" << d << ") - " << glslFloat(*n.shell) << ";\n";
      if (n.round.value_or(0) != 0) body << "  " << d << " -= " << glslFloat(*n.round) << ";\n";
    }
    if (!isTop && n.xf.scale != 1) body << "  " << d << " *= " << glslFloat(n.xf.scale) << ";\n";
    return d;
  };
  std::string res = emit(top, "p", true);
  std::ostringstream o;
  o << "// SDF of \"" << top.name << "\" in its own local space (generated, self-contained).\n"
    << "// Helpers available: opExtrude, sU, sI, sS, INF.\n" << pre.str() << "float sdf(vec4 p) {\n" << body.str() << "  return " << res << ";\n}\n";
  return o.str();
}

}  // namespace fd
