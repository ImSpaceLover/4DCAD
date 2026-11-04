#include "camera.hpp"

namespace fd {

void FullCamera::setFromView(const View& v, double dist) {
  yaw = 0;
  pitch = 0;
  rot4 = orthonormalize(mul(transpose(axes()), v.frame));  // e3() == identity
  distance = dist;
  target = v.eye + col(rot4, 2) * dist;
}

// Orbit about orbitCenter(): the frame turns and the eye swings around the centre,
// so the point under the pivot stays put (no view jump when the pivot != target).
void FullCamera::orbit(double dx, double dy) {
  View before = view();
  V4 c = orbitCenter();
  yaw += dx * 0.008 * (std::cos(pitch) < 0 ? -1 : 1);  // upside down: keep drag direction intuitive
  pitch -= dy * 0.008;  // no limit: orbit freely over the top
  M4 f = frame();
  V4 eye = c + mul(mul(f, transpose(before.frame)), before.eye - c);
  target = eye + col(f, 2) * distance;
}
void FullCamera::pan(double dx, double dy) {
  M4 f = frame();
  double k = distance * 0.0015;
  V4 d = col(f, 1) * (dy * k) - col(f, 0) * (dx * k);
  target = target + d;
  pivot = pivot + d;
}
void FullCamera::zoom(double delta) { distance = clampd(distance * std::exp(delta * 0.1), 0.2, 200); }
void FullCamera::rotate4(int a, int b, double angle) {
  View before = view();
  V4 c = orbitCenter();
  M4 e = e3();
  rot4 = orthonormalize(mul(mul(rot4, e), mul(planeRotation(a, b, angle), transpose(e))));
  M4 f = frame();
  V4 eye = c + mul(mul(f, transpose(before.frame)), before.eye - c);
  target = eye + col(f, 2) * distance;
}
void FullCamera::look(double dx, double dy) {
  V4 eye = view().eye;
  yaw += dx * 0.003;
  pitch = clampd(pitch - dy * 0.003, -1.55, 1.55);
  target = eye + col(frame(), 2) * distance;
}
void FullCamera::rotate4AroundEye(int a, int b, double angle) {
  V4 eye = view().eye;
  rotate4(a, b, angle);
  target = eye + col(frame(), 2) * distance;
}
void FullCamera::move(const V4& d, const std::function<bool(const V4&)>& blocked) {
  M4 f = frame();
  V4 delta = col(f, 0) * d[0] + col(f, 1) * d[1] + col(f, 2) * d[2] + col(f, 3) * d[3];
  V4 to = view().eye + delta;
  if (blocked && blocked(to)) return;
  target = target + delta;
}

// ---------------------------------------------------------------- curved spaces
static M4 rodrigues(const V4& axis, double a) {
  double l = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (l < 1e-12) return identity();
  double x = axis[0] / l, y = axis[1] / l, z = axis[2] / l, c = std::cos(a), s = std::sin(a), t = 1 - c;
  // columns (column-major M4)
  return {t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
          t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
          t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,
          0, 0, 0, 1};
}

// "up" at a point (as a direction in the given frame's local coordinates)
static V4 localUp(const Space& s, const M4& frame) {
  V4 p = col(frame, 3);
  V4 up = cs::tangent(s.k(), p, V4{0, 1, 0, 0});
  if (cs::mdot(s.k(), up, up) < 1e-16) return {0, 1, 0, 0};  // at the pole of a spherical space
  V4 l = mul(cs::inverse(s.k(), frame), cs::tnormalize(s.k(), up));
  l[3] = 0;
  return l;
}

void CurvedCamera::reset(const Space& s) {
  // spherical: stay well inside a quarter turn (further away, things start to look magnified)
  distance = s.kind == SpaceKind::Spherical ? std::min(8.0, 0.3 * PI * s.R()) : 8.0;
  M4 e = mul(planeRotation(2, 0, 0.6), planeRotation(2, 1, -0.35));
  M4 F = mul(cs::translation(s, {0, 0.5, 0, 0}), e);
  iso = cs::renorm(s.k(), mul(F, cs::translation(s, {0, 0, -distance, 0})));
}

M4 CurvedCamera::centreFrame(const Space& s) const { return mul(iso, cs::translation(s, {0, 0, distance, 0})); }

void CurvedCamera::setDistance(const Space& s, double d) {
  M4 F = centreFrame(s);
  distance = d;
  iso = cs::renorm(s.k(), mul(F, cs::translation(s, {0, 0, -distance, 0})));
}

void CurvedCamera::orbit(const Space& s, double dx, double dy) {
  M4 F = centreFrame(s);
  V4 up = localUp(s, F);
  double side = up[1] < 0 ? -1 : 1;  // upside down: keep the drag direction intuitive
  M4 R = mul(rodrigues(up, dx * 0.008 * side), planeRotation(2, 1, -dy * 0.008));
  iso = cs::renorm(s.k(), mul(mul(F, R), cs::translation(s, {0, 0, -distance, 0})));
}

void CurvedCamera::pan(const Space& s, double dx, double dy) {
  double k = distance * 0.0015;
  M4 F = mul(centreFrame(s), cs::translation(s, {-dx * k, dy * k, 0, 0}));
  iso = cs::renorm(s.k(), mul(F, cs::translation(s, {0, 0, -distance, 0})));
}

void CurvedCamera::zoom(const Space& s, double delta) {
  double maxD = s.kind == SpaceKind::Spherical ? 0.95 * PI * s.R() : 200;
  setDistance(s, clampd(distance * std::exp(delta * 0.1), 0.2, maxD));
}

void CurvedCamera::focus(const Space& s, const V4& p, double d) {
  M4 F = centreFrame(s);
  M4 T = cs::transvection(s.k(), col(F, 3), p);  // carry the centre (and orientation) to p
  distance = d;
  iso = cs::renorm(s.k(), mul(mul(T, F), cs::translation(s, {0, 0, -distance, 0})));
}

void CurvedCamera::standard(const Space& s, int which) {
  M4 F = centreFrame(s);
  V4 c = col(F, 3);
  // an upright frame at the centre: transported from the origin, so "up" is away from the ground
  M4 base = cs::transvection(s.k(), cs::origin(), c);
  M4 r = which == 1 ? planeRotation(2, 0, -PI / 2) : which == 2 ? planeRotation(2, 1, -PI / 2 + 0.001) : identity();
  iso = cs::renorm(s.k(), mul(mul(base, r), cs::translation(s, {0, 0, -distance, 0})));
}

void CurvedCamera::look(const Space& s, double dx, double dy) {
  V4 up = localUp(s, iso);
  M4 R = mul(rodrigues(up, dx * 0.003), planeRotation(2, 1, -dy * 0.003));
  iso = cs::renorm(s.k(), mul(iso, R));
}

void CurvedCamera::roll(const Space& s, double a) { iso = cs::renorm(s.k(), mul(iso, planeRotation(0, 1, a))); }

void CurvedCamera::move(const Space& s, const V4& local, const std::function<bool(const V4&)>& blocked) {
  V4 u = local;
  u[3] = 0;
  M4 n = cs::renorm(s.k(), mul(iso, cs::translation(s, u)));
  if (blocked && blocked(col(n, 3))) return;
  iso = n;
}

void GeoCamera::initFrom(const SurfaceField& f, const View& v, double dist) {
  pivot = projectToSurface(f, v.eye, 40);
  frame = tangentFrame(f, pivot, v.frame);
  distance = dist;
  valid = std::isfinite(pivot[0]) && std::abs(f.dist(pivot)) < 1e-2;
}
View GeoCamera::view(const SurfaceField& f, double maxStep) const {
  if (distance <= 1e-6) return {pivot, frame};
  auto r = shoot(f, pivot, frame, {0, 0, -1}, distance, maxStep);
  return {r.pos, r.frame};
}
void GeoCamera::orbit(double dx, double dy) {
  frame = rotateFrame(frame, 2, 0, dx * 0.008);
  frame = rotateFrame(frame, 2, 1, -dy * 0.008);
}
void GeoCamera::pan(const SurfaceField& f, double dx, double dy, double maxStep) {
  double k = std::max(distance, 1.0) * 0.0015;
  double len = std::hypot(dx, dy) * k;
  if (len < 1e-6) return;
  auto r = shoot(f, pivot, frame, {-dx, dy, 0}, len, maxStep);
  pivot = r.pos;
  frame = r.frame;
}
void GeoCamera::look(double dx, double dy) {
  frame = rotateFrame(frame, 2, 0, dx * 0.003);
  frame = rotateFrame(frame, 2, 1, -dy * 0.003);
}
void GeoCamera::move(const SurfaceField& f, std::array<double, 3> d, double maxStep, const std::function<bool(const V4&)>& blocked) {
  double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (len < 1e-9) return;
  auto r = shoot(f, pivot, frame, {d[0] / len, d[1] / len, d[2] / len}, len, maxStep);
  if (blocked && blocked(r.pos)) return;
  pivot = r.pos;
  frame = r.frame;
}

}  // namespace fd
