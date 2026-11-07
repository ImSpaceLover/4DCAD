// Core unit tests (no OpenGL). Run: ctest, or ./fdtests
#include <cstdio>
#include <cstdlib>
#include <string>

#include "animation.hpp"
#include "camera.hpp"
#include "codegen.hpp"
#include "polytope.hpp"
#include "shaderlang.hpp"
#include "defs.hpp"
#include "textures.hpp"
#include <nlohmann/json.hpp>
#include "shaders.hpp"
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
    auto noRound = [](Node& n) {
      n.mods.erase(std::remove_if(n.mods.begin(), n.mods.end(), [](const Modifier& m) { return m.type == MOD_DEF && m.def == "round"; }), n.mods.end());
    };
    noRound(sq1); noRound(sq2);
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
    CHECK(fs.size() == 4 && fs[3].objects.size() == POLY_COUNT + 1 && fs[1].objects.size() >= 5 && fs[2].objects.size() >= 9, "builtin folders");
    LibraryObject o = makeLibraryObject(makeShape("torus", "T"), reg, "My torus");
    LibraryFolder f{"custom", "Custom", false, {o}};
    auto imp = importLibraryJson(libraryToJson({f}));
    CHECK(imp.folders.size() == 1 && imp.folders[0].objects[0].name == "My torus" && imp.folders[0].objects[0].node.mods.size() == 3, "library json");
  }

  // materials and custom SDFs: JSON round trip, CPU bounding-box approximation, code renaming, code view
  {
    Node cust = makeCustom("C");
    cust.codeBound = 2;
    cust.mat.emissive = 0.5;
    cust.mat.tex[CH_COLOR] = legacyColorSlot(TEX_CHECKER, 1, 0.7, {0, 0, 0});
    cust.mat.color2 = {0.1, 0.2, 0.3};
    Node back = nodeFromJson(nodeToJson(cust));
    CHECK(back.isCustom() && back.code == cust.code && back.codeBound == 2 && back.mat.emissive == 0.5 && back.mat.tex[CH_COLOR] == cust.mat.tex[CH_COLOR] &&
          back.mat.color2[2] == 0.3, "material/custom json");
    CHECK(nodeDim(back, reg) == 4, "custom SDFs are 4D");
    Node r = makeGroup(BoolOp::Union, {back});
    SceneEvaluator ev(r, reg, "");
    CLOSE(ev.evalFull({3, 0, 0, 0}).d, 2, 1e-9);  // the default code (a unit hypersphere, wavy), run exactly on the CPU
    std::string ren = renameCustomCode("float helper(float x) { return x; }\nfloat sdf(vec4 p) { return helper(length(p)) - 1.0; }", "_c7");
    CHECK(ren.find("float sdf_c7(vec4 p)") != std::string::npos && ren.find("helper_c7(length") != std::string::npos, "rename: %s", ren.c_str());
    auto cs = compileScene(r, reg, "", false);
    CHECK(cs.glsl.find("sdf_c1(") != std::string::npos, "custom function in scene");
    std::unordered_set<std::string> bad{back.code};
    auto cs2 = compileScene(r, reg, "", false, &bad);
    CHECK(cs2.glsl.find("custom SDF:") == std::string::npos, "bad code replaced by its box");
    std::string sa = standaloneGLSL(playgroundScene().root, reg);
    CHECK(sa.find("float sdf(vec4 p)") != std::string::npos && sa.find("sU(") != std::string::npos, "standalone code");
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

  // shell / rounding act on a thin object's own shape (not on its 4D slab)
  {
    Node cube = makeShape("cube", "c");  // 3D, thickness 0.1, half size 1
    appendRoundShell(cube.mods, 0, 0.2);
    Node r = makeGroup(BoolOp::Union, {cube});
    SceneEvaluator ev(r, reg, "");
    CHECK(ev.evalFull({0, 0, 0, 0}).d > 0.5, "shelled cube is hollow: %.3f", ev.evalFull({0, 0, 0, 0}).d);
    CHECK(ev.evalFull({0.9, 0, 0, 0}).d < 0, "shelled cube has a skin");
    CLOSE(ev.evalFull({0.9, 0, 0, 0.5}).d, 0.45, 1e-6);  // slab thickness unchanged: still 0.1 in W
    Node c2 = makeShape("cube", "c2");
    c2.mods.erase(std::remove_if(c2.mods.begin(), c2.mods.end(), [](const Modifier& m) { return m.type == MOD_DEF && m.def == "round"; }), c2.mods.end());
    appendRoundShell(c2.mods, -0.06, 0);  // more than half the thickness: used to delete the object
    Node r2 = makeGroup(BoolOp::Union, {c2});
    SceneEvaluator ev2(r2, reg, "");
    CHECK(ev2.evalFull({0, 0, 0, 0}).d < 0, "negative rounding shrinks, keeps the object");
    CLOSE(ev2.evalFull({1.5, 0, 0, 0}).d, 0.56, 1e-6);
  }
  // mirror offset
  {
    Node s = makeShape("sphere", "s");  // radius 1
    Modifier m; m.type = MOD_MIRROR; m.axis = 0; m.offset = 3;
    s.mods.push_back(m);
    Node r = makeGroup(BoolOp::Union, {s});
    SceneEvaluator ev(r, reg, "");
    CLOSE(ev.evalFull({3, 0, 0, 0}).d, -0.05, 1e-9);
    CLOSE(ev.evalFull({-3, 0, 0, 0}).d, -0.05, 1e-9);
    CLOSE(ev.evalFull({0, 0, 0, 0}).d, 2, 1e-9);
  }
  // radial repeat: full ring, partial arc, single copy = rotation
  {
    auto radial = [&](int count, double step, double start, double radius) {
      Node s = makeShape("sphere", "s");
      s.xf.scale = 0.5;
      Node g = makeGroup(BoolOp::Union, {s});
      Modifier m; m.type = MOD_RADIAL; m.i = 0; m.j = 2; m.count = count; m.angle = step; m.startAngle = start; m.radius = radius;
      g.mods.push_back(m);
      return makeGroup(BoolOp::Union, {g});
    };
    auto at = [](double deg, double r) { double a = deg * PI / 180; return V4{r * std::cos(a), 0, r * std::sin(a), 0}; };
    Node ring = radial(4, 90, 0, 2);
    SceneEvaluator ev(ring, reg, "");
    for (double d : {0.0, 90.0, 180.0, 270.0}) CHECK(ev.evalFull(at(d, 2)).d < 0, "ring copy at %.0f", d);
    CHECK(ev.evalFull(at(45, 2)).d > 0.5, "gap between copies");
    Node arc = radial(3, 60, 0, 2);
    SceneEvaluator ev2(arc, reg, "");
    for (double d : {0.0, 60.0, 120.0}) CHECK(ev2.evalFull(at(d, 2)).d < 0, "arc copy at %.0f", d);
    CHECK(ev2.evalFull(at(240, 2)).d > 1, "no copy outside the arc");
    CHECK(ev2.evalFull(at(-60, 2)).d > 0.5, "no copy before the start");
    // one copy, radius 0 = rotation of the shape (an off-centre sphere ends up rotated by 30 degrees)
    Node s = makeShape("sphere", "s");
    s.xf.scale = 0.5;
    s.xf.pos = {2, 0, 0, 0};
    Node g = makeGroup(BoolOp::Union, {s});
    Modifier m; m.type = MOD_RADIAL; m.i = 0; m.j = 2; m.count = 1; m.startAngle = 30; m.radius = 0;
    g.mods.push_back(m);
    Node r3 = makeGroup(BoolOp::Union, {g});
    SceneEvaluator ev3(r3, reg, "");
    CHECK(ev3.evalFull(at(30, 2)).d < 0 && ev3.evalFull(at(0, 2)).d > 0, "single copy rotates");
    // legacy "rotate" modifiers load as the same rotation
    Node legacy = nodeFromJson(R"({"id":"a","name":"a","kind":"leaf","prim":"point","modifiers":[{"type":"rotate","i":0,"j":2,"angle":30}]})");
    CHECK(legacy.mods.size() == 1 && legacy.mods[0].type == MOD_RADIAL && legacy.mods[0].count == 1 && legacy.mods[0].radius == 0 &&
              std::abs(legacy.mods[0].startAngle - 30) < 1e-12, "legacy rotate -> radial");
    // JSON round trip of the new fields
    Node rt = nodeFromJson(nodeToJson(ring));
    const Modifier& q = rt.children[0].mods[0];
    CHECK(q.type == MOD_RADIAL && q.count == 4 && std::abs(q.angle - 90) < 1e-12 && std::abs(q.radius - 2) < 1e-12, "radial round trip");
  }

  // node data buffer: NODE_VEC4S vec4 per node; new material fields and lights survive JSON
  {
    Node l = makeShape("cube", "c");
    l.mat.roughness = 0.2; l.mat.specular = 1.1; l.mat.transparency = 0.4; l.mat.ior = 1.33; l.mat.metallic = 0.8;
    Node r = makeGroup(BoolOp::Union, {l});
    auto cs = compileScene(r, reg, "", false, nullptr);
    auto d = packNodes(r, reg, cs, {});
    CHECK(d.size() >= cs.nodeIds.size() * (NODE_VEC4S * 4) && d.size() % (NODE_VEC4S * 4) == 0, "node buffer size (+ modifier rows)");
    size_t li = 1;  // the leaf (root first)
    CLOSE(d[li * (NODE_VEC4S * 4) + 44], 0.2, 1e-6);
    CLOSE(d[li * (NODE_VEC4S * 4) + 46], 0.4, 1e-6);
    CHECK(cs.glsl.find("uniform") == std::string::npos, "no uniform arrays in the scene code (node data buffer)");
    Node rt = nodeFromJson(nodeToJson(l));
    CHECK(rt.mat.roughness == 0.2 && rt.mat.specular == 1.1 && rt.mat.transparency == 0.4 && rt.mat.ior == 1.33 && rt.mat.metallic == 0.8,
          "material round trip");
    CLOSE(d[li * (NODE_VEC4S * 4) + 47], 0.8, 1e-6);  // metallic
    CLOSE(d[li * (NODE_VEC4S * 4) + 33], 1.33, 1e-6); // index of refraction
    std::vector<PointLight> ls(1);
    ls[0].pos = {1, 2, 3, 4}; ls[0].range = 3; ls[0].shadows = false;
    auto back = lightsFromJson(lightsToJson(ls));
    CHECK(back.size() == 1 && back[0].pos[3] == 4 && back[0].range == 3 && !back[0].shadows, "lights round trip");
    // a scene far beyond the old ~90 node uniform limit still compiles to one shader
    Node big = makeGroup(BoolOp::Union, {});
    for (int i = 0; i < 300; i++) big.children.push_back(makeShape("sphere", "s", {double(i), 0, 0, 0}));
    auto cb = compileScene(big, reg, "", false, nullptr);
    CHECK(cb.nodeIds.size() == 301 && packNodes(big, reg, cb, {}).size() >= 301 * (NODE_VEC4S * 4), "300 objects");
  }

  // values are node data: editing them keeps the structure key (no recompile); structure changes it
  {
    Node l = makeShape("cube", "c");
    Modifier ex; ex.type = MOD_EXTRUDE; ex.length = 2;
    Modifier rp; rp.type = MOD_REPEAT; rp.axis = 0; rp.spacing = 3; rp.count = 2;
    l.mods = {rp, ex};
    appendRoundShell(l.mods, 0.01, 0.01);  // (Onion, Round)
    l.mat.tex[CH_COLOR] = legacyColorSlot(TEX_CHECKER, 1, 0.7, {0, 0, 0});
    Node r = makeGroup(BoolOp::Union, {l});
    std::string k0 = structureKey(r, reg, "", false, SpaceKind::Flat);
    Node e = r;
    Node& c = e.children[0];
    c.mods[1].length = 5; c.mods[0].spacing = 1.5; c.mods[0].count = 0;
    setModParam(c.mods[2], *defs().modifier("onion"), 0, 0.1);
    setModParam(c.mods[3], *defs().modifier("round"), 0, 0.05);
    c.mat.tex[CH_COLOR].strength = 0.4; c.mat.tex[CH_COLOR].map.scale = 2.5; c.mat.tex[CH_COLOR].map.rot[2] = 30; c.mat.emissive = 2; c.xf.scale = 3; c.color = {0.1, 0.2, 0.3};
    CHECK(structureKey(e, reg, "", false, SpaceKind::Flat) == k0, "value edits keep the structure key");
    auto cs = compileScene(e, reg, "", false, nullptr);
    auto d = packNodes(e, reg, cs, {});
    int row = (int)d[1 * NODE_VEC4S * 4 + 12 * 4];
    CHECK(row >= 2, "modifier row after the nodes");
    CLOSE(d[row * NODE_VEC4S * 4 + 0], 1.5, 1e-6);   // repeat spacing
    CHECK(d[row * NODE_VEC4S * 4 + 1] > 1e8, "infinite repeat count packed as huge");
    CLOSE(d[row * NODE_VEC4S * 4 + 4], 2.5, 1e-6);   // extrude half length
    int trow = (int)d[1 * NODE_VEC4S * 4 + 12 * 4 + 1];  // texture rows (after the modifier rows)
    CHECK(trow > row, "texture rows after the modifier rows (%d, %d)", trow, row);
    CLOSE(d[trow * NODE_VEC4S * 4 + 20], 0.4, 1e-6);   // strength
    CLOSE(d[trow * NODE_VEC4S * 4 + 5], 2.5, 1e-6);    // A: scale on Y (the XW rotation leaves Y alone)
    {
      Node e2 = e;
      e2.children[0].mat.tex[CH_BUMP] = legacyColorSlot(TEX_NOISE, 1, 1, {0, 0, 0});
      CHECK(structureKey(e2, reg, "", false, SpaceKind::Flat) != k0, "adding a texture changes the structure key");
      CHECK(compileScene(e2, reg, "", false, nullptr).glsl.find("SCENE_BUMP") != std::string::npos, "bump scenes take the centre normal sample");
    }
    CLOSE(d[row * NODE_VEC4S * 4 + 8], 0.1, 1e-6);    // onion thickness (data-driven modifier: its own vec4)
    CLOSE(d[row * NODE_VEC4S * 4 + 12], 0.05, 1e-6);  // round radius
    Node f = r;
    Modifier mi; mi.type = MOD_MIRROR;
    f.children[0].mods.push_back(mi);
    CHECK(structureKey(f, reg, "", false, SpaceKind::Flat) != k0, "adding a modifier changes the key");
  }

  // an object attached to a world surface stays on its own sheet (double wormhole: not in the other room)
  {
    std::string text;
    CHECK(readFile(std::string(FD_SOURCE_DIR) + "/test_worlds/double_wormhole.4dscene.json", text), "read example");
    SceneFile f = sceneFromJson(text);
    PrimitiveRegistry r2;
    for (auto& p : f.primitives) r2.set(p);
    const Node* cyl = nullptr;
    walkc(f.root, [&](const Node& n, const Node*, int) { if (n.anchor && !cyl) cyl = &n; });
    CHECK(cyl != nullptr, "attached object in the example");
    SceneEvaluator ev(f.root, r2, f.world);
    V4 a = cyl->xf.pos, nrm = col(cyl->anchor->basis, 3);
    CHECK(ev.evalGeo(a).node == cyl && ev.evalGeo(a).d < 0, "the object is at its anchor");
    // walk along the normal to the next sheet of the world surface (either side)
    int found = 0;
    for (double sgn : {1.0, -1.0}) {
      double prev = ev.worldDist(a + nrm * (sgn * 0.3));
      for (double t = 0.35; t < 40; t += 0.05) {
        V4 p = a + nrm * (sgn * t);
        double w = ev.worldDist(p);
        if ((w > 0) != (prev > 0)) {
          found++;
          Sample s = ev.evalGeo(p);
          CHECK(!(s.node == cyl && s.d < 0), "object also on the other sheet at %.2f along the normal", sgn * t);
          break;
        }
        prev = w;
      }
    }
    CHECK(found > 0, "another sheet along the normal");
  }

  // ---- animation ----
  {
    // 4D rotation interpolation: ends exact, shortest path at constant speed (both planes)
    std::srand(7);
    auto rnd = [] { return std::rand() / (double)RAND_MAX * 2 - 1; };
    auto randRot = [&] {
      double d[6];
      for (auto& x : d) x = rnd() * 170;
      return rotationFromAngles(d);
    };
    auto angleBetween = [](const M4& a, const M4& b) {  // sum of the two plane angles of a^T b
      M4 q = mul(transpose(a), b);
      double tr = q[0] + q[5] + q[10] + q[15];  // = 2 cos t1 + 2 cos t2
      return tr;
    };
    int bad = 0;
    for (int i = 0; i < 40; i++) {
      M4 A = randRot(), B = randRot();
      M4 r0 = interpolateRotation(A, B, 0), r1 = interpolateRotation(A, B, 1);
      M4 h = interpolateRotation(A, B, 0.5);
      // the midpoint is halfway: from A to h and from h to B are the same rotation
      double e = 0;
      M4 d1 = mul(transpose(A), h), d2 = mul(transpose(h), B);
      for (int k = 0; k < 16; k++) e = std::max(e, std::abs(d1[k] - d2[k]));
      double e1 = 0;
      M4 hb = interpolateRotation(A, B, 0.999999);
      for (int k = 0; k < 16; k++) e1 = std::max(e1, std::abs(hb[k] - B[k]));
      if (e > 1e-6 || e1 > 1e-4 || r0 != A || r1 != B) bad++;
      (void)angleBetween;
    }
    CHECK(bad == 0, "rotation interpolation: %d of 40 random pairs wrong", bad);
    // a single-plane 90 degree turn passes 45 degrees halfway
    M4 q = interpolateRotation(identity(), planeRotation(0, 3, PI / 2), 0.5);
    CLOSE(q[0], std::cos(PI / 4), 1e-9);
    // 180 degrees in one plane (degenerate case) stays in that plane
    M4 q2 = interpolateRotation(identity(), planeRotation(1, 2, PI), 0.5);
    CLOSE(q2[0], 1, 1e-9);
    CLOSE(q2[15], 1, 1e-9);
    CLOSE(q2[5], 0, 1e-9);

    // scalar tracks: constant / linear / bezier / ease, holds outside the keys
    Animation an;
    setKey(an, "n1", "pos.x", 1, 0, Interp::Linear);
    setKey(an, "n1", "pos.x", 11, 10, Interp::Linear);
    Track* t = findTrack(an, "n1", "pos.x");
    CLOSE(evalTrack(*t, 6), 5, 1e-9);
    CLOSE(evalTrack(*t, -5), 0, 1e-12);
    CLOSE(evalTrack(*t, 50), 10, 1e-12);
    t->keys[0].interp = Interp::Constant;
    CLOSE(evalTrack(*t, 10.9), 0, 1e-12);
    t->keys[0].interp = Interp::EaseIn;
    CHECK(evalTrack(*t, 6) < 5, "ease in starts slow");
    t->keys[0].interp = Interp::Bezier;
    double prev = -1;
    bool mono = true;
    for (double f = 1; f <= 11; f += 0.25) { double v = evalTrack(*t, f); if (v < prev - 1e-9) mono = false; prev = v; }
    CHECK(mono && std::abs(evalTrack(*t, 6) - 5) < 1e-6, "bezier: smooth, symmetric, monotonic between two keys");
    setKey(an, "n1", "pos.x", 21, 0);
    sortKeys(*findTrack(an, "n1", "pos.x"));
    double peak = 0;
    for (double f = 1; f <= 21; f += 0.1) peak = std::max(peak, evalTrack(*findTrack(an, "n1", "pos.x"), f));
    CHECK(peak <= 10 + 1e-6, "auto handles do not overshoot a peak (%.3f)", peak);
    // stepped values (counts, visibility) jump
    setKey(an, "n1", "hidden", 1, 0);
    setKey(an, "n1", "hidden", 5, 1);
    CLOSE(evalTrack(*findTrack(an, "n1", "hidden"), 4.9), 0, 1e-12);

    // every listed value can be read and written
    Node L = makeShape("cube", "c");
    Modifier rm; rm.type = MOD_RADIAL; L.mods.push_back(rm);
    int failsV = 0;
    for (auto& v : animatableValues(L)) {
      if (v.path == "rot") continue;
      double x;
      if (!getValue(L, v.path, x) || !setValue(L, v.path, x + (v.stepped ? 1 : 0.25))) failsV++;
      double y;
      getValue(L, v.path, y);
      if (std::abs(y - (x + (v.stepped ? 1 : 0.25))) > 1e-9) failsV++;
    }
    CHECK(failsV == 0, "value access (%d failures)", failsV);
    Node cam = makeCamera("c", {0, 0, 0, 0}, identity(), 60, 3);
    double fv;
    CHECK(getValue(cam, "cam.fov", fv) && fv == 60, "camera fov");

    // apply + JSON round trip
    Node root = makeGroup(BoolOp::Union, {L});
    std::vector<PointLight> lights(1);
    lights[0].id = "L1";
    Animation a2;
    setKey(a2, L.id, "pos.y", 1, 0, Interp::Linear);
    setKey(a2, L.id, "pos.y", 9, 4, Interp::Linear);
    setRotKey(a2, L.id, 1, identity());
    setRotKey(a2, L.id, 9, planeRotation(0, 1, 1.0));
    setKey(a2, "light:L1", "intensity", 1, 1, Interp::Linear);
    setKey(a2, "light:L1", "intensity", 9, 3, Interp::Linear);
    findTrack(a2, L.id, "pos.y")->keys[1].autoHandles = false;
    Animation rt = animationFromJson(animationToJson(a2));
    CHECK(rt.tracks.size() == 3 && !findTrack(rt, L.id, "pos.y")->keys[1].autoHandles, "animation JSON round trip");
    applyAnimation(rt, root, lights, 5);
    CLOSE(root.children[0].xf.pos[1], 2, 1e-9);
    CLOSE(root.children[0].xf.rot[0], std::cos(0.5), 1e-6);
    CLOSE(lights[0].intensity, 2, 1e-9);
    pruneTracks(rt, makeGroup(BoolOp::Union, {}), lights);
    CHECK(rt.tracks.size() == 1, "tracks of deleted objects removed");
  }

  // frame conversions: compose / relative round trip
  {
    Transform P; P.pos = {1, -2, 0.5, 3}; P.rot = mul(planeRotation(0, 3, 0.7), planeRotation(1, 2, -0.4)); P.scale = 2;
    Transform C; C.pos = {0.3, 0.1, -1, 2}; C.rot = planeRotation(0, 2, 1.1); C.scale = 0.5;
    Transform W = composeXf(P, C), back = relativeXf(P, W);
    CLOSE(length(back.pos - C.pos), 0, 1e-9);
    CLOSE(back.scale, C.scale, 1e-12);
    double e = 0; for (int i = 0; i < 16; i++) e = std::max(e, std::abs(back.rot[i] - C.rot[i]));
    CLOSE(e, 0, 1e-9);
  }
  // empty groups are pruned bottom-up
  {
    Node root = makeGroup(BoolOp::Union, {}, "root");
    Node inner = makeGroup(BoolOp::Union, {}, "inner");
    Node outer = makeGroup(BoolOp::Union, {inner}, "outer");
    root.children = {outer, makeLeaf("point", "a")};
    pruneEmptyGroups(root);
    CHECK(root.children.size() == 1 && root.children[0].isLeaf(), "nested empty groups removed");
  }
  // centring a group on its first object keeps world positions
  {
    Node a = makeLeaf("point", "a", {1, 2, 3, 4}), b = makeLeaf("point", "b", {-1, 0, 2, 0});
    Node g = makeGroup(BoolOp::Union, {a, b});
    g.xf.pos = {0.5, 0, 0, 0}; g.xf.rot = planeRotation(0, 1, 0.6); g.xf.scale = 1.5;
    V4 wa = composeXf(g.xf, g.children[0].xf).pos, wb = composeXf(g.xf, g.children[1].xf).pos;
    CHECK(centreGroupOnFirst(g), "centred");
    CLOSE(length(g.children[0].xf.pos), 0, 1e-12);
    CLOSE(length(composeXf(g.xf, g.children[0].xf).pos - wa), 0, 1e-9);
    CLOSE(length(composeXf(g.xf, g.children[1].xf).pos - wb), 0, 1e-9);
  }
  // multi-object editing: changed fields copied, transforms relative, kinds respected
  {
    Node p = makeLeaf("point", "p", {0, 0, 0, 0}), t = makeLeaf("point", "t", {5, 0, 0, 0});
    Node grp = makeGroup(BoolOp::Union, {makeLeaf("point", "x")});
    Modifier ex; ex.type = MOD_EXTRUDE;
    p.mods = {ex}; t.mods = {ex};
    t.xf.scale = 2; t.color = {0, 0, 1}; t.thickness = 0.7;
    Node before = p;
    p.color = {1, 0, 0};
    p.xf.pos = {0, 1, 0, 0};
    p.xf.scale = 1.5;
    p.xf.rot = planeRotation(0, 1, 0.3);
    p.mods[0].length = 5;
    propagateEdit(before, p, t, true);
    propagateEdit(before, p, grp, true);
    CHECK(t.color == p.color, "colour copied");
    CLOSE(t.thickness, 0.7, 1e-12);  // untouched field kept
    CLOSE(length(t.xf.pos - V4{5, 1, 0, 0}), 0, 1e-12);
    CLOSE(t.xf.scale, 3, 1e-12);
    CLOSE(t.xf.rot[0], std::cos(0.3), 1e-9);
    CLOSE(t.mods[0].length, 5, 1e-12);
    CHECK(grp.color != p.color, "leaf fields do not reach groups");
    CLOSE(grp.xf.pos[1], 1, 1e-12);  // transforms do
    Node u = makeLeaf("point", "u");
    Modifier mi; mi.type = MOD_MIRROR;
    u.mods = {mi};
    propagateEdit(before, p, u, false);
    CHECK(u.mods.size() == 1 && u.mods[0].type == MOD_MIRROR, "different stacks are not touched");
    ModStackOp add{ModStackOp::Add, -1, mi};
    Node v = makeLeaf("point", "v"); v.mods = {ex};
    propagateEdit(p, p, v, true, add);
    CHECK(v.mods.size() == 2 && v.mods[1].type == MOD_MIRROR, "added modifier reaches matching stacks");
  }

  // ---- curved spaces (hyperbolic / spherical) ----
  for (SpaceKind kind : {SpaceKind::Hyperbolic, SpaceKind::Spherical}) {
    Space sp{kind, 2.5};
    double k = sp.k();
    const char* nm = SPACE_NAMES[(int)kind];
    V4 u{1.1, -0.7, 0.4, 0};
    V4 p = cs::expO(sp, u);
    CHECK(std::abs(cs::mdot(k, p, p) - k) < 1e-12, "%s: expO lies on the model", nm);
    CHECK(length(cs::logO(sp, p) - u) < 1e-10, "%s: logO inverts expO", nm);
    CHECK(std::abs(cs::distance(sp, cs::origin(), p) - length(u)) < 1e-10, "%s: distance from the origin", nm);
    M4 T = cs::translation(sp, u);
    CHECK(length(mul(T, cs::origin()) - p) < 1e-10, "%s: translation moves O to expO", nm);
    V4 a = cs::expO(sp, {-0.3, 0.9, 1.2, 0}), b = cs::expO(sp, {0.5, 0.2, -0.8, 0});
    CHECK(std::abs(cs::distance(sp, mul(T, a), mul(T, b)) - cs::distance(sp, a, b)) < 1e-10, "%s: translations are isometries", nm);
    M4 Ti = cs::inverse(k, T);
    CHECK(length(mul(Ti, p) - cs::origin()) < 1e-10, "%s: inverse isometry", nm);
    M4 Tab = cs::transvection(k, a, b);
    CHECK(length(mul(Tab, a) - b) < 1e-10, "%s: transvection a -> b", nm);
    V4 v = cs::tnormalize(k, cs::tangent(k, a, V4{0.3, 1, -0.2, 0.4}));
    V4 tv = cs::transport(k, a, b, v);
    CHECK(std::abs(cs::mdot(k, tv, b)) < 1e-10 && std::abs(cs::mdot(k, tv, tv) - 1) < 1e-10, "%s: transport keeps tangency and length", nm);
    CHECK(length(mul(Tab, v) - tv) < 1e-10, "%s: transvection transports tangent vectors", nm);
    // walking a geodesic: the distance travelled is the distance reached (less than half way round)
    V4 q = a, d = v;
    cs::geodesic(sp, q, d, 1.7);
    CHECK(std::abs(cs::distance(sp, a, q) - 1.7) < 1e-9, "%s: geodesic distance", nm);
    // right triangle with legs x, y at the origin: cosh(c/R) = cosh(x/R) cosh(y/R) (hyperbolic),
    // cos(c/R) = cos(x/R) cos(y/R) (spherical) -- the Pythagorean theorem of each geometry
    double x = 1.3, y = 0.9, R = sp.R();
    double c = cs::distance(sp, cs::expO(sp, {x, 0, 0, 0}), cs::expO(sp, {0, y, 0, 0}));
    double expect = R * (k < 0 ? std::acosh(std::cosh(x / R) * std::cosh(y / R)) : std::acos(std::cos(x / R) * std::cos(y / R)));
    CHECK(std::abs(c - expect) < 1e-9, "%s: Pythagoras (%.6f vs %.6f)", nm, c, expect);
    CHECK(k < 0 ? c > std::hypot(x, y) : c < std::hypot(x, y), "%s: hypotenuse longer / shorter than flat", nm);
    // node transforms: relative isometry and back
    Transform xf;
    xf.pos = {0.8, -1.2, 2.0, 0};
    double ang[6] = {20, 35, 0, -15, 0, 0};
    xf.rot = cs::rot3(rotationFromAngles(ang));
    xf.scale = 1.7;
    M4 iso = cs::nodeIso(sp, xf, 1.3);
    Transform back;
    back.scale = xf.scale;
    cs::setFromIso(sp, back, iso, 1.3);
    CHECK(length(back.pos - xf.pos) < 1e-9, "%s: node position round trip", nm);
    bool rotOk = true;
    for (int i = 0; i < 16; i++) rotOk &= std::abs(back.rot[i] - xf.rot[i]) < 1e-9;
    CHECK(rotOk, "%s: node rotation round trip", nm);
    // the CPU evaluator: a ball of radius 1 is a geodesic ball (exact in normal coordinates)
    PrimitiveRegistry reg;
    Node ball = makeShape("sphere", "Ball", {3, 0, 0, 0});
    Node root = makeGroup(BoolOp::Union, {ball}, "Scene");
    SceneEvaluator ev(root, reg, "", sp);
    V4 dir3 = normalize(V4{0.3, 0.8, -0.5, 0});
    M4 Tb = cs::translation(sp, {3, 0, 0, 0});
    V4 at = mul(Tb, cs::expO(sp, dir3 * 1.6));  // 1.6 from the ball's centre
    double dd = ev.evalCurved(at).d;
    if (k < 0) CHECK(std::abs(dd - 0.6) < 1e-6, "%s: distance to a geodesic ball %.6f", nm, dd);
    else CHECK(dd > 0 && dd <= 0.6 + 1e-9 && dd > 0.3, "%s: spherical distances stay safe (%.6f <= 0.6)", nm, dd);
    V4 inside = mul(Tb, cs::expO(sp, dir3 * 0.5));
    CHECK(ev.evalCurved(inside).d < 0, "%s: inside the ball", nm);
    // camera isometry stays an isometry after many moves
    CurvedCamera cam;
    cam.reset(sp);
    for (int i = 0; i < 200; i++) { cam.orbit(sp, 13, -7); cam.pan(sp, 9, 4); cam.move(sp, {0.05, 0.02, 0.1, 0}, nullptr); }
    M4 I = mul(cs::inverse(k, cam.iso), cam.iso);
    bool isoOk = true;
    for (int i = 0; i < 16; i++) isoOk &= std::abs(I[i] - identity()[i]) < 1e-9;
    CHECK(isoOk, "%s: camera stays an isometry", nm);
    // scene files keep the space and the curved camera
    SceneFile f;
    f.root = root;
    f.space = sp;
    CurvedCamState cc = cam.save();
    std::string text = sceneToJson(f.root, reg, "", nullptr, nullptr, nullptr, "", nullptr, &sp, &cc);
    SceneFile g = sceneFromJson(text);
    CHECK(g.space.kind == kind && std::abs(g.space.radius - 2.5) < 1e-12, "%s: space saved", nm);
    CHECK(g.curvedCam && std::abs(g.curvedCam->iso[5] - cc.iso[5]) < 1e-9, "%s: curved camera saved", nm);
  }
  {  // ---- polyhedra and honeycombs ----
    auto hcIndex = [](const char* sch) { for (size_t i = 0; i < honeycombs().size(); i++) if (std::string(honeycombs()[i].schlafli) == sch) return (int)i; return -1; };
    // known cell sizes: the tesseract's cubes (pi/4), the 120-cell's dodecahedra (pi/10), the 24-cell's octahedra (pi/6)
    CLOSE(honeycombInradius(honeycombs()[hcIndex("{4,3,3}")]), PI / 4, 1e-12);
    CLOSE(honeycombInradius(honeycombs()[hcIndex("{5,3,3}")]), PI / 10, 1e-9);
    CLOSE(honeycombInradius(honeycombs()[hcIndex("{3,4,3}")]), PI / 6, 1e-12);
    // order-5 cubic: cosh(h)^2 = 1 + cos 72
    CLOSE(std::cosh(honeycombInradius(honeycombs()[hcIndex("{4,3,5}")])), std::sqrt(1 + std::cos(72 * PI / 180)), 1e-12);
    CHECK(polyEdges(POLY_CUBE).size() == 12 && polyEdges(POLY_DODECA).size() == 30 && polyEdges(POLY_ICOSA).size() == 30 &&
              polyEdges(POLY_TETRA).size() == 6 && polyEdges(POLY_OCTA).size() == 12, "edge counts");
    CLOSE(polyFlatDihedral(POLY_CUBE), PI / 2, 1e-12);
    // a curved polyhedron: the face centre is on the surface, the centre is inradius deep
    Space hs{SpaceKind::Hyperbolic, 2};
    CLOSE(polyDistCurved(hs, POLY_DODECA, 0.7, cs::origin()), -0.7, 1e-9);
    CLOSE(polyDistCurved(hs, POLY_DODECA, 0.7, cs::expO(hs, polyFaces(POLY_DODECA)[3] * 0.7)), 0.0, 1e-9);
    // a honeycomb is the same in every cell: a point and its image under a face reflection
    PolyParams pp;
    pp.honey = hcIndex("{4,3,5}");
    pp.width = 0.05;
    V4 q = cs::expO(hs, {0.3, -0.2, 0.15, 0});
    double d0 = honeycombDist(hs, pp, q);
    const Honeycomb& hc = honeycombs()[pp.honey];
    double th = honeycombInradius(hc);
    V4 f = polyFaces(hc.cell)[2];
    V4 n{f[0] * std::cosh(th), f[1] * std::cosh(th), f[2] * std::cosh(th), std::sinh(th)};
    V4 q2 = q - n * (2 * cs::mdot(-1, q, n));
    CHECK(std::abs(cs::mdot(-1, q2, q2) + 1) < 1e-9 && cs::distance(hs, q, q2) > 0.1, "reflected point is elsewhere in the space");
    CLOSE(honeycombDist(hs, pp, q2), d0, 1e-9);
    // an edge of the central cell (two faces meet): distance = -edge radius
    V4 e3 = normalize(polyFaces(hc.cell)[0] + polyFaces(hc.cell)[2]);
    // walk out along the bisector until both faces are reached
    double lo = 0, hi = 3;
    for (int i = 0; i < 80; i++) { double mid = (lo + hi) / 2; (polyDistCurved(hs, hc.cell, th * hs.R(), cs::expO(hs, e3 * mid)) < 0 ? lo : hi) = mid; }
    CLOSE(honeycombDist(hs, pp, cs::expO(hs, e3 * lo)), -0.05, 1e-6);
    // far away: the cell finder brings a point back into the central cell
    V4 far = cs::expO(hs, {7.3, 2.1, -4.4, 0});
    int par = 0;
    M4 g = honeycombCellOf(hs, pp, far, &par);
    CHECK(polyDistCurved(hs, hc.cell, th * hs.R(), mul(g, far)) <= 1e-9, "cell finder");
    CLOSE(honeycombDist(hs, pp, mul(g, far)), honeycombDist(hs, pp, far), 1e-6);
    // file round trip
    PrimitiveRegistry reg;
    Node h = makeLeaf("honeycomb", "H");
    h.poly = pp;
    h.poly.style = 1;
    Node pl = makeLeaf("polyhedron", "P");
    pl.poly.type = POLY_ICOSA;
    pl.poly.size = 0.42;
    Node r = makeGroup(BoolOp::Union, {h, pl}, "Scene");
    SceneFile back = sceneFromJson(sceneToJson(r, reg, "", nullptr));
    CHECK(back.root.children.size() == 2 && back.root.children[0].poly == h.poly && back.root.children[1].poly == pl.poly, "polytopes saved");
    CHECK(nodeDim(back.root.children[0], reg) == 3, "polytopes are 3D");
  }
  {  // ---- linear extrapolation ----
    Animation an;
    Track& t = setKey(an, "n1", "pos.x", 1, 0, Interp::Linear);
    setKey(an, "n1", "pos.x", 11, 10, Interp::Linear);
    Track* tr = findTrack(an, "n1", "pos.x");
    CLOSE(evalTrack(*tr, 21), 10.0, 1e-12);  // hold by default
    tr->linearExtrap = true;
    CLOSE(evalTrack(*tr, 21), 20.0, 1e-6);
    CLOSE(evalTrack(*tr, -9), -10.0, 1e-6);
    (void)t;
    // bezier keys: the end handles follow the segment, so the motion carries on at its speed
    Track* tb = &setKey(an, "n1", "pos.y", 1, 0);
    setKey(an, "n1", "pos.y", 11, 5);
    tb = findTrack(an, "n1", "pos.y");
    tb->linearExtrap = true;
    autoHandles(*tb);
    CLOSE(evalTrack(*tb, 21), 10.0, 1e-3);
    // rotations keep turning past 180 degrees (90 per 10 frames -> 270 at frame 31)
    setRotKey(an, "n1", 1, identity(), Interp::Linear);
    setRotKey(an, "n1", 11, planeRotation(0, 1, PI / 2), Interp::Linear);
    Track* trr = findTrack(an, "n1", "rot");
    trr->linearExtrap = true;
    M4 r = evalRotation(*trr, 31), e = planeRotation(0, 1, 1.5 * PI);
    double err = 0;
    for (int i = 0; i < 16; i++) err = std::max(err, std::abs(r[i] - e[i]));
    CHECK(err < 1e-6, "rotation keeps turning (err %g)", err);
    // stepped values (e.g. visibility) always hold
    Track& th = setKey(an, "n1", "hidden", 1, 0, Interp::Linear);
    setKey(an, "n1", "hidden", 11, 1, Interp::Linear);
    findTrack(an, "n1", "hidden")->linearExtrap = true;
    CLOSE(evalTrack(*findTrack(an, "n1", "hidden"), 30), 1.0, 1e-12);
    (void)th;
    // saved
    Animation back = animationFromJson(animationToJson(an));
    CHECK(findTrack(back, "n1", "pos.x")->linearExtrap && !findTrack(back, "n1", "scale"), "extrapolation saved");
  }
  {  // flat scenes do not write a space
    PrimitiveRegistry reg;
    Node root = makeGroup(BoolOp::Union, {}, "Scene");
    Space flat;
    std::string text = sceneToJson(root, reg, "", nullptr, nullptr, nullptr, "", nullptr, &flat);
    CHECK(text.find("\"space\"") == std::string::npos && sceneFromJson(text).space.kind == SpaceKind::Flat, "flat space is the default");
  }

  {  // custom code's loops are bounded (GPU watchdog)
    std::string g = guardLoops("float sdf(vec4 p) { // for (x) in a comment\n float d = 0.0; int format = 1;\n"
                               "  for (int i = 0; i < f(min(2, 3)); i++) d += 1.0;\n  while (d > 0.0) d -= 0.5;\n  for (;;) { break; }\n  return d; }");
    CHECK(g.find("for (int i = 0; (i < f(min(2, 3))) && (g_loopGuard++ < LOOP_GUARD); i++)") != std::string::npos, "for loop guarded: %s", g.c_str());
    CHECK(g.find("while ((d > 0.0) && (g_loopGuard++ < LOOP_GUARD))") != std::string::npos, "while loop guarded");
    CHECK(g.find("for (; (true) && (g_loopGuard++ < LOOP_GUARD);)") != std::string::npos, "endless for guarded");
    CHECK(g.find("// for (x) in a comment") != std::string::npos && g.find("int format = 1;") != std::string::npos, "comments / names untouched");
  }
  {  // shader-language interpreter
    using namespace fd::sl;
    std::string err;
    auto run = [&](const std::string& src, const std::string& fn, std::vector<Value> args) {
      Value out;
      auto p = compile(src, err);
      if (!p) { std::printf("compile error: %s\n", err.c_str()); return Value::f(-12345); }
      std::string e2;
      if (!call(*p, fn, args, out, &e2)) { std::printf("run error: %s\n", e2.c_str()); return Value::f(-12345); }
      return out;
    };
    // arithmetic, int -> float, swizzles, built-ins
    CLOSE(run("float f(float x) { return 2 * x + 1; }", "f", {Value::f(3)}).v[0], 7, 1e-12);
    CLOSE(run("int f(int a, int b) { return a / b + a % b; }", "f", {Value::i(7), Value::i(2)}).v[0], 4, 1e-12);
    Value v = run("vec4 f(vec4 p) { vec4 q = p.wzyx; q.xy = q.yx * 2.0; return q; }", "f", {Value::vec4(1, 2, 3, 4)});
    CHECK(v.t == T_VEC4 && v.v[0] == 6 && v.v[1] == 8 && v.v[2] == 2 && v.v[3] == 1, "swizzle read / write");
    CLOSE(run("float f(vec3 p) { return length(p) - 1.0; }", "f", {Value::vec(3, std::array<double, 3>{3, 4, 0}.data())}).v[0], 4, 1e-12);
    CLOSE(run("float f(float x) { return clamp(mix(0.0, 10.0, x), 1.0, 8.0) + smoothstep(0.0, 1.0, 0.5) + step(0.5, x); }", "f", {Value::f(0.9)}).v[0],
          8 + 0.5 + 1, 1e-12);
    CLOSE(run("float f(float x) { return mod(-1.5, 1.0) + fract(-0.25) + sign(-3.0) + abs(-2); }", "f", {Value::f(0)}).v[0], 0.5 + 0.75 - 1 + 2, 1e-12);
    // control flow, loops, user functions, globals, #define
    CLOSE(run("#define N 5\nint g = 2;\nfloat sq(float x) { return x * x; }\nfloat f(float x) {\n  float s = 0.0;\n"
              "  for (int i = 0; i < N; i++) { if (i == 3) continue; s += sq(float(i)); }\n  int k = 0; while (true) { k++; if (k >= g) break; }\n"
              "  return s + float(k) + (x > 0.0 ? 100.0 : -100.0); }", "f", {Value::f(1)}).v[0], 0 + 1 + 4 + 16 + 2 + 100, 1e-12);
    // forward calls, matrices
    CLOSE(run("float f(float x) { return later(x); }\nfloat later(float x) { return x + 1.0; }", "f", {Value::f(1)}).v[0], 2, 1e-12);
    v = run("vec4 f(vec4 p) { mat4 m = mat4(0.0, 1.0, 0.0, 0.0,  -1.0, 0.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0); return m * p; }", "f",
            {Value::vec4(1, 0, 0, 0)});
    CHECK(v.v[0] == 0 && v.v[1] == 1, "mat4 * vec4 (column-major)");
    v = run("vec2 f(vec2 p) { mat2 m = mat2(2.0); return (m * m) * p + p * mat2(1.0); }", "f", {Value::vec(2, std::array<double, 2>{1, 1}.data())});
    CHECK(v.v[0] == 5 && v.v[1] == 5, "mat2 products");
    // errors with positions
    CHECK(!compile("float f(float x) {\n  return y;\n}", err) && err.rfind("2:10: unknown name 'y'", 0) == 0, "unknown name error: %s", err.c_str());
    CHECK(!compile("float f(float x) { return vec3(1.0); }", err) && err.find("cannot convert vec3 to float") != std::string::npos, "type error: %s", err.c_str());
    CHECK(!compile("float f(float x) { return cell(x); }", err) && err.find("unknown function 'cell'") != std::string::npos, "unknown function: %s", err.c_str());
    // the loop guard of custom code works the same here (guarded endless loop ends)
    std::string g = guardLoops("float f(float x) { float d = x; for (;;) { d += 1.0; } return d; }");
    CLOSE(run("#define LOOP_GUARD 4096\nint g_loopGuard = 0;\n" + g, "f", {Value::f(0)}).v[0], 4096, 1e-9);
    CHECK(hasFunction(*compile("vec4 warp(vec4 p) { return p; }", err), "warp", {T_VEC4}), "hasFunction");
    // the shared prelude (the shader's SDF helpers) runs in the interpreter
    std::string pre = sdfHelpersGLSL();
    auto pp = compile(pre + "\nfloat f(vec4 p) { return vnoise(p); }\nfloat g(vec4 p) { return h4(p); }\n"
                            "float e(float d, float w) { return opExtrude(d, w); }\nvec2 r(vec2 v) { return rot2(v, PI * 0.5); }\n"
                            "float u(float a, float b) { return sU(a, b, 0.5); }\nfloat fb(vec4 p) { return fbm(p); }", err);
    CHECK(pp != nullptr, "prelude compiles in the interpreter: %s", err.c_str());
    if (pp) {
      Value o;
      call(*pp, "f", {Value::vec4(2, 3, -1, 5)}, o);
      Value h;
      call(*pp, "g", {Value::vec4(2, 3, -1, 5)}, h);
      CLOSE(o.v[0], h.v[0], 1e-9);  // value noise at a lattice point = its hashed value
      call(*pp, "e", {Value::f(0.3), Value::f(0.4)}, o);
      CLOSE(o.v[0], 0.5, 1e-12);
      call(*pp, "r", {Value::vec(2, std::array<double, 2>{1, 0}.data())}, o);
      CHECK(std::abs(o.v[0]) < 1e-9 && std::abs(o.v[1] - 1) < 1e-9, "rot2");
      call(*pp, "u", {Value::f(1), Value::f(1)}, o);
      CLOSE(o.v[0], 0.875, 1e-12);
      call(*pp, "fb", {Value::vec4(0.3, 1.7, 2.2, -0.4)}, o);
      CHECK(o.v[0] > 0 && o.v[0] < 0.875, "fbm in range %f", o.v[0]);
    }
  }
  {  // custom SDF code runs exactly on the CPU (the interpreter); broken code: its bound box
    Node c = makeCustom("c", "float sdf(vec4 p) { return length(p) - 0.7; }");
    CLOSE(customSdf(c, {1, 0, 0, 0}), 0.3, 1e-12);
    CLOSE(customSdf(c, {0, 0, 0, 0}), -0.7, 1e-12);
    c.code = "float sdf(vec4 p) { return nope; }";
    c.codeBound = 1;
    CLOSE(customSdf(c, {2, 0, 0, 0}), 1.0, 1e-12);
  }
  {  // modifier definitions: the engine modifiers' reference code equals their native code
    for (const char* id : {"mirror", "repeat", "extrude", "revolve", "radial", "round", "onion", "elongate", "twist", "bend", "displace",
                           "noisedisplace", "taper", "softmirror", "stretch"}) {
      auto d = defs().modifier(id);
      CHECK(d != nullptr, "built-in definition %s", id);
      if (!d) continue;
      CHECK(checkModifierDef(*d).empty(), "%s checks: %s", id, checkModifierDef(*d).c_str());
    }
    uint32_t seed = 3;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) * (1.0 / 16777216.0) * 8 - 4; };
    struct Case { int type; Modifier m; };
    std::vector<Modifier> cases;
    { Modifier m; m.type = MOD_MIRROR; m.axis = 1; m.offset = 0.7; cases.push_back(m); }
    { Modifier m; m.type = MOD_REPEAT; m.axis = 2; m.spacing = 1.3; m.count = 2; cases.push_back(m); }
    { Modifier m; m.type = MOD_REPEAT; m.axis = 0; m.spacing = 0.9; m.count = 0; cases.push_back(m); }
    { Modifier m; m.type = MOD_RADIAL; m.i = 0; m.j = 3; m.radius = 1.5; m.count = 5; m.angle = 72; m.startAngle = 10; cases.push_back(m); }
    for (auto& m : cases) {
      auto d = modifierDefOf(m);
      Modifier g = makeModifier(*d);  // the same values through the generic path
      for (int i = 0; i < (int)d->params.size(); i++) setModParam(g, *d, i, modParam(m, *d, i));
      g.type = MOD_DEF; g.def = "copy-of-" + d->id;
      ModifierDef c = *d; c.id = g.def; c.role = 0;
      for (int i = 0; i < (int)d->params.size(); i++) g.params.clear();
      for (int i = 0; i < (int)c.params.size(); i++) { conformParams(g, c); g.params[paramOffset(c.params, i)] = modParam(m, *d, i); }
      auto rt = modRuntime(c);
      CHECK(rt->prog != nullptr, "%s runtime: %s", d->id.c_str(), rt->error.c_str());
      if (!rt->prog) continue;
      auto P = packedParams(g, c);
      for (int k = 0; k < 20; k++) {
        V4 q{rnd(), rnd(), rnd(), rnd()}, nq = q;
        applyModifier(nq, m);
        std::vector<sl::Value> args = {sl::Value::vec4(q[0], q[1], q[2], q[3])};
        for (size_t v = 0; v < P.size(); v += 4) args.push_back(sl::Value::vec4(P[v], P[v + 1], P[v + 2], P[v + 3]));
        args.push_back(sl::Value::i(-1));
        args.push_back(sl::Value::i(-1));
        sl::Value out;
        std::string e;
        bool ok = sl::call(*rt->prog, "warp_d", args, out, &e);
        CHECK(ok, "%s warp: %s", d->id.c_str(), e.c_str());
        double diff = 0;
        for (int a = 0; a < 4; a++) diff = std::max(diff, std::abs(out.v[a] - nq[a]));
        CHECK(diff < 1e-9, "%s reference code = native (%g)", d->id.c_str(), diff);
      }
    }
  }
  {  // texture definitions, slots, remap, mapping
    for (auto& d : builtinTextureDefs()) {
      std::string e = checkTextureDef(d);
      CHECK(e.empty(), "built-in texture %s: %s", d.id.c_str(), e.c_str());
    }
    CHECK(textureKind(*defs().texture("checker")) == TEXK_MASK && textureKind(*defs().texture("palette")) == TEXK_RGB, "texture kinds");
    // CPU values = the old fixed functions at their defaults
    auto texAt = [&](const char* id, V4 p) {
      auto d = defs().texture(id);
      std::vector<double> vals;
      double out[4];
      runTexture(*d, packValues(vals, d->params), p.data(), out);
      return out[0];
    };
    CLOSE(texAt("checker", {0.5, 0.5, 0.5, 0.5}), 0, 1e-12);
    CLOSE(texAt("checker", {1.5, 0.5, 0.5, 0.5}), 1, 1e-12);
    CLOSE(texAt("stripes", {0.7, 0, 0, 0}), 1, 1e-12);
    CLOSE(texAt("stripes", {0.2, 0, 0, 0}), 0, 1e-12);
    CLOSE(texAt("dots", {0.5, 0.5, 0.5, 0.5}), 1, 1e-12);
    CLOSE(texAt("gradient", {0, 0.5, 0, 0}), 0.75, 1e-12);
    {
      V4 q{0.3, 1.7, -0.4, 0.9};
      double n = std::sin(q[0] * 1.7 + std::sin(q[1] * 2.3 + q[3])) + std::sin(q[2] * 1.9 + std::sin(q[3] * 2.9 + q[0])) + 0.5 * std::sin(3.1 * q[1] + 2.0 * std::sin(1.3 * q[2]));
      CLOSE(texAt("noise", q), std::clamp(0.5 + 0.2 * n, 0.0, 1.0), 1e-12);
    }
    double cells = texAt("cells", {0.37, 1.21, -2.4, 0.66});
    CHECK(cells >= 0 && cells <= 1, "cells in 0..1 (%.3f)", cells);
    // remap: default = identity; low / high; invert; contrast keeps 0, 0.5 and 1; a two-point curve
    TexSlot t;
    CLOSE(texRemap(t, 0.3), 0.3, 1e-12);
    t.low = 0.2; t.high = 0.6;
    CLOSE(texRemap(t, 0.4), 0.5, 1e-12);
    CLOSE(texRemap(t, 0.9), 1, 1e-12);
    t = TexSlot{}; t.invert = true;
    CLOSE(texRemap(t, 0.3), 0.7, 1e-12);
    t = TexSlot{}; t.contrast = 0.7;
    CLOSE(texRemap(t, 0.5), 0.5, 1e-12);
    CHECK(texRemap(t, 0.3) < 0.3 && texRemap(t, 0.7) > 0.7, "contrast steepens");
    t = TexSlot{}; t.curve = {{1, 0}, {0, 1}};  // (unsorted on purpose)
    CLOSE(texRemap(t, 0), 1, 1e-12);
    CLOSE(texRemap(t, 0.5), 0.5, 1e-12);
    CLOSE(texRemap(t, 1), 0, 1e-12);
    // mapping: tex = scale * R^T (x - offset); thin objects sit mid-cell on their unused axes
    Material m;
    m.tex[CH_COLOR].def = "checker";
    m.tex[CH_COLOR].map.scale = 2;
    m.tex[CH_COLOR].map.offset = {1, 0, 0, 0};
    M4 A; V4 b;
    texMapMatrix(m, CH_COLOR, 3, A, b);
    V4 tq = mul(A, V4{1.5, 0.25, 0, 0}) + b;
    CLOSE(tq[0], 1, 1e-12); CLOSE(tq[1], 0.5, 1e-12); CLOSE(tq[3], 0.5, 1e-12);
    m.tex[CH_COLOR].map.rot[0] = 90;  // XY: x -> y
    texMapMatrix(m, CH_COLOR, 4, A, b);
    tq = mul(A, V4{1, 1, 0, 0}) + b;  // (x - offset) = (0, 1): rotated back by -90 = (1, 0), times 2
    CLOSE(tq[0], 2, 1e-9); CLOSE(tq[1], 0, 1e-9);
    // shared mapping follows the other channel (no cycles)
    m.tex[CH_BUMP].def = "noise";
    m.tex[CH_BUMP].mapFrom = CH_COLOR;
    CHECK(texMappingChannel(m, CH_BUMP) == CH_COLOR, "shared mapping");
    m.tex[CH_COLOR].mapFrom = CH_BUMP;
    CHECK(texMappingChannel(m, CH_BUMP) == CH_BUMP, "mapping cycle: own mapping");
    // colour on the CPU: a checker mask blends to the second colour
    Node leaf = makeShape("hypersphere", "s");
    leaf.color = {1, 0, 0};
    leaf.mat.tex[CH_COLOR] = legacyColorSlot(TEX_CHECKER, 1, 1, {0, 0, 1});
    auto c0 = texturedColor(leaf, 4, {0.5, 0.5, 0.5, 0.5}, {}, {});
    auto c1 = texturedColor(leaf, 4, {1.5, 0.5, 0.5, 0.5}, {}, {});
    CHECK(c0 == (std::array<double, 3>{1, 0, 0}) && c1 == (std::array<double, 3>{0, 0, 1}), "textured colour on the CPU");
    leaf.mat.tex[CH_COLOR].def = "colorgradient";
    leaf.mat.tex[CH_COLOR].params.clear();
    conformValues(leaf.mat.tex[CH_COLOR].params, defs().texture("colorgradient")->params);
    auto cg = texturedColor(leaf, 4, {0, -1, 0, 0}, {}, {});
    CLOSE(cg[0], 0.1, 1e-9); CLOSE(cg[2], 0.8, 1e-9);  // its "from" colour
    leaf.mat.tex[CH_ROUGHNESS] = legacyColorSlot(TEX_CHECKER, 1, 1, {0, 0, 0});
    leaf.mat.tex[CH_ROUGHNESS].value2 = 0.1;
    CLOSE(texturedChannel(leaf, CH_ROUGHNESS, 4, {1.5, 0.5, 0.5, 0.5}, {}, {}), 0.1, 1e-12);
    CLOSE(texturedChannel(leaf, CH_ROUGHNESS, 4, {0.5, 0.5, 0.5, 0.5}, {}, {}), leaf.mat.roughness, 1e-12);
    // files: slots round-trip; old materials become a colour slot
    leaf.mat.tex[CH_BUMP] = legacyColorSlot(TEX_STRIPES, 3, 0.5, {0, 0, 0});
    leaf.mat.tex[CH_BUMP].value2 = 0.03;
    leaf.mat.tex[CH_BUMP].curve = {{0, 0}, {0.5, 1}, {1, 0}};
    leaf.mat.tex[CH_BUMP].map.space = TS_WORLD;
    leaf.mat.tex[CH_BUMP].map.rot = {1, 2, 3, 4, 5, 6};
    leaf.mat.tex[CH_BUMP].invert = true;
    leaf.mat.tex[CH_ROUGHNESS].mapFrom = CH_BUMP;
    setParamValue(leaf.mat.tex[CH_BUMP].params, defs().texture("stripes")->params, 1, 0.25);
    Node back = nodeFromJson(nodeToJson(leaf));
    for (int c = 0; c < CH_COUNT; c++) CHECK(back.mat.tex[c] == leaf.mat.tex[c], "texture slot %s round-trips", CHANNEL_IDS[c]);
    Node old = nodeFromJson(R"({"id":"o","name":"o","kind":"leaf","prim":"point","color":[1,1,1],
      "material":{"texture":"Bricks","texScale":2.5,"texStrength":0.4,"color2":[0.1,0.2,0.3]}})");
    CHECK(old.mat.tex[CH_COLOR].def == "bricks" && old.mat.tex[CH_COLOR].map.scale == 2.5 && old.mat.tex[CH_COLOR].strength == 0.4 &&
          old.mat.tex[CH_COLOR].color2 == (std::array<double, 3>{0.1, 0.2, 0.3}), "old texture fields -> colour slot");
    // texture definitions: files, custom ones in the registry, checks
    TextureDef td;
    td.id = "test_rings2"; td.name = "Rings 2"; td.category = "Custom";
    td.params = {ParamDef{"freq", "Frequency", "", P_FLOAT, 0, 10, 3}};
    td.code = "vec4 tex(vec4 p) { float r = fract(length(p) * freq); return vec4(r, 1.0 - r, 0.5, step(0.5, r)); }\n";
    CHECK(checkTextureDef(td).empty(), "custom texture compiles: %s", checkTextureDef(td).c_str());
    TextureDef tb = textureDefFromJson(textureDefToJson(td));
    CHECK(tb.code == td.code && tb.params == td.params && tb.name == td.name, "texture definition round-trips");
    defs().setTexture(td);
    Node cl = makeShape("hypersphere", "c");
    cl.mat.tex[CH_COLOR].def = "test_rings2";
    double rgba[4];
    TexKind kk;
    CHECK(sampleTexSlot(cl.mat, CH_COLOR, 4, {0.1, 0, 0, 0}, rgba, &kk) && kk == TEXK_RGBA, "rgba texture sample");
    CLOSE(rgba[0], 0.3, 1e-9); CLOSE(rgba[3], 0, 1e-12);
    TextureDef bad = td;
    bad.code = "float notTex(vec4 p) { return 1.0; }\n";
    CHECK(!checkTextureDef(bad).empty(), "a texture needs tex()");
    bad.code = "float tex(vec4 p) { return q; }\n";
    CHECK(checkTextureDef(bad).rfind("1:", 0) == 0, "errors on the user's line: %s", checkTextureDef(bad).c_str());
    {
      ModifierDef md;
      md.id = "test_badline"; md.name = "b";
      md.params = {ParamDef{"amount", "", "", P_FLOAT, 0, 1, 0.5}};
      md.code = "float shape(float d, vec4 p) {\n  return d - amount * q;\n}\n";
      CHECK(checkModifierDef(md).rfind("2:", 0) == 0, "modifier errors on the user's line: %s", checkModifierDef(md).c_str());
    }
    // definition files: import registers them; ids of new definitions are free
    {
      TextureDef imp = td;
      imp.id = "checker";  // (a built-in id: gets a fresh one)
      imp.name = "Imported tex";
      std::string nm = importDefinition(textureDefToJson(imp).dump());
      CHECK(nm == "Imported tex" && defs().texture("user_imported_tex") != nullptr, "texture import with a fresh id");
      CHECK(newDefId("Imported tex", true) == "user_imported_tex2", "next free id");
      bool threw = false;
      try { importDefinition("{\"format\":\"4dcad-scene\"}"); } catch (...) { threw = true; }
      CHECK(threw, "importing a non-definition fails");
      defs().eraseTexture("user_imported_tex");
    }
    // animation paths of texture slots
    double v = 0;
    CHECK(setValue(leaf, "tex.bump.p.width", 0.6) && getValue(leaf, "tex.bump.p.width", v) && std::abs(v - 0.6) < 1e-12, "texture parameter path");
    CHECK(setValue(leaf, "tex.bump.rot.ZW", 45) && leaf.mat.tex[CH_BUMP].map.rot[5] == 45, "texture rotation path");
    CHECK(setValue(leaf, "tex.color.offset.w", 2) && leaf.mat.tex[CH_COLOR].map.offset[3] == 2, "texture offset path");
    CHECK(!setValue(leaf, "tex.metallic.strength", 1), "no path for an empty slot");
    bool listed = false;
    for (auto& av : animatableValues(leaf)) if (av.path == "tex.bump.value2") listed = true;
    CHECK(listed, "texture values are animatable");
  }
  {  // data-driven modifiers on the CPU (Inigo Quilez's operators, Onion layers)
    auto withMods = [&](Node n, std::vector<std::pair<std::string, std::vector<double>>> ms) {
      for (auto& [id, vals] : ms) {
        auto d = defs().modifier(id);
        Modifier m = makeModifier(*d);
        for (size_t i = 0; i < vals.size(); i++) setModParam(m, *d, (int)i, vals[i]);
        n.mods.push_back(m);
      }
      return n;
    };
    auto distAt = [&](const Node& leaf, V4 p) {
      Node r = makeGroup(BoolOp::Union, {leaf});
      SceneEvaluator ev(r, reg, "");
      return ev.evalFull(p).d;
    };
    Node hs = makeLeaf("hypersphere", "h");  // radius 1 in 4D
    Node ball = makeShape("hypersphere", "b");
    // elongate: a hypersphere stretched 1 along X keeps radius 1 around a segment
    CLOSE(distAt(withMods(ball, {{"elongate", {1, 0, 0, 0}}}), {3, 0, 0, 0}), 1, 1e-9);
    CLOSE(distAt(withMods(ball, {{"elongate", {1, 0, 0, 0}}}), {1.5, 0, 0, 0}), -0.5, 1e-9);
    // twist keeps distances from the origin: a hypersphere stays one (with the safety factor 0.6)
    CLOSE(distAt(withMods(ball, {{"twist", {0, 2, 1, 90}}}), {0.3, 2, 0.4, 0}), (std::sqrt(0.09 + 4 + 0.16) - 1) * 0.6, 1e-9);
    // stretch: conservative (never more than the true distance)
    CLOSE(distAt(withMods(ball, {{"stretch", {2, 1, 1, 1}}}), {3, 0, 0, 0}), 0.5, 1e-9);
    // onion twice: two thin skins
    double d1 = std::abs(1.1 - 1) - 0.1;
    CLOSE(distAt(withMods(ball, {{"onion", {0.1}}, {"onion", {0.03}}}), {1.1, 0, 0, 0}), std::abs(d1) - 0.03, 1e-9);
    // round, soft mirror, taper at its neutral height, displace at a node of the sine grid
    CLOSE(distAt(withMods(ball, {{"round", {0.2}}}), {2, 0, 0, 0}), 0.8, 1e-9);
    CLOSE(distAt(withMods(ball, {{"taper", {1, -0.3}}}), {0, 0, 2, 0}), 0.8, 1e-9);  // safety 0.8
    CLOSE(distAt(withMods(ball, {{"displace", {0.1, 8, 0}}}), {2, 0, 0, 0}), 1 / (1 + 0.8), 1e-9);
    double sm = std::sqrt(1 + 0.09) - 0.5;  // soft |x| of x = 1 (softness 0.3, offset 0.5)
    CLOSE(distAt(withMods(ball, {{"softmirror", {0, 0.3, 0.5}}}), {1, 0, 0, 0}), std::abs(sm) - 1, 1e-9);
    // bounds follow the bound hooks
    auto bnd = computeBounds(makeGroup(BoolOp::Union, {withMods(ball, {{"elongate", {3, 0, 0, 0}}})}), reg);
    double br = 0;
    for (auto& [id, b] : bnd) if (id != "root" && std::isfinite(b.r)) br = std::max(br, b.r);
    CHECK(br >= 4 - 1e-9, "elongate bound %.3f", br);
    (void)hs;
  }
  {  // the regular 4D polytopes
    const int counts[6] = {5, 8, 16, 24, 120, 600};
    const double ratio[6] = {4.0, 2.0, 2.0, std::sqrt(2.0), 1 / 0.9256147, 1 / 0.9256147};  // circumradius / inradius (120- / 600-cell: r / R = 0.92561...)
    for (int i = 0; i < 6; i++) {
      int t = POLY_5CELL + i;
      CHECK((int)polyFaces(t).size() == counts[i], "%s: %d cells", POLY_NAMES[t], (int)polyFaces(t).size());
      CLOSE(polyCircumRatio(t), ratio[i], 1e-5);
      for (auto& f : polyFaces(t)) CLOSE(length(f), 1.0, 1e-9);
      CLOSE(polyDistFlat(t, 0.8, {0, 0, 0, 0}), -0.8, 1e-9);
      // the folded distance equals the plain one (largest facet distance over all cells)
      uint32_t seed = 7;
      auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) * (1.0 / 16777216.0) * 4 - 2; };
      for (int k = 0; k < 20; k++) {
        V4 u{rnd(), rnd(), rnd(), rnd()};
        double full = -1e30;
        for (auto& f : polyFaces(t)) full = std::max(full, dot(u, f) - 0.8);
        CLOSE(polyDistFlat(t, 0.8, u), full, 1e-9);
      }
    }
    CHECK(polyIs4D(POLY_24CELL) && !polyIs4D(POLY_ICOSA) && !polySignSymmetric(POLY_5CELL), "4D polytope flags");
    PrimitiveRegistry reg;
    Node n = makeLeaf("polyhedron", "p");
    n.poly.type = POLY_120CELL;
    CHECK(nodeDim(n, reg) == 4, "4D polytopes are 4D");
    Node back = nodeFromJson(nodeToJson(n));
    CHECK(back.poly.type == POLY_120CELL, "polytope type saved");
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
