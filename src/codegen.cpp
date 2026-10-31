#include "codegen.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>

namespace fd {

static const char* C = "xyzw";

static const char* HELPERS = R"(
#define INF 1e9
bool g_exact = false;  // disable bounding-sphere shortcuts (shadow / AO samples need true distances)
float opExtrude(float d, float wy) {
  vec2 w = vec2(d, wy);
  return min(max(w.x, w.y), 0.0) + length(max(w, 0.0));
}
float opU(float a, inout vec4 ma, float b, vec4 mb) { if (b < a) { ma = mb; return b; } return a; }
float opI(float a, inout vec4 ma, float b, vec4 mb) { if (b > a) { ma = mb; return b; } return a; }
float opS(float a, inout vec4 ma, float b, vec4 mb) { if (-b > a) { ma = mb; return -b; } return a; }
float opSU(float a, inout vec4 ma, float b, vec4 mb, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); ma = mix(mb, ma, h); return mix(b, a, h) - k * h * (1.0 - h);
}
float opSI(float a, inout vec4 ma, float b, vec4 mb, float k) {
  float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0); ma = mix(mb, ma, h); return mix(b, a, h) + k * h * (1.0 - h);
}
float opSS(float a, inout vec4 ma, float b, vec4 mb, float k) {
  float h = clamp(0.5 - 0.5 * (a + b) / k, 0.0, 1.0); ma = mix(ma, mb, h); return mix(a, -b, h) + k * h * (1.0 - h);
}
)";

static std::string modifierGLSL(const Modifier& m, const char* v) {
  std::string a = std::string(v) + "." + C[m.axis];
  if (m.type == 1) return "  " + a + " = abs(" + a + ");\n";
  if (m.spacing <= 0) return "";
  std::string s = glslFloat(m.spacing);
  std::string r = m.count > 0 ? "clamp(floor(" + a + " / " + s + " + 0.5), " + glslFloat(-m.count) + ", " + glslFloat(m.count) + ")"
                              : "floor(" + a + " / " + s + " + 0.5)";
  return "  " + a + " -= " + s + " * " + r + ";\n";
}

CompiledScene compileScene(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorldIn, bool geo) {
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
    << "];\nuniform vec4 u_col[" << N << "];\nuniform vec4 u_bc[" << N << "];\nuniform float u_br[" << N << "];\n" << HELPERS;

  // primitives
  std::map<std::string, std::string> primFn;
  walk(root, [&](Node& n, Node*, int) {
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
    L << "float " << name << "(vec4 p, vec4 g, float sAcc, out vec4 m) {\n";
    const PrimitiveDef* def = n.isLeaf() ? reg.get(n.prim) : nullptr;
    if (role == Role::Empty || (n.isLeaf() && !def) || (!n.isLeaf() && n.children.empty())) {
      L << "  m = vec4(0.0); return INF;\n}\n";
      o << L.str();
      return;
    }
    // bounding-sphere early out (threshold covers the parent's smooth blend radius)
    if (n.id != root.id) L << "  float bd = length(p - u_bc[" << i << "]) - u_br[" << i << "];\n  if (bd > u_par[" << i << "].w && !g_exact) { m = vec4(0.0); return bd; }\n";
    L << "  float s = u_par[" << i << "].x;\n  vec4 q = (u_rot[" << i << "] * (p - u_pos[" << i << "])) / s;\n";
    bool anchored = role == Role::Tangent || role == Role::Hug;
    if (anchored) L << "  float nd = q.w;\n";
    if (role == Role::Tangent) L << "  q.w = 0.0;\n";
    if (role == Role::Hug) L << "  q.w = g_world" << wslot(n.anchor->world) << " / (sAcc * s);\n";
    for (auto& m : n.mods) L << modifierGLSL(m, "q");
    std::string reachExpr = "u_col[" + std::to_string(i) + "].w";  // groups: reach stored in colour.w (unused)
    if (n.isLeaf()) {
      L << "  float d = " << primFn[n.prim] << "(q);\n";
      for (int a = primitiveDim(*def); a < 4; a++) L << "  d = opExtrude(d, abs(q." << C[a] << ") - 0.5 * u_par[" << i << "].z);\n";
      double shell = n.shell.value_or(def->shell), round = n.round.value_or(def->round);
      if (shell > 0) L << "  d = abs(d) - " << glslFloat(shell) << ";\n";
      if (round != 0) L << "  d -= " << glslFloat(round) << ";\n";
      L << "  m = u_col[" << i << "];\n";
      if (anchored) L << "  return max(d * s, -nd * s - u_par[" << i << "].y);\n}\n";
      else L << "  return d * s;\n}\n";
    } else {
      static const char* opf[6] = {"opU", "opI", "opS", "opSU", "opSI", "opSS"};
      L << "  float k = max(u_par[" << i << "].y, 1e-4);\n  vec4 mb;\n";
      L << "  float d = " << fnName(n.children[0], ctx) << "(q, g, sAcc * s, m);\n";
      for (size_t c = 1; c < n.children.size(); c++)
        L << "  { float b = " << fnName(n.children[c], ctx) << "(q, g, sAcc * s, mb); d = " << opf[(int)n.op] << "(d, m, b, mb"
          << (isSmooth(n.op) ? ", k" : "") << "); }\n";
      if (anchored) L << "  return d >= INF * 0.5 ? d : max(d * s, -nd * s - " << reachExpr << ");\n}\n";
      else L << "  return d >= INF * 0.5 ? d : d * s;\n}\n";
    }
    o << L.str();
  };

  for (size_t k = 0; k < worlds.size(); k++) {
    auto path = pathTo(root, worlds[k]);
    Node& target = *path.back();
    emitNode(target, EvalCtx::World);
    o << "float worldSDF" << k << "(vec4 g) {\n  vec4 p = g; float s = 1.0; vec4 m;\n";
    for (size_t a = 0; a + 1 < path.size(); a++) {
      int i = index[path[a]->id];
      o << "  p = (u_rot[" << i << "] * (p - u_pos[" << i << "])) / u_par[" << i << "].x; s *= u_par[" << i << "].x;\n";
      for (auto& m : path[a]->mods) o << modifierGLSL(m, "p");
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
  o << "float mapScene(vec4 p, out vec4 m) { " << hug.str() << "return " << fnName(root, main) << "(p, p, 1.0, m); }\n";
  if (!world.empty()) o << "float worldSDF(vec4 p) { return worldSDF0(p); }\n";
  else o << "float worldSDF(vec4 p) { return INF; }\n";
  cs.glsl = o.str();
  return cs;
}

PackedUniforms packUniforms(Node& root, const PrimitiveRegistry& reg, const CompiledScene& cs, const std::unordered_set<std::string>& highlight) {
  size_t N = cs.nodeIds.size();
  PackedUniforms u;
  u.rot.assign(N * 16, 0); u.pos.assign(N * 4, 0); u.par.assign(N * 4, 0); u.col.assign(N * 4, 0); u.bc.assign(N * 4, 0); u.br.assign(N, 1e9f);
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
    for (auto& m : n.mods) o << "m" << m.type << m.axis << m.spacing << ":" << m.count;
    if (n.round) o << "r" << *n.round;
    if (n.shell) o << "s" << *n.shell;
    o << ";";
  });
  for (auto& p : reg.custom()) o << primitiveToJson(p, false);
  return o.str();
}

}  // namespace fd
