// Compiles the scene tree into GLSL:
//   float mapScene(vec4 p, out vec4 m)   // m = (rgb, highlight); VIEW_GEO selects the geodesic context
//   float worldSDF(vec4 p)               // active world surface (INF if none)
// Structure (operators, recipes, modifiers) is baked in; per-node transforms,
// colours, smoothness and bounds come from uniform arrays, so moving or
// recolouring never recompiles. Only one view context is compiled per program.
#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "evaluator.hpp"

namespace fd {

struct CompiledScene {
  std::string glsl;               // scene functions (no #version header)
  std::vector<std::string> nodeIds;  // uniform index -> node id
  bool hasWorld = false;
};

/** badCode: custom SDF sources known not to compile (replaced by their bounding box). */
CompiledScene compileScene(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorld, bool geo,
                           const std::unordered_set<std::string>* badCode = nullptr, SpaceKind space = SpaceKind::Flat);
/** Helper functions (opExtrude, sU, sI, sS, INF) that custom SDF code may use. */
const char* sdfHelpersGLSL();
/** Renames the functions defined in custom SDF code by appending a suffix (sdf -> sdf<suffix>). */
std::string renameCustomCode(const std::string& code, const std::string& suffix);
/** Self-contained GLSL `float sdf(vec4 p)` of a node in its local space (for viewing / copying). */
std::string standaloneGLSL(const Node& n, const PrimitiveRegistry& reg);

/** Node data buffer: 12 vec4 per node in CompiledScene::nodeIds order (read in shaders with ndat(node, slot)). */
std::vector<float> packNodes(Node& root, const PrimitiveRegistry& reg, const CompiledScene& cs, const std::unordered_set<std::string>& highlight,
                             const Space* space = nullptr, const M4* camIso = nullptr);

/** Signature of everything that requires recompiling (structure, recipes, world, view). */
std::string structureKey(const Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo, SpaceKind space = SpaceKind::Flat);

}  // namespace fd
