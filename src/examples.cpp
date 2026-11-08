#include "examples.hpp"

#include "surface.hpp"

namespace fd {

static Node anchored(const char* prim, const std::string& name, V4 c, double R, V4 dir, std::array<double, 3> color, double scale, const std::string& world) {
  V4 n = normalize(dir);
  Node l = makeShape(prim, name, c + n * R, color);
  l.anchor = Anchor{world, basisFromNormal(n), R};
  l.xf.scale = scale;
  l.thickness = 0.4;
  return l;
}

SceneFile playgroundScene() {
  Node ground = makeShape("hyperplane", "Ground", {0, 0, 0, 0}, {0.5, 0.52, 0.56});
  ground.id = "ground";
  ground.mat.tex[CH_COLOR] = legacyColorSlot(TEX_CHECKER, 1, 0.25, {0.3, 0.31, 0.35});  // 4D checkerboard floor
  Node tess = makeShape("tesseract", "Tesseract", {0, 1.2, 0, 0}, {0.85, 0.35, 0.3});
  double ang[6] = {0, 0, 30, 0, 20, 0};
  tess.xf.rot = rotationFromAngles(ang);
  Node glome = makeShape("hypersphere", "Hypersphere", {0, 1.2, 0, 0}, {0.95, 0.75, 0.3});
  glome.xf.scale = 1.25;
  Node carved = makeGroup(BoolOp::Subtraction, {tess, glome}, "Carved tesseract");
  Node duo = makeShape("duocylinder", "Duocylinder", {-3.5, 1.1, 0, 0}, {0.3, 0.6, 0.9});
  Node tiger = makeShape("tiger", "Tiger", {3.5, 1.6, 0, 0}, {0.4, 0.85, 0.5});
  tiger.mat.tex[CH_COLOR] = legacyColorSlot(TEX_STRIPES, 3, 0.7, {0.1, 0.3, 0.15});
  Node wavy = makeCustom("Wavy glome (custom GLSL)");
  wavy.xf.pos = {4, 1.2, 4, 0};
  wavy.color = {0.95, 0.55, 0.25};
  Node a = makeShape("hypersphere", "Blob A", {0, 1, -4, 0}, {0.8, 0.4, 0.8});
  Node b = makeShape("hypersphere", "Blob B", {0.9, 1.5, -4, 0.4}, {0.4, 0.4, 0.9});
  b.xf.scale = 0.7;
  b.mat.emissive = 1;  // glowing, casts no shadow
  Node blobs = makeGroup(BoolOp::SmoothUnion, {a, b}, "Smooth blobs");
  blobs.k = 0.5;
  Node pillar = makeShape("cubinder", "Pillars", {0, 1, 5, 0}, {0.75, 0.7, 0.6});
  pillar.xf.scale = 0.4;
  pillar.mods.push_back({2, 0, 2, 3});
  SceneFile f;
  f.root = makeGroup(BoolOp::Union, {ground, carved, duo, tiger, blobs, pillar, wavy}, "Scene");
  f.root.id = "root";
  return f;
}

SceneFile planetScene() {
  V4 c{0, 0, 0, 0};
  const double R = 3;
  const std::string W = "planet";
  Node planet = makeShape("hypersphere", "Planet (world)", c, {0.35, 0.5, 0.75});
  planet.id = W;
  planet.xf.scale = R;
  std::vector<Node> objs = {
      anchored("cube", "Red cube", c, R, {0, 1, 0, 0}, {0.9, 0.3, 0.25}, 0.5, W),
      anchored("sphere", "Yellow sphere", c, R, {0, 1, 0.5, 0}, {0.95, 0.8, 0.25}, 0.5, W),
      anchored("torus", "Green torus", c, R, {0.6, 1, 0, 0}, {0.3, 0.85, 0.4}, 0.5, W),
      anchored("cylinder", "Blue cylinder", c, R, {0, 1, -0.6, 0.2}, {0.3, 0.5, 0.95}, 0.5, W),
      anchored("cube", "Far cube", c, R, {0, 0, 1, 0}, {0.9, 0.5, 0.9}, 0.5, W),
      anchored("sphere", "W-side sphere", c, R, {0, 0.3, 0, 1}, {0.9, 0.9, 0.9}, 0.5, W),
      anchored("cube", "Antipode cube", c, R, {0, -1, 0, 0}, {0.95, 0.55, 0.2}, 0.5, W),
  };
  for (int i = 0; i < 10; i++) {
    double a = i / 10.0 * 2 * PI + 0.3;
    objs.push_back(anchored(i % 2 ? "cube" : "cylinder", "Ring A " + std::to_string(i + 1), c, R, {std::cos(a), 0, std::sin(a), 1e-4},
                            {0.95, 0.35 + 0.05 * i, 0.3}, 0.3, W));
  }
  for (int i = 0; i < 8; i++) {
    double a = i / 8.0 * 2 * PI + 0.2;
    objs.push_back(anchored(i % 2 ? "torus" : "sphere", "Ring B " + std::to_string(i + 1), c, R, {1e-4, std::cos(a), 0, std::sin(a)},
                            {0.3, 0.6 + 0.04 * i, 0.95}, 0.35, W));
  }
  std::vector<Node> all{planet};
  for (auto& o : objs) all.push_back(o);
  SceneFile f;
  f.root = makeGroup(BoolOp::Union, all, "Scene");
  f.root.id = "root";
  f.world = W;
  CameraState cs;
  cs.target = {0, 0, 0, 0}; cs.distance = 11; cs.yaw = 0.5; cs.pitch = -0.3;
  f.camera = cs;
  return f;
}

// Every material channel with a plain 0-1 checker on it (one channel per object), a reference row
// with each channel at 0 and at 1, and surroundings to reflect / refract (File > Examples)
SceneFile materialsScene() {
  auto checker = [](double v2) {  // the built-in Checker, 2 cells per unit, mask 0 -> 1 = base value -> v2
    TexSlot t = legacyColorSlot(TEX_CHECKER, 2, 1, {1, 1, 1});
    t.value2 = v2;
    return t;
  };
  std::vector<Node> objs;
  auto ball = [&](const std::string& name, double x, double z, double scale, std::array<double, 3> col) {
    Node b = makeShape("hypersphere", name, {x, 1.0, z, 0}, col);
    b.xf.scale = scale;
    return b;
  };
  const double X0 = -6, DX = 2;
  // row 1: one channel each, checker 0 -> 1
  Node c = ball("Colour 0-1 checker", X0, 0, 0.85, {0.12, 0.12, 0.12});
  c.mat.tex[CH_COLOR] = checker(1);  // (colour: dark grey -> white)
  Node r = ball("Roughness 0-1 checker", X0 + DX, 0, 0.85, {0.85, 0.85, 0.9});
  r.mat.metallic = 1; r.mat.roughness = 0.05;
  r.mat.tex[CH_ROUGHNESS] = checker(1);
  Node m = ball("Metallic 0-1 checker", X0 + 2 * DX, 0, 0.85, {0.9, 0.72, 0.35});
  m.mat.metallic = 0; m.mat.roughness = 0.15;
  m.mat.tex[CH_METALLIC] = checker(1);
  Node sp = ball("Specular 0-1 checker", X0 + 3 * DX, 0, 0.85, {0.03, 0.03, 0.04});  // (dark: the reflection is what shows)
  sp.mat.specular = 0; sp.mat.roughness = 0.15;
  sp.mat.tex[CH_SPECULAR] = checker(1);
  Node e = ball("Emission 0-1 checker", X0 + 4 * DX, 0, 0.85, {1.0, 0.55, 0.2});
  e.mat.emissive = 0; e.mat.emissionStrength = 3;
  e.mat.tex[CH_EMISSION] = checker(1);
  Node t = ball("Transparency 0-0.9 checker", X0 + 5 * DX, 0, 0.85, {0.6, 0.8, 1.0});
  t.mat.transparency = 0; t.mat.roughness = 0.1;
  t.mat.tex[CH_TRANSPARENCY] = checker(0.9);
  Node bu = ball("Bump 0-0.03 checker", X0 + 6 * DX, 0, 0.85, {0.75, 0.75, 0.75});
  bu.mat.roughness = 0.3; bu.mat.specular = 0.8;
  bu.mat.tex[CH_BUMP] = checker(0.03);
  for (Node* n : {&c, &r, &m, &sp, &e, &t, &bu}) objs.push_back(*n);
  // row 2: references, each channel at 0 (left) and at 1 (right)
  struct Ref { const char* name; std::array<double, 3> col; void (*set)(Material&, double); };
  const Ref refs[7] = {
      {"Colour", {0.12, 0.12, 0.12}, nullptr},
      {"Roughness", {0.85, 0.85, 0.9}, [](Material& mm, double v) { mm.metallic = 1; mm.roughness = v < 0.5 ? 0.05 : 1; }},
      {"Metallic", {0.9, 0.72, 0.35}, [](Material& mm, double v) { mm.metallic = v; mm.roughness = 0.15; }},
      {"Specular", {0.03, 0.03, 0.04}, [](Material& mm, double v) { mm.specular = v; mm.roughness = 0.15; }},
      {"Emission", {1.0, 0.55, 0.2}, [](Material& mm, double v) { mm.emissive = v; mm.emissionStrength = 3; }},
      {"Transparency", {0.6, 0.8, 1.0}, [](Material& mm, double v) { mm.transparency = v * 0.9; mm.roughness = 0.1; }},
      {"Bump", {0.75, 0.75, 0.75}, [](Material& mm, double) { mm.roughness = 0.3; mm.specular = 0.8; }},
  };
  std::vector<Node> refNodes;
  for (int i = 0; i < 7; i++)
    for (int v = 0; v < 2; v++) {
      std::array<double, 3> col = refs[i].col;
      if (i == 0 && v == 1) col = {1, 1, 1};
      Node b = ball(std::string(refs[i].name) + (v ? " = 1" : " = 0"), X0 + i * DX + (v ? 0.45 : -0.45), -2.6, 0.38, col);
      if (refs[i].set) refs[i].set(b.mat, v);
      refNodes.push_back(b);
    }
  objs.push_back(makeGroup(BoolOp::Union, refNodes, "Reference: each channel at 0 and 1"));
  // surroundings: a checker floor, coloured pillars and a back wall to reflect / refract
  Node ground = makeShape("hyperplane", "Ground", {0, 0, 0, 0}, {0.55, 0.56, 0.6});
  ground.id = "ground";
  ground.mat.tex[CH_COLOR] = legacyColorSlot(TEX_CHECKER, 1, 0.35, {0.25, 0.26, 0.3});
  objs.push_back(ground);
  std::array<double, 3> pc[3] = {{0.85, 0.25, 0.2}, {0.25, 0.75, 0.35}, {0.25, 0.4, 0.9}};
  for (int i = 0; i < 6; i++) {
    Node p = makeShape("cylinder", "Pillar " + std::to_string(i + 1), {X0 - 1 + i * 2.8, 1.6, 3.2, 0}, pc[i % 3]);
    p.xf.scale = 0.35;
    double up[6] = {0, 0, 0, 90, 0, 0};
    p.xf.rot = rotationFromAngles(up);
    objs.push_back(p);
  }
  Node wall = makeShape("tesseract", "Back wall", {0, 1.5, 5, 0}, {0.85, 0.82, 0.75});
  wall.xf.scale = 1;
  objs.push_back(wall);
  SceneFile f;
  f.root = makeGroup(BoolOp::Union, objs, "Scene");
  f.root.id = "root";
  PointLight l1;
  l1.id = "light1"; l1.name = "Key light"; l1.pos = {-3, 4, -3, 0}; l1.intensity = 2; l1.range = 14;
  PointLight l2;
  l2.id = "light2"; l2.name = "Fill light"; l2.pos = {5, 3, -2, 0.5}; l2.color = {0.7, 0.8, 1.0}; l2.intensity = 1.2; l2.range = 12;
  f.lights = {l1, l2};
  CameraState cs;
  cs.target = {0, 0.9, -0.8, 0}; cs.distance = 12; cs.yaw = 0; cs.pitch = -0.3;
  f.camera = cs;
  return f;
}

}  // namespace fd
