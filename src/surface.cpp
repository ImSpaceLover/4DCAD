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
