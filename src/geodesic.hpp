// Movement constrained to the 3D boundary of a 4D world object.
// Newton projection p <- p - f(p) grad f / |grad f|^2 keeps points on f = 0, and
// frames are carried along by projecting onto the new tangent space and
// re-orthonormalising (discrete parallel transport). The shader does the same per ray.
#pragma once
#include <functional>

#include "math4.hpp"

namespace fd {

struct SurfaceField {
  std::function<double(const V4&)> dist;
  std::function<V4(const V4&)> grad;
};

V4 projectToSurface(const SurfaceField& f, V4 p, int iters = 6);
/** Tangent frame at p: cols 0..2 tangent (from hint's right/up/forward), col 3 = normal. */
M4 tangentFrame(const SurfaceField& f, const V4& p, const M4& hint);
M4 transportFrame(const SurfaceField& f, const V4& p, const M4& frame);
struct GeoResult { V4 pos; M4 frame; };
/** Walk a geodesic from p with direction given in frame coords (right, up, forward). */
GeoResult shoot(const SurfaceField& f, const V4& p, const M4& frame, std::array<double, 3> dirLocal, double dist, double maxStep = 0.1);
/** Rotate column a toward column b. */
M4 rotateFrame(const M4& frame, int a, int b, double angle);

}  // namespace fd
