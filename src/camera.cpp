#include "camera.hpp"

namespace fd {

void FullCamera::setFromView(const View& v, double dist) {
  yaw = 0;
  pitch = 0;
  rot4 = orthonormalize(v.frame);  // e3() == identity
  distance = dist;
  target = v.eye + col(rot4, 2) * dist;
}

void FullCamera::orbit(double dx, double dy) {
  yaw += dx * 0.008;
  pitch = clampd(pitch - dy * 0.008, -1.55, 1.55);
}
void FullCamera::pan(double dx, double dy) {
  M4 f = frame();
  double k = distance * 0.0015;
  target = target - col(f, 0) * (dx * k) + col(f, 1) * (dy * k);
}
void FullCamera::zoom(double delta) { distance = clampd(distance * std::exp(delta * 0.1), 0.2, 200); }
void FullCamera::rotate4(int a, int b, double angle) {
  M4 e = e3();
  rot4 = orthonormalize(mul(mul(rot4, e), mul(planeRotation(a, b, angle), transpose(e))));
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
