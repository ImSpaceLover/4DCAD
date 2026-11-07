// CPU evaluation of the scene SDF. Mirrors the GLSL emitted by codegen.cpp
// (same roles, same operators). Used for picking, placement, collisions and
// moving cameras along world surfaces.
#pragma once
#include <map>
#include <memory>
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
void applyModifier(V4& q, const Modifier& m);  // mirror / repeat / rotate
/** Coordinate pass of a node's modifier stack (outermost first); captures extrude coordinates in t. */
/** Per-evaluation scratch of a modifier stack: extrusion coordinates, points before each warp. */
struct ModScratch {
  double t[64];
  V4 pre[64];
};
double modifierCoords(const Node& n, const std::vector<ModAxis>& ax, V4& q, ModScratch& s);
/** Data-driven modifiers: the bounding radius after it (its bound hook), whether it has one. */
double modifierBound(const Modifier& m, const ModAxis& ax, double r);
bool modifierHasBound(const Modifier& m);
/** Distance pass (innermost first): applies the captured extrusions. */
double modifierDistance(const Node& n, const std::vector<ModAxis>& ax, double d, const ModScratch& s);
/** Radial repeat: angle (radians) of the copy nearest to polar angle a. */
double radialCopyAngle(const Modifier& m, double a);
double opExtrudeD(double d, double wy);
/** Custom SDF code evaluated on the CPU (interpreted; its code-bound box if it cannot run). */
double customSdf(const Node& n, const V4& q);

struct Sample {
  double d = INF;
  std::array<double, 3> color{0, 0, 0};
  const Node* node = nullptr;  // closest leaf
};

struct Bound { V4 c{0, 0, 0, 0}; double r = INFINITY; };
/** Conservative bounding hyperspheres of every node, in the parent's frame. */
std::unordered_map<std::string, Bound> computeBounds(const Node& root, const PrimitiveRegistry& reg);
double leafLocalRadius(const Node& n, const PrimitiveDef* def);
/** How far (along the surface normal) an attached object may reach from its anchor's tangent space:
 *  how much its own sheet of the world bends away within the object's size (radius), plus a
 *  small margin. Anything further is another sheet of the world and must not show the object. */
inline double attachCut(double reach, double radius) {
  if (!std::isfinite(radius)) return reach;
  double bend = radius < reach ? reach - std::sqrt(reach * reach - radius * radius) : reach;
  return bend + 0.05;
}

/** Follow surface: the local coordinates of p for an attached object bent along its path (false: not bent). */
bool followLocal(const Node& n, const V4& p, V4& q);
/** Curved scenes: local axes along which a leaf extends forever unchanged (infinite extrusions). */
std::vector<int> symmetryAxes(const Node& n, const PrimitiveRegistry& reg);
/** A frame change for an infinite object (plane, rod, infinite repeat) that brings its origin next to
 *  c (a point in its frame) without changing the object; identity when there is none. S = total scale. */
M4 anchorNear(const Space& sp, const Node& n, const PrimitiveRegistry& reg, const V4& c, double S);
/** Curved scenes: the outermost repeat of a leaf, done exactly (copies = translates along a geodesic). */
const Modifier* exactRepeat(const Node& n);
/** Index of the copy nearest to the model point q (in the node's frame); S = the node's total scale. */
double repeatCopy(const Space& sp, const Modifier& m, const V4& q, double S);
/** Moves q (node frame) so its foot on the geodesic subspace spanned by the axes is the origin. */
V4 toSymmetrySlice(const Space& sp, const std::vector<int>& axes, const V4& q);
/** Bounding radius (local units, with its scale) of one copy of a repeated leaf. */
double anchoredRadiusAny(const Node& n, const PrimitiveRegistry& reg);
/** Bounding radius (local units, with the node's scale) of the part near the re-anchored origin. */
double anchoredRadius(const Node& n, const PrimitiveRegistry& reg);
/** Spherical scenes: factor applied to local distances of objects of this world radius (1 otherwise). */
double curvedFactor(const Space& s, double radius);

class SceneEvaluator {
 public:
  SceneEvaluator(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorld, const Space& space = {});
  /** Curved scenes: distance at a model point (world units). */
  Sample evalCurved(const V4& p) const;
  Space space;
  Sample evalFull(const V4& p) const;
  Sample evalGeo(const V4& p) const;
  Sample eval(const V4& p, bool geo) const { return geo ? evalGeo(p) : evalFull(p); }
  double worldDist(const V4& g, const std::string& worldId) const;
  /** Distance + colour of the world surface nearest to g. */
  Sample worldSample(const V4& g, const std::string& worldId) const;
  double worldDist(const V4& g) const { return worldDist(g, activeWorld); }
  V4 worldGrad(const V4& g, const std::string& worldId) const;
  V4 worldGrad(const V4& g) const { return worldGrad(g, activeWorld); }
  std::string activeWorld;
  const Node& root() const { return root_; }

 private:
  struct Prep {
    M4 inv;
    const PrimitiveDef* prim = nullptr;
    int dim = 4;
    std::vector<ModAxis> axes;
    double radius = INFINITY;
    // curved scenes: inverse isometry relative to the parent, total scale, distance factor (spherical)
    M4 isoInv = identity();
    double S = 1, factor = 1;
    std::vector<int> symAxes;             // infinite extrusion axes (measured from the foot point)
    std::shared_ptr<Node> noRepeat;       // the shape without its outermost repeat (exact repeat)
    std::shared_ptr<struct Prep> noRepeatPrep;
  };
  Node& root_;
  const PrimitiveRegistry& reg_;
  std::unordered_map<std::string, Prep> prep_;
  mutable std::unordered_map<std::string, double> wcache_;
  V4 toLocal(const Node& n, const V4& p) const;
  Sample evalNode(const Node& n, const V4& p, const V4& g, double sAcc, EvalCtx ctx) const;
  Sample evalCurvedNode(const Node& n, const V4& p) const;
  double leafShape(const Node& n, const Prep& pr, V4& q) const;  // local distance of a leaf (local units)
};

}  // namespace fd
