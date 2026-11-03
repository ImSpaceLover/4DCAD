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

  // materials and custom SDFs: JSON round trip, CPU bounding-box approximation, code renaming, code view
  {
    Node cust = makeCustom("C");
    cust.codeBound = 2;
    cust.mat.emissive = 0.5;
    cust.mat.texture = TEX_CHECKER;
    cust.mat.color2 = {0.1, 0.2, 0.3};
    Node back = nodeFromJson(nodeToJson(cust));
    CHECK(back.isCustom() && back.code == cust.code && back.codeBound == 2 && back.mat.emissive == 0.5 && back.mat.texture == TEX_CHECKER &&
          back.mat.color2[2] == 0.3, "material/custom json");
    CHECK(nodeDim(back, reg) == 4, "custom SDFs are 4D");
    Node r = makeGroup(BoolOp::Union, {back});
    SceneEvaluator ev(r, reg, "");
    CLOSE(ev.evalFull({3, 0, 0, 0}).d, 1, 1e-9);  // box of half size 2
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
    cube.shell = 0.2;
    Node r = makeGroup(BoolOp::Union, {cube});
    SceneEvaluator ev(r, reg, "");
    CHECK(ev.evalFull({0, 0, 0, 0}).d > 0.5, "shelled cube is hollow: %.3f", ev.evalFull({0, 0, 0, 0}).d);
    CHECK(ev.evalFull({0.9, 0, 0, 0}).d < 0, "shelled cube has a skin");
    CLOSE(ev.evalFull({0.9, 0, 0, 0.5}).d, 0.45, 1e-6);  // slab thickness unchanged: still 0.1 in W
    Node c2 = makeShape("cube", "c2");
    c2.round = -0.06;  // more than half the thickness: used to delete the object
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

  // node data buffer: 12 vec4 per node; new material fields and lights survive JSON
  {
    Node l = makeShape("cube", "c");
    l.mat.roughness = 0.2; l.mat.specular = 1.1; l.mat.transparency = 0.4; l.mat.ior = 1.33; l.mat.metallic = 0.8;
    Node r = makeGroup(BoolOp::Union, {l});
    auto cs = compileScene(r, reg, "", false, nullptr);
    auto d = packNodes(r, reg, cs, {});
    CHECK(d.size() == cs.nodeIds.size() * 48, "node buffer size");
    size_t li = 1;  // the leaf (root first)
    CLOSE(d[li * 48 + 44], 0.2, 1e-6);
    CLOSE(d[li * 48 + 46], 0.4, 1e-6);
    CHECK(cs.glsl.find("uniform") == std::string::npos, "no uniform arrays in the scene code (node data buffer)");
    Node rt = nodeFromJson(nodeToJson(l));
    CHECK(rt.mat.roughness == 0.2 && rt.mat.specular == 1.1 && rt.mat.transparency == 0.4 && rt.mat.ior == 1.33 && rt.mat.metallic == 0.8,
          "material round trip");
    CLOSE(d[li * 48 + 47], 0.8, 1e-6);  // metallic
    CLOSE(d[li * 48 + 33], 1.33, 1e-6); // index of refraction
    std::vector<PointLight> ls(1);
    ls[0].pos = {1, 2, 3, 4}; ls[0].range = 3; ls[0].shadows = false;
    auto back = lightsFromJson(lightsToJson(ls));
    CHECK(back.size() == 1 && back[0].pos[3] == 4 && back[0].range == 3 && !back[0].shadows, "lights round trip");
    // a scene far beyond the old ~90 node uniform limit still compiles to one shader
    Node big = makeGroup(BoolOp::Union, {});
    for (int i = 0; i < 300; i++) big.children.push_back(makeShape("sphere", "s", {double(i), 0, 0, 0}));
    auto cb = compileScene(big, reg, "", false, nullptr);
    CHECK(cb.nodeIds.size() == 301 && packNodes(big, reg, cb, {}).size() == 301 * 48, "300 objects");
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

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
