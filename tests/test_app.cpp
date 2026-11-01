// App-level tests (no window / GL context needed): editing tools, slices, thumbnail.
#include <cstdio>
#include <cstdlib>

#include "app.hpp"
#include "examples.hpp"

using namespace fd;
static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static InputState frameInput(double x, double y, double dx, double dy, bool down, bool pressed, bool released, bool shift = false) {
  InputState in;
  in.vpHovered = true; in.mx = x; in.my = y; in.dx = dx; in.dy = dy;
  in.down[0] = down; in.pressed[0] = pressed; in.released[0] = released; in.shift = shift;
  return in;
}

// drag with the left button from the screen position of `id`
static void dragNode(App& app, const std::string& id, double dx, double dy, bool shift = false) {
  View v = app.currentView();
  double sx, sy;
  Node* n = findNode(app.root, id);
  if (!app.project(n->xf.pos, v, sx, sy)) { std::printf("not visible\n"); return; }
  app.handleInput(frameInput(sx, sy, 0, 0, true, true, false, shift), 0.016);
  for (int i = 1; i <= 10; i++) app.handleInput(frameInput(sx + dx * i / 10, sy + dy * i / 10, dx / 10, dy / 10, true, false, false, shift), 0.016);
  app.handleInput(frameInput(sx + dx, sy + dy, 0, 0, false, false, true, shift), 0.016);
}

int main() {
  App app;  // no init(): no renderer needed
  app.vpX = 0; app.vpY = 0; app.vpW = 1000; app.vpH = 700;
  app.loadScene(playgroundScene());
  app.root.children.resize(1);  // keep only the ground
  // a 3D cube in front of the camera, camera turned partly into W
  app.placeItem("builtin-cube", std::nullopt);
  std::string cube = app.selection[0];
  app.editCam.rotate4(0, 3, 0.6);
  app.editCam.rotate4(1, 3, 0.3);
  View v = app.editCam.view();
  V4 ana = col(v.frame, 3);
  // put the cube into the visible slice at the orbit target
  findNode(app.root, cube)->xf.pos = app.editCam.target;
  app.rebuild();

  // --- Move tool: displacement lies in the visible slice (no component along ana) ---
  app.tool = Tool::Move;
  V4 before = findNode(app.root, cube)->xf.pos;
  dragNode(app, cube, 120, -40);
  V4 after = findNode(app.root, cube)->xf.pos;
  CHECK(length(after - before) > 0.3, "moved %.3f", length(after - before));
  CHECK(std::abs(dot(after - before, ana)) < 1e-9, "move left the slice: %.6f", dot(after - before, ana));
  // Shift: along depth, still in the slice
  before = after;
  dragNode(app, cube, 0, -60, true);
  after = findNode(app.root, cube)->xf.pos;
  CHECK(std::abs(dot(after - before, col(v.frame, 2))) > 0.1, "depth move");
  CHECK(std::abs(dot(after - before, ana)) < 1e-9, "depth move left the slice");
  CHECK(app.canUndo(), "move is undoable");

  // --- Move gizmo: drag the X arrow; the object moves along its (projected) X axis, inside the slice ---
  app.selection = {cube};
  app.tool = Tool::Move;
  app.handleInput(frameInput(0, 0, 0, 0, false, false, false), 0.016);
  Gizmo g = app.gizmo();
  CHECK(g.visible && g.axes[0].visible, "move gizmo visible");
  {
    double ax = (g.cx + g.axes[0].ex) / 2, ay = (g.cy + g.axes[0].ey) / 2;
    before = findNode(app.root, cube)->xf.pos;
    app.handleInput(frameInput(ax, ay, 0, 0, false, false, false), 0.016);  // hover
    app.handleInput(frameInput(ax, ay, 0, 0, true, true, false), 0.016);
    for (int i = 1; i <= 10; i++) app.handleInput(frameInput(ax + 6 * i, ay, 6, 0, true, false, false), 0.016);
    app.handleInput(frameInput(ax + 60, ay, 0, 0, false, false, true), 0.016);
    after = findNode(app.root, cube)->xf.pos;
    V4 d = after - before;
    CHECK(length(d) > 0.05, "arrow moved %.4f", length(d));
    CHECK(std::abs(dot(normalize(d), g.axes[0].dir)) > 0.9999, "moved along the X arrow");
    CHECK(std::abs(dot(d, ana)) < 1e-9, "arrow move left the slice");
  }

  // --- Rotate gizmo: drag along a ring; ana stays fixed ---
  app.tool = Tool::Rotate;
  app.handleInput(frameInput(0, 0, 0, 0, false, false, false), 0.016);
  g = app.gizmo();
  CHECK(!g.rings.empty(), "rotation rings");
  {
    auto& ring = g.rings[0];
    auto p0 = ring.pts[4], p1 = ring.pts[12];
    M4 r0 = findNode(app.root, cube)->xf.rot;
    app.handleInput(frameInput(p0.first, p0.second, 0, 0, false, false, false), 0.016);
    app.handleInput(frameInput(p0.first, p0.second, 0, 0, true, true, false), 0.016);
    for (int i = 1; i <= 8; i++) {
      double x = p0.first + (p1.first - p0.first) * i / 8, y = p0.second + (p1.second - p0.second) * i / 8;
      app.handleInput(frameInput(x, y, (p1.first - p0.first) / 8, (p1.second - p0.second) / 8, true, false, false), 0.016);
    }
    app.handleInput(frameInput(p1.first, p1.second, 0, 0, false, false, true), 0.016);
    M4 r1 = findNode(app.root, cube)->xf.rot;
    M4 q = mul(r1, transpose(r0));
    CHECK(length(mul(q, ana) - ana) < 1e-6, "ring rotation moved ana by %.6f", length(mul(q, ana) - ana));
    CHECK(length(mul(q, ring.u) - ring.u) > 0.05, "ring rotated");
  }

  // --- MMB click sets the centre of rotation; orbiting keeps it on screen ---
  {
    app.tool = Tool::Select;
    View v0 = app.currentView();
    double sx, sy;
    app.project(app.nodeWorldPos(cube), v0, sx, sy);
    InputState in = frameInput(sx, sy, 0, 0, false, false, false);
    in.down[2] = in.pressed[2] = true; in.down[0] = in.pressed[0] = false;
    app.handleInput(in, 0.016);
    in = frameInput(sx, sy, 0, 0, false, false, false);
    in.released[2] = true;
    app.handleInput(in, 0.016);
    CHECK(app.editCam.pivotSet, "pivot set");
    V4 piv = app.editCam.pivot;
    double px0, py0, px1, py1;
    app.project(piv, app.currentView(), px0, py0);
    app.editCam.orbit(80, 30);
    app.project(piv, app.currentView(), px1, py1);
    CHECK(std::hypot(px1 - px0, py1 - py0) < 1e-6, "pivot stays put while orbiting");
  }

  // --- modal grab with X constraint = screen right ---
  app.selection = {cube};
  before = findNode(app.root, cube)->xf.pos;
  app.handleInput(frameInput(500, 350, 0, 0, false, false, false), 0.016);
  app.tool = Tool::Move;
  Gizmo gb = app.gizmo();
  V4 xdirBefore = gb.axes[0].dir;
  double ux = gb.axes[0].ex - gb.cx, uy = gb.axes[0].ey - gb.cy, ul = std::hypot(ux, uy);
  app.startModal(Modal::Grab);
  app.handleKey(88 /*X*/, false, false);
  app.handleInput(frameInput(500 + 60 * ux / ul, 350 + 60 * uy / ul, 60 * ux / ul, 60 * uy / ul, false, false, false), 0.016);  // along the arrow
  app.confirmModal();
  after = findNode(app.root, cube)->xf.pos;
  V4 d = after - before;
  V4 xdir = xdirBefore;
  CHECK(length(d) > 0.05 && std::abs(dot(normalize(d), xdir)) > 0.999, "grab X = object X axis in the slice: |d|=%.4f dot=%.4f", length(d), length(d) > 0 ? dot(normalize(d), xdir) : 0.0);
  CHECK(std::abs(dot(d, ana)) < 1e-9, "grab stayed in the slice");

  // --- W slider moves the visible slice along ana ---
  double w0 = app.sliceW();
  V4 e0 = app.editCam.view().eye;
  app.setSliceW(w0 + 1.5);
  V4 e1 = app.editCam.view().eye;
  CHECK(std::abs(dot(e1 - e0, ana) - 1.5) < 1e-9 && length(e1 - e0) - 1.5 < 1e-9, "slice W");

  // --- Edit and Explore share one camera: switching modes never moves you ---
  {
    View e0 = app.currentView();
    app.setMode(Mode::Explore);
    View x0 = app.currentView();
    CHECK(length(x0.eye - e0.eye) < 1e-9, "edit -> explore keeps the position");
    app.editCam.move({0.5, 0, 1.0, 2.0}, nullptr);  // walk, including along W
    View x1 = app.currentView();
    app.setMode(Mode::Edit);
    View e1 = app.currentView();
    CHECK(length(e1.eye - x1.eye) < 1e-9, "explore -> edit keeps the position");
    double fe = 0;
    for (int i = 0; i < 16; i++) fe = std::max(fe, std::abs(e1.frame[i] - x1.frame[i]));
    CHECK(fe < 1e-9, "and the orientation");
  }
  // --- free orbit: no pitch limit ---
  {
    double p0 = app.editCam.pitch;
    for (int i = 0; i < 100; i++) app.editCam.orbit(0, -10);
    CHECK(app.editCam.pitch - p0 > 7.5, "orbit went over the top (%.2f rad)", app.editCam.pitch - p0);
  }
  // --- gizmo: always 3 local axes, orthonormal and inside the slice, even rotated into W ---
  {
    Node* c = findNode(app.root, cube);
    double ang[6] = {0, 0, 45, 0, 30, 20};
    c->xf.rot = rotationFromAngles(ang);
    app.rebuild();
    app.tool = Tool::Move;
    app.selection = {cube};
    Gizmo gz = app.gizmo();
    V4 ana2 = col(app.currentView().frame, 3);
    int n = 0;
    for (int a = 0; a < 3; a++) {
      n += gz.axes[a].visible;
      CHECK(std::abs(dot(gz.axes[a].dir, ana2)) < 1e-9, "gizmo axis %d inside the slice", a);
      for (int b = a + 1; b < 3; b++) CHECK(std::abs(dot(gz.axes[a].dir, gz.axes[b].dir)) < 1e-9, "axes %d,%d orthogonal", a, b);
    }
    CHECK(n == 3, "3 arrows");
    // the X arrow follows the local X axis as closely as the slice allows (mix of world X and W)
    V4 lx = col(app.nodeWorldFrame(cube), 0);
    CHECK(dot(gz.axes[0].dir, lx) > 0.5, "X arrow ~ local X");
    app.tool = Tool::Rotate;
    CHECK(app.gizmo().rings.size() == 3, "3 rings");
  }
  // --- clicking an object inside a group selects that object, not the group ---
  {
    app.loadScene(playgroundScene());
    app.tool = Tool::Move;
    std::string tessId;
    walk(app.root, [&](Node& m, Node*, int) { if (m.name == "Tesseract") tessId = m.id; });
    app.editCam.target = app.nodeWorldPos(tessId);
    app.editCam.distance = 6;
    app.editCam.yaw = 0.3; app.editCam.pitch = -0.2; app.editCam.reset4();
    app.rebuild();
    double sx, sy;
    // aim at the tesseract's frame (its centre is hollowed out by the hypersphere)
    View vv = app.currentView();
    app.project(app.nodeWorldPos(tessId) + col(vv.frame, 0) * 0.85, vv, sx, sy);
    auto hit = app.pick(sx, sy);
    CHECK(hit && hit->node, "aiming at the carved tesseract");
    tessId = hit->node->id;  // a leaf (tesseract or the subtracted hypersphere's cut surface)
    app.handleInput(frameInput(sx, sy, 0, 0, true, true, false), 0.016);
    app.handleInput(frameInput(sx, sy, 0, 0, false, false, true), 0.016);
    CHECK(app.selection.size() == 1 && app.selection[0] == tessId, "single click selects the object");
    app.handleInput(frameInput(sx, sy, 0, 0, true, true, false), 0.016);
    app.handleInput(frameInput(sx, sy, 0, 0, false, false, true), 0.016);
    CHECK(app.selection.size() == 1 && app.selection[0] == tessId, "second click still selects the object");
    app.tool = Tool::Select;
    InputState in = frameInput(sx, sy, 0, 0, true, true, false);
    in.ctrl = true;
    app.handleInput(in, 0.016);
    in = frameInput(sx, sy, 0, 0, false, false, true);
    in.ctrl = true;
    app.handleInput(in, 0.016);
    CHECK(app.selection.size() == 1 && findNode(app.root, app.selection[0])->name == "Carved tesseract", "ctrl+click selects the group");
  }
  // --- unsaved changes are tracked; saving clears them ---
  {
    CHECK(!app.dirty(), "fresh scene is clean");
    findNode(app.root, app.root.children[1].id)->xf.pos[0] += 1;
    app.commit();
    CHECK(app.dirty(), "edit marks dirty");
    std::string err;
    std::string path = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/test.4dscene.json";
    CHECK(app.saveSceneFile(path, err), "save: %s", err.c_str());
    CHECK(!app.dirty(), "saved scene is clean");
    CHECK(app.lastFile == path, "remembered as last file");
  }

  // --- geodesic: anchored objects slide on the surface; thumbnail slice contains the camera ---
  app.loadScene(planetScene());
  app.setGeo(true);
  CHECK(app.geo && app.geoEdit.valid, "geo view");
  std::string red = app.root.children[1].id;
  app.selection = {red};
  app.tool = Tool::Move;
  findNode(app.root, red);
  app.translateNodes({red}, col(app.currentView().frame, 0) * 0.8);
  CHECK(std::abs(length(findNode(app.root, red)->xf.pos) - 3) < 1e-3, "anchored stays on surface");
  // geodesic edit: hold RMB + W flies along the surface
  {
    V4 p0 = app.currentView().eye;
    InputState in = frameInput(500, 350, 0, 0, false, false, false);
    in.down[1] = in.pressed[1] = true;
    in.keyDown = [](int k) { return k == 87; /* W */ };
    app.handleInput(in, 0.1);
    in.pressed[1] = false;
    for (int i = 0; i < 10; i++) app.handleInput(in, 0.1);
    in.down[1] = false; in.released[1] = true;
    app.handleInput(in, 0.1);
    V4 p1 = app.currentView().eye;
    CHECK(length(p1 - p0) > 0.5 && std::abs(length(p1) - 3) < 1e-3, "flew %.3f along the surface", length(p1 - p0));
  }
  {
    auto sc = app.worldColor();
    CHECK(std::abs(sc[0] - 0.35f) < 1e-4 && std::abs(sc[2] - 0.75f) < 1e-4, "geodesic sky = world surface colour");
  }
  View tv = app.thumbnailView();
  V4 cam = app.currentView().eye;
  CHECK(std::abs(dot(cam - tv.eye, col(tv.frame, 3))) < 1e-6, "thumbnail slice contains camera");
  CHECK(std::abs(dot(V4{0, 0, 0, 0} - tv.eye, col(tv.frame, 3))) < 1e-6, "thumbnail slice contains world centre");

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
