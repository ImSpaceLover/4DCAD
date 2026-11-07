// App: transforms (move / rotate / scale, gizmos, snapping) and viewport input.
#include "app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "examples.hpp"

namespace fd {

// ============================================================ transforms
M4 App::parentWorldRotation(const std::string& id, double* scaleAcc) {
  M4 r = identity();
  double s = 1;
  auto path = pathTo(root, id);
  for (size_t i = 0; i + 1 < path.size(); i++) { r = mul(r, nodeRotation(*path[i])); s *= path[i]->xf.scale; }
  if (scaleAcc) *scaleAcc = s;
  return r;
}

void App::translateNodes(const std::vector<std::string>& ids, const V4& delta) {
  if (space.curved()) {
    // the direction (tangent at dirBase_) is carried to each object, which slides along the geodesic
    double k = space.k();
    V4 dt = cs::tangent(k, dirBase_, delta);
    for (auto& id : ids) {
      Node* n = findNode(root, id);
      if (!n || id == root.id) continue;
      double sAnc = 1;
      M4 W = nodeWorldIso(id, &sAnc);
      V4 p = col(W, 3);
      V4 q = along(p, cs::transport(k, dirBase_, p, dt));
      M4 W2 = mul(cs::transvection(k, p, q), W);
      Node* par = findParent(root, id);
      M4 P = par ? nodeWorldIso(par->id) : identity();
      cs::setFromIso(space, n->xf, mul(cs::inverse(k, P), W2), sAnc);
    }
    return;
  }
  for (auto& id : ids) {
    Node* n = findNode(root, id);
    if (!n || id == root.id) continue;
    if (n->anchor && n->anchor->world == world && !world.empty()) {
      // anchored objects slide along the world surface (geodesically), carrying their frame
      auto f = field();
      const M4& b = n->anchor->basis;
      std::array<double, 3> local{dot(delta, col(b, 0)), dot(delta, col(b, 1)), dot(delta, col(b, 2))};
      double len = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
      if (len < 1e-9) continue;
      auto r = shoot(f, n->xf.pos, b, local, len, quality.geoMaxStep * 0.5);
      n->xf.pos = r.pos;
      n->anchor->basis = r.frame;
      n->anchor->reach = curvatureRadius(f, r.pos, r.frame);
      setRestFromPose(*n);  // with surface motion: the whole motion moves along
    } else {
      double s;
      M4 pr = parentWorldRotation(id, &s);
      n->xf.pos = n->xf.pos + mul(transpose(pr), delta) * (1 / s);
    }
  }
}

void App::rotateNodesPlane(const std::vector<std::string>& ids, const V4& u, const V4& v, double ang) {
  if (!space.curved()) { rotateNodes(ids, rotationInPlane(u, v, ang)); return; }
  double k = space.k();
  for (auto& id : ids) {
    Node* n = findNode(root, id);
    if (!n || id == root.id) continue;
    double sAnc = 1;
    M4 W = nodeWorldIso(id, &sAnc), Wi = cs::inverse(k, W);
    V4 p = col(W, 3);
    // the plane, carried to the object and written in its own axes
    V4 ul = mul(Wi, cs::transport(k, dirBase_, p, cs::tangent(k, dirBase_, u)));
    V4 vl = mul(Wi, cs::transport(k, dirBase_, p, cs::tangent(k, dirBase_, v)));
    ul[3] = vl[3] = 0;
    if (length(ul) < 1e-9) continue;
    ul = normalize(ul);
    vl = reject(vl, ul);
    if (length(vl) < 1e-9) continue;
    vl = normalize(vl);
    M4 W2 = mul(W, rotationInPlane(ul, vl, ang));
    Node* par = findParent(root, id);
    M4 P = par ? nodeWorldIso(par->id) : identity();
    cs::setFromIso(space, n->xf, mul(cs::inverse(k, P), W2), sAnc);
  }
}

void App::rotateNodes(const std::vector<std::string>& ids, const M4& q) {
  for (auto& id : ids) {
    Node* n = findNode(root, id);
    if (!n || id == root.id) continue;
    M4 pr = parentWorldRotation(id, nullptr);
    M4 local = mul(mul(transpose(pr), q), pr);
    if (n->anchor) {
      const M4& b = n->anchor->basis;
      n->xf.rot = orthonormalize(mul(mul(mul(transpose(b), local), b), n->xf.rot));
      setRestFromPose(*n);
    } else n->xf.rot = orthonormalize(mul(local, n->xf.rot));
  }
}

/** Object axis projected into the visible 3D slice (zero if it points straight out of it). */
std::array<V4, 3> App::gizmoAxes(const std::string& id, const View& v) {
  // local X, Y, Z of the object, with the component along the hidden 4th direction removed and
  // straightened into an orthonormal frame of the visible slice: always 3 usable axes. Moving
  // along one may change several world coordinates (e.g. X and W) but stays inside the slice.
  Node* n = findNode(root, id);
  if (space.curved()) {
    // the object's axes at its position (Local), or the world axes carried there from the origin (Global)
    V4 p = nodeWorldPos(id);
    M4 F = editSet.globalAxes ? cs::transvection(space.k(), cs::origin(), p) : nodeWorldIso(id);
    return {col(F, 0), col(F, 1), col(F, 2)};
  }
  M4 F = nodeWorldFrame(id);
  if (editSet.globalAxes) {
    if (n && n->anchor) F = n->anchor->basis;  // on a world surface: the surface's directions at the object
    else {
      // the shown world axes, exactly: an arrow changes a single coordinate (it may leave the slice)
      std::array<V4, 3> r;
      for (int k = 0; k < 3; k++) r[k] = unit(globalAxisLabel(k));
      return r;
    }
  }
  V4 out = (geo && n && n->anchor) ? col(n->anchor->basis, 3) : col(v.frame, 3);
  out = normalize(out);
  V4 a[3];
  for (int i = 0; i < 3; i++) a[i] = reject(col(F, i), out);
  // orthonormalise, most visible axes first so they stay closest to the real local axes
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int x, int y) { return length(a[x]) > length(a[y]); });
  std::array<V4, 3> r{};
  std::vector<V4> done{out};
  for (int k = 0; k < 3; k++) {
    V4 d = a[order[k]];
    for (auto& u : done) d = reject(d, u);
    if (length(d) < 1e-3) {  // degenerate: complete the slice's frame
      auto full = completeBasis(done);
      d = full[done.size()];
    }
    d = normalize(d);
    if (dot(d, col(F, order[k])) < 0) d = -d;
    r[order[k]] = d;
    done.push_back(d);
  }
  return r;
}

bool App::globalExact(const std::string& id) {
  if (space.curved()) return false;
  Node* n = findNode(root, id);
  return editSet.globalAxes && n && !n->anchor;
}

int App::globalAxisLabel(int k) const {
  if (geo) return k;
  M4 p = editCam.axes();
  V4 c = col(p, k);
  for (int i = 0; i < 4; i++) if (std::abs(c[i]) > 0.5) return i;
  return k;
}

V4 App::keyAxis(const std::string& id, const View& v, int k) {
  if (globalExact(id)) return unit(k);
  return k < 3 ? gizmoAxes(id, v)[k] : V4{0, 0, 0, 0};
}

void App::setSliderAxis(int s) {
  if (s < 0 || s > 3) return;
  editCam.setSlider(s);
  lastChange_ = now;
}

static double snapTo(double x, double step) { return step > 0 ? std::round(x / step) * step : x; }

std::string App::primaryOf(const std::vector<std::string>& ids) const {
  if (ids.empty()) return "";
  if (!selection.empty())
    for (auto& id : ids) if (id == selection.back()) return id;
  return ids[0];
}

double App::snapAng(double a) const { return snapNow_ ? snapTo(a, editSet.snapAngle * PI / 180) : a; }

V4 App::snapDistance(const std::vector<std::string>& ids, const V4& delta) {
  Node* n = findNode(root, primaryOf(ids));
  if (!snapNow_ || !n || ((!n->anchor || world.empty()) && !space.curved())) return delta;
  if (space.curved()) {  // no grid in curved space: the distance travelled snaps
    V4 t = cs::tangent(space.k(), dirBase_, delta);
    double l = std::sqrt(std::max(cs::mdot(space.k(), t, t), 0.0));
    return l < 1e-12 ? delta : t * (snapTo(l, editSet.snapMove) / l);
  }
  double l = length(delta);
  return l < 1e-12 ? delta : delta * (snapTo(l, editSet.snapMove) / l);
}

void App::snapToGrid(const std::vector<std::string>& ids, const std::vector<V4>& dirs) {
  double step = editSet.snapMove;
  std::string pid = primaryOf(ids);
  Node* n = findNode(root, pid);
  if (!snapNow_ || step <= 0 || !n || (n->anchor && !world.empty()) || space.curved()) return;
  V4 p = nodeWorldPos(pid);
  // orthonormal movement directions
  std::vector<V4> d;
  for (V4 v : dirs) {
    for (auto& u : d) v = reject(v, u);
    if (length(v) > 1e-6 && d.size() < 3) d.push_back(normalize(v));
  }
  int m = (int)d.size();
  if (m == 0) return;
  // each direction puts its dominant world coordinate on the grid (solve for the offsets
  // along the directions, so the object never leaves the line / plane it is moved in)
  int comp[3];
  bool used[4] = {false, false, false, false};
  for (int k = 0; k < m; k++) {
    int best = -1;
    for (int i = 0; i < 4; i++) if (!used[i] && (best < 0 || std::abs(d[k][i]) > std::abs(d[k][best]))) best = i;
    comp[k] = best;
    used[best] = true;
  }
  double A[3][4];
  for (int j = 0; j < m; j++) {
    for (int k = 0; k < m; k++) A[j][k] = d[k][comp[j]];
    A[j][m] = snapTo(p[comp[j]], step) - p[comp[j]];
  }
  for (int c = 0; c < m; c++) {  // Gaussian elimination, partial pivoting
    int piv = c;
    for (int r = c + 1; r < m; r++) if (std::abs(A[r][c]) > std::abs(A[piv][c])) piv = r;
    if (std::abs(A[piv][c]) < 1e-6) return;
    for (int k = 0; k <= m; k++) std::swap(A[c][k], A[piv][k]);
    for (int r = 0; r < m; r++) {
      if (r == c) continue;
      double f = A[r][c] / A[c][c];
      for (int k = c; k <= m; k++) A[r][k] -= f * A[c][k];
    }
  }
  V4 delta{0, 0, 0, 0};
  for (int k = 0; k < m; k++) delta = delta + d[k] * (A[k][m] / A[k][k]);
  if (length(delta) > 1e-12) translateNodes(ids, delta);
}

static double segDist(double px, double py, double ax, double ay, double bx, double by) {
  double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
  double t = l2 > 0 ? clampd(((px - ax) * vx + (py - ay) * vy) / l2, 0, 1) : 0;
  return std::hypot(px - ax - t * vx, py - ay - t * vy);
}

Gizmo App::gizmo() {
  Gizmo g;
  Node* n = selected();
  if (mode != Mode::Edit || tool == Tool::Select || !n || n->id == root.id || modal != Modal::None || isLocked(n->id)) return g;
  View v = currentView();
  V4 c = nodeWorldPos(n->id);
  if (!project(c, v, g.cx, g.cy)) return g;
  double depth = std::max(0.3, viewDepth(c, v));
  g.worldLen = 95.0 * 2 * depth * tanHalf() / vpH;
  g.visible = true;
  auto dirs = gizmoAxes(n->id, v);
  for (int a = 0; a < 3; a++) {
    auto& ax = g.axes[a];
    ax.axis = a;
    ax.dir = dirs[a];
    if (!project(along(c, ax.dir * g.worldLen), v, ax.ex, ax.ey)) { ax.ex = g.cx; ax.ey = g.cy; }
    ax.visible = true;  // always 3 arrows (an arrow pointing at the camera is just short)
    bool exact = globalExact(n->id);
    ax.axis = exact ? globalAxisLabel(a) : a;
    ax.faded = exact && !geo && std::abs(dot(ax.dir, col(v.frame, 3))) > 0.05;
  }
  if (tool == Tool::Rotate) {
    double r = g.worldLen * 0.85;
    const int planes[3][2] = {{1, 2}, {0, 2}, {0, 1}};  // rings about X, Y, Z
    for (auto& pl : planes) {
      Gizmo::Ring ring;
      ring.a = pl[0]; ring.b = pl[1];
      ring.u = dirs[pl[0]];
      ring.v = dirs[pl[1]];
      ring.la = g.axes[pl[0]].axis;
      ring.lb = g.axes[pl[1]].axis;
      bool ok = true;
      for (int k = 0; k <= 64 && ok; k++) {
        double t = k / 64.0 * 2 * PI, sx, sy;
        ok = project(along(c, (ring.u * std::cos(t) + ring.v * std::sin(t)) * r), v, sx, sy);
        ring.pts.push_back({sx, sy});
      }
      if (!ok) continue;
      double ux, uy, wx, wy;
      project(along(c, ring.u * r), v, ux, uy);
      project(along(c, ring.v * r), v, wx, wy);
      double cr = (ux - g.cx) * (wy - g.cy) - (uy - g.cy) * (wx - g.cx);
      ring.orient = cr >= 0 ? 1 : -1;
      g.rings.push_back(ring);
    }
  }
  if (gizmoHandle_ >= 0) g.hot = gizmoHandle_;
  else {
    double best = 9;
    if (tool == Tool::Move) {
      if (std::hypot(lastMx_ - g.cx, lastMy_ - g.cy) < 10) { g.hot = 4; best = 0; }
      for (int a = 0; a < 3; a++) {
        if (std::hypot(g.axes[a].ex - g.cx, g.axes[a].ey - g.cy) < 6) continue;
        double d = segDist(lastMx_, lastMy_, g.cx, g.cy, g.axes[a].ex, g.axes[a].ey);
        if (d < best && std::hypot(lastMx_ - g.cx, lastMy_ - g.cy) > 10) { best = d; g.hot = a; }
      }
    } else {
      for (size_t r = 0; r < g.rings.size(); r++) {
        auto& p = g.rings[r].pts;
        for (size_t k = 0; k + 1 < p.size(); k++) {
          double d = segDist(lastMx_, lastMy_, p[k].first, p[k].second, p[k + 1].first, p[k + 1].second);
          if (d < best) { best = d; g.hot = 10 + (int)r; }
        }
      }
    }
  }
  return g;
}

void App::captureOrig() {
  orig_.clear();
  for (auto& id : selection)
    if (Node* n = findNode(root, id); n && id != root.id && !isLocked(id)) orig_.push_back({id, {n->xf, n->anchor}});
}
void App::restoreOrig() {
  for (auto& [id, o] : orig_)
    if (Node* n = findNode(root, id)) { n->xf = o.xf; n->anchor = o.anchor; }
}

void App::startModal(Modal m) {
  if (mode != Mode::Edit || selection.empty()) return;
  captureOrig();
  if (orig_.empty()) return;
  modal = m;
  modalAxes.clear();
  modalX0_ = modalX_ = lastMx_;
  modalY0_ = modalY_ = lastMy_;
  modalView_ = currentView();
}
void App::confirmModal() { if (modal != Modal::None) { modal = Modal::None; commit(); } }
void App::cancelModal() { if (modal != Modal::None) { restoreOrig(); modal = Modal::None; rebuild(); } }

std::string App::transformHint() const {
  std::string a;
  for (int x : modalAxes) a += AXIS[x];
  std::string space = editSet.globalAxes ? "global " : "object ";
  std::string snap = snapNow_ ? " (snap)" : "";
  switch (modal) {
    case Modal::Grab: return "Grab" + (a.empty() ? std::string(" in screen plane") : " along " + space + a) + snap + " | X/Y/Z: axis | click/Enter confirm, Esc/RMB cancel";
    case Modal::Rotate: return "Rotate" + (a.empty() ? std::string(" in screen plane") : a.size() == 1 ? " about " + space + a : " in " + space + "plane " + a) + snap +
                               " | X/Y/Z: about axis, two keys (e.g. X Y): plane | click/Enter confirm, Esc/RMB cancel";
    case Modal::Scale: return std::string("Scale") + (snapNow_ ? " (snap)" : "") + " | click/Enter confirm, Esc/RMB cancel";
    default: return "";
  }
}

void App::applyModal() {
  restoreOrig();
  double dx = modalX_ - modalX0_, dy = modalY_ - modalY0_;
  const View& v = modalView_;
  V4 right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2);
  std::vector<std::string> ids;
  for (auto& [id, o] : orig_) ids.push_back(id);
  if (modal == Modal::Grab) {
    std::vector<V4> dirs{right, up};
    if (modalAxes.size() == 1) dirs = {keyAxis(primaryOf(ids), v, modalAxes[0])};
    for (auto& [id, o] : orig_) {
      V4 c = nodeWorldPos(id);
      dirBase_ = v.eye;
      double depth = std::max(0.5, std::abs(viewDepth(c, v)));
      double k = 2 * depth * tanHalf() / vpH;
      V4 delta = right * (dx * k) - up * (dy * k);
      if (modalAxes.size() == 1) {
        V4 axis = keyAxis(id, v, modalAxes[0]);
        double sx0, sy0, sx1, sy1;
        dirBase_ = c;  // the axis is a direction at the object
        if (length(axis) > 0.5 && project(c, v, sx0, sy0) && project(along(c, axis), v, sx1, sy1)) {
          double ax = sx1 - sx0, ay = sy1 - sy0, l2 = ax * ax + ay * ay;
          delta = l2 > 1e-6 ? axis * ((dx * ax + dy * ay) / l2) : axis * (dx * k);
        } else delta = {0, 0, 0, 0};
      }
      translateNodes({id}, snapDistance(ids, delta));
    }
    snapToGrid(ids, dirs);
  } else if (modal == Modal::Rotate) {
    double ang = snapAng(dx * 0.01);
    for (auto& id : ids) {
      if (modalAxes.empty()) { dirBase_ = v.eye; rotateNodesPlane({id}, right, up, -ang); continue; }
      int a, b;
      if (modalAxes.size() >= 2) { a = modalAxes[0]; b = modalAxes[1]; }
      else { int x = modalAxes[0]; a = x == 0 ? 1 : 0; b = x == 2 ? 1 : x == 3 ? 1 : 2; if (x == 1) { a = 2; b = 0; } }
      V4 u = keyAxis(id, v, a), w = keyAxis(id, v, b);
      if (space.curved()) {  // axes at the object (orthonormal in the space's metric)
        dirBase_ = nodeWorldPos(id);
        if (cs::mdot(space.k(), u, u) < 0.25 || cs::mdot(space.k(), w, w) < 0.04) continue;
        rotateNodesPlane({id}, u, w, ang);
        continue;
      }
      w = reject(w, u);
      if (length(u) < 0.5 || length(w) < 0.2) continue;
      rotateNodes({id}, rotationInPlane(u, normalize(w), ang));
    }
  } else if (modal == Modal::Scale) {
    for (auto& [id, o] : orig_) if (Node* n = findNode(root, id)) {
          double sc = o.xf.scale * std::exp(dx * 0.005);
          if (snapNow_ && editSet.snapScale > 0) sc = std::max(editSet.snapScale, snapTo(sc, editSet.snapScale));
          n->xf.scale = std::max(0.01, sc);
        }
  }
  rebuild();
}

// ============================================================ input
void App::handleKey(int key, bool shift, bool ctrl, bool alt) {
  auto P = [&](Action a) { return keys.pressed(a, key, ctrl, shift, alt); };
  if (P(Action::Undo)) { undo(); return; }
  if (P(Action::Redo)) { redo(); return; }
  if (P(Action::Group)) { groupSelection(BoolOp::Union); return; }
  if (P(Action::ToggleMode)) { setMode(mode == Mode::Edit ? Mode::Explore : Mode::Edit); return; }
  if (mode == Mode::Explore) {
    if (P(Action::ReleaseMouse)) { if (mouseCaptured) wantRelease = true; else setMode(Mode::Edit); }
    if (P(Action::Reset4D) && !geo) { V4 e = editCam.view().eye; editCam.reset4(); editCam.target = e + col(editCam.frame(), 2) * editCam.distance; }
    if (P(Action::DebugRay)) { debugRay = !debugRay; debugFrozen = false; lastChange_ = now; }
    if (P(Action::DebugRayFreeze) && debugRay) debugFrozen = !debugFrozen;  // the status line shows it
    if (P(Action::ExplorePlay)) playing = !playing;  // the animation plays while you walk around
    if (P(Action::ExploreRestart)) { setFrame(anim.start); playing = true; }
    return;
  }
  if (drag_ == Drag::Fly) return;  // movement keys while RMB is held
  if (modal != Modal::None) {
    int ax = P(Action::AxisX) ? 0 : P(Action::AxisY) ? 1 : P(Action::AxisZ) ? 2 : P(Action::AxisW) ? 3 : -1;
    if (ax >= 0) {
      if (modal == Modal::Rotate) modalAxes = (modalAxes.size() >= 2 || (modalAxes.size() == 1 && modalAxes[0] == ax)) ? std::vector<int>{ax} : (modalAxes.push_back(ax), modalAxes);
      else modalAxes = (modalAxes.size() == 1 && modalAxes[0] == ax) ? std::vector<int>{} : std::vector<int>{ax};
      applyModal();
    } else if (P(Action::Confirm)) confirmModal();
    else if (P(Action::Cancel)) cancelModal();
    return;
  }
  if (P(Action::ToggleView)) setGeo(!geo);
  else if (P(Action::ToolSelect)) tool = Tool::Select;
  else if (P(Action::ToolMove)) tool = Tool::Move;
  else if (P(Action::ToolRotate)) tool = Tool::Rotate;
  else if (P(Action::ToggleGlobal)) editSet.globalAxes = !editSet.globalAxes;
  else if (P(Action::ToggleSnap)) editSet.snap = !editSet.snap;
  else if (P(Action::Grab)) startModal(Modal::Grab);
  else if (P(Action::Rotate)) startModal(Modal::Rotate);
  else if (P(Action::Scale)) startModal(Modal::Scale);
  else if (P(Action::Delete)) { if (!selection.empty()) deleteSelection(); }
  else if (P(Action::Duplicate)) duplicateSelection();
  else if (P(Action::DuplicateLinked)) duplicateLinked();
  else if (P(Action::Isolate)) toggleIsolate();
  else if (P(Action::Focus)) focusSelection();
  else if (P(Action::Deselect)) selection.clear();
  else if (P(Action::SelectAll)) { selection.clear(); for (auto& c : root.children) selection.push_back(c.id); }
  else if (P(Action::ViewFront)) standardView(0);
  else if (P(Action::ViewRight)) standardView(1);
  else if (P(Action::ViewTop)) standardView(2);
  else if (P(Action::ViewAlongW)) standardView(3);
  else if (P(Action::Reset4D)) editCam.reset4();
  else if (P(Action::PlayPause)) playing = !playing;
  else if (P(Action::FrameStart)) { playing = false; setFrame(anim.start); }
  else if (P(Action::FrameEnd)) { playing = false; setFrame(anim.end); }
  else if (P(Action::FramePrev)) setFrame(std::round(anim.frame) - 1);
  else if (P(Action::FrameNext)) setFrame(std::round(anim.frame) + 1);
  else if (P(Action::InsertKey)) keyMenuRequest = true;
  else if (P(Action::DeleteKey)) deleteKeys();
  else if (P(Action::CameraView)) viewThroughCamera = !viewThroughCamera;
  else if (P(Action::LookThrough) || P(Action::CameraToView)) {
    // the selected camera, else the active one
    std::string id = selected() && selected()->isCamera() ? selected()->id : activeCamera;
    if (Node* c = findNode(root, id); c && c->isCamera()) { if (P(Action::LookThrough)) lookThrough(id); else cameraToView(id); }
    else toast("No camera: View > Cameras > Add camera at the view", true);
  }
}

void App::flyGeoEdit(const InputState& in, double dt) {
  // game-like editor camera in the Geodesic view: hold RMB, mouse looks, WASD/Space/Shift move, Q/E roll
  auto f = field();
  if (in.dx != 0 || in.dy != 0) geoEdit.look(in.dx * editSet.lookSpeed, in.dy * editSet.lookSpeed);
  if (!in.keyDown) return;
  auto K = [&](Action a) { return keys.held(a, in.keyDown) ? 1.0 : 0.0; };
  double sp = editSet.flySpeed * dt;
  double mr = K(Action::MoveRight) - K(Action::MoveLeft), mf = K(Action::MoveForward) - K(Action::MoveBack);
  double mu = K(Action::MoveUp) - K(Action::MoveDown);
  double roll = K(Action::RollRight) - K(Action::RollLeft);
  if (roll != 0) geoEdit.roll(roll * 1.2 * dt);
  if (mr != 0 || mf != 0 || mu != 0) geoEdit.move(f, {mr * sp, mu * sp, mf * sp}, quality.geoMaxStep * 0.5, nullptr);
}

void App::handleInput(const InputState& in, double dt) {
  lastMx_ = in.mx;
  lastMy_ = in.my;
  if (mode == Mode::Explore) { updateExplore(in, dt); return; }
  bool g = geo && geoEdit.valid;
  bool snapWas = snapNow_;
  snapNow_ = editSet.snap != in.ctrl;
  if (modal != Modal::None) {
    modalX_ = in.mx; modalY_ = in.my;
    if (in.dx != 0 || in.dy != 0 || snapWas != snapNow_) applyModal();
    if (in.pressed[0]) confirmModal();
    else if (in.pressed[1]) cancelModal();
    return;
  }
  // --- start drags ---
  for (int b = 0; b < 3; b++) {
    if (!in.pressed[b] || !in.vpHovered || drag_ != Drag::None) continue;
    pressX_ = in.mx; pressY_ = in.my; dragMoved_ = false; dragButton_ = b;
    selectionAtPress_ = selection;
    bool nav = (g && b == 1) || b == 1 || b == 2 || (b == 0 && in.alt);
    if (nav && inCameraView()) { lookThrough(activeCamera); viewThroughCamera = false; g = geo && geoEdit.valid; }  // navigating leaves the camera view
    if (g && b == 1) { drag_ = Drag::Fly; wantCapture = true; continue; }
    if (b == 1 || b == 2 || (b == 0 && in.alt)) {
      drag_ = in.shift ? Drag::Pan : in.ctrl ? Drag::Zoom : (in.alt && b != 0) ? Drag::Rot4 : Drag::Orbit;
      continue;
    }
    // left button: gizmo handle, object body (Move tool) or click-select
    Gizmo gz = gizmo();
    if (gz.visible && gz.hot >= 0) {
      gizmoHandle_ = gz.hot;
      gizmoAtPress_ = gz;
      drag_ = Drag::Gizmo;
      resetDragTotals();
      captureOrig();
      modalView_ = currentView();
      continue;
    }
    drag_ = Drag::Click;
    if (tool == Tool::Move) {
      auto hit = pick(in.mx, in.my);
      std::string camHit = pickCamera(in.mx, in.my);
      if (!camHit.empty() || (hit && hit->node)) {
        std::string id = !camHit.empty() ? camHit : hit->node->id;
        bool keep = false;
        for (auto& s : selection) if (s == id || isAncestor(root, s, id)) keep = true;
        if (!keep) selection = {id};
        resetDragTotals();
        captureOrig();
        modalView_ = currentView();
        drag_ = Drag::FreeMove;
      }
    }
  }
  if (in.wheel != 0 && in.vpHovered && inCameraView()) { lookThrough(activeCamera); viewThroughCamera = false; g = geo && geoEdit.valid; }
  if (in.wheel != 0 && in.vpHovered) {
    if (g) editSet.flySpeed = clampd(editSet.flySpeed * std::exp(in.wheel * 0.15), 0.2, 30);
    else if (space.curved()) curvCam.zoom(space, -in.wheel * editSet.zoomSpeed);
    else editCam.zoom(-in.wheel * editSet.zoomSpeed);
  }

  // --- continue drags ---
  if (drag_ != Drag::None && dragButton_ >= 0 && in.down[dragButton_]) {
    if (std::abs(in.mx - pressX_) + std::abs(in.my - pressY_) > 3) dragMoved_ = true;
    const View& v = modalView_;
    bool cv = space.curved();
    switch (drag_) {
      case Drag::Orbit:
        if (!dragMoved_) break;
        if (cv) curvCam.orbit(space, in.dx * editSet.orbitSpeed, in.dy * editSet.orbitSpeed);
        else editCam.orbit(in.dx * editSet.orbitSpeed, in.dy * editSet.orbitSpeed);
        break;
      case Drag::Pan:
        if (cv) curvCam.pan(space, in.dx * editSet.panSpeed, in.dy * editSet.panSpeed);
        else editCam.pan(in.dx * editSet.panSpeed, in.dy * editSet.panSpeed);
        break;
      case Drag::Zoom:
        if (cv) curvCam.zoom(space, in.dy * 0.05 * editSet.zoomSpeed);
        else editCam.zoom(in.dy * 0.05 * editSet.zoomSpeed);
        break;
      case Drag::Rot4:
        if (cv) curvCam.orbit(space, in.dx * editSet.orbitSpeed, in.dy * editSet.orbitSpeed);  // no 4th dimension here
        else { editCam.rotate4(0, 3, in.dx * 0.006 * editSet.rot4Speed); editCam.rotate4(1, 3, -in.dy * 0.006 * editSet.rot4Speed); }
        break;
      case Drag::Fly: flyGeoEdit(in, dt); break;
      case Drag::FreeMove: {
        if (in.dx == 0 && in.dy == 0 && snapWas == snapNow_) break;
        if (in.shift) dragDepth_ -= in.dy; else { dragDx_ += in.dx; dragDy_ += in.dy; }
        restoreOrig();
        V4 right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2);
        std::vector<std::string> ids;
        for (auto& [id, o] : orig_) ids.push_back(id);
        dirBase_ = v.eye;
        for (auto& id : ids) {
          double depth = std::max(0.5, std::abs(viewDepth(nodeWorldPos(id), v)));
          double k = 2 * depth * tanHalf() / vpH;
          translateNodes({id}, snapDistance(ids, right * (dragDx_ * k) - up * (dragDy_ * k) + fwd * (dragDepth_ * k * 2)));
        }
        std::vector<V4> dirs;
        if (dragDx_ != 0 || dragDy_ != 0) { dirs.push_back(right); dirs.push_back(up); }
        if (dragDepth_ != 0) dirs.push_back(fwd);
        snapToGrid(ids, dirs);
        rebuild();
        break;
      }
      case Drag::Gizmo: {
        if (in.dx == 0 && in.dy == 0 && snapWas == snapNow_) break;
        Gizmo& gz = gizmoAtPress_;
        std::vector<std::string> ids;
        for (auto& [id, o] : orig_) ids.push_back(id);
        restoreOrig();  // re-apply the whole drag from the original state
        if (gizmoHandle_ <= 3) {  // arrow: move along the (projected) object axis
          auto& ax = gz.axes[gizmoHandle_];
          double sx = ax.ex - gz.cx, sy = ax.ey - gz.cy, l2 = sx * sx + sy * sy;
          if (l2 > 1) dragAlong_ += (in.dx * sx + in.dy * sy) / l2 * gz.worldLen;
          if (Node* pn = findNode(root, primaryOf(ids))) dirBase_ = nodeWorldPos(pn->id);  // arrows: directions at the object
          translateNodes(ids, snapDistance(ids, ax.dir * dragAlong_));
          snapToGrid(ids, {ax.dir});
        } else if (gizmoHandle_ == 4) {  // centre: free move in the screen plane (Shift: along depth)
          double k = gz.worldLen / 95.0;
          if (in.shift) dragDepth_ -= in.dy; else { dragDx_ += in.dx; dragDy_ += in.dy; }
          V4 right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2);
          dirBase_ = v.eye;
          translateNodes(ids, snapDistance(ids, right * (dragDx_ * k) - up * (dragDy_ * k) + fwd * (dragDepth_ * k * 2)));
          std::vector<V4> dirs;
          if (dragDx_ != 0 || dragDy_ != 0) { dirs.push_back(right); dirs.push_back(up); }
          if (dragDepth_ != 0) dirs.push_back(fwd);
          snapToGrid(ids, dirs);
        } else {  // ring: rotate in the plane of two axes, by the angle swept on screen
          auto& ring = gz.rings[gizmoHandle_ - 10];
          double a0 = std::atan2(in.my - in.dy - gz.cy, in.mx - in.dx - gz.cx), a1 = std::atan2(in.my - gz.cy, in.mx - gz.cx);
          double da = a1 - a0;
          if (da > PI) da -= 2 * PI;
          if (da < -PI) da += 2 * PI;
          dragAngle_ += da * ring.orient;
          if (Node* pn = findNode(root, primaryOf(ids))) dirBase_ = nodeWorldPos(pn->id);
          rotateNodesPlane(ids, ring.u, ring.v, snapAng(dragAngle_));
        }
        rebuild();
        break;
      }
      default: break;
    }
  }
  // --- release ---
  if (dragButton_ >= 0 && in.released[dragButton_]) {
    if ((drag_ == Drag::FreeMove || drag_ == Drag::Gizmo) && dragMoved_) commit();
    if (drag_ == Drag::Fly) wantRelease = true;
    // MMB click (no drag) in the Full 4D view: set the centre of rotation
    if (drag_ == Drag::Orbit && dragButton_ == 2 && !dragMoved_ && space.curved()) {
      if (auto hit = pick(in.mx, in.my)) { curvCam.focus(space, hit->pos, cs::distance(space, curvCam.view().eye, hit->pos)); toast("Centre of rotation set"); }
    } else if (drag_ == Drag::Orbit && dragButton_ == 2 && !dragMoved_ && !g) {
      if (auto hit = pick(in.mx, in.my)) { editCam.setPivot(hit->pos); toast("Centre of rotation set"); }
      else { editCam.pivotSet = false; toast("Centre of rotation reset to the view centre"); }
    }
    bool click = !dragMoved_ && dragButton_ == 0 && (drag_ == Drag::Click || drag_ == Drag::FreeMove);
    if (click && in.vpHovered) {
      auto hit = pick(in.mx, in.my);
      std::string id = pickCamera(in.mx, in.my);  // camera icons first (they have no shape)
      if (id.empty()) id = hit && hit->node ? hit->node->id : "";
      // a click selects exactly the object under the cursor; Ctrl+click selects its group instead
      if (!id.empty() && in.ctrl) {
        Node* p = findParent(root, id);
        if (p && p->id != root.id) id = p->id;
      }
      (void)selectionAtPress_;
      if (in.shift && !id.empty()) {
        auto it = std::find(selection.begin(), selection.end(), id);
        if (it != selection.end()) selection.erase(it); else selection.push_back(id);
      } else if (id.empty()) selection.clear();
      else selection = {id};
    }
    drag_ = Drag::None;
    dragButton_ = -1;
    gizmoHandle_ = -1;
  }
}

void App::updateExplore(const InputState& in, double dt) {
  const auto& X = exploreSet;
  if (mouseCaptured && (in.dx != 0 || in.dy != 0)) {
    if (space.curved()) curvCam.look(space, in.dx * X.lookSpeed, in.dy * X.lookSpeed);
    else if (geo && geoEdit.valid) geoEdit.look(in.dx * X.lookSpeed, in.dy * X.lookSpeed);
    else editCam.look(in.dx * X.lookSpeed, in.dy * X.lookSpeed);
  }
  if (in.pressed[0] && in.vpHovered && !mouseCaptured) wantCapture = true;
  if (in.wheel != 0) {
    double& sp = geo ? exploreSet.geoMoveSpeed : exploreSet.moveSpeed;
    sp = clampd(sp * std::exp(in.wheel * 0.1), 0.2, 30);
  }
  if (!in.keysEnabled || !in.keyDown) return;
  auto K = [&](Action a) { return keys.held(a, in.keyDown) ? 1.0 : 0.0; };
  double rot = 1.2 * dt * X.turn4Speed;
  double mr = K(Action::MoveRight) - K(Action::MoveLeft), mf = K(Action::MoveForward) - K(Action::MoveBack);
  double mu = K(Action::MoveUp) - K(Action::MoveDown);
  double ma = K(Action::RollRight) - K(Action::RollLeft);
  double r = X.bodyRadius;
  if (space.curved()) {
    double sp = X.moveSpeed * dt;
    if (ma != 0) curvCam.roll(space, ma * 1.2 * dt);
    if (mr != 0 || mf != 0 || mu != 0)
      curvCam.move(space, {mr * sp, mu * sp, mf * sp, 0},
                   X.collide ? std::function<bool(const V4&)>([&](const V4& to) { return ev->evalCurved(to).d < r; }) : nullptr);
  } else if (geo && geoEdit.valid) {
    double sp = X.geoMoveSpeed * dt;
    auto f = field();
    if (ma != 0) geoEdit.roll(ma * 1.2 * dt);
    if (mr != 0 || mf != 0 || mu != 0)
      geoEdit.move(f, {mr * sp, mu * sp, mf * sp}, quality.geoMaxStep * 0.5,
                   X.geoCollide ? std::function<bool(const V4&)>([&](const V4& to) { return ev->evalGeo(to).d < r; }) : nullptr);
  } else {
    double sp = X.moveSpeed * dt;
    double zx = K(Action::TurnWPlus) - K(Action::TurnWMinus), cv = K(Action::SideWPlus) - K(Action::SideWMinus);
    double mw = K(Action::MoveAnaPlus) - K(Action::MoveAnaMinus);  // along the slider (4th) axis
    if (zx != 0) editCam.rotate4AroundEye(2, 3, zx * rot);
    if (cv != 0) editCam.rotate4AroundEye(0, 3, cv * rot);
    if (ma != 0) editCam.rotate4AroundEye(0, 1, ma * 1.2 * dt);  // Q/E roll, as in the Geodesic view
    if (mr != 0 || mf != 0 || mu != 0 || mw != 0)
      editCam.move({mr * sp, mu * sp, mf * sp, mw * sp},
                   X.collide ? std::function<bool(const V4&)>([&](const V4& to) { return ev->evalFull(to).d < r; }) : nullptr);
  }
}

}  // namespace fd
