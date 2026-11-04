// Constant-curvature 3D spaces: Hyperbolic (curvature -1/R^2) and Spherical (+1/R^2).
//
// Model: points are 4-vectors on the unit hyperboloid  x^2 + y^2 + z^2 - w^2 = -1 (w > 0) or the
// unit 3-sphere  x^2 + y^2 + z^2 + w^2 = 1, with the bilinear form
//     mdot(a, b) = a.x b.x + a.y b.y + a.z b.z + k a.w b.w      (k = -1 hyperbolic, +1 spherical),
// so mdot(p, p) = k for points. World distances are R times the model's angles. The origin is
// O = (0, 0, 0, 1); tangent vectors at O are (x, y, z, 0). Isometries are 4x4 matrices (columns:
// images of the basis); an isometry that maps O to p carries the frame at O to a frame at p, so a
// node / camera is one matrix (columns 0-2: its X, Y, Z directions, column 3: its position).
//
// Scene nodes keep their usual Transform: in a curved scene `pos` (x, y, z) are normal
// coordinates in the parent's frame (the point reached by walking |pos| along the direction pos)
// and `rot` is a 3D rotation, so changing the curvature bends space around unchanged numbers.
#pragma once
#include <string>

#include "math4.hpp"

namespace fd {

struct Transform;

enum class SpaceKind { Flat = 0, Hyperbolic = 1, Spherical = 2 };
inline const char* SPACE_NAMES[3] = {"Flat 4D", "Hyperbolic 3D", "Spherical 3D"};
inline const char* SPACE_IDS[3] = {"flat", "hyperbolic", "spherical"};

struct Space {
  SpaceKind kind = SpaceKind::Flat;
  double radius = 8;  // curvature radius R (curvature = -1/R^2 or +1/R^2)
  bool curved() const { return kind != SpaceKind::Flat; }
  double k() const { return kind == SpaceKind::Spherical ? 1.0 : -1.0; }
  double R() const { return radius > 1e-6 ? radius : 1e-6; }
};

namespace cs {  // curved-space maths (only meaningful when space.curved())

inline V4 origin() { return {0, 0, 0, 1}; }
inline double mdot(double k, const V4& a, const V4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + k * a[3] * b[3]; }
inline double C(double k, double t) { return k > 0 ? std::cos(t) : std::cosh(t); }
inline double S(double k, double t) { return k > 0 ? std::sin(t) : std::sinh(t); }

/** Metric-unit tangent vector (tangent vectors are space-like: mdot > 0). */
V4 tnormalize(double k, const V4& v);
/** Component of v tangent at point p. */
V4 tangent(double k, const V4& p, const V4& v);
/** Puts a (slightly off) point back on the model. */
V4 renormPoint(double k, const V4& p);
/** World distance between two points. */
double distance(const Space& s, const V4& p, const V4& q);
/** Unit tangent direction at p toward q (any direction if q == p; spherical antipode: undefined). */
V4 dirTo(const Space& s, const V4& p, const V4& q);
/** Geodesic from p along unit tangent dir for world distance d: new point and direction. */
void geodesic(const Space& s, V4& p, V4& dir, double d);
/** Point at world distance |u| from O along u (u = x, y, z; w ignored). */
V4 expO(const Space& s, const V4& u);
/** Inverse of expO: normal coordinates (x, y, z, 0) of p, in world units. */
V4 logO(const Space& s, const V4& p);
/** The isometry along the geodesic from p to q (maps p to q, parallel-transports tangent vectors). */
M4 transvection(double k, const V4& p, const V4& q);
/** Translation by u from the origin: transvection(O, expO(u)). */
M4 translation(const Space& s, const V4& u);
/** Parallel transport of a tangent vector at p to q along their geodesic. */
V4 transport(double k, const V4& p, const V4& q, const V4& v);
M4 inverse(double k, const M4& m);
/** Re-orthonormalises an isometry (removes drift after many products). */
M4 renorm(double k, const M4& m);
/** A rotation about the origin made 3D (no w): the curved spaces have only these. */
M4 rot3(const M4& r);

/** Isometry of a node relative to its parent: translation(pos * sAnc) * rot.
 *  sAnc = product of the scales of the node's ancestors (group scale also spreads its children). */
M4 nodeIso(const Space& s, const Transform& xf, double sAnc);
/** Inverse of nodeIso for a given relative isometry (keeps the scale). */
void setFromIso(const Space& s, Transform& xf, const M4& iso, double sAnc);

}  // namespace cs

std::string spaceToJson(const Space& s);
}  // namespace fd
