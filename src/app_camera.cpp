// App: camera objects, views and modes (Edit / Explore, Full 4D / Geodesic, curved spaces).
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

// ============================================================ cameras (scene objects)
View App::cameraView(const std::string& id, bool* geoCam, double* fov) {
  Node* n = findNode(root, id);
  View v;
  if (!n) return v;
  v.eye = nodeWorldPos(id);
  v.frame = space.curved() ? nodeWorldIso(id) : orthonormalize(nodeWorldFrame(id));
  if (geoCam) *geoCam = n->anchor && !world.empty() && n->anchor->world == world;
  if (fov) *fov = n->camFov;
  return v;
}

std::string App::addCamera() {
  View v = currentView();
  int count = 0;
  walk(root, [&](Node& m, Node*, int) { if (m.isCamera()) count++; });
  Node c = makeCamera("Camera " + std::to_string(count + 1), v.eye, v.frame, fov(), editCam.slider);
  if (space.curved()) {  // the camera's isometry, as normal coordinates + rotation (root child)
    Transform w;
    cs::setFromIso(space, w, v.frame, 1.0);
    c.xf.pos = w.pos;
    c.xf.rot = w.rot;
  } else if (geo && geoEdit.valid && !world.empty()) {
    // a Geodesic camera: attached to the world surface (it then moves along it)
    c.xf.pos = geoEdit.pivot;
    c.xf.rot = identity();
    c.anchor = Anchor{world, geoEdit.frame, 1e4};
  }
  std::string id = c.id;
  root.children.push_back(std::move(c));
  if (activeCamera.empty() || !findNode(root, activeCamera)) activeCamera = id;
  selection = {id};
  commit();
  return id;
}

void App::lookThrough(const std::string& id) {
  Node* n = findNode(root, id);
  if (!n || !n->isCamera()) return;
  bool gcam = false;
  double f = 60;
  View v = cameraView(id, &gcam, &f);
  if (space.curved()) curvCam.iso = cs::renorm(space.k(), v.frame);
  else if (gcam) {
    if (!geo) setGeo(true);
    if (geo) { geoEdit.pivot = v.eye; geoEdit.frame = v.frame; geoEdit.valid = true; }
  } else {
    if (geo) setGeo(false);
    editCam.slider = n->camSlider;
    editCam.setFromView(v, std::max(1.0, editCam.distance));
    editCam.pivotSet = false;
  }
  (mode == Mode::Explore ? exploreSet.fov : editSet.fov) = f;
  lastChange_ = now;
}

void App::cameraToView(const std::string& id) {
  Node* n = findNode(root, id);
  if (!n || !n->isCamera()) return;
  View v = currentView();
  n->camFov = fov();
  if (geo && geoEdit.valid && !world.empty()) {
    n->xf.pos = geoEdit.pivot;
    n->xf.rot = identity();
    n->anchor = Anchor{world, geoEdit.frame, 1e4};
    if (findParent(root, id) != &root) {  // attached objects live at the scene root
      auto m = removeNode(root, id);
      root.children.push_back(std::move(*m));
    }
  } else {
    n->anchor.reset();
    n->camSlider = editCam.slider;
    Transform w{v.eye, v.frame, 1};
    if (space.curved()) cs::setFromIso(space, w, v.frame, 1.0);
    Node* p = findParent(root, id);
    auto pw = p ? worldTransform(p->id) : std::optional<Transform>(Transform{});
    Transform local = pw ? relativeTransform(*pw, w) : w;
    n->xf.pos = local.pos;
    n->xf.rot = local.rot;
  }
  commit();
}

std::string App::pickCamera(double x, double y) {
  // cameras have no shape: they are clicked on their drawn icon (its centre)
  View v = currentView();
  std::string best;
  double bd = 14;
  walk(root, [&](Node& n, Node*, int) {
    if (!n.isCamera() || n.hidden || isLocked(n.id)) return;
    double sx, sy;
    if (!project(nodeWorldPos(n.id), v, sx, sy)) return;
    double d = std::hypot(sx - x, sy - y);
    if (d < bd) { bd = d; best = n.id; }
  });
  return best;
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
  // every object keeps its place in the world: express it in the new parent's frame
  auto parentW = worldTransform(parentId);
  double parentScale = scaleAbove(parentId) * parent->xf.scale;
  std::vector<std::optional<Transform>> worldOf;
  for (auto& id : top) worldOf.push_back(worldTransform(id));
  std::vector<Node> kids;
  for (size_t k = 0; k < top.size(); k++)
    if (auto n = removeNode(root, top[k])) {
      if (parentW && worldOf[k]) n->xf = relativeTransform(*parentW, *worldOf[k]);
      kids.push_back(std::move(*n));
    }
  Node g = makeGroup(op, std::move(kids));
  centreGroup(g, parentScale);  // the group's origin is its first object (which ends up at 0,0,0,0)
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
  if (!p->children[i].isGroup()) return;
  double sAnc = scaleAbove(id);
  Node g = std::move(p->children[i]);
  p->children.erase(p->children.begin() + i);
  selection.clear();
  // back to the parent's coordinates, keeping every object where it is
  for (auto& c : g.children) {
    if (!g.anchor && !c.anchor) c.xf = composeLocal(g.xf, c.xf, sAnc);
    selection.push_back(c.id);
  }
  p->children.insert(p->children.begin() + i, g.children.begin(), g.children.end());
  commit();
}

void App::reparent(const std::string& id, const std::string& targetId, Drop where) {
  if (id == targetId || id == root.id || isAncestor(root, id, targetId)) return;
  if (!findNode(root, targetId)) return;
  auto W = worldTransform(id);  // the object stays where it is in the world
  auto n = removeNode(root, id);
  if (!n) return;
  auto into = [&](const std::string& parentId) {
    auto P = worldTransform(parentId);
    if (W && P) n->xf = relativeTransform(*P, *W);
  };
  Node* target = findNode(root, targetId);
  if (where == Drop::Into && target->isGroup()) { into(targetId); target->children.push_back(std::move(*n)); }
  else {
    Node* p = findParent(root, targetId);
    if (!p) { into(root.id); root.children.push_back(std::move(*n)); commit(); return; }
    into(p->id);
    size_t i = 0;
    while (i < p->children.size() && p->children[i].id != targetId) i++;
    if (where == Drop::Wrap) {
      Node t = std::move(p->children[i]);
      Node g = makeGroup(BoolOp::Union, {std::move(t), std::move(*n)});
      centreGroup(g, scaleAbove(p->id) * p->xf.scale);
      p->children[i] = std::move(g);
    } else p->children.insert(p->children.begin() + (where == Drop::After ? i + 1 : i), std::move(*n));
  }
  commit();
}

void App::setWorld(const std::string& id) {
  world = id;
  geoEdit.valid = false;
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

void App::recentreGroup(const std::string& id) {
  Node* g = findNode(root, id);
  if (!g || g->id == root.id) return;
  if (!centreGroup(*g, scaleAbove(id))) { toast("Cannot recentre this group (empty, or objects on a world surface)", true); return; }
  commit();
}

void App::detach(const std::string& id) {
  if (Node* n = findNode(root, id)) { n->anchor.reset(); commit(); }
}

M4 App::nodeWorldIso(const std::string& id, double* sAncOut) {
  M4 m = identity();
  double s = 1;
  for (Node* n : pathTo(root, id)) {
    m = mul(m, cs::nodeIso(space, n->xf, s));
    if (sAncOut) *sAncOut = s;
    s *= n->xf.scale;
  }
  return cs::renorm(space.k(), m);
}

V4 App::along(const V4& p, const V4& v) const {
  if (!space.curved()) return p + v;
  double k = space.k();
  V4 t = cs::tangent(k, p, v);
  double l = std::sqrt(std::max(cs::mdot(k, t, t), 0.0));
  if (l < 1e-12) return p;
  V4 q = p, d = t * (1.0 / l);
  cs::geodesic(space, q, d, l);
  return q;
}

std::optional<Transform> App::worldTransform(const std::string& id) {
  if (!space.curved()) return worldXf(root, id);
  Node* n = findNode(root, id);
  if (!n) return std::nullopt;
  double sAnc = 1;
  M4 W = nodeWorldIso(id, &sAnc);
  Transform t;
  cs::setFromIso(space, t, W, 1.0);
  t.scale = sAnc * n->xf.scale;
  return t;
}

Transform App::relativeTransform(const Transform& P, const Transform& W) {
  if (!space.curved()) return relativeXf(P, W);
  M4 L = mul(cs::inverse(space.k(), cs::nodeIso(space, P, 1.0)), cs::nodeIso(space, W, 1.0));
  Transform t;
  cs::setFromIso(space, t, L, P.scale);
  t.scale = W.scale / P.scale;
  return t;
}

Transform App::composeLocal(const Transform& parent, const Transform& child, double sAnc) {
  if (!space.curved()) return composeXf(parent, child);
  M4 W = mul(cs::nodeIso(space, parent, sAnc), cs::nodeIso(space, child, sAnc * parent.scale));
  Transform t;
  cs::setFromIso(space, t, W, sAnc);
  t.scale = parent.scale * child.scale;
  return t;
}

double App::scaleAbove(const std::string& id) {
  double s = 1;
  auto path = pathTo(root, id);
  for (size_t i = 0; i + 1 < path.size(); i++) s *= path[i]->xf.scale;
  return s;
}

bool App::centreGroup(Node& g, double sAnc) {
  if (!space.curved()) return centreGroupOnFirst(g);
  if (g.isLeaf() || g.children.empty()) return false;
  // the group's origin moves to its first child's (keeping its orientation); children compensate
  double sg = sAnc * g.xf.scale, k = space.k();
  V4 o = g.children[0].xf.pos;
  o[3] = 0;
  M4 Tm = cs::translation(space, o * sg);
  cs::setFromIso(space, g.xf, mul(cs::nodeIso(space, g.xf, sAnc), Tm), sAnc);
  M4 Ti = cs::inverse(k, Tm);
  for (auto& c : g.children) cs::setFromIso(space, c.xf, mul(Ti, cs::nodeIso(space, c.xf, sg)), sg);
  return true;
}

double App::viewDepth(const V4& p, const View& v) const {
  if (!space.curved()) return dot(p - v.eye, col(v.frame, 2));
  return cs::logO(space, cs::renormPoint(space.k(), mul(cs::inverse(space.k(), v.frame), p)))[2];
}

V4 App::lightWorldPos(const PointLight& l) const { return space.curved() ? cs::expO(space, l.pos) : l.pos; }

void App::setCurvatureRadius(double r) {
  if (!space.curved()) return;
  space.radius = clampd(r, 0.2, 1e4);
  curvCam.iso = cs::renorm(space.k(), curvCam.iso);
  rebuild();
}

V4 App::nodeWorldPos(const std::string& id) {
  if (space.curved()) return col(nodeWorldIso(id), 3);
  auto path = pathTo(root, id);
  if (path.empty()) return {0, 0, 0, 0};
  V4 p = path.back()->xf.pos;
  for (int i = (int)path.size() - 2; i >= 0; i--) p = path[i]->xf.pos + mul(nodeRotation(*path[i]), p) * path[i]->xf.scale;
  return p;
}

M4 App::nodeWorldFrame(const std::string& id) {
  if (space.curved()) return nodeWorldIso(id);  // columns 0-2: the object's axes at its position
  M4 r = identity();
  for (Node* n : pathTo(root, id)) r = mul(r, nodeRotation(*n));
  return r;
}

// ============================================================ modes & views
void App::setMode(Mode m) {
  if (m == mode) return;
  if (texMode.active && m == Mode::Explore) return;  // (texture editing mode: Edit only)
  cancelModal();
  drag_ = Drag::None;
  // one shared camera: switching modes keeps the exact position and orientation
  if (!geo && !space.curved()) {
    View v = editCam.view();
    double dist = editCam.distance;
    editCam.setFromView(v, dist);  // re-base yaw/pitch so first-person look starts level-free
    editCam.pivotSet = false;
  }
  if (m == Mode::Explore) wantCapture = true;
  else wantRelease = true;
  mode = m;
  lastChange_ = now;
}

void App::setGeo(bool g) {
  if (g == geo) return;
  if (g && texMode.active) return;  // (texture editing mode: Full 4D only)
  if (g && space.curved()) { toast("Curved scenes are already a 3D space (no world surface view)", true); return; }
  if (g && world.empty()) { toast("Choose a world object first (World menu or Properties > Use as world)", true); return; }
  cancelModal();
  drag_ = Drag::None;
  if (g) {
    geoEdit.initFrom(field(), editCam.view(), 0);  // stand where the 4D camera is, on the surface
    if (!geoEdit.valid) { toast("Could not place the camera on the world surface", true); return; }
  } else if (geoEdit.valid) {
    editCam.setFromView({geoEdit.pivot, geoEdit.frame}, 4);  // keep looking from the same place
    editCam.pivotSet = false;
  }
  geo = g;
  lastChange_ = now;
}

bool App::inCameraView() const {
  if (!viewThroughCamera || mode != Mode::Edit) return false;
  const Node* c = findNode(const_cast<Node&>(root), activeCamera);
  if (!c || !c->isCamera()) return false;
  return !c->anchor || (!world.empty() && c->anchor->world == world);  // a surface camera needs its world
}

double App::fov() const {
  if (inCameraView()) return findNode(const_cast<Node&>(root), activeCamera)->camFov;
  return mode == Mode::Explore ? exploreSet.fov : editSet.fov;
}

View App::currentView() {
  if (inCameraView()) return cameraView(activeCamera);
  if (space.curved()) return curvCam.view();
  if (geo && geoEdit.valid) return {geoEdit.pivot, geoEdit.frame};
  return editCam.view();
}

View App::playerView() { return currentView(); }

V4 App::screenRay(double x, double y, const View& v) const {
  double sx = ((x - vpX) / vpW) * 2 - 1, sy = 1 - ((y - vpY) / vpH) * 2;
  if (space.curved()) {  // a tangent direction at the eye (the frame is an isometry)
    V4 l = normalize(V4{sx * (vpW / vpH) * tanHalf(), sy * tanHalf(), 1, 0});
    return mul(v.frame, l);
  }
  return rayDir(v, sx * (vpW / vpH), sy, tanHalf());
}

bool App::project(const V4& p, const View& v, double& sx, double& sy) const {
  if (space.curved()) {
    // the pixel of p is the direction of the geodesic from the eye to it (how rays travel)
    V4 u = cs::logO(space, cs::renormPoint(space.k(), mul(cs::inverse(space.k(), v.frame), p)));
    if (u[2] < 1e-3) return false;
    double x = u[0] / (u[2] * tanHalf()), y = u[1] / (u[2] * tanHalf());
    sx = vpX + (x / (vpW / vpH) + 1) * 0.5 * vpW;
    sy = vpY + (1 - y) * 0.5 * vpH;
    return true;
  }
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
  auto hit = castRay(*ev, geo, v.eye, screenRay(x, y, v), quality.geoMaxStep, v.frame);
  if (hit && hit->node && isLocked(hit->node->id)) hit->node = nullptr;  // locked: not selectable
  return hit;
}

bool App::isLocked(const std::string& id) {
  for (Node* n : pathTo(root, id)) if (n->locked) return true;
  return false;
}

void App::focusSelection() {
  Node* n = selected();
  if (!n) return;
  if (space.curved()) { curvCam.focus(space, nodeWorldPos(n->id), std::max(2.0, 4 * n->xf.scale)); return; }
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
  if (space.curved()) { if (which < 3) curvCam.standard(space, which); return; }
  editCam.reset4();
  editCam.pivotSet = false;
  if (which == 0) { editCam.yaw = 0; editCam.pitch = 0; }
  if (which == 1) { editCam.yaw = -PI / 2; editCam.pitch = 0; }
  if (which == 2) { editCam.yaw = 0; editCam.pitch = -PI / 2 + 0.001; }
  if (which == 3) { editCam.yaw = 0; editCam.pitch = 0; editCam.rotate4(2, 3, PI / 2); }
}

std::array<float, 3> App::worldColor() {
  // colour of the world surface under the camera (e.g. a yellow plane gives a yellow sky)
  std::array<float, 3> c{0.35f, 0.4f, 0.55f};
  if (world.empty() || !ev || space.curved()) return c;
  Sample s = ev->worldSample(currentView().eye, world);
  if (s.d >= INF * 0.5) return c;
  for (int i = 0; i < 3; i++) c[i] = (float)s.color[i];
  return c;
}

}  // namespace fd
