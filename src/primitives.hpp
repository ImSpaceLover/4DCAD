// Primitive recipes: start from a 0D point and raise the dimension one axis at a
// time (x, y, z, w) with Extrude / Revolve steps; Rotate turns the profile.
//
//   * The 0D point is "everything" in 0D space, so its distance is -INF.
//   * Extrude (finite length L) along new axis t:
//         w = (d, |t| - L/2);  d' = min(max(w.x, w.y), 0) + length(max(w, 0))
//     Infinite: d' = d.   Half (keep t <= 0): w = (d, t).
//   * Revolve (radius R) of existing axis a into new axis t: earlier steps see
//         coord[a] := length(coord[a], t) - R
//   * Rotate (plane i,j, angle): earlier steps see coordinates rotated in that plane.
// The raw distance is returned; thin extrusion over unused axes, shell and
// rounding are applied by the scene evaluator / shader.
#pragma once
#include <string>
#include <vector>

#include "math4.hpp"

namespace fd {

enum class StepOp { Extrude, Revolve, Rotate };
enum class ExtrudeMode { Finite, Infinite, Half };

struct Step {
  StepOp op = StepOp::Extrude;
  ExtrudeMode mode = ExtrudeMode::Finite;
  double length = 2;  // extrude
  double radius = 0;  // revolve
  int axis = -1;      // revolve: existing axis spun into the new one (-1 = newest)
  int i = 0, j = 1;   // rotate plane
  double angle = 0;   // rotate, degrees
};

struct PrimitiveDef {
  std::string id, name, description;
  std::vector<Step> steps;
  double round = 0, shell = 0;
  bool builtin = false;
};

constexpr double NEG_INF = -1e9;

int primitiveDim(const PrimitiveDef& d);
std::vector<std::string> validatePrimitive(const PrimitiveDef& d);
double evalPrimitive(const PrimitiveDef& d, const V4& p);
/** Per-axis half extents of the recipe (INFINITY when unbounded). */
std::array<double, 4> primitiveExtents(const PrimitiveDef& d);
/** GLSL function `float <fn>(vec4 c)` evaluating the recipe. */
std::string primitiveGLSL(const PrimitiveDef& d, const std::string& fn);
std::string glslFloat(double x);
const std::vector<PrimitiveDef>& builtinPrimitives();

}  // namespace fd
