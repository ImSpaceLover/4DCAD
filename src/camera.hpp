// Cameras. Controllers take abstract input deltas (pixels, booleans).
//
// FullCamera: orientation = rot4 * E(yaw, pitch); E is a 3D turntable rotation in
// XYZ, rot4 accumulates rotations involving W. Frame columns: right, up, forward, ana.
// Rays leave the eye inside the (right, up, forward) hyperplane = the visible 3D slice.
//
// GeoCamera: pivot on the world surface + orthonormal frame (right, up, forward
// tangent; normal). Edit: eye sits `distance` behind the pivot along a geodesic.
// Explore: distance 0, eye = pivot.
#pragma once
#include "geodesic.hpp"
#include "scene.hpp"

namespace fd {

struct View {
  V4 eye{0, 0, 0, 0};
  M4 frame = identity();  // right, up, forward, ana/normal
};

class FullCamera {
 public:
  V4 target{0, 0.5, 0, 0};
  double distance = 8, yaw = 0.6, pitch = -0.35;
  M4 rot4 = identity();
  V4 pivot{0, 0.5, 0, 0};  // centre of rotation for orbiting (MMB click sets it)
  bool pivotSet = false;

  void load(const CameraState& s) { target = s.target; distance = s.distance; yaw = s.yaw; pitch = s.pitch; rot4 = s.rot4; pivotSet = false; }
  V4 orbitCenter() const { return pivotSet ? pivot : target; }
  void setPivot(const V4& p) { pivot = p; pivotSet = true; }
  CameraState save() const { return {target, distance, yaw, pitch, rot4}; }
  M4 e3() const { return mul(planeRotation(2, 0, yaw), planeRotation(2, 1, pitch)); }
  M4 frame() const { return mul(rot4, e3()); }
  View view() const { M4 f = frame(); return {target - col(f, 2) * distance, f}; }
  /** Set orientation + eye from an arbitrary orthonormal frame. */
  void setFromView(const View& v, double dist);

  // Edit (orbit about target)
  void orbit(double dx, double dy);
  void pan(double dx, double dy);
  void zoom(double delta);
  /** Rotate in a camera-relative plane (indices 0..3 = right, up, forward, ana). Orbits about the target. */
  void rotate4(int a, int b, double angle);
  void reset4() { rot4 = identity(); }
  /** Position of the visible slice along the camera's ana axis. */
  double sliceW() const { return dot(target, col(frame(), 3)); }
  void setSliceW(double w) { V4 ana = col(frame(), 3); V4 d = ana * (w - sliceW()); target = target + d; pivot = pivot + d; }

  // Explore (first person)
  void look(double dx, double dy);
  void rotate4AroundEye(int a, int b, double angle);
  void move(const V4& d, const std::function<bool(const V4&)>& blocked);
};

class GeoCamera {
 public:
  V4 pivot{0, 0, 0, 0};
  M4 frame = identity();
  double distance = 4;
  bool valid = false;

  void initFrom(const SurfaceField& f, const View& v, double distance);
  View view(const SurfaceField& f, double maxStep) const;
  void orbit(double dx, double dy);
  void pan(const SurfaceField& f, double dx, double dy, double maxStep);
  void zoom(double delta) { distance = clampd(distance * std::exp(delta * 0.1), 0.2, 50); }
  void look(double dx, double dy);
  void roll(double a) { frame = rotateFrame(frame, 0, 1, a); }
  void move(const SurfaceField& f, std::array<double, 3> d, double maxStep, const std::function<bool(const V4&)>& blocked);
};

}  // namespace fd
