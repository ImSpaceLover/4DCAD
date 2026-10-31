// CPU evaluation of the scene SDF. Mirrors the GLSL emitted by codegen.cpp
// (same roles, same operators). Used for picking, placement, collisions and
// moving cameras along world surfaces.
#pragma once
#include <map>
#include <string>
#include <unordered_map>

#include "scene.hpp"

namespace fd {

constexpr double INF = 1e9;

/** full: plain 4D scene.  geo: Geodesic-3D view of the active world.  world: a world's own SDF. */
enum class EvalCtx { Full, Geo, World };
/**
 * How a node participates in a context:
 *   Empty   – contributes nothing (INF)
 *   Normal  – ordinary 4D SDF
 *   Tangent – anchored to the active world, evaluated in the anchor's tangent space (W := 0)
 *   Hug     – anchored; local W := signed distance to its world surface (thin shell hugging it)
 */
enum class Role { Empty, Normal, Tangent, Hug };

Role nodeRole(const Node& n, EvalCtx ctx, const std::string& activeWorld, const std::function<bool(const std::string&)>& worldExists);
void applyModifier(V4& q, const Modifier& m);
double opExtrudeD(double d, double wy);

struct Sample {
  double d = INF;
  std::array<double, 3> color{0, 0, 0};
  const Node* node = nullptr;  // closest leaf
};

struct Bound { V4 c{0, 0, 0, 0}; double r = INFINITY; };
/** Conservative bounding hyperspheres of every node, in the parent's frame. */
std::unordered_map<std::string, Bound> computeBounds(const Node& root, const PrimitiveRegistry& reg);
double leafLocalRadius(const Node& n, const PrimitiveDef* def);

class SceneEvaluator {
 public:
  SceneEvaluator(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorld);
  Sample evalFull(const V4& p) const;
  Sample evalGeo(const V4& p) const;
  Sample eval(const V4& p, bool geo) const { return geo ? evalGeo(p) : evalFull(p); }
  double worldDist(const V4& g, const std::string& worldId) const;
  double worldDist(const V4& g) const { return worldDist(g, activeWorld); }
  V4 worldGrad(const V4& g, const std::string& worldId) const;
  V4 worldGrad(const V4& g) const { return worldGrad(g, activeWorld); }
  std::string activeWorld;

 private:
  struct Prep { M4 inv; const PrimitiveDef* prim = nullptr; int dim = 4; };
  Node& root_;
  const PrimitiveRegistry& reg_;
  std::unordered_map<std::string, Prep> prep_;
  mutable std::unordered_map<std::string, double> wcache_;
  V4 toLocal(const Node& n, const V4& p) const;
  Sample evalNode(const Node& n, const V4& p, const V4& g, double sAcc, EvalCtx ctx) const;
};

}  // namespace fd
