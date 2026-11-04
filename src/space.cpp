#include "space.hpp"

#include "scene.hpp"

namespace fd {
namespace cs {

V4 tnormalize(double k, const V4& v) {
  double l = mdot(k, v, v);
  return l > 1e-30 ? v * (1.0 / std::sqrt(l)) : v;
}

V4 tangent(double k, const V4& p, const V4& v) { return v - p * (mdot(k, v, p) / k); }  // mdot(p, p) = k

V4 renormPoint(double k, const V4& p) {
  V4 q = p;
  if (k < 0 && q[3] < 0) q = -q;  // the upper sheet of the hyperboloid
  double l = mdot(k, q, q) / k;
  if (l <= 1e-30) return origin();
  return q * (1.0 / std::sqrt(l));
}

double distance(const Space& s, const V4& p, const V4& q) {
  double k = s.k();
  if (k > 0) {
    // atan2 form: accurate for small and large angles
    V4 d = p - q, e = p + q;
    return s.R() * 2 * std::atan2(length(d), length(e));
  }
  // hyperbolic: d = 2 asinh(|p - q|_L / 2) (accurate when p and q are close)
  V4 d = p - q;
  double l = mdot(k, d, d);
  return s.R() * 2 * std::asinh(std::sqrt(std::max(l, 0.0)) / 2);
}

V4 dirTo(const Space& s, const V4& p, const V4& q) {
  V4 t = tangent(s.k(), p, q);
  if (mdot(s.k(), t, t) < 1e-28) {  // same point: any tangent direction
    V4 a = tangent(s.k(), p, V4{1, 0, 0, 0});
    if (mdot(s.k(), a, a) < 1e-12) a = tangent(s.k(), p, V4{0, 1, 0, 0});
    return tnormalize(s.k(), a);
  }
  return tnormalize(s.k(), t);
}

void geodesic(const Space& s, V4& p, V4& dir, double d) {
  double k = s.k(), t = d / s.R(), c = C(k, t), sn = S(k, t);
  V4 np = p * c + dir * sn;
  V4 nd = dir * c - p * (k * sn);
  p = renormPoint(k, np);
  dir = tnormalize(k, tangent(k, p, nd));
}

V4 expO(const Space& s, const V4& u) {
  double l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
  if (l < 1e-15) return origin();
  double t = l / s.R(), sn = S(s.k(), t) / l;
  return {u[0] * sn, u[1] * sn, u[2] * sn, C(s.k(), t)};
}

V4 logO(const Space& s, const V4& p) {
  double l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  if (l < 1e-15) return {0, 0, 0, 0};
  double t = s.k() > 0 ? std::atan2(l, p[3]) : std::asinh(l);
  double f = t * s.R() / l;
  return {p[0] * f, p[1] * f, p[2] * f, 0};
}

M4 transvection(double k, const V4& p, const V4& q) {
  // T x = x - mdot(x, p+q) / (k + mdot(p, q)) (p+q) + (2/k) mdot(x, p) q
  double b = mdot(k, p, q), den = k + b;
  M4 m = identity();
  if (std::abs(den) < 1e-14) return m;  // spherical antipodes: no unique geodesic
  V4 s = p + q;
  for (int c = 0; c < 4; c++) {
    V4 x = unit(c);
    V4 y = x - s * (mdot(k, x, s) / den) + q * (2.0 / k * mdot(k, x, p));
    setCol(m, c, y);
  }
  return m;
}

M4 translation(const Space& s, const V4& u) { return transvection(s.k(), origin(), expO(s, u)); }

V4 transport(double k, const V4& p, const V4& q, const V4& v) {
  double den = k + mdot(k, p, q);
  if (std::abs(den) < 1e-14) return v;
  return v - (p + q) * (mdot(k, v, q) / den);
}

M4 inverse(double k, const M4& m) {
  // m^T G m = G  =>  m^-1 = G m^T G  with G = diag(1, 1, 1, k)
  M4 t = transpose(m);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      double g = (r == 3 ? k : 1.0) * (c == 3 ? k : 1.0);
      t[c * 4 + r] *= g;
    }
  return t;
}

M4 renorm(double k, const M4& m) {
  V4 c[4] = {col(m, 0), col(m, 1), col(m, 2), col(m, 3)};
  c[3] = renormPoint(k, c[3]);
  for (int i = 0; i < 3; i++) {
    c[i] = tangent(k, c[3], c[i]);
    for (int j = 0; j < i; j++) c[i] = c[i] - c[j] * mdot(k, c[i], c[j]);
    c[i] = tnormalize(k, c[i]);
  }
  return fromCols(c[0], c[1], c[2], c[3]);
}

M4 rot3(const M4& r) {
  M4 m = r;
  for (int i = 0; i < 3; i++) { m[i * 4 + 3] = 0; m[3 * 4 + i] = 0; }
  m[15] = 1;
  // keep it a rotation (Gram-Schmidt on the 3x3 block)
  return orthonormalize(m);
}

M4 nodeIso(const Space& s, const Transform& xf, double sAnc) {
  V4 u = xf.pos * sAnc;
  u[3] = 0;
  return mul(translation(s, u), rot3(xf.rot));
}

void setFromIso(const Space& s, Transform& xf, const M4& iso, double sAnc) {
  M4 m = renorm(s.k(), iso);
  V4 u = logO(s, col(m, 3));
  xf.pos = u * (1.0 / std::max(sAnc, 1e-12));
  xf.pos[3] = 0;
  xf.rot = rot3(mul(inverse(s.k(), translation(s, u)), m));
}

}  // namespace cs

std::string spaceToJson(const Space& s) { return std::string(SPACE_IDS[(int)s.kind]) + ":" + std::to_string(s.radius); }

}  // namespace fd
