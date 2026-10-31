#include "app.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "examples.hpp"

namespace fd {

static std::array<double, 3> hsl(double h, double s, double l) {
  auto f = [&](double n) {
    double k = std::fmod(n + h * 12, 12.0);
    return l - s * std::min(l, 1 - l) * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
  };
  return {f(0), f(8), f(4)};
}

App::App() {}

void App::init() {
  renderer = std::make_unique<Renderer>();
  std::string text;
  std::string auto_ = (std::filesystem::path(dataDir()) / "autosave.4dscene.json").string();
  bool ok = false;
  if (readFile(auto_, text)) {
    try { loadScene(sceneFromJson(text)); ok = true; } catch (...) {}
  }
  if (!ok) loadScene(playgroundScene());
}

void App::toast(const std::string& msg, bool error) {
  toastMsg = msg;
  toastError = error;
  toastUntil = now + (error ? 5 : 2.5);
}

// ============================================================ scene lifecycle
std::string App::snapshot() { return world + "\n" + nodeToJson(root); }
void App::restore(const std::string& s) {
  auto nl = s.find('\n');
  world = s.substr(0, nl);
  root = nodeFromJson(s.substr(nl + 1));
}

void App::loadScene(SceneFile f) {
  root = std::move(f.root);
  world = (!f.world.empty() && findNode(root, f.world)) ? f.world : "";
  selection.clear();
  if (f.camera) editCam.load(*f.camera);
  mode = Mode::Edit;
  geo = false;
  geoEdit.valid = geoPlayer.valid = false;
  playerPlaced = false;
  modal = Modal::None;
  history_.clear();
  future_.clear();
  rebuild();
  committed_ = snapshot();
}

void App::newScene() {
  SceneFile f = playgroundScene();
  std::vector<Node> keep;
  for (auto& c : f.root.children) if (c.id == "ground") keep.push_back(c);
  f.root.children = keep;
  loadScene(f);
  currentFile.clear();
}

bool App::loadSceneFile(const std::string& path, std::string& err) {
  std::string text;
  if (!readFile(path, text)) { err = "Cannot read " + path; return false; }
  try { loadScene(sceneFromJson(text)); } catch (const std::exception& e) { err = e.what(); return false; }
  currentFile = path;
  return true;
}

std::string App::serialize() {
  CameraState cs = editCam.save();
  return sceneToJson(root, reg, world, &cs);
}

bool App::saveSceneFile(const std::string& path, std::string& err) {
  if (!writeFile(path, serialize())) { err = "Cannot write " + path; return false; }
  currentFile = path;
  return true;
}

void App::autosave() { writeFile((std::filesystem::path(dataDir()) / "autosave.4dscene.json").string(), serialize()); }

void App::rebuild() {
  version_++;
  lastChange_ = now;
  if (!world.empty() && !findNode(root, world)) world.clear();
  ev = std::make_unique<SceneEvaluator>(root, reg, world);
}

void App::commit() {
  rebuild();
  std::string s = snapshot();
  if (s != committed_) {
    history_.push_back(committed_);
    if (history_.size() > 200) history_.erase(history_.begin());
    future_.clear();
    committed_ = s;
    autosave();
  }
}

void App::undo() {
  if (history_.empty()) return;
  future_.push_back(committed_);
  committed_ = history_.back();
  history_.pop_back();
  restore(committed_);
  selection.erase(std::remove_if(selection.begin(), selection.end(), [&](auto& id) { return !findNode(root, id); }), selection.end());
  rebuild();
}
void App::redo() {
  if (future_.empty()) return;
  history_.push_back(committed_);
  committed_ = future_.back();
  future_.pop_back();
  restore(committed_);
  rebuild();
}

// ============================================================ selection & tree
Node* App::selected() { return selection.empty() ? nullptr : findNode(root, selection.back()); }

std::unordered_set<std::string> App::highlightSet() {
  std::unordered_set<std::string> s;
  if (mode != Mode::Edit) return s;
  for (auto& id : selection)
    if (Node* n = findNode(root, id)) walk(*n, [&](Node& m, Node*, int) { s.insert(m.id); });
  return s;
}

void App::deleteSelection() {
  for (auto& id : selection) if (id != root.id) removeNode(root, id);
  selection.clear();
  commit();
}

void App::duplicateSelection() {
  std::vector<std::string> out;
  for (auto& id : selection) {
    Node* p = findParent(root, id);
    Node* n = findNode(root, id);
    if (!p || !n) continue;
    Node c = cloneFresh(*n);
    c.name += " copy";
    size_t i = 0;
    while (i < p->children.size() && p->children[i].id != id) i++;
    out.push_back(c.id);
    p->children.insert(p->children.begin() + i + 1, std::move(c));
  }
  selection = out;
  commit();
  if (!out.empty()) startModal(Modal::Grab);
}

void App::groupSelection(BoolOp op) {
  std::vector<std::string> top;
  for (auto& id : selection) {
    if (id == root.id || !findNode(root, id)) continue;
    bool covered = false;
    for (auto& o : selection) if (o != id && isAncestor(root, o, id)) covered = true;
    if (!covered) top.push_back(id);
  }
  if (top.empty()) return;
  Node* parent = findParent(root, top[0]);
  std::string parentId = parent->id;
  size_t idx = 0;
  while (idx < parent->children.size() && parent->children[idx].id != top[0]) idx++;
  std::vector<Node> kids;
  for (auto& id : top) if (auto n = removeNode(root, id)) kids.push_back(std::move(*n));
  Node g = makeGroup(op, std::move(kids));
  std::string gid = g.id;
  Node* p2 = findNode(root, parentId);
  p2->children.insert(p2->children.begin() + std::min(idx, p2->children.size()), std::move(g));
  selection = {gid};
  commit();
}

void App::ungroup(const std::string& id) {
  Node* p = findParent(root, id);
  if (!p) return;
  size_t i = 0;
  while (i < p->children.size() && p->children[i].id != id) i++;
  if (p->children[i].isLeaf()) return;
  Node g = std::move(p->children[i]);
  p->children.erase(p->children.begin() + i);
  selection.clear();
  for (auto& c : g.children) selection.push_back(c.id);
  p->children.insert(p->children.begin() + i, g.children.begin(), g.children.end());
  commit();
}

void App::reparent(const std::string& id, const std::string& targetId, Drop where) {
  if (id == targetId || id == root.id || isAncestor(root, id, targetId)) return;
  if (!findNode(root, targetId)) return;
  auto n = removeNode(root, id);
  if (!n) return;
  Node* target = findNode(root, targetId);
  if (where == Drop::Into && !target->isLeaf()) target->children.push_back(std::move(*n));
  else {
    Node* p = findParent(root, targetId);
    if (!p) { root.children.push_back(std::move(*n)); commit(); return; }
    size_t i = 0;
    while (i < p->children.size() && p->children[i].id != targetId) i++;
    if (where == Drop::Wrap) {
      Node t = std::move(p->children[i]);
      p->children[i] = makeGroup(BoolOp::Union, {std::move(t), std::move(*n)});
    } else p->children.insert(p->children.begin() + (where == Drop::After ? i + 1 : i), std::move(*n));
  }
  commit();
}

void App::setWorld(const std::string& id) {
  world = id;
  geoEdit.valid = geoPlayer.valid = false;
  bool wasGeo = geo;
  geo = false;
  commit();
  if (wasGeo && !world.empty()) setGeo(true);
}

void App::attachToWorld(const std::string& id) {
  Node* n = findNode(root, id);
  if (!n || world.empty() || id == world || isAncestor(root, id, world)) return;
  if (nodeDim(*n, reg) > 3) { toast("Only 3D (or lower) objects can be attached to a world surface", true); return; }
  auto a = makeAnchor(*ev, world, nodeWorldPos(id), currentView().frame);
  n->xf.pos = a.pos;
  n->anchor = a.anchor;
  if (findParent(root, id) != &root) {
    auto m = removeNode(root, id);
    root.children.push_back(std::move(*m));
  }
  commit();
}

void App::detach(const std::string& id) {
  if (Node* n = findNode(root, id)) { n->anchor.reset(); commit(); }
}

V4 App::nodeWorldPos(const std::string& id) {
  auto path = pathTo(root, id);
  if (path.empty()) return {0, 0, 0, 0};
  V4 p = path.back()->xf.pos;
  for (int i = (int)path.size() - 2; i >= 0; i--) p = path[i]->xf.pos + mul(nodeRotation(*path[i]), p) * path[i]->xf.scale;
  return p;
}

M4 App::nodeWorldFrame(const std::string& id) {
  M4 r = identity();
  for (Node* n : pathTo(root, id)) r = mul(r, nodeRotation(*n));
  return r;
}

// ============================================================ modes & views
void App::setMode(Mode m) {
  if (m == mode) return;
  cancelModal();
  drag_ = Drag::None;
  if (m == Mode::Explore) {
    // the player always starts where the editor camera currently is
    if (geo && geoEdit.valid) { geoPlayer = geoEdit; geoPlayer.distance = 0; geoPlayer.valid = true; }
    else { playerCam.setFromView(editCam.view(), 3); playerPlaced = true; }
    wantCapture = true;
  } else wantRelease = true;
  mode = m;
  lastChange_ = now;
}

void App::setGeo(bool g) {
  if (g == geo) return;
  if (g && world.empty()) { toast("Choose a world object first (World menu or Properties > Use as world)", true); return; }
  cancelModal();
  drag_ = Drag::None;
  if (g) {
    auto f = field();
    // the (first-person) geodesic editor camera starts where the 4D camera is, on the surface
    geoEdit.initFrom(f, mode == Mode::Explore ? playerCam.view() : editCam.view(), 0);
    if (!geoEdit.valid) { toast("Could not place the camera on the world surface", true); return; }
    if (mode == Mode::Explore || !geoPlayer.valid) { geoPlayer = geoEdit; }
  } else {
    if (geoPlayer.valid) { playerCam.setFromView({geoPlayer.pivot, geoPlayer.frame}, 3); playerPlaced = true; }
    if (geoEdit.valid && mode == Mode::Edit) {
      // keep looking from the same place
      editCam.setFromView({geoEdit.pivot, geoEdit.frame}, 4);
      editCam.pivotSet = false;
    }
  }
  geo = g;
  lastChange_ = now;
}

View App::currentView() {
  if (geo && geoEdit.valid) {
    if (mode == Mode::Explore && geoPlayer.valid) return {geoPlayer.pivot, geoPlayer.frame};
    return {geoEdit.pivot, geoEdit.frame};
  }
  return mode == Mode::Explore ? playerCam.view() : editCam.view();
}

View App::playerView() {
  if (geo && geoPlayer.valid) return {geoPlayer.pivot, geoPlayer.frame};
  return playerCam.view();
}

V4 App::screenRay(double x, double y, const View& v) const {
  double sx = ((x - vpX) / vpW) * 2 - 1, sy = 1 - ((y - vpY) / vpH) * 2;
  return rayDir(v, sx * (vpW / vpH), sy, tanHalf());
}

bool App::project(const V4& p, const View& v, double& sx, double& sy) const {
  V4 r = p - v.eye;
  double z = dot(r, col(v.frame, 2));
  if (z < 1e-3) return false;
  double x = dot(r, col(v.frame, 0)) / (z * tanHalf()), y = dot(r, col(v.frame, 1)) / (z * tanHalf());
  sx = vpX + (x / (vpW / vpH) + 1) * 0.5 * vpW;
  sy = vpY + (1 - y) * 0.5 * vpH;
  return true;
}

std::optional<RayHit> App::pick(double x, double y) {
  View v = currentView();
  return castRay(*ev, geo, v.eye, screenRay(x, y, v), quality.geoMaxStep, v.frame);
}

void App::focusSelection() {
  Node* n = selected();
  if (!n) return;
  if (!geo) {
    editCam.target = nodeWorldPos(n->id);
    editCam.distance = std::max(2.0, 4 * n->xf.scale);
    editCam.pivotSet = false;
  } else if (n->anchor && n->anchor->world == world) {
    auto r = shoot(field(), n->xf.pos, n->anchor->basis, {0, 0, -1}, std::max(2.0, 4 * n->xf.scale), quality.geoMaxStep * 0.5);
    geoEdit.pivot = r.pos;
    geoEdit.frame = r.frame;
  }
}

void App::standardView(int which) {
  editCam.reset4();
  editCam.pivotSet = false;
  if (which == 0) { editCam.yaw = 0; editCam.pitch = 0; }
  if (which == 1) { editCam.yaw = -PI / 2; editCam.pitch = 0; }
  if (which == 2) { editCam.yaw = 0; editCam.pitch = -PI / 2 + 0.001; }
  if (which == 3) { editCam.yaw = 0; editCam.pitch = 0; editCam.rotate4(2, 3, PI / 2); }
}

std::array<float, 3> App::worldColor() {
  std::array<float, 3> c{0.35f, 0.4f, 0.55f};
  Node* w = world.empty() ? nullptr : findNode(root, world);
  if (!w) return c;
  double s[3] = {0, 0, 0};
  int n = 0;
  walk(*w, [&](Node& m, Node*, int) { if (m.isLeaf() && !m.hidden) { for (int i = 0; i < 3; i++) s[i] += m.color[i]; n++; } });
  if (n) for (int i = 0; i < 3; i++) c[i] = (float)(s[i] / n);
  return c;
}

// ============================================================ placement
int App::itemDim(const std::string& id) {
  const LibraryObject* o = lib.find(id);
  return o ? nodeDim(o->node, reg) : 0;
}

bool App::canPlace(const std::string& id, std::string* reason) {
  if (mode != Mode::Edit) { if (reason) *reason = "Switch to Edit mode to place objects"; return false; }
  if (geo && itemDim(id) > 3) { if (reason) *reason = "Geodesic view: only 3D (or lower) objects can be placed on the world surface"; return false; }
  return true;
}

void App::placeItem(const std::string& id, std::optional<std::pair<double, double>> screen) {
  std::string why;
  if (!canPlace(id, &why)) { toast(why, true); return; }
  const LibraryObject* o = lib.find(id);
  if (!o) return;
  Node node = cloneFresh(o->node);
  node.name = o->name;
  if (id.rfind("builtin-", 0) == 0) {  // give built-in shapes a fresh colour
    static int hueCounter = 0;
    auto c = hsl(std::fmod(0.13 + 0.618 * hueCounter++, 1.0), 0.55, 0.6);
    walk(node, [&](Node& m, Node*, int) { m.color = c; });
  }
  View v = currentView();
  V4 dir = col(v.frame, 2);
  std::optional<RayHit> hit;
  if (screen) {
    dir = screenRay(screen->first, screen->second, v);
    hit = castRay(*ev, geo, v.eye, dir, quality.geoMaxStep, v.frame);
    if (hit && hit->t > 40) hit.reset();
  }
  if (geo) {
    auto f = field();
    double dist = hit ? std::max(0.5, hit->t - 1) : 3;
    std::array<double, 3> local{dot(dir, col(v.frame, 0)), dot(dir, col(v.frame, 1)), dot(dir, col(v.frame, 2))};
    auto r = shoot(f, v.eye, v.frame, local, dist, quality.geoMaxStep * 0.5);
    auto a = makeAnchor(*ev, world, r.pos, r.frame);
    node.xf.pos = a.pos;
    node.anchor = a.anchor;
    walk(node, [](Node& m, Node*, int) { if (m.isLeaf()) m.thickness = std::max(m.thickness, 0.4); });
  } else if (hit) {
    const double e = 1e-4;
    double d0 = ev->evalFull(hit->pos).d;
    V4 g;
    for (int i = 0; i < 4; i++) { V4 q = hit->pos; q[i] += e; g[i] = ev->evalFull(q).d - d0; }
    node.xf.pos = hit->pos + normalize(g) * 1.0;
  } else {
    node.xf.pos = screen ? v.eye + dir * editCam.distance : editCam.target;
  }
  std::string nid = node.id;
  root.children.push_back(std::move(node));
  selection = {nid};
  commit();
}

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
    } else {
      double s;
      M4 pr = parentWorldRotation(id, &s);
      n->xf.pos = n->xf.pos + mul(transpose(pr), delta) * (1 / s);
    }
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
    } else n->xf.rot = orthonormalize(mul(local, n->xf.rot));
  }
}

/** Object axis projected into the visible 3D slice (zero if it points straight out of it). */
V4 App::gizmoAxisDir(const std::string& id, int axis, const View& v) {
  Node* n = findNode(root, id);
  if (!n) return {0, 0, 0, 0};
  V4 a = col(nodeWorldFrame(id), axis);
  V4 out = (geo && n->anchor) ? col(n->anchor->basis, 3) : col(v.frame, 3);
  V4 d = reject(a, normalize(out));
  return length(d) > 0.2 ? normalize(d) : V4{0, 0, 0, 0};
}

static double segDist(double px, double py, double ax, double ay, double bx, double by) {
  double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
  double t = l2 > 0 ? clampd(((px - ax) * vx + (py - ay) * vy) / l2, 0, 1) : 0;
  return std::hypot(px - ax - t * vx, py - ay - t * vy);
}

Gizmo App::gizmo() {
  Gizmo g;
  Node* n = selected();
  if (mode != Mode::Edit || tool == Tool::Select || !n || n->id == root.id || modal != Modal::None) return g;
  View v = currentView();
  V4 c = nodeWorldPos(n->id);
  if (!project(c, v, g.cx, g.cy)) return g;
  double depth = std::max(0.3, dot(c - v.eye, col(v.frame, 2)));
  g.worldLen = 95.0 * 2 * depth * tanHalf() / vpH;
  g.visible = true;
  std::vector<int> vis;
  for (int a = 0; a < 4; a++) {
    auto& ax = g.axes[a];
    ax.axis = a;
    ax.dir = gizmoAxisDir(n->id, a, v);
    if (length(ax.dir) < 0.5) continue;
    if (!project(c + ax.dir * g.worldLen, v, ax.ex, ax.ey)) continue;
    if (std::hypot(ax.ex - g.cx, ax.ey - g.cy) < 8) continue;  // pointing at the camera
    ax.visible = true;
    vis.push_back(a);
  }
  if (tool == Tool::Rotate) {
    double r = g.worldLen * 0.85;
    for (size_t i = 0; i < vis.size(); i++)
      for (size_t j = i + 1; j < vis.size(); j++) {
        Gizmo::Ring ring;
        ring.a = vis[i]; ring.b = vis[j];
        ring.u = g.axes[vis[i]].dir;
        V4 v2 = reject(g.axes[vis[j]].dir, ring.u);
        if (length(v2) < 0.2) continue;
        ring.v = normalize(v2);
        bool ok = true;
        for (int k = 0; k <= 64 && ok; k++) {
          double t = k / 64.0 * 2 * PI, sx, sy;
          ok = project(c + (ring.u * std::cos(t) + ring.v * std::sin(t)) * r, v, sx, sy);
          ring.pts.push_back({sx, sy});
        }
        if (!ok) continue;
        double ux, uy, wx, wy;
        project(c + ring.u * r, v, ux, uy);
        project(c + ring.v * r, v, wx, wy);
        double cr = (ux - g.cx) * (wy - g.cy) - (uy - g.cy) * (wx - g.cx);
        ring.orient = cr >= 0 ? 1 : -1;
        g.rings.push_back(ring);
      }
  }
  // hover
  if (gizmoHandle_ >= 0) g.hot = gizmoHandle_;
  else {
    double best = 9;
    if (tool == Tool::Move) {
      if (std::hypot(lastMx_ - g.cx, lastMy_ - g.cy) < 10) { g.hot = 4; best = 0; }
      for (int a = 0; a < 4; a++) {
        if (!g.axes[a].visible) continue;
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
    if (Node* n = findNode(root, id); n && id != root.id) orig_.push_back({id, {n->xf, n->anchor}});
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
  switch (modal) {
    case Modal::Grab: return "Grab" + (a.empty() ? std::string(" in screen plane") : " along object " + a) + " | X/Y/Z/W: object axis | click/Enter confirm, Esc/RMB cancel";
    case Modal::Rotate: return "Rotate" + (a.empty() ? std::string(" in screen plane") : a.size() == 1 ? " about object " + a : " in object plane " + a) +
                               " | X/Y/Z: about axis, two keys (e.g. X W): plane | click/Enter confirm, Esc/RMB cancel";
    case Modal::Scale: return "Scale | click/Enter confirm, Esc/RMB cancel";
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
    for (auto& [id, o] : orig_) {
      V4 c = nodeWorldPos(id);
      double depth = std::max(0.5, std::abs(dot(c - v.eye, fwd)));
      double k = 2 * depth * tanHalf() / vpH;
      V4 delta = right * (dx * k) - up * (dy * k);
      if (modalAxes.size() == 1) {
        V4 axis = gizmoAxisDir(id, modalAxes[0], v);
        double sx0, sy0, sx1, sy1;
        if (length(axis) > 0.5 && project(c, v, sx0, sy0) && project(c + axis, v, sx1, sy1)) {
          double ax = sx1 - sx0, ay = sy1 - sy0, l2 = ax * ax + ay * ay;
          delta = l2 > 1e-6 ? axis * ((dx * ax + dy * ay) / l2) : axis * (dx * k);
        } else delta = {0, 0, 0, 0};
      }
      translateNodes({id}, delta);
    }
  } else if (modal == Modal::Rotate) {
    double ang = dx * 0.01;
    for (auto& id : ids) {
      M4 q;
      if (modalAxes.empty()) q = rotationInPlane(right, up, -ang);
      else {
        int a, b;
        if (modalAxes.size() >= 2) { a = modalAxes[0]; b = modalAxes[1]; }
        else { int x = modalAxes[0]; a = x == 0 ? 1 : 0; b = x == 2 ? 1 : x == 3 ? 1 : 2; if (x == 1) { a = 2; b = 0; } }
        V4 u = gizmoAxisDir(id, a, v), w = gizmoAxisDir(id, b, v);
        w = reject(w, u);
        if (length(u) < 0.5 || length(w) < 0.2) continue;
        q = rotationInPlane(u, normalize(w), ang);
      }
      rotateNodes({id}, q);
    }
  } else if (modal == Modal::Scale) {
    for (auto& [id, o] : orig_) if (Node* n = findNode(root, id)) n->xf.scale = std::max(0.01, o.xf.scale * std::exp(dx * 0.005));
  }
  rebuild();
}

// ============================================================ input
void App::handleKey(int key, bool shift, bool ctrl) {
  if (ctrl && key == GLFW_KEY_Z) { shift ? redo() : undo(); return; }
  if (ctrl && key == GLFW_KEY_Y) { redo(); return; }
  if (ctrl && key == GLFW_KEY_G) { groupSelection(BoolOp::Union); return; }
  if (key == GLFW_KEY_TAB) { setMode(mode == Mode::Edit ? Mode::Explore : Mode::Edit); return; }
  if (mode == Mode::Explore) {
    if (key == GLFW_KEY_ESCAPE) { if (mouseCaptured) wantRelease = true; else setMode(Mode::Edit); }
    if (key == GLFW_KEY_R && !geo) playerCam.reset4();
    return;
  }
  if (drag_ == Drag::Fly) return;  // WASD are flying keys while RMB is held
  if (modal != Modal::None) {
    int ax = key == GLFW_KEY_X ? 0 : key == GLFW_KEY_Y ? 1 : key == GLFW_KEY_Z ? 2 : key == GLFW_KEY_W ? 3 : -1;
    if (ax >= 0) {
      if (modal == Modal::Rotate) modalAxes = (modalAxes.size() >= 2 || (modalAxes.size() == 1 && modalAxes[0] == ax)) ? std::vector<int>{ax} : (modalAxes.push_back(ax), modalAxes);
      else modalAxes = (modalAxes.size() == 1 && modalAxes[0] == ax) ? std::vector<int>{} : std::vector<int>{ax};
      applyModal();
    } else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) confirmModal();
    else if (key == GLFW_KEY_ESCAPE) cancelModal();
    return;
  }
  if (ctrl) return;
  switch (key) {
    case GLFW_KEY_V: setGeo(!geo); break;
    case GLFW_KEY_Q: tool = Tool::Select; break;
    case GLFW_KEY_W: if (shift) editCam.reset4(); else tool = Tool::Move; break;
    case GLFW_KEY_E: tool = Tool::Rotate; break;
    case GLFW_KEY_G: startModal(Modal::Grab); break;
    case GLFW_KEY_R: startModal(Modal::Rotate); break;
    case GLFW_KEY_S: startModal(Modal::Scale); break;
    case GLFW_KEY_X: case GLFW_KEY_DELETE: if (!selection.empty()) deleteSelection(); break;
    case GLFW_KEY_D: if (shift) duplicateSelection(); break;
    case GLFW_KEY_F: case GLFW_KEY_PERIOD: case GLFW_KEY_KP_DECIMAL: focusSelection(); break;
    case GLFW_KEY_ESCAPE: selection.clear(); break;
    case GLFW_KEY_A: { selection.clear(); for (auto& c : root.children) selection.push_back(c.id); break; }
    case GLFW_KEY_1: case GLFW_KEY_KP_1: standardView(0); break;
    case GLFW_KEY_3: case GLFW_KEY_KP_3: standardView(1); break;
    case GLFW_KEY_7: case GLFW_KEY_KP_7: standardView(2); break;
    case GLFW_KEY_9: case GLFW_KEY_KP_9: standardView(3); break;
    default: break;
  }
}

void App::flyGeoEdit(const InputState& in, double dt) {
  // game-like editor camera in the Geodesic view: hold RMB, mouse looks, WASD/Space/Shift move, Q/E roll
  auto f = field();
  if (in.dx != 0 || in.dy != 0) geoEdit.look(in.dx, in.dy);
  if (!in.keyDown) return;
  auto K = [&](int k) { return in.keyDown(k) ? 1.0 : 0.0; };
  double sp = editFlySpeed * dt;
  double mr = K(GLFW_KEY_D) - K(GLFW_KEY_A), mf = K(GLFW_KEY_W) - K(GLFW_KEY_S);
  double mu = K(GLFW_KEY_SPACE) - std::max(K(GLFW_KEY_LEFT_SHIFT), K(GLFW_KEY_RIGHT_SHIFT));
  double roll = K(GLFW_KEY_E) - K(GLFW_KEY_Q);
  if (roll != 0) geoEdit.roll(roll * 1.2 * dt);
  if (mr != 0 || mf != 0 || mu != 0) geoEdit.move(f, {mr * sp, mu * sp, mf * sp}, quality.geoMaxStep * 0.5, nullptr);
}

void App::handleInput(const InputState& in, double dt) {
  lastMx_ = in.mx;
  lastMy_ = in.my;
  if (mode == Mode::Explore) { updateExplore(in, dt); return; }
  bool g = geo && geoEdit.valid;
  if (modal != Modal::None) {
    modalX_ = in.mx; modalY_ = in.my;
    if (in.dx != 0 || in.dy != 0) applyModal();
    if (in.pressed[0]) confirmModal();
    else if (in.pressed[1]) cancelModal();
    return;
  }
  // --- start drags ---
  for (int b = 0; b < 3; b++) {
    if (!in.pressed[b] || !in.vpHovered || drag_ != Drag::None) continue;
    pressX_ = in.mx; pressY_ = in.my; dragMoved_ = false; dragButton_ = b;
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
      captureOrig();
      modalView_ = currentView();
      continue;
    }
    drag_ = Drag::Click;
    if (tool == Tool::Move) {
      auto hit = pick(in.mx, in.my);
      if (hit && hit->node) {
        std::string id = hit->node->id;
        bool keep = false;
        for (auto& s : selection) if (s == id || isAncestor(root, s, id)) keep = true;
        if (!keep) selection = {id};
        captureOrig();
        modalView_ = currentView();
        drag_ = Drag::FreeMove;
      }
    }
  }
  if (in.wheel != 0 && in.vpHovered) {
    if (g) editFlySpeed = clampd(editFlySpeed * std::exp(in.wheel * 0.15), 0.2, 30);
    else editCam.zoom(-in.wheel);
  }

  // --- continue drags ---
  if (drag_ != Drag::None && dragButton_ >= 0 && in.down[dragButton_]) {
    if (std::abs(in.mx - pressX_) + std::abs(in.my - pressY_) > 3) dragMoved_ = true;
    const View& v = modalView_;
    switch (drag_) {
      case Drag::Orbit: if (dragMoved_) editCam.orbit(in.dx, in.dy); break;
      case Drag::Pan: editCam.pan(in.dx, in.dy); break;
      case Drag::Zoom: editCam.zoom(in.dy * 0.05); break;
      case Drag::Rot4: editCam.rotate4(0, 3, in.dx * 0.006); editCam.rotate4(1, 3, -in.dy * 0.006); break;
      case Drag::Fly: flyGeoEdit(in, dt); break;
      case Drag::FreeMove: {
        if (in.dx == 0 && in.dy == 0) break;
        V4 right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2);
        for (auto& [id, o] : orig_) {
          double depth = std::max(0.5, std::abs(dot(nodeWorldPos(id) - v.eye, fwd)));
          double k = 2 * depth * tanHalf() / vpH;
          translateNodes({id}, in.shift ? fwd * (-in.dy * k * 2) : right * (in.dx * k) - up * (in.dy * k));
        }
        rebuild();
        break;
      }
      case Drag::Gizmo: {
        if (in.dx == 0 && in.dy == 0) break;
        Gizmo& gz = gizmoAtPress_;
        std::vector<std::string> ids;
        for (auto& [id, o] : orig_) ids.push_back(id);
        if (gizmoHandle_ <= 3) {  // arrow: move along the (projected) object axis
          auto& ax = gz.axes[gizmoHandle_];
          double sx = ax.ex - gz.cx, sy = ax.ey - gz.cy, l2 = sx * sx + sy * sy;
          if (l2 > 1) translateNodes(ids, ax.dir * ((in.dx * sx + in.dy * sy) / l2 * gz.worldLen));
        } else if (gizmoHandle_ == 4) {  // centre: free move in the screen plane (Shift: along depth)
          double k = gz.worldLen / 95.0;
          translateNodes(ids, in.shift ? col(v.frame, 2) * (-in.dy * k * 2) : col(v.frame, 0) * (in.dx * k) - col(v.frame, 1) * (in.dy * k));
        } else {  // ring: rotate in the plane of two object axes, by the angle swept on screen
          auto& ring = gz.rings[gizmoHandle_ - 10];
          double a0 = std::atan2(in.my - in.dy - gz.cy, in.mx - in.dx - gz.cx), a1 = std::atan2(in.my - gz.cy, in.mx - gz.cx);
          double da = a1 - a0;
          if (da > PI) da -= 2 * PI;
          if (da < -PI) da += 2 * PI;
          rotateNodes(ids, rotationInPlane(ring.u, ring.v, da * ring.orient));
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
    if (drag_ == Drag::Orbit && dragButton_ == 2 && !dragMoved_ && !g) {
      if (auto hit = pick(in.mx, in.my)) { editCam.setPivot(hit->pos); toast("Centre of rotation set"); }
      else { editCam.pivotSet = false; toast("Centre of rotation reset to the view centre"); }
    }
    bool click = !dragMoved_ && dragButton_ == 0 && (drag_ == Drag::Click || drag_ == Drag::FreeMove);
    if (click && in.vpHovered) {
      auto hit = pick(in.mx, in.my);
      std::string id = hit && hit->node ? hit->node->id : "";
      if (!id.empty() && !in.shift && selection.size() == 1) {
        if (selection[0] == id) {
          Node* p = findParent(root, id);
          if (p && p->id != root.id) id = p->id;
        } else if (selection[0] != root.id && isAncestor(root, selection[0], id)) id = selection[0];
      }
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
  if (mouseCaptured && (in.dx != 0 || in.dy != 0)) {
    if (geo && geoPlayer.valid) geoPlayer.look(in.dx, in.dy);
    else playerCam.look(in.dx, in.dy);
  }
  if (in.pressed[0] && in.vpHovered && !mouseCaptured) wantCapture = true;
  if (in.wheel != 0) moveSpeed = clampd(moveSpeed * std::exp(in.wheel * 0.1), 0.2, 30);
  if (!in.keysEnabled || !in.keyDown) return;
  auto K = [&](int k) { return in.keyDown(k) ? 1.0 : 0.0; };
  double sp = moveSpeed * dt, rot = 1.2 * dt;
  double mr = K(GLFW_KEY_D) - K(GLFW_KEY_A), mf = K(GLFW_KEY_W) - K(GLFW_KEY_S);
  double mu = K(GLFW_KEY_SPACE) - std::max(K(GLFW_KEY_LEFT_SHIFT), K(GLFW_KEY_RIGHT_SHIFT));
  double ma = K(GLFW_KEY_E) - K(GLFW_KEY_Q);
  double r = avatarRadius;
  if (geo && geoPlayer.valid) {
    auto f = field();
    if (ma != 0) geoPlayer.roll(ma * rot);
    if (mr != 0 || mf != 0 || mu != 0)
      geoPlayer.move(f, {mr * sp, mu * sp, mf * sp}, quality.geoMaxStep * 0.5,
                     collide ? std::function<bool(const V4&)>([&](const V4& to) { return ev->evalGeo(to).d < r; }) : nullptr);
  } else {
    double zx = K(GLFW_KEY_Z) - K(GLFW_KEY_X), cv = K(GLFW_KEY_C) - K(GLFW_KEY_V);
    if (zx != 0) playerCam.rotate4AroundEye(2, 3, zx * rot);
    if (cv != 0) playerCam.rotate4AroundEye(0, 3, cv * rot);
    if (mr != 0 || mf != 0 || mu != 0 || ma != 0)
      playerCam.move({mr * sp, mu * sp, mf * sp, ma * sp},
                     collide ? std::function<bool(const V4&)>([&](const V4& to) { return ev->evalFull(to).d < r; }) : nullptr);
  }
}

// ============================================================ rendering
View App::thumbnailView() {
  View cur = currentView();
  auto bounds = computeBounds(root, reg);
  V4 c{0, 0, 0, 0};
  double R = 5;
  auto it = bounds.find(world);
  if (it != bounds.end() && std::isfinite(it->second.r)) { c = nodeWorldPos(world); R = std::max(0.5, it->second.r); }
  else c = cur.eye - col(cur.frame, 3) * 2;
  V4 out = normalize(cur.eye - c);
  if (length(out) < 0.5) out = col(cur.frame, 3);
  V4 right = normalize(reject(col(cur.frame, 0), out));
  V4 fwd = normalize(reject(reject(col(cur.frame, 2), out), right));
  V4 ana = normalize(reject(reject(reject(col(cur.frame, 1), out), right), fwd));
  if (length(ana) < 0.5) ana = completeBasis({out, right, fwd})[3];
  FullCamera cam;
  cam.rot4 = fromCols(right, out, fwd, ana);
  cam.yaw = thumbYaw;
  cam.pitch = thumbPitch;
  cam.target = c;
  cam.distance = R * 3.2 * thumbZoom;
  return cam.view();
}

void App::renderFrame() {
  if (!renderer) return;
  View v = currentView();
  bool interacting = drag_ != Drag::None && drag_ != Drag::Click;
  bool moving = (now - lastChange_) < 0.25 || interacting || modal != Modal::None || mode == Mode::Explore;
  float scale = quality.resolutionScale * (moving ? quality.motionScale : 1.0f);
  std::ostringstream key;
  key.precision(9);
  for (double x : v.eye) key << x << ',';
  for (double x : v.frame) key << x << ',';
  key << version_ << '|' << geo << (int)mode << '|' << vpW << 'x' << vpH << '|' << scale << '|' << fov << '|' << quality.maxSteps
      << quality.shadows << quality.ao << quality.geoMaxStep << quality.geoProjectIters << showPlayerMarker << showOwnBody << '|';
  for (auto& s : selection) key << s << ';';

  FrameParams fp;
  fp.view = v;
  fp.geo = geo && geoEdit.valid;
  fp.grid = mode == Mode::Edit;
  fp.fovDeg = fov;
  fp.quality = &quality;
  fp.light = light;
  fp.skyCol = worldColor();
  // the player's body: always in Explore (you can see yourself / your shadow, e.g. light
  // wrapping around a closed world); in Edit it marks where the player is
  float r = (float)avatarRadius;
  if (mode == Mode::Explore && showOwnBody) {
    fp.marker = v.eye;
    fp.markerFwd = col(v.frame, 2);
    fp.markerCol = {0.95f, 0.8f, 0.2f, r};
    fp.markerSkip = r * 3;  // the primary ray starts inside the body
  } else if (showPlayerMarker && (fp.geo ? geoPlayer.valid : playerPlaced)) {
    View pv = playerView();
    V4 ana = col(v.frame, 3);
    fp.marker = fp.geo ? pv.eye : pv.eye - ana * dot(pv.eye - v.eye, ana);
    V4 f = col(pv.frame, 2);
    fp.markerFwd = fp.geo ? f : normalize(reject(f, ana));
    if (length(fp.marker - v.eye) > 0.8) fp.markerCol = {1.0f, 0.85f, 0.2f, r};
    for (double x : pv.eye) key << x << ',';
  }
  std::string k = key.str();
  if (k != lastKey_) {
    if (k.substr(0, k.find('|')) != lastKey_.substr(0, lastKey_.find('|'))) lastChange_ = now;
    lastKey_ = k;
    fp.scale = scale;
    renderer->render(mainRT, (int)vpW, (int)vpH, root, reg, world, highlightSet(), fp);
  }
  if (fp.geo && showThumbnail) {
    View tv = thumbnailView();
    FrameParams tp;
    tp.view = tv;
    tp.geo = false;
    tp.grid = false;
    tp.fovDeg = 40;
    tp.quality = &quality;
    tp.light = light;
    tp.marker = v.eye;
    tp.markerFwd = col(v.frame, 2);
    double R = 3;
    auto bounds = computeBounds(root, reg);
    if (auto it = bounds.find(world); it != bounds.end() && std::isfinite(it->second.r)) R = it->second.r;
    tp.markerCol = {1.0f, 0.85f, 0.2f, (float)std::max(0.08, R * 0.07)};
    tp.scale = 1;
    std::ostringstream tk;
    tk << k << thumbYaw << thumbPitch << thumbZoom << thumbW << thumbH;
    if (tk.str() != lastThumbKey_) {
      lastThumbKey_ = tk.str();
      renderer->render(thumbRT, thumbW, thumbH, root, reg, world, {}, tp);
    }
  }
}

}  // namespace fd
