#include "surface.hpp"

namespace fd {

SurfaceField worldField(const SceneEvaluator& ev, const std::string& worldId) {
  return {[&ev, worldId](const V4& p) { return ev.worldDist(p, worldId); },
          [&ev, worldId](const V4& p) { return ev.worldGrad(p, worldId); }};
}

V4 rayDir(const View& v, double sx, double sy, double tanHalf) {
  return normalize(col(v.frame, 0) * (sx * tanHalf) + col(v.frame, 1) * (sy * tanHalf) + col(v.frame, 2));
}

std::optional<RayHit> castRay(const SceneEvaluator& ev, bool geo, const V4& eye, const V4& dir, double maxStep, const M4& frame0, double maxDist) {
  if (ev.space.curved()) {  // constant curvature: exact geodesics
    const Space& s = ev.space;
    double md = maxDist < 0 ? 120 : maxDist;
    if (s.kind == SpaceKind::Spherical) md = std::min(md, 2 * PI * s.R());
    if (s.kind == SpaceKind::Hyperbolic) md = std::min(md, 25 * s.R());
    V4 p = eye, d = cs::tnormalize(s.k(), cs::tangent(s.k(), eye, dir));
    double t = 0;
    for (int i = 0; i < 800; i++) {
      Sample sm = ev.evalCurved(p);
      if (sm.d < 0.0015 * (1 + t * 0.02)) return RayHit{p, d, t, sm.node, frame0};
      if (t > md || sm.d >= INF * 0.5) break;
      cs::geodesic(s, p, d, sm.d);
      t += sm.d;
    }
    return std::nullopt;
  }
  if (maxDist < 0) maxDist = geo ? 60 : 120;
  V4 p = eye, d4 = dir;
  M4 frame = frame0;
  double t = 0;
  SurfaceField wf;
  if (geo) wf = worldField(ev);
  for (int i = 0; i < 800; i++) {
    Sample s = ev.eval(p, geo);
    if (s.d < 0.0015 * (1 + t * 0.02)) return RayHit{p, d4, t, s.node, frame};
    if (t > maxDist || (!geo && s.d >= INF * 0.5)) break;
    if (geo) {
      double h = std::min(s.d, maxStep);
      p = projectToSurface(wf, p + d4 * h, 2);
      V4 n = normalize(wf.grad(p));
      d4 = normalize(reject(d4, n));
      frame = transportFrame(wf, p, frame);
      t += h;
    } else {
      p = p + d4 * s.d;
      t += s.d;
    }
  }
  return std::nullopt;
}

RayTrace traceRay(const SceneEvaluator& ev, bool geo, const V4& eye, const V4& dir, const QualitySettings& q) {
  RayTrace r;
  double maxDist = geo ? q.maxDistGeo : q.maxDistFull;
  V4 p = eye, d4 = normalize(dir);
  SurfaceField wf;
  if (geo) wf = worldField(ev);
  r.pts.push_back(p);
  for (;;) {
    Sample s = ev.eval(p, geo);
    if (s.d < q.eps * (1 + r.t * 0.02)) { r.reason = RayTrace::Hit; r.node = s.node; break; }
    if (r.t > maxDist) { r.reason = RayTrace::MaxDistance; break; }
    if (r.steps >= q.maxSteps) { r.reason = RayTrace::MaxSteps; break; }
    if (!geo && s.d >= INF * 0.5) { r.reason = RayTrace::Escaped; break; }
    double h = s.d * q.stepScale;
    if (geo) {
      h = std::min(h, (double)q.geoMaxStep);
      p = projectToSurface(wf, p + d4 * h, std::max(1, q.geoProjectIters));
      d4 = normalize(reject(d4, normalize(wf.grad(p))));
    } else {
      h = std::min(h, maxDist + 1 - r.t);  // empty space: stop the line at the distance limit
      p = p + d4 * h;
    }
    r.t += h;
    r.steps++;
    r.pts.push_back(p);
  }
  return r;
}

const char* rayReasonText(RayTrace::Reason r) {
  switch (r) {
    case RayTrace::Hit: return "hit an object";
    case RayTrace::MaxDistance: return "max ray distance";
    case RayTrace::MaxSteps: return "max march steps";
    default: return "left the scene";
  }
}

double curvatureRadius(const SurfaceField& f, const V4& p, const M4& frame) {
  V4 n0 = normalize(f.grad(p));
  const double e = 0.05;
  double kmax = 0;
  for (int c = 0; c < 3; c++) {
    V4 q = projectToSurface(f, p + col(frame, c) * e, 4);
    V4 n1 = normalize(f.grad(q));
    double dist = length(q - p);
    if (dist > 1e-6) kmax = std::max(kmax, length(n1 - n0) / dist);
  }
  return kmax > 1e-4 ? std::min(1 / kmax, 1e4) : 1e4;
}

AnchorResult makeAnchor(const SceneEvaluator& ev, const std::string& worldId, const V4& p, const M4& hint) {
  auto f = worldField(ev, worldId);
  V4 pos = projectToSurface(f, p, 40);
  M4 basis = tangentFrame(f, pos, hint);
  return {pos, Anchor{worldId, basis, curvatureRadius(f, pos, basis)}};
}

M4 basisFromNormal(const V4& n) {
  auto b = completeBasis({normalize(n)});
  return fromCols(b[1], b[2], b[3], b[0]);
}

}  // namespace fd
