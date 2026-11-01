#include "evaluator.hpp"

namespace fd {

Role nodeRole(const Node& n, EvalCtx ctx, const std::string& activeWorld, const std::function<bool(const std::string&)>& worldExists) {
  if (n.hidden) return Role::Empty;
  if (ctx == EvalCtx::Geo && !activeWorld.empty() && n.id == activeWorld) return Role::Empty;
  if (n.anchor) {
    if (ctx == EvalCtx::World) return Role::Empty;
    if (!worldExists(n.anchor->world)) return Role::Normal;
    if (ctx == EvalCtx::Geo && n.anchor->world == activeWorld) return Role::Tangent;
    return Role::Hug;
  }
  return Role::Normal;
}

void applyModifier(V4& q, const Modifier& m) {
  if (m.type == MOD_MIRROR) q[m.axis] = std::abs(q[m.axis]);
  else if (m.type == MOD_REPEAT) {
    if (m.spacing <= 0) return;
    double r = std::floor(q[m.axis] / m.spacing + 0.5);
    if (m.count > 0) r = clampd(r, -m.count, m.count);
    q[m.axis] -= m.spacing * r;
  } else if (m.type == MOD_ROTATE && m.i != m.j) {
    double ang = -m.angle * PI / 180, ci = q[m.i], cj = q[m.j];
    q[m.i] = std::cos(ang) * ci - std::sin(ang) * cj;
    q[m.j] = std::sin(ang) * ci + std::cos(ang) * cj;
  }
}

double modifierCoords(const Node& n, const std::vector<ModAxis>& ax, V4& q, double* t) {
  for (int k = (int)n.mods.size() - 1; k >= 0; k--) {
    const Modifier& m = n.mods[k];
    if (!ax[k].valid) continue;
    if (m.type == MOD_EXTRUDE) { t[k] = q[ax[k].newAxis]; q[ax[k].newAxis] = 0; }
    else if (m.type == MOD_REVOLVE) {
      int a = ax[k].srcAxis, nn = ax[k].newAxis;
      q[a] = std::hypot(q[a], q[nn]) - m.radius;
      q[nn] = 0;
    } else applyModifier(q, m);
  }
  return 0;
}

double modifierDistance(const Node& n, const std::vector<ModAxis>& ax, double d, const double* t) {
  for (size_t k = 0; k < n.mods.size(); k++) {
    const Modifier& m = n.mods[k];
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
  return r + std::max(0.0, n.round.value_or(0.0)) + std::max(0.0, n.shell.value_or(0.0));
}

std::unordered_map<std::string, Bound> computeBounds(const Node& root, const PrimitiveRegistry& reg) {
  std::unordered_map<std::string, Bound> out;
  std::function<Bound(const Node&)> visit = [&](const Node& n) -> Bound {
    // per-axis half extents around the local origin, pushed through the modifier stack
    std::array<double, 4> e{0, 0, 0, 0};
    auto ax = modifierAxes(n, reg);
    int dimFinal = nodeDim(n, reg);
    if (n.isCustom()) e = {n.codeBound, n.codeBound, n.codeBound, n.codeBound};
    else if (n.isLeaf()) {
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
      if (m.type == MOD_REPEAT) e[m.axis] = m.count > 0 ? e[m.axis] + m.count * m.spacing : INFINITY;
      else if (m.type == MOD_EXTRUDE) e[ax[k].newAxis] = m.mode == ExtrudeMode::Finite ? m.length / 2 : INFINITY;
      else if (m.type == MOD_REVOLVE) { double v = m.radius + e[ax[k].srcAxis]; e[ax[k].srcAxis] = v; e[ax[k].newAxis] = v; }
      else if (m.type == MOD_ROTATE) { double v = std::hypot(e[m.i], e[m.j]); e[m.i] = v; e[m.j] = v; }
    }
    double r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
    if (n.isLeaf()) {
      auto* def = reg.get(n.prim);
      r += std::max(0.0, n.round.value_or(0.0)) + std::max(0.0, n.shell.value_or(0.0));
    }
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

SceneEvaluator::SceneEvaluator(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorld) : root_(root), reg_(reg) {
  this->activeWorld = (!activeWorld.empty() && findNode(root, activeWorld)) ? activeWorld : "";
  walk(root, [&](Node& n, Node*, int) {
    Prep p;
    p.inv = transpose(nodeRotation(n));
    if (n.isLeaf()) p.prim = reg.get(n.prim);
    p.dim = nodeDim(n, reg);
    p.axes = modifierAxes(n, reg);
    prep_[n.id] = p;
  });
}

V4 SceneEvaluator::toLocal(const Node& n, const V4& p) const {
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
    double t[64];
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
  double t[64];
  if (n.mods.size() > 64) return {};
  modifierCoords(n, pr.axes, q, t);
  Sample r;
  if (n.isLeaf()) {
    if (!pr.prim) return {};
    double d;
    if (n.isCustom()) {  // GLSL code cannot run on the CPU: use its bounding box
      double e[4], o = 0, in = -1e9;
      for (int a = 0; a < 4; a++) { e[a] = std::abs(q[a]) - n.codeBound; o += std::max(e[a], 0.0) * std::max(e[a], 0.0); in = std::max(in, e[a]); }
      d = std::sqrt(o) + std::min(in, 0.0);
    } else d = evalPrimitive(*pr.prim, q);
    for (int a = pr.dim; a < 4; a++) d = opExtrudeD(d, std::abs(q[a]) - n.thickness / 2);
    d = modifierDistance(n, pr.axes, d, t);
    double shell = n.shell.value_or(0.0);
    if (shell > 0) d = std::abs(d) - shell;
    d -= n.round.value_or(0.0);
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
  if (role == Role::Tangent || role == Role::Hug) r.d = std::max(r.d, -nd * s - n.anchor->reach);
  return r;
}

}  // namespace fd
