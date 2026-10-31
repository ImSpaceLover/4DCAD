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
  app.startModal(Modal::Grab);
  app.handleKey(88 /*X*/, false, false);
  app.handleInput(frameInput(560, 380, 60, 30, false, false, false), 0.016);
  app.confirmModal();
  after = findNode(app.root, cube)->xf.pos;
  V4 d = after - before;
  V4 xdir = normalize(reject(col(app.nodeWorldFrame(cube), 0), ana));
  CHECK(length(d) > 0.05 && std::abs(dot(normalize(d), xdir)) > 0.999, "grab X = object X axis in the slice");
  CHECK(std::abs(dot(d, ana)) < 1e-9, "grab stayed in the slice");

  // --- W slider moves the visible slice along ana ---
  double w0 = app.sliceW();
  V4 e0 = app.editCam.view().eye;
  app.setSliceW(w0 + 1.5);
  V4 e1 = app.editCam.view().eye;
  CHECK(std::abs(dot(e1 - e0, ana) - 1.5) < 1e-9 && length(e1 - e0) - 1.5 < 1e-9, "slice W");

  // --- the player starts where the edit camera is; marker projected into the edit slice ---
  app.setMode(Mode::Explore);
  CHECK(length(app.currentView().eye - app.editCam.view().eye) < 1e-9, "player starts at the edit camera");
  app.playerCam.move({0, 0, 0, 2.0}, nullptr);  // player walks 2 units along its ana
  app.setMode(Mode::Edit);
  app.editCam.orbit(100, 0);
  app.setMode(Mode::Explore);
  CHECK(length(app.currentView().eye - app.editCam.view().eye) < 1e-9, "player re-spawns at the edit camera");
  app.playerCam.move({0, 0, 0, 2.0}, nullptr);
  app.setMode(Mode::Edit);
  View pv = app.playerView();
  View ev = app.currentView();
  V4 a2 = col(ev.frame, 3);
  V4 m = pv.eye - a2 * dot(pv.eye - ev.eye, a2);
  CHECK(std::abs(dot(m - ev.eye, a2)) < 1e-9, "marker in slice");

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
  View tv = app.thumbnailView();
  V4 cam = app.currentView().eye;
  CHECK(std::abs(dot(cam - tv.eye, col(tv.frame, 3))) < 1e-6, "thumbnail slice contains camera");
  CHECK(std::abs(dot(V4{0, 0, 0, 0} - tv.eye, col(tv.frame, 3))) < 1e-6, "thumbnail slice contains world centre");

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
