// Picking rays, anchoring to world surfaces, curvature estimate.
#pragma once
#include <optional>

#include "camera.hpp"
#include "evaluator.hpp"

namespace fd {

SurfaceField worldField(const SceneEvaluator& ev, const std::string& worldId);
inline SurfaceField worldField(const SceneEvaluator& ev) { return worldField(ev, ev.activeWorld); }
/** Ray direction for screen coords sx in [-aspect, aspect], sy in [-1, 1]. */
V4 rayDir(const View& v, double sx, double sy, double tanHalf);

struct RayHit {
  V4 pos, dir;
  double t = 0;
  const Node* node = nullptr;
  M4 frame = identity();  // transported frame (geodesic rays)
};
std::optional<RayHit> castRay(const SceneEvaluator& ev, bool geo, const V4& eye, const V4& dir, double maxStep, const M4& frame, double maxDist = -1);

double curvatureRadius(const SurfaceField& f, const V4& p, const M4& frame);
struct AnchorResult { V4 pos; Anchor anchor; };
AnchorResult makeAnchor(const SceneEvaluator& ev, const std::string& worldId, const V4& p, const M4& hint);
/** Basis whose last column is the given normal. */
M4 basisFromNormal(const V4& n);

}  // namespace fd
