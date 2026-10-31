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
  Node ground = makeShape("hyperplane", "Ground", {0, 0, 0, 0}, {0.45, 0.46, 0.5});
  ground.id = "ground";
  Node tess = makeShape("tesseract", "Tesseract", {0, 1.2, 0, 0}, {0.85, 0.35, 0.3});
  double ang[6] = {0, 0, 30, 0, 20, 0};
  tess.xf.rot = rotationFromAngles(ang);
  Node glome = makeShape("hypersphere", "Hypersphere", {0, 1.2, 0, 0}, {0.95, 0.75, 0.3});
  glome.xf.scale = 1.25;
  Node carved = makeGroup(BoolOp::Subtraction, {tess, glome}, "Carved tesseract");
  Node duo = makeShape("duocylinder", "Duocylinder", {-3.5, 1.1, 0, 0}, {0.3, 0.6, 0.9});
  Node tiger = makeShape("tiger", "Tiger", {3.5, 1.6, 0, 0}, {0.4, 0.85, 0.5});
  Node a = makeShape("hypersphere", "Blob A", {0, 1, -4, 0}, {0.8, 0.4, 0.8});
  Node b = makeShape("hypersphere", "Blob B", {0.9, 1.5, -4, 0.4}, {0.4, 0.4, 0.9});
  b.xf.scale = 0.7;
  Node blobs = makeGroup(BoolOp::SmoothUnion, {a, b}, "Smooth blobs");
  blobs.k = 0.5;
  Node pillar = makeShape("cubinder", "Pillars", {0, 1, 5, 0}, {0.75, 0.7, 0.6});
  pillar.xf.scale = 0.4;
  pillar.mods.push_back({2, 0, 2, 3});
  SceneFile f;
  f.root = makeGroup(BoolOp::Union, {ground, carved, duo, tiger, blobs, pillar}, "Scene");
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

}  // namespace fd
