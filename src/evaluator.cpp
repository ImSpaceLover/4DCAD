#include "evaluator.hpp"

#include "codegen.hpp"
#include "defs.hpp"
#include "shaderlang.hpp"

#include <unordered_map>

namespace fd {

Role nodeRole(const Node& n, EvalCtx ctx, const std::string& activeWorld, const std::function<bool(const std::string&)>& worldExists) {
  if (n.hidden || n.isCamera()) return Role::Empty;  // cameras have no shape
  if (ctx == EvalCtx::Geo && !activeWorld.empty() && n.id == activeWorld) return Role::Empty;
  if (n.anchor) {
    if (ctx == EvalCtx::World) return Role::Empty;
    if (!worldExists(n.anchor->world)) return Role::Normal;
    if (ctx == EvalCtx::Geo && n.anchor->world == activeWorld) return Role::Tangent;
    return Role::Hug;
  }
  return Role::Normal;
}

double radialCopyAngle(const Modifier& m, double a) {
  // angle (radians) of the radial copy nearest to the polar angle a; copies at start + k * step
  int n = std::max(1, m.count);
  double start = m.startAngle * PI / 180, step = m.angle * PI / 180;
  auto wrapd = [](double x) { x = std::fmod(x + PI, 2 * PI); if (x < 0) x += 2 * PI; return std::abs(x - PI); };
  double rel = std::fmod(a - start, 2 * PI);
  if (rel < 0) rel += 2 * PI;
  int k0 = std::abs(step) > 1e-9 ? (int)clampd(std::floor(rel / step), 0, n - 1) : 0;
  int cand[4] = {k0, std::min(k0 + 1, n - 1), 0, n - 1};
  double best = 1e9, out = start;
  for (int k : cand) {
    double ang = start + k * step, dd = wrapd(a - ang);
    if (dd < best) { best = dd; out = ang; }
  }
  return out;
}

double customSdf(const Node& n, const V4& q) {
  // the custom GLSL code runs here too (the shader-language interpreter, with the same prelude and
  // loop guard as the GPU); code it cannot run is a box of the code bound, as the shader draws it
  std::string err;
  auto prog = sl::compileCached(std::string(sdfHelpersGLSL()) + "\n" + guardLoops(n.code), &err);
  if (prog) {
    sl::Value out;
    if (sl::call(*prog, "sdf", {sl::Value::vec4(q[0], q[1], q[2], q[3])}, out) && out.t == sl::T_FLOAT && std::isfinite(out.v[0])) return out.v[0];
  }
  double o = 0, in = -1e9;
  for (int a = 0; a < 4; a++) { double e = std::abs(q[a]) - n.codeBound; o += std::max(e, 0.0) * std::max(e, 0.0); in = std::max(in, e); }
  return std::sqrt(o) + std::min(in, 0.0);
}

void applyModifier(V4& q, const Modifier& m) {
  if (m.type == MOD_MIRROR) q[m.axis] = std::abs(q[m.axis]) - m.offset;
  else if (m.type == MOD_REPEAT) {
    if (m.spacing <= 0) return;
    double r = std::floor(q[m.axis] / m.spacing + 0.5);
    if (m.count > 0) r = clampd(r, -m.count, m.count);
    q[m.axis] -= m.spacing * r;
  } else if (m.type == MOD_RADIAL && m.i != m.j) {
    // rotate the nearest copy back onto the axis direction, then remove the radial offset
    double ci = q[m.i], cj = q[m.j];
    double ang = -radialCopyAngle(m, (ci == 0 && cj == 0) ? 0 : std::atan2(cj, ci));
    q[m.i] = std::cos(ang) * ci - std::sin(ang) * cj - m.radius;
    q[m.j] = std::sin(ang) * ci + std::cos(ang) * cj;
  }
}

// ---- data-driven modifiers on the CPU: their hooks, interpreted ----
namespace {
struct DefCache {
  std::shared_ptr<const ModifierDef> def;
  std::shared_ptr<const ModRuntime> rt;
};
// per thread: definition id -> its runtime (refreshed when the definition is replaced)
const DefCache* defCache(const Modifier& m) {
  thread_local std::unordered_map<std::string, DefCache> cache;
  auto d = modifierDefOf(m);
  if (!d) return nullptr;
  DefCache& c = cache[m.def];
  if (c.def != d) { c.def = d; c.rt = modRuntime(*d); }
  return c.rt->prog ? &c : nullptr;
}
bool callHook(const Modifier& m, const DefCache& c, const char* hook, std::vector<sl::Value> args, const ModAxis& ax, sl::Value& out) {
  auto P = packedParams(m, *c.def);
  for (size_t v = 0; v + 3 < P.size(); v += 4) args.push_back(sl::Value::vec4(P[v], P[v + 1], P[v + 2], P[v + 3]));
  args.push_back(sl::Value::i(ax.newAxis));
  args.push_back(sl::Value::i(ax.srcAxis));
  return sl::call(*c.rt->prog, std::string(hook) + "_d", args, out);
}
sl::Value v4(const V4& q) { return sl::Value::vec4(q[0], q[1], q[2], q[3]); }
}  // namespace

bool modifierHasBound(const Modifier& m) {
  const DefCache* c = defCache(m);
  return c && c->rt->hooks.bound;
}
double modifierBound(const Modifier& m, const ModAxis& ax, double r) {
  const DefCache* c = defCache(m);
  if (!c || !c->rt->hooks.bound) return r;
  sl::Value out;
  if (!callHook(m, *c, "bound", {sl::Value::f(r)}, ax, out) || !std::isfinite(out.v[0])) return INFINITY;
  return std::max(0.0, out.v[0]);
}

double modifierCoords(const Node& n, const std::vector<ModAxis>& ax, V4& q, ModScratch& s) {
  double* t = s.t;
  for (int k = (int)n.mods.size() - 1; k >= 0; k--) {
    const Modifier& m = n.mods[k];
    if (!ax[k].valid) continue;
    if (m.type == MOD_DEF) {
      s.pre[k] = q;
      const DefCache* c = defCache(m);
      sl::Value out;
      if (c && c->rt->hooks.warp && callHook(m, *c, "warp", {v4(q)}, ax[k], out)) q = {out.v[0], out.v[1], out.v[2], out.v[3]};
      continue;
    }
    if (m.type == MOD_EXTRUDE) { t[k] = q[ax[k].newAxis]; q[ax[k].newAxis] = 0; }
    else if (m.type == MOD_REVOLVE) {
      int a = ax[k].srcAxis, nn = ax[k].newAxis;
      q[a] = std::hypot(q[a], q[nn]) - m.radius;
      q[nn] = 0;
    } else applyModifier(q, m);
  }
  return 0;
}

double modifierDistance(const Node& n, const std::vector<ModAxis>& ax, double d, const ModScratch& s) {
  const double* t = s.t;
  for (size_t k = 0; k < n.mods.size(); k++) {
    const Modifier& m = n.mods[k];
    if (m.type == MOD_DEF && ax[k].valid) {
      const DefCache* c = defCache(m);
      if (!c) continue;
      sl::Value out;
      if (c->rt->hooks.shape && callHook(m, *c, "shape", {sl::Value::f(d), v4(s.pre[k])}, ax[k], out)) d = out.v[0];
      if (c->def->safety < 1) d *= c->def->safety;
      continue;
    }
    if (!ax[k].valid || m.type != MOD_EXTRUDE || m.mode == ExtrudeMode::Infinite) continue;
    d = opExtrudeD(d, m.mode == ExtrudeMode::Half ? t[k] : std::abs(t[k]) - m.length / 2);
  }
  return d;
}

double opExtrudeD(double d, double wy) {
  return std::min(std::max(d, wy), 0.0) + std::hypot(std::max(d, 0.0), std::max(wy, 0.0));
}

static Sample combine(BoolOp op, double k, const Sample& a, const Sample& b) {
  // empty operands (>= INF/2): keep the smooth formulas away from them
  bool ea = a.d >= INF * 0.5, eb = b.d >= INF * 0.5;
  if (ea || eb) {
    if (op == BoolOp::SmoothUnion) return ea ? b : a;
    if (op == BoolOp::SmoothIntersection) return Sample{};
    if (op == BoolOp::SmoothSubtraction) return a;
  }
  auto mixc = [](const std::array<double, 3>& x, const std::array<double, 3>& y, double t) {
    return std::array<double, 3>{mixd(x[0], y[0], t), mixd(x[1], y[1], t), mixd(x[2], y[2], t)};
  };
  switch (op) {
    case BoolOp::Union: return b.d < a.d ? b : a;
    case BoolOp::Intersection: return b.d > a.d ? b : a;
    case BoolOp::Subtraction: { if (-b.d > a.d) { Sample s = b; s.d = -b.d; return s; } return a; }
    case BoolOp::SmoothUnion: {
      double h = clampd(0.5 + 0.5 * (b.d - a.d) / k, 0, 1);
      return {mixd(b.d, a.d, h) - k * h * (1 - h), mixc(b.color, a.color, h), h > 0.5 ? a.node : b.node};
    }
    case BoolOp::SmoothIntersection: {
      double h = clampd(0.5 - 0.5 * (b.d - a.d) / k, 0, 1);
      return {mixd(b.d, a.d, h) + k * h * (1 - h), mixc(b.color, a.color, h), h > 0.5 ? a.node : b.node};
    }
    case BoolOp::SmoothSubtraction: {
      double h = clampd(0.5 - 0.5 * (a.d + b.d) / k, 0, 1);
      return {mixd(a.d, -b.d, h) + k * h * (1 - h), mixc(a.color, b.color, h), h > 0.5 ? b.node : a.node};
    }
  }
  return a;
}

double leafLocalRadius(const Node& n, const PrimitiveDef* def) {
  if (!def) return 0;
  auto e = primitiveExtents(*def);
  for (int a = primitiveDim(*def); a < 4; a++) e[a] = n.thickness / 2;
  double r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
  return r;
}

std::unordered_map<std::string, Bound> computeBounds(const Node& root, const PrimitiveRegistry& reg) {
  std::unordered_map<std::string, Bound> out;
  std::function<Bound(const Node&)> visit = [&](const Node& n) -> Bound {
    // per-axis half extents around the local origin, pushed through the modifier stack
    std::array<double, 4> e{0, 0, 0, 0};
    auto ax = modifierAxes(n, reg);
    int dimFinal = nodeDim(n, reg);
    if (n.isCustom()) e = {n.codeBound, n.codeBound, n.codeBound, n.codeBound};
    else if (n.isHoneycomb()) e = {INFINITY, INFINITY, INFINITY, INFINITY};
    else if (n.isPoly()) {
      // circumradius (x2: curved polyhedra are larger than flat ones of the same inradius)
      double r = n.poly.size * polyCircumRatio(n.poly.type) * 2;
      e = {r, r, r, polyIs4D(n.poly.type) ? r : n.thickness / 2};
    } else if (n.isLeaf()) {
      if (auto* def = reg.get(n.prim)) e = primitiveExtents(*def);
      for (int a = dimFinal; a < 4; a++) e[a] = n.thickness / 2;
    } else {
      std::vector<Bound> kids;
      for (auto& ch : n.children) kids.push_back(visit(ch));
      std::vector<Bound> use = kids;
      if (!kids.empty() && n.op != BoolOp::Union && n.op != BoolOp::SmoothUnion) use = {kids[0]};
      for (auto& k : use)
        for (int a = 0; a < 4; a++) e[a] = std::max(e[a], std::abs(k.c[a]) + k.r);
      if (isSmooth(n.op)) for (auto& x : e) x += n.k;
    }
    for (size_t k = 0; k < n.mods.size(); k++) {
      auto& m = n.mods[k];
      if (!ax[k].valid) continue;
      if (m.type == MOD_MIRROR) e[m.axis] += m.offset;
      else if (m.type == MOD_REPEAT) e[m.axis] = m.count > 0 ? e[m.axis] + m.count * m.spacing : INFINITY;
      else if (m.type == MOD_EXTRUDE) e[ax[k].newAxis] = m.mode == ExtrudeMode::Finite ? m.length / 2 : INFINITY;
      else if (m.type == MOD_REVOLVE) { double v = m.radius + e[ax[k].srcAxis]; e[ax[k].srcAxis] = v; e[ax[k].newAxis] = v; }
      else if (m.type == MOD_RADIAL) { double v = m.radius + std::hypot(e[m.i], e[m.j]); e[m.i] = v; e[m.j] = v; }
      else if (m.type == MOD_DEF && m.raises() && ax[k].newAxis >= 0 && !modifierHasBound(m)) e[ax[k].newAxis] = INFINITY;  // (unknown reach)
    }
    double r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
    // data-driven modifiers: their bound hook gives the new radius (in the stack's order)
    for (size_t k = 0; k < n.mods.size(); k++)
      if (n.mods[k].type == MOD_DEF && ax[k].valid && std::isfinite(r)) r = modifierBound(n.mods[k], ax[k], r);
    Bound b;
    if (!std::isfinite(r)) b = {n.xf.pos, INFINITY};
    else {
      b = {n.xf.pos, r * n.xf.scale};
      if (n.anchor) b.r = b.r * 1.5 + 0.1;  // anchored objects bend along the surface
    }
    out[n.id] = b;
    return b;
  };
  visit(root);
  return out;
}

SceneEvaluator::SceneEvaluator(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorld, const Space& sp)
    : space(sp), root_(root), reg_(reg) {
  this->activeWorld = (!activeWorld.empty() && findNode(root, activeWorld)) ? activeWorld : "";
  if (space.curved()) this->activeWorld.clear();  // curved spaces have no world surfaces
  auto bounds = computeBounds(root, reg);
  walk(root, [&](Node& n, Node*, int) {
    Prep p;
    if (auto b = bounds.find(n.id); b != bounds.end()) p.radius = b->second.r;
    p.inv = transpose(nodeRotation(n));
    if (n.isLeaf()) p.prim = reg.get(n.prim);
    p.dim = nodeDim(n, reg);
    p.axes = modifierAxes(n, reg);
    prep_[n.id] = p;
  });
  if (space.curved()) {
    std::function<void(Node&, double)> visit = [&](Node& n, double sAnc) {
      Prep& p = prep_[n.id];
      p.isoInv = cs::inverse(space.k(), cs::nodeIso(space, n.xf, sAnc));
      p.S = sAnc * n.xf.scale;
      p.factor = curvedFactor(space, std::isfinite(p.radius) ? p.radius * sAnc : INFINITY);
      p.symAxes = symmetryAxes(n, reg);
      if (exactRepeat(n)) {
        p.noRepeat = std::make_shared<Node>(n);
        p.noRepeat->mods.pop_back();
        p.noRepeatPrep = std::make_shared<Prep>(p);
        p.noRepeatPrep->axes = modifierAxes(*p.noRepeat, reg);
      }
      for (auto& c : n.children) visit(c, p.S);
    };
    visit(root, 1.0);
  }
}

double curvedFactor(const Space& s, double radius) {
  // spherical space: normal coordinates stretch distances by at most 1 / sinc(r / R) within radius r
  // (the exponential map shrinks them), so local distances are scaled down to stay safe for marching
  if (s.kind != SpaceKind::Spherical) return 1.0;
  double t = std::min(std::isfinite(radius) ? radius + 0.5 : 1e9, PI * s.R() / 2) / s.R();
  return t < 1e-6 ? 1.0 : std::max(std::sin(t) / t, 0.5);
}

// ---- infinite objects in curved spaces: re-anchoring near a point (precision) ----
// Local axes (0..2) along which a leaf extends forever unchanged: infinite extrusions, with no
// other modifier that would depend on the position along them. Moving the leaf's frame along
// those axes (inside the geodesic plane / line they span) is a symmetry of the shape.
std::vector<int> symmetryAxes(const Node& n, const PrimitiveRegistry& reg) {
  std::vector<int> out;
  if (!n.isLeaf() || n.isPolytope() || n.isCustom()) return out;
  auto ax = modifierAxes(n, reg);
  for (size_t i = 0; i < n.mods.size(); i++) {
    const Modifier& m = n.mods[i];
    if (!ax[i].valid) continue;
    if (m.type != MOD_EXTRUDE) return {};
    if (m.mode == ExtrudeMode::Infinite && ax[i].newAxis < 3) out.push_back(ax[i].newAxis);
  }
  if (out.size() >= 3) return {};  // fills all of space: nothing to gain
  return out;
}

// A frame change for an infinite object that brings its origin next to the camera (c = the
// camera in the object's frame) without changing the object: identity when there is none.
M4 anchorNear(const Space& sp, const Node& n, const PrimitiveRegistry& reg, const V4& c, double S) {
  double k = sp.k();
  auto axes = symmetryAxes(n, reg);
  if (!axes.empty()) {
    // the nearest point of the geodesic plane / line through the origin spanned by those axes:
    // (components along it, w) normalised; its norm^2 = 1 - k |components off it|^2 (no cancellation)
    V4 f{0, 0, 0, c[3]};
    double off2 = 0;
    for (int a = 0; a < 3; a++) {
      if (std::find(axes.begin(), axes.end(), a) != axes.end()) f[a] = c[a];
      else off2 += c[a] * c[a];
    }
    double l = 1 - k * off2;
    if (l <= 1e-12) return identity();
    return cs::transvection(k, cs::origin(), f * (1 / std::sqrt(l)));
  }
  // repeat (the outermost modifier): the copy nearest to c along the line of copies
  if (const Modifier* m = exactRepeat(n); m && m->count == 0) {  // (a finite one would shift its window of copies)
    double kk = repeatCopy(sp, *m, c, S);
    if (kk != 0) { V4 t{0, 0, 0, 0}; t[m->axis] = kk * m->spacing * S; return cs::translation(sp, t); }
  }
  return identity();
}

const Modifier* exactRepeat(const Node& n) {
  if (!n.isLeaf() || n.isPolytope() || n.mods.empty()) return nullptr;
  const Modifier& m = n.mods.back();
  return m.type == MOD_REPEAT && m.spacing > 0 && m.axis >= 0 && m.axis < 3 ? &m : nullptr;
}

double repeatCopy(const Space& sp, const Modifier& m, const V4& q, double S) {
  // where q's foot lies along the geodesic of copies (the local axis), in units of the spacing
  int a = m.axis;
  double o2 = 0;
  for (int i = 0; i < 3; i++) if (i != a) o2 += q[i] * q[i];
  double rho = std::sqrt(std::max(1 - sp.k() * o2, 1e-30));
  double t = sp.k() < 0 ? std::asinh(q[a] / rho) : std::atan2(q[a], q[3]);
  double kk = std::round(t * sp.R() / (m.spacing * S));
  if (m.count > 0) kk = clampd(kk, -m.count, m.count);
  return kk;
}

V4 toSymmetrySlice(const Space& sp, const std::vector<int>& axes, const V4& q) {
  // the transvection that takes q's foot on the geodesic subspace (spanned by the axes) to the
  // origin, in closed form: it keeps the components off the subspace and zeroes those along it
  V4 r = q;
  for (int a : axes) r[a] = 0;
  double o2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
  r[3] = std::sqrt(std::max(1 - sp.k() * o2, 0.0));
  return r;
}

double anchoredRadiusAny(const Node& n, const PrimitiveRegistry& reg) {
  Node c = n;
  if (exactRepeat(c)) c.mods.pop_back();
  c.xf = Transform{};
  c.xf.scale = n.xf.scale;
  auto b = computeBounds(c, reg);
  auto it = b.find(c.id);
  return it == b.end() ? INFINITY : it->second.r;
}

double anchoredRadius(const Node& n, const PrimitiveRegistry& reg) {
  // the node with what the anchoring makes irrelevant removed: the outermost infinite repeat, and
  // the infinite extrusions along its symmetry axes
  Node c = n;
  c.xf = Transform{};
  c.xf.scale = n.xf.scale;
  if (const Modifier* m = exactRepeat(c); m && m->count == 0) c.mods.pop_back();
  auto ax = modifierAxes(c, reg);
  for (size_t i = 0; i < c.mods.size(); i++)
    if (ax[i].valid && c.mods[i].type == MOD_EXTRUDE && c.mods[i].mode == ExtrudeMode::Infinite) { c.mods[i].mode = ExtrudeMode::Finite; c.mods[i].length = 0; }
  auto b = computeBounds(c, reg);
  auto it = b.find(c.id);
  return it == b.end() ? INFINITY : it->second.r;
}

Sample SceneEvaluator::evalCurved(const V4& p) const { return evalCurvedNode(root_, p); }

double SceneEvaluator::leafShape(const Node& n, const Prep& pr, V4& q) const {
  ModScratch t;
  if (n.mods.size() > 64) return INF;
  if (n.isPoly() || n.isHoneycomb()) {  // flat polyhedra / honeycombs (curved ones: evalCurvedNode)
    modifierCoords(n, pr.axes, q, t);
    double d = n.isPoly() ? polyDistFlat(n.poly.type, n.poly.size, q) : honeycombDist(Space{}, n.poly, q);
    d = modifierDistance(n, pr.axes, d, t);
    for (int a = pr.dim; a < 4; a++) d = opExtrudeD(d, std::abs(q[a]) - n.thickness / 2);
    return d;
  }
  if (!pr.prim) return INF;
  modifierCoords(n, pr.axes, q, t);
  double d;
  if (n.isCustom()) d = customSdf(n, q);
  else d = evalPrimitive(*pr.prim, q);
  d = modifierDistance(n, pr.axes, d, t);  // (round / onion are modifiers: before the thickness along unused axes)
  for (int a = pr.dim; a < 4; a++) d = opExtrudeD(d, std::abs(q[a]) - n.thickness / 2);
  return d;
}

Sample SceneEvaluator::evalCurvedNode(const Node& n, const V4& p) const {
  if (n.hidden || n.isCamera()) return {};
  const Prep& pr = prep_.at(n.id);
  V4 q = mul(pr.isoInv, p);  // the node's own frame
  if (n.isPoly() || n.isHoneycomb()) {  // exact flat faces of the curved space, on the model point
    int par = 0;
    double d = n.isPoly() ? polyDistCurved(space, n.poly.type, n.poly.size * pr.S, q) : honeycombDist(space, n.poly, q, &par);
    if (d >= INF * 0.5) return {};
    if (!n.mods.empty() && n.mods.size() <= 64) {  // (their distance modifiers, e.g. Round, in local units)
      ModScratch t;
      V4 lq = cs::logO(space, q) * (1.0 / pr.S);
      for (size_t k = 0; k < n.mods.size(); k++) t.pre[k] = lq;
      d = modifierDistance(n, pr.axes, d / pr.S, t) * pr.S;
    }
    return {d, par && n.poly.alternate && n.isHoneycomb() ? n.mat.color2 : n.color, &n};
  }
  if (n.isLeaf()) {
    // planes, rods, infinite repeats: evaluated from the copy / foot point nearest to p (exact for
    // planes, and keeps the numbers small far from the object's origin)
    // exact in curved space: infinite extrusions are measured from the point's foot on their plane /
    // line (true equidistant slabs and tubes), a repeat uses the nearest exact copy along its line
    if (!pr.symAxes.empty()) q = toSymmetrySlice(space, pr.symAxes, q);
    const Node* shape = &n;
    if (const Modifier* m = exactRepeat(n)) {
      // the nearest copy moved to the origin (closed form, as in the shader)
      int a = m->axis;
      double kk = repeatCopy(space, *m, q, pr.S);
      double o2 = 0;
  for (int i = 0; i < 3; i++) if (i != a) o2 += q[i] * q[i];
      double rho = std::sqrt(std::max(1 - space.k() * o2, 1e-30));
      double t = (space.k() < 0 ? std::asinh(q[a] / rho) : std::atan2(q[a], q[3])) - kk * m->spacing * pr.S / space.R();
      q[a] = rho * cs::S(space.k(), t);
      q[3] = rho * cs::C(space.k(), t);
      shape = pr.noRepeat.get();
    }
    V4 u = cs::logO(space, q) * (1.0 / pr.S);  // local coordinates (normal coordinates at the node)
    double d = shape == &n ? leafShape(n, pr, u) : leafShape(*shape, pr.noRepeatPrep ? *pr.noRepeatPrep : pr, u);
    if (d >= INF * 0.5) return {};
    return {d * pr.S * pr.factor, n.color, &n};
  }
  ModScratch t;
  bool mods = !n.mods.empty() && n.mods.size() <= 64;
  if (mods) {  // group modifiers act on its normal coordinates
    V4 u = cs::logO(space, q) * (1.0 / pr.S);
    modifierCoords(n, pr.axes, u, t);
    u[3] = 0;
    q = cs::expO(space, u * pr.S);
  }
  Sample r;
  bool first = true;
  for (auto& c : n.children) {
    Sample cs = evalCurvedNode(c, q);
    if (first) { r = cs; first = false; continue; }
    r = combine(n.op, std::max(n.k * pr.S, 1e-4), r, cs);
  }
  if (mods && r.d < INF * 0.5) r.d = modifierDistance(n, pr.axes, r.d / pr.S, t) * pr.S;
  return r;
}

bool followLocal(const Node& n, const V4& p, V4& q) {
  // the nearest piece of the path (the ends extend straight on), then: x = arc length, y / z / w =
  // the offset in the path's carried frame there (the shader does the same, see FOLLOW_GLSL)
  if (!n.anchor || !n.anchor->follow || n.anchor->path.size() != (size_t)NPATH + 1) return false;
  const auto& P = n.anchor->path;
  double best = 1e300, bh = 0;
  int bk = 0;
  for (int k = 0; k < NPATH; k++) {
    V4 ab = P[k + 1].pos - P[k].pos;
    double h = dot(p - P[k].pos, ab) / std::max(dot(ab, ab), 1e-24);
    if (k > 0) h = std::max(h, 0.0);
    if (k < NPATH - 1) h = std::min(h, 1.0);
    V4 c = P[k].pos + ab * h;
    double d2 = dot(p - c, p - c);
    if (d2 < best) { best = d2; bh = h; bk = k; }
  }
  const auto &A = P[bk], &B = P[bk + 1];
  double t = clampd(bh, 0, 1);
  V4 c = A.pos + (B.pos - A.pos) * bh, o = p - c;
  V4 Y = normalize(A.y + (B.y - A.y) * t), Z = normalize(A.z + (B.z - A.z) * t), Nn = normalize(A.n + (B.n - A.n) * t);
  double ds = 2 * n.anchor->pathLen / NPATH;
  q = V4{-n.anchor->pathLen + (bk + bh) * ds, dot(o, Y), dot(o, Z), dot(o, Nn)} * (1.0 / n.xf.scale);
  return true;
}

V4 SceneEvaluator::toLocal(const Node& n, const V4& p) const {
  V4 bent;
  if (followLocal(n, p, bent)) return bent;
  V4 q = mul(prep_.at(n.id).inv, p - n.xf.pos);
  return q * (1.0 / n.xf.scale);
}

Sample SceneEvaluator::evalFull(const V4& p) const { wcache_.clear(); return evalNode(root_, p, p, 1, EvalCtx::Full); }
Sample SceneEvaluator::evalGeo(const V4& p) const { wcache_.clear(); return evalNode(root_, p, p, 1, EvalCtx::Geo); }

Sample SceneEvaluator::worldSample(const V4& g, const std::string& worldId) const {
  if (worldId.empty()) return {};
  auto path = pathTo(root_, worldId);
  if (path.empty()) return {};
  V4 p = g;
  double s = 1;
  for (size_t i = 0; i + 1 < path.size(); i++) {
    p = toLocal(*path[i], p);
    s *= path[i]->xf.scale;
    ModScratch t;
    if (path[i]->mods.size() <= 64) modifierCoords(*path[i], prep_.at(path[i]->id).axes, p, t);
  }
  Sample r = evalNode(*path.back(), p, g, s, EvalCtx::World);
  if (r.d < INF * 0.5) r.d *= s;
  return r;
}

double SceneEvaluator::worldDist(const V4& g, const std::string& worldId) const { return worldSample(g, worldId).d; }

V4 SceneEvaluator::worldGrad(const V4& g, const std::string& worldId) const {
  const double e = 1e-4;
  double f0 = worldDist(g, worldId);
  V4 r;
  for (int i = 0; i < 4; i++) { V4 q = g; q[i] += e; r[i] = (worldDist(q, worldId) - f0) / e; }
  return r;
}

Sample SceneEvaluator::evalNode(const Node& n, const V4& p, const V4& g, double sAcc, EvalCtx ctx) const {
  auto exists = [&](const std::string& id) { return prep_.count(id) > 0; };
  Role role = nodeRole(n, ctx, activeWorld, exists);
  if (role == Role::Empty) return {};
  double s = n.xf.scale;
  V4 q = toLocal(n, p);
  double nd = q[3];
  if (role == Role::Tangent) q[3] = 0;
  else if (role == Role::Hug) {
    auto it = wcache_.find(n.anchor->world);
    double wd;
    if (it == wcache_.end()) { wd = worldDist(g, n.anchor->world); wcache_[n.anchor->world] = wd; }
    else wd = it->second;
    q[3] = wd / (sAcc * s);
  }
  auto& pr = prep_.at(n.id);
  ModScratch t;
  if (n.mods.size() > 64) return {};
  modifierCoords(n, pr.axes, q, t);
  Sample r;
  if (n.isLeaf() && n.isPolytope()) {
    V4 lq = q;
    double d = leafShape(n, pr, lq);
    r = {d, n.color, &n};
  } else if (n.isLeaf()) {
    if (!pr.prim) return {};
    double d;
    if (n.isCustom()) d = customSdf(n, q);  // (the GLSL code itself, interpreted)
    else d = evalPrimitive(*pr.prim, q);
    // (round / onion are modifiers: they act on the object's own shape, before it gets its
    // thickness along the unused axes)
    d = modifierDistance(n, pr.axes, d, t);
    for (int a = pr.dim; a < 4; a++) d = opExtrudeD(d, std::abs(q[a]) - n.thickness / 2);
    r = {d, n.color, &n};
  } else {
    bool first = true;
    for (auto& c : n.children) {
      Sample cs = evalNode(c, q, g, sAcc * s, ctx);
      if (first) { r = cs; first = false; continue; }
      r = combine(n.op, std::max(n.k, 1e-4), r, cs);
    }
    if (r.d < INF * 0.5) r.d = modifierDistance(n, pr.axes, r.d, t);
  }
  if (r.d >= INF * 0.5) return r;
  r.d *= s;
  if (role == Role::Tangent || role == Role::Hug) {
    // keep an attached object to its own sheet of the world surface (two-sided, within its size)
    double cut = attachCut(n.anchor->reach, prep_.at(n.id).radius);
    r.d = std::max(r.d, std::abs(nd) * s - cut);
  }
  return r;
}

}  // namespace fd
