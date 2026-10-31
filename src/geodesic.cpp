#include "geodesic.hpp"

namespace fd {

V4 projectToSurface(const SurfaceField& f, V4 q, int iters) {
  for (int i = 0; i < iters; i++) {
    double d = f.dist(q);
    if (std::abs(d) < 1e-6) break;
    V4 g = f.grad(q);
    double gg = dot(g, g);
    if (gg < 1e-12) break;
    q = q - g * (d / gg);
  }
  return q;
}

M4 tangentFrame(const SurfaceField& f, const V4& p, const M4& hint) {
  V4 n = normalize(f.grad(p));
  const int order[3] = {2, 1, 0};
  V4 t[3];
  std::vector<V4> good{n};
  for (int k = 0; k < 3; k++) {
    V4 v = reject(col(hint, order[k]), n);
    for (int j = 0; j < k; j++) if (length(t[j]) > 0.5) v = reject(v, t[j]);
    t[k] = length(v) > 1e-4 ? normalize(v) : V4{0, 0, 0, 0};
    if (length(t[k]) > 0.5) good.push_back(t[k]);
  }
  auto full = completeBasis(good);
  size_t next = good.size();
  for (int k = 0; k < 3; k++) if (length(t[k]) < 0.5) t[k] = full[next++];
  V4 fwd = t[0], up = t[1], right = t[2];
  if (dot(right, col(hint, 0)) < 0) right = -right;
  return fromCols(right, up, fwd, n);
}

M4 transportFrame(const SurfaceField& f, const V4& p, const M4& frame) {
  V4 n = normalize(f.grad(p));
  V4 c[3];
  for (int k = 0; k < 3; k++) {
    V4 v = reject(col(frame, k), n);
    for (int j = 0; j < k; j++) v = reject(v, c[j]);
    if (length(v) < 1e-6) return tangentFrame(f, p, frame);
    c[k] = normalize(v);
  }
  return fromCols(c[0], c[1], c[2], n);
}

GeoResult shoot(const SurfaceField& f, const V4& p, const M4& frame, std::array<double, 3> dl, double dist, double maxStep) {
  V4 pos = p;
  M4 fr = frame;
  int steps = std::max(1, (int)std::ceil(std::abs(dist) / maxStep));
  double h = dist / steps;
  for (int i = 0; i < steps; i++) {
    V4 dir = normalize(col(fr, 0) * dl[0] + col(fr, 1) * dl[1] + col(fr, 2) * dl[2]);
    pos = projectToSurface(f, pos + dir * h, 3);
    fr = transportFrame(f, pos, fr);
  }
  return {pos, fr};
}

M4 rotateFrame(const M4& frame, int a, int b, double angle) {
  V4 ca = col(frame, a), cb = col(frame, b);
  double c = std::cos(angle), s = std::sin(angle);
  M4 m = frame;
  setCol(m, a, ca * c + cb * s);
  setCol(m, b, ca * -s + cb * c);
  return m;
}

}  // namespace fd
