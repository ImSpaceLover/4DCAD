// Core unit tests (no OpenGL). Run: ctest, or ./fdtests
#include <cstdio>
#include <cstdlib>
#include <string>

#include "camera.hpp"
#include "codegen.hpp"
#include "evaluator.hpp"
#include "examples.hpp"
#include "geodesic.hpp"
#include "library.hpp"
#include "surface.hpp"

using namespace fd;

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    checks++;                                                              \
    if (!(cond)) {                                                         \
      failures++;                                                          \
      std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);          \
      std::printf(__VA_ARGS__);                                            \
      std::printf("\n");                                                   \
    }                                                                      \
  } while (0)
#define CLOSE(a, b, eps) CHECK(std::abs((a) - (b)) < (eps), "%.9f vs %.9f", (double)(a), (double)(b))

static const PrimitiveDef& P(const char* id) {
  for (auto& p : builtinPrimitives()) if (p.id == id) return p;
  std::abort();
}

int main() {
  for (auto& p : builtinPrimitives()) CHECK(validatePrimitive(p).empty(), "%s", p.id.c_str());

  // extrusion chain
  CLOSE(evalPrimitive(P("segment"), {3, 0, 0, 0}), 2, 1e-9);
  CLOSE(evalPrimitive(P("cube"), {0, 0, 0, 0}), -1, 1e-9);
  CLOSE(evalPrimitive(P("cube"), {2, 2, 0, 0}), std::sqrt(2.0), 1e-9);
  CLOSE(evalPrimitive(P("tesseract"), {0, 0, 0, 3}), 2, 1e-9);
  // revolutions
  CLOSE(evalPrimitive(P("sphere"), {1, 1, 1, 0}), std::sqrt(3.0) - 1, 1e-9);
  CLOSE(evalPrimitive(P("hypersphere"), {1, 1, 1, 1}), 1, 1e-9);
  CLOSE(evalPrimitive(P("torus"), {1, 0, 0, 0}), -0.3, 1e-9);
  CLOSE(evalPrimitive(P("duocylinder"), {2, 0, 0, 0}), 1, 1e-9);
  CLOSE(evalPrimitive(P("spheritorus"), {1.2, 0, 0, 0}), -0.4, 1e-9);
  CLOSE(evalPrimitive(P("plane"), {5, 3, 1, 0}), 3, 1e-9);

  PrimitiveRegistry reg;
  {  // thin 3D object in 4D, booleans, modifiers
    Node root = makeGroup(BoolOp::Union, {makeLeaf("sphere", "s")});
    SceneEvaluator ev(root, reg, "");
    CLOSE(ev.evalFull({0, 0, 0, 0}).d, -0.05, 1e-9);
    CLOSE(ev.evalFull({0, 0, 0, 1.05}).d, 1, 1e-6);
    Node a = makeLeaf("tesseract", "a"), b = makeLeaf("hypersphere", "b");
    b.xf.scale = 0.5;
    Node r2 = makeGroup(BoolOp::Subtraction, {a, b});
    SceneEvaluator ev2(r2, reg, "");
    CLOSE(ev2.evalFull({0, 0, 0, 0}).d, 0.5, 1e-9);
    Node c = makeLeaf("hypersphere", "c");
    c.mods.push_back({2, 0, 4, 0});
    Node r3 = makeGroup(BoolOp::Union, {c});
    SceneEvaluator ev3(r3, reg, "");
    CLOSE(ev3.evalFull({8, 0, 0, 0}).d, -1, 1e-9);
  }

  // JSON round trip + legacy file format + codegen
  for (auto& ex : EXAMPLES) {
    SceneFile f = ex.make();
    std::string text = sceneToJson(f.root, reg, f.world, nullptr);
    SceneFile back = sceneFromJson(text);
    CHECK(nodeToJson(back.root) == nodeToJson(f.root), "%s round trip", ex.id);
    for (bool geo : {false, true}) {
      auto cs = compileScene(back.root, reg, back.world, geo);
      CHECK(cs.glsl.find("float mapScene") != std::string::npos, "codegen");
    }
  }
  {
    const char* legacy = R"({"format":"4dcad-scene","version":1,"world":null,"primitives":[],
      "root":{"id":"r","name":"S","kind":"group","op":"union","k":0.3,"xf":{"pos":[0,0,0,0],"rot":[0,0,0,0,0,0],"scale":1},"modifiers":[],
      "children":[{"id":"a","name":"A","kind":"leaf","prim":"cube","color":[1,0,0],"thickness":0.1,
      "xf":{"pos":[1,0,0,0],"rot":[90,0,0,0,0,0],"scale":1},"modifiers":[]}]}})";
    SceneFile f = sceneFromJson(legacy);
    CLOSE(f.root.children[0].xf.rot[1], 1, 1e-9);  // 90 deg XY: x axis -> y
  }

  // the example files in the repository load (they use the older angle-based rotation format)
  for (const char* f : {"/examples/playground.4dscene.json", "/examples/planet.4dscene.json"}) {
    std::string text;
    CHECK(readFile(std::string(FD_SOURCE_DIR) + f, text), "read %s", f);
    try { SceneFile sf = sceneFromJson(text); CHECK(!sf.root.children.empty(), "%s", f); }
    catch (const std::exception& e) { CHECK(false, "%s: %s", f, e.what()); }
  }

  // geodesic walk on the planet stays on the surface and reaches the antipode
  {
    SceneFile f = planetScene();
    SceneEvaluator ev(f.root, reg, f.world);
    auto wf = worldField(ev);
    M4 frame = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0};
    auto r = shoot(wf, {0, 3, 0, 0}, frame, {0, 0, 1}, PI * 3, 0.05);
    CLOSE(length(r.pos), 3, 1e-3);
    CHECK(length(r.pos - V4{0, -3, 0, 0}) < 0.05, "antipode");
    // anchored: single image, visible in geo mode, hugs in full mode
    Node only = makeGroup(BoolOp::Union, {f.root.children[0], f.root.children[1]});
    SceneEvaluator ev2(only, reg, f.world);
    CHECK(ev2.evalGeo({0, 3, 0, 0}).d < 0, "inside red cube (geo)");
    CHECK(ev2.evalGeo({0, -3, 0, 0}).d > 0.5, "no antipodal image");
    CHECK(ev2.evalFull({0, 3.02, 0, 0}).d < 0, "hug shell");
    auto hit = castRay(ev2, true, {0, -3, 0, 0}, {0, 0, 1, 0}, 0.25, identity());
    CHECK(hit && hit->node && hit->node->name == "Red cube", "geodesic ray wraps to red cube");
  }

  // bounding spheres are conservative
  for (auto& ex : EXAMPLES) {
    SceneFile f = ex.make();
    auto bounds = computeBounds(f.root, reg);
    for (auto& child : f.root.children) {
      auto& b = bounds[child.id];
      if (!std::isfinite(b.r) || child.anchor) continue;
      Node only = makeGroup(BoolOp::Union, {child});
      SceneEvaluator ev(only, reg, "");
      unsigned seed = 1;
      auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / double(1 << 24) * 16 - 8; };
      for (int k = 0; k < 400; k++) {
        V4 p{rnd(), rnd(), rnd(), rnd()};
        double bd = length(p - b.c) - b.r;
        if (bd > 0) CHECK(ev.evalFull(p).d >= bd - 1e-6, "%s bound", child.name.c_str());
      }
    }
  }

  // modifier-built shapes equal the old primitive recipes; groups can be extruded too
  {
    const char* ids[] = {"cube", "sphere", "torus", "tesseract", "duocylinder", "spheritorus", "tiger", "cylinder"};
    for (const char* id : ids) {
      Node shape = makeShape(id, id);
      Node r = makeGroup(BoolOp::Union, {shape});
      SceneEvaluator ev(r, reg, "");
      unsigned seed = 7;
      auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / double(1 << 24) * 6 - 3; };
      for (int k = 0; k < 200; k++) {
        V4 p{rnd(), rnd(), rnd(), rnd()};
        double ref = evalPrimitive(P(id), p);
        int dim = primitiveDim(P(id));
        for (int a = dim; a < 4; a++) ref = opExtrudeD(ref, std::abs(p[a]) - 0.05);
        CLOSE(ev.evalFull(p).d, ref, 1e-9);
      }
    }
    // a group of two squares extruded along Z == two boxes
    Node sq1 = makeShape("square", "a", {-2, 0, 0, 0}), sq2 = makeShape("square", "b", {2, 0, 0, 0});
    sq1.round.reset(); sq2.round.reset();
    Node g = makeGroup(BoolOp::Union, {sq1, sq2});
    Modifier ex; ex.type = MOD_EXTRUDE; ex.length = 2;
    g.mods.push_back(ex);
    CHECK(nodeDim(g, reg) == 3, "group dim %d", nodeDim(g, reg));
    Node r = makeGroup(BoolOp::Union, {g});
    SceneEvaluator ev(r, reg, "");
    CLOSE(ev.evalFull({2, 0, 0, 0}).d, -0.05, 1e-9);   // thin in W only
    CLOSE(ev.evalFull({2, 0, 3, 0}).d, 2, 1e-9);       // 1 beyond the extruded Z extent
    CLOSE(ev.evalFull({-2, 0, 0.99, 0}).d, -0.01, 1e-9);
    auto cs = compileScene(r, reg, "", false);
    CHECK(cs.glsl.find("opExtrude(d, abs(t") != std::string::npos, "group extrude in GLSL");
  }

  // absolute rotation angles <-> matrix
  {
    unsigned seed = 3;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / double(1 << 24) * 300 - 150; };
    for (int k = 0; k < 200; k++) {
      double a[6];
      for (double& x : a) x = rnd() * 0.55;
      M4 m = rotationFromAngles(a);
      auto b = anglesFromRotation(m);
      double bb[6];
      for (int i = 0; i < 6; i++) bb[i] = b[i];
      M4 m2 = rotationFromAngles(bb);
      double err = 0;
      for (int i = 0; i < 16; i++) err = std::max(err, std::abs(m[i] - m2[i]));
      CHECK(err < 1e-9, "angle round trip %g", err);
    }
  }

  // library: built-in folders hold modifier-built shapes
  {
    auto fs = builtinFolders();
    CHECK(fs.size() == 3 && fs[1].objects.size() >= 5 && fs[2].objects.size() >= 9, "builtin folders");
    LibraryObject o = makeLibraryObject(makeShape("torus", "T"), reg, "My torus");
    LibraryFolder f{"custom", "Custom", false, {o}};
    auto imp = importLibraryJson(libraryToJson({f}));
    CHECK(imp.folders.size() == 1 && imp.folders[0].objects[0].name == "My torus" && imp.folders[0].objects[0].node.mods.size() == 3, "library json");
  }

  // full camera slice W and frame round trip
  {
    FullCamera cam;
    cam.setSliceW(2.5);
    CLOSE(cam.view().eye[3], 2.5, 1e-9);
    View v = cam.view();
    FullCamera c2;
    c2.setFromView(v, 3);
    CLOSE(length(c2.view().eye - v.eye), 0, 1e-9);
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
