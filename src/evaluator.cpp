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
  if (m.type == 1) q[m.axis] = std::abs(q[m.axis]);
  else {
    if (m.spacing <= 0) return;
    double r = std::floor(q[m.axis] / m.spacing + 0.5);
    if (m.count > 0) r = clampd(r, -m.count, m.count);
    q[m.axis] -= m.spacing * r;
  }
}

double opExtrudeD(double d, double wy) {
  return std::min(std::max(d, wy), 0.0) + std::hypot(std::max(d, 0.0), std::max(wy, 0.0));
}

static Sample combine(BoolOp op, double k, const Sample& a, const Sample& b) {
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
  return r + std::max(0.0, n.round.value_or(def->round)) + std::max(0.0, n.shell.value_or(def->shell));
}

std::unordered_map<std::string, Bound> computeBounds(const Node& root, const PrimitiveRegistry& reg) {
  std::unordered_map<std::string, Bound> out;
  std::function<Bound(const Node&)> visit = [&](const Node& n) -> Bound {
    V4 c{0, 0, 0, 0};
    double r;
    if (n.isLeaf()) r = leafLocalRadius(n, reg.get(n.prim));
    else {
      std::vector<Bound> kids;
      for (auto& ch : n.children) kids.push_back(visit(ch));
      if (kids.empty()) r = 0;
      else if (n.op != BoolOp::Union && n.op != BoolOp::SmoothUnion) { c = kids[0].c; r = kids[0].r; }
      else {
        bool inf = false;
        for (auto& k : kids) if (!std::isfinite(k.r)) inf = true;
        if (inf) r = INFINITY;
        else {
          for (auto& k : kids) c = c + k.c * (1.0 / kids.size());
          r = 0;
          for (auto& k : kids) r = std::max(r, length(k.c - c) + k.r);
        }
      }
      if (isSmooth(n.op)) r += n.k;
    }
    for (auto& m : n.mods) {
      if (m.type == 1) { r += std::abs(c[m.axis]); c[m.axis] = 0; }
      else r = m.count > 0 ? r + m.count * m.spacing : INFINITY;
    }
    Bound b;
    double s = n.xf.scale;
    if (!std::isfinite(r)) b = {n.xf.pos, INFINITY};
    else {
      b = {n.xf.pos + mul(nodeRotation(n), c) * s, r * s};
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
    if (n.isLeaf()) { p.prim = reg.get(n.prim); p.dim = p.prim ? primitiveDim(*p.prim) : 4; }
    prep_[n.id] = p;
  });
}

V4 SceneEvaluator::toLocal(const Node& n, const V4& p) const {
  V4 q = mul(prep_.at(n.id).inv, p - n.xf.pos);
  return q * (1.0 / n.xf.scale);
}

Sample SceneEvaluator::evalFull(const V4& p) const { wcache_.clear(); return evalNode(root_, p, p, 1, EvalCtx::Full); }
Sample SceneEvaluator::evalGeo(const V4& p) const { wcache_.clear(); return evalNode(root_, p, p, 1, EvalCtx::Geo); }

double SceneEvaluator::worldDist(const V4& g, const std::string& worldId) const {
  if (worldId.empty()) return INF;
  auto path = pathTo(root_, worldId);
  if (path.empty()) return INF;
  V4 p = g;
  double s = 1;
  for (size_t i = 0; i + 1 < path.size(); i++) {
    p = toLocal(*path[i], p);
    s *= path[i]->xf.scale;
    for (auto& m : path[i]->mods) applyModifier(p, m);
  }
  return evalNode(*path.back(), p, g, s, EvalCtx::World).d * s;
}

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
  for (auto& m : n.mods) applyModifier(q, m);
  Sample r;
  if (n.isLeaf()) {
    auto& pr = prep_.at(n.id);
    if (!pr.prim) return {};
    double d = evalPrimitive(*pr.prim, q);
    for (int a = pr.dim; a < 4; a++) d = opExtrudeD(d, std::abs(q[a]) - n.thickness / 2);
    double shell = n.shell.value_or(pr.prim->shell);
    if (shell > 0) d = std::abs(d) - shell;
    d -= n.round.value_or(pr.prim->round);
    r = {d, n.color, &n};
  } else {
    bool first = true;
    for (auto& c : n.children) {
      Sample cs = evalNode(c, q, g, sAcc * s, ctx);
      if (first) { r = cs; first = false; continue; }
      r = combine(n.op, std::max(n.k, 1e-4), r, cs);
    }
  }
  if (r.d >= INF * 0.5) return r;
  r.d *= s;
  if (role == Role::Tangent || role == Role::Hug) r.d = std::max(r.d, -nd * s - n.anchor->reach);
  return r;
}

}  // namespace fd
