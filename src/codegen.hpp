// Compiles the scene tree into GLSL:
//   float mapScene(vec4 p, out vec4 m)   // m = (rgb, highlight); VIEW_GEO selects the geodesic context
//   float worldSDF(vec4 p)               // active world surface (INF if none)
// Structure (operators, recipes, modifiers) is baked in; per-node transforms,
// colours, smoothness and bounds come from uniform arrays, so moving or
// recolouring never recompiles. Only one view context is compiled per program.
#pragma once
#include <map>
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
/** Custom code with every for / while loop also bounded (LOOP_GUARD iterations per call). */
std::string guardLoops(const std::string& code);
/** Self-contained GLSL `float sdf(vec4 p)` of a node in its local space (for viewing / copying). */
std::string standaloneGLSL(const Node& n, const PrimitiveRegistry& reg);
/** The parts of standalone code, for embedding it in other code (textures made of shapes). */
struct StandaloneOptions {
  bool color = false;  // also track each point's colour (the renderer's rules: deepest part, smooth blends, cut faces)
  std::map<std::string, std::string> colorExpr;  // leaf id -> its colour expression (default: its colour constant)
  // leaf id -> statements run with the leaf's colour (%C, a vec3 variable) and its local point before
  // its modifiers (%Q): fills
  std::map<std::string, std::string> fill;
  // exposed values as expressions: (node id, path) with paths pos.x .. pos.w, scale, k, mods.<i>.p.<name>
  std::map<std::pair<std::string, std::string>, std::string> expr;
};
struct StandaloneCode {
  std::string pre;   // top-level functions (shapes, modifiers, custom SDFs)
  std::string body;  // statements computing the distance (and colour) at vec4 p
  std::string d, c;  // variables holding them at the end
};
StandaloneCode standaloneParts(const Node& n, const PrimitiveRegistry& reg, const StandaloneOptions& opt);

/** Node data buffer: 12 vec4 per node in CompiledScene::nodeIds order (read in shaders with ndat(node, slot)). */
std::vector<float> packNodes(Node& root, const PrimitiveRegistry& reg, const CompiledScene& cs, const std::unordered_set<std::string>& highlight,
                             const Space* space = nullptr, const M4* camIso = nullptr);

/** Signature of everything that requires recompiling (structure, recipes, world, view). */
std::string structureKey(const Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo, SpaceKind space = SpaceKind::Flat);

}  // namespace fd
