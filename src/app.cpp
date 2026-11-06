#include "app.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "examples.hpp"

#include <nlohmann/json.hpp>

namespace fd {

static std::array<double, 3> hsl(double h, double s, double l) {
  auto f = [&](double n) {
    double k = std::fmod(n + h * 12, 12.0);
    return l - s * std::min(l, 1 - l) * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
  };
  return {f(0), f(8), f(4)};
}

App::App() {}

void App::init(std::unique_ptr<Renderer> r) {
  renderer = std::move(r);
  loadSettings();
  // reopen the last saved / opened scene file (changes are never written without asking)
  std::string err;
  if (lastFile.empty() || !loadSceneFile(lastFile, err)) { loadScene(playgroundScene()); currentFile.clear(); }
  pendingRecovery = findRecovery();  // left behind by a crash: ask the user
}

// ============================================================ autosave / recovery
static std::string recoveryPath() { return (std::filesystem::path(dataDir()) / "recovery.4dscene.json").string(); }
static std::string recoveryInfoPath() { return (std::filesystem::path(dataDir()) / "recovery.json").string(); }

void App::autosaveTick(bool force) {
  if (!dirty()) {  // nothing unsaved (saved, undone back, or another scene loaded): no recovery copy
    if (!autosaved_.empty()) clearRecovery();
    return;
  }
  if (committed_ == autosaved_) return;
  if (!force && (editSet.autosaveSeconds <= 0 || now - lastAutosave_ < editSet.autosaveSeconds)) return;
  lastAutosave_ = now;
  if (!writeFile(recoveryPath(), serialize())) return;
  std::time_t t = std::time(nullptr);
  char when[64];
  std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  nlohmann::json j = {{"file", currentFile}, {"time", when}};
  writeFile(recoveryInfoPath(), j.dump(2));
  autosaved_ = committed_;
}

void App::clearRecovery() {
  std::error_code ec;
  std::filesystem::remove(recoveryPath(), ec);
  std::filesystem::remove(recoveryInfoPath(), ec);
  autosaved_.clear();
}

std::optional<App::RecoveryInfo> App::findRecovery() {
  std::string text;
  if (!std::filesystem::exists(recoveryPath())) return std::nullopt;
  RecoveryInfo info;
  if (readFile(recoveryInfoPath(), text)) {
    try {
      auto j = nlohmann::json::parse(text);
      info.file = j.value("file", std::string());
      info.time = j.value("time", std::string());
    } catch (...) {}
  }
  return info;
}

bool App::recover(std::string& err) {
  auto info = findRecovery();
  std::string text;
  if (!info || !readFile(recoveryPath(), text)) { err = "No recovery file"; return false; }
  try {
    loadScene(sceneFromJson(text));
  } catch (const std::exception& e) { err = e.what(); return false; }
  currentFile = info->file;  // saving writes back to the original file
  saved_.clear();            // recovered changes are unsaved
  autosaved_ = committed_;
  return true;
}

void App::toast(const std::string& msg, bool error) {
  toastMsg = msg;
  toastError = error;
  toastUntil = now + (error ? 5 : 2.5);
}

// ============================================================ scene lifecycle
// undo snapshot: world id, active camera, lights, animation, scene tree (one line each)
std::string App::snapshot() {
  return std::to_string(space.radius) + "\n" + world + "\n" + activeCamera + "\n" + lightsToJson(lights) + "\n" + animationToJson(anim) + "\n" + nodeToJson(root);
}
void App::restore(const std::string& snap) {
  auto nl0 = snap.find('\n');
  space.radius = std::max(0.05, std::atof(snap.substr(0, nl0).c_str()));
  std::string s = snap.substr(nl0 + 1);
  auto nl = s.find('\n'), nl2 = s.find('\n', nl + 1), nl3 = s.find('\n', nl2 + 1), nl4 = s.find('\n', nl3 + 1);
  world = s.substr(0, nl);
  activeCamera = s.substr(nl + 1, nl2 - nl - 1);
  lights = lightsFromJson(s.substr(nl2 + 1, nl3 - nl2 - 1));
  double frame = anim.frame;
  anim = animationFromJson(s.substr(nl3 + 1, nl4 - nl3 - 1));
  anim.frame = frame;  // undo does not move the time
  root = nodeFromJson(s.substr(nl4 + 1));
  recordLinks(root, linkDefs_);
  if (!anim.empty()) applyAnim();
  evalRoot_ = root;
  evalLights_ = lights;
  evalRadius_ = space.radius;
}

void App::loadScene(SceneFile f) {
  root = std::move(f.root);
  space = f.space;
  if (space.curved()) {
    f.world.clear();
    walk(root, [](Node& n, Node*, int) { n.anchor.reset(); n.xf.pos[3] = 0; n.xf.rot = cs::rot3(n.xf.rot); });
    if (f.curvedCam) curvCam.load(space, *f.curvedCam); else curvCam.reset(space);
  }
  world = (!f.world.empty() && findNode(root, f.world)) ? f.world : "";
  // old camera bookmarks become camera objects
  for (auto& b : f.bookmarks) {
    FullCamera fc;
    fc.load(b.cam);
    View v = fc.view();
    Node c = makeCamera(b.name, v.eye, v.frame, 60, b.cam.slider);
    if (b.geo && !f.world.empty()) { c.xf.pos = b.geoPivot; c.xf.rot = identity(); c.anchor = Anchor{f.world, b.geoFrame, 1e4}; }
    root.children.push_back(std::move(c));
  }
  activeCamera = f.activeCamera;
  if (Node* a = findNode(root, activeCamera); !a || !a->isCamera()) {
    activeCamera.clear();
    walk(root, [&](Node& n, Node*, int) { if (n.isCamera() && activeCamera.empty()) activeCamera = n.id; });
  }
  lights = std::move(f.lights);
  anim = f.animation.empty() ? Animation{} : animationFromJson(f.animation);
  playing = false;
  recordLinks(root, linkDefs_);
  isolate.clear();
  selection.clear();
  if (f.camera) editCam.load(*f.camera);
  mode = Mode::Edit;
  geo = false;
  geoEdit.valid = false;
  modal = Modal::None;
  history_.clear();
  future_.clear();
  rebuild();
  if (!anim.empty()) applyAnim();
  evalRoot_ = root;
  evalLights_ = lights;
  evalRadius_ = space.radius;
  committed_ = snapshot();
  saved_ = committed_;
}

// ============================================================ animation
void App::applyAnim() {
  applyAnimation(anim, root, lights, anim.frame);
  if (space.curved())
    for (auto& t : anim.tracks)
      if (t.target == "scene" && t.path == "space.radius" && !t.keys.empty()) space.radius = std::max(0.2, evalTrack(t, anim.frame));
  rebuild();
  // objects with surface motion: placed from their rest pose by travel / heading / spin
  bool surf = false;
  walk(root, [&](Node& n, Node*, int) { if (n.anchor && n.anchor->hasRest) { applySurfacePose(n); surf = true; } });
  if (surf) rebuild();
  // objects / cameras on a world surface: the interpolated point is put back on the surface and
  // their frame re-aligned to it (so they travel along the surface between keys)
  if (world.empty() || !ev) return;
  std::unordered_set<std::string> moved;
  for (auto& t : anim.tracks) if (t.path == "rot" || t.path.rfind("pos.", 0) == 0) moved.insert(t.target);
  walk(root, [&](Node& n, Node*, int) { if (n.anchor && n.anchor->hasRest) moved.erase(n.id); });  // (placed above)
  bool any = false;
  auto f = field();
  for (auto& id : moved) {
    Node* n = findNode(root, id);
    if (!n || !n->anchor || n->anchor->world != world) continue;
    M4 F = nodeRotation(*n);
    V4 p = projectToSurface(f, n->xf.pos, 20);
    if (!std::isfinite(p[0])) continue;
    M4 basis = tangentFrame(f, p, F);
    n->xf.pos = p;
    n->anchor->basis = basis;
    n->xf.rot = orthonormalize(mul(transpose(basis), F));
    any = true;
  }
  if (any) rebuild();
}

void App::setFrame(double frame) {
  anim.frame = frame;
  if (!anim.empty()) {
    applyAnim();
    evalRoot_ = root;
    evalLights_ = lights;
    evalRadius_ = space.radius;
  }
  lastChange_ = now;
}

double App::playbackEnd() const {
  // Explore: the animation repeats from its last keyframe; Edit: from the end of the frame range
  if (mode == Mode::Explore) {
    double last = -1e18;
    for (auto& t : anim.tracks) if (!t.keys.empty()) last = std::max(last, t.keys.back().frame);
    if (last > -1e17) return std::max(last, anim.start + 1.0);
  }
  return anim.end + 0.999;
}

void App::tickAnimation(double dt) {
  if (!playing) return;
  double f = anim.frame + dt * anim.fps, end = playbackEnd();
  if (f > end) {  // loop: back to the first frame, carrying the overshoot (repeats indefinitely)
    double len = std::max(1e-6, end - anim.start);
    f = anim.start + std::fmod(f - end, len);
  }
  setFrame(f);
}

void App::insertKeys(bool transformOnly) {
  double fr = std::round(anim.frame);
  int n = 0;
  for (auto& id : selection)
    if (Node* nd = findNode(root, id); nd && id != root.id) {
      keyNode(anim, *nd, fr, transformOnly);  // (attached objects with surface motion key Travel / Heading / Spin)
      n++;
    }
  if (n) commit();
  else toast("Select objects to key them", true);
}

void App::deleteKeys() {
  double fr = std::round(anim.frame);
  for (auto& id : selection)
    for (auto& t : anim.tracks) if (t.target == id) deleteKey(anim, t.target, t.path, fr);
  commit();
}

void App::toggleValueKey(const std::string& target, const std::string& path) {
  double fr = std::round(anim.frame);
  if (hasKeyAt(anim, target, path, fr)) { deleteKey(anim, target, path, fr); commit(); return; }
  if (path.rfind("surf.", 0) == 0) if (Node* n = findNode(root, target)) ensureRest(*n);
  if (target == "scene") { if (path == "space.radius") setKey(anim, target, path, fr, space.radius); }
  else if (target.rfind("light:", 0) == 0) {
    for (auto& l : lights) if ("light:" + l.id == target) { double v; if (getLightValue(l, path, v)) setKey(anim, target, path, fr, v); }
  } else if (Node* n = findNode(root, target)) {
    if (path == "rot") setRotKey(anim, target, fr, nodeRotation(*n));
    else { double v; if (getValue(*n, path, v)) setKey(anim, target, path, fr, v); }
  }
  commit();
}

void App::autoKeyChanges() {
  // key every value that differs from what the animation (or the last edit) left there
  double fr = std::round(anim.frame);
  if (space.curved() && std::abs(space.radius - evalRadius_) > 1e-9) setKey(anim, "scene", "space.radius", fr, space.radius);
  walk(root, [&](Node& n, Node*, int) {
    if (n.id == root.id) return;
    Node* before = findNode(evalRoot_, n.id);
    if (!before) return;
    for (auto& v : animatableValues(n)) {
      if (v.path == "rot") {
        M4 a = nodeRotation(*before), b = nodeRotation(n);
        double e = 0;
        for (int k = 0; k < 16; k++) e = std::max(e, std::abs(a[k] - b[k]));
        if (e > 1e-9) setRotKey(anim, n.id, fr, b);
        continue;
      }
      double x, y;
      if (getValue(*before, v.path, x) && getValue(n, v.path, y) && std::abs(x - y) > 1e-9) setKey(anim, n.id, v.path, fr, y);
    }
  });
  for (auto& l : lights)
    for (auto& b : evalLights_)
      if (b.id == l.id)
        for (auto& v : animatableLightValues()) {
          double x, y;
          if (getLightValue(b, v.path, x) && getLightValue(l, v.path, y) && std::abs(x - y) > 1e-9) setKey(anim, "light:" + l.id, v.path, fr, y);
        }
}

void App::newScene(SpaceKind kind, double radius) {
  SceneFile f = playgroundScene();
  std::vector<Node> keep;
  for (auto& c : f.root.children) if (c.id == "ground") keep.push_back(c);
  f.root.children = keep;
  if (kind != SpaceKind::Flat) {
    // a curved 3D space: a ground plane, a few shapes and a row of pillars that shows the curvature
    f.space.kind = kind;
    f.space.radius = std::max(0.2, radius);
    f.root.children[0].mat.texScale = 1;
    Node cube = makeShape("cube", "Cube", {0, 0.8, 0, 0}, {0.85, 0.35, 0.3});
    double ang[6] = {0, 25, 0, 0, 0, 0};
    cube.xf.rot = rotationFromAngles(ang);
    cube.xf.scale = 0.8;
    Node ball = makeShape("sphere", "Sphere", {3, 1, 0.5, 0}, {0.95, 0.75, 0.3});
    Node torus = makeShape("torus", "Torus", {-3, 0.6, 0.5, 0}, {0.3, 0.6, 0.9});
    std::vector<Node> pillars;
    for (int i = -3; i <= 3; i++) {
      Node c = makeShape("cylinder", "Pillar " + std::to_string(i + 4), {i * 2.5, 1.5, -5, 0}, {0.75, 0.72, 0.65});
      c.xf.scale = 0.4;
      double up[6] = {0, 0, 0, 90, 0, 0};  // cylinder axis vertical
      c.xf.rot = rotationFromAngles(up);
      pillars.push_back(c);
    }
    Node row = makeGroup(BoolOp::Union, pillars, "Pillars");
    f.root.children.push_back(cube);
    f.root.children.push_back(ball);
    f.root.children.push_back(torus);
    f.root.children.push_back(row);
    f.camera.reset();
  }
  loadScene(f);
  currentFile.clear();
}

bool App::loadSceneFile(const std::string& path, std::string& err) {
  std::string text;
  if (!readFile(path, text)) { err = "Cannot read " + path; return false; }
  try { loadScene(sceneFromJson(text)); } catch (const std::exception& e) { err = e.what(); return false; }
  currentFile = path;
  lastFile = path;
  saveSettings();
  return true;
}

std::string App::serialize() {
  CameraState cs = editCam.save();
  CurvedCamState cc = curvCam.save();
  std::string aj = anim.empty() ? std::string() : animationToJson(anim);
  return sceneToJson(root, reg, world, &cs, nullptr, &lights, activeCamera, &aj, &space, &cc);
}

bool App::saveSceneFile(const std::string& path, std::string& err) {
  if (!writeFile(path, serialize())) { err = "Cannot write " + path; return false; }
  currentFile = path;
  lastFile = path;
  saved_ = committed_;
  clearRecovery();
  saveSettings();
  return true;
}

V4 App::light() const {
  double cy = std::cos(quality.lightYaw), sy = std::sin(quality.lightYaw), cp = std::cos(quality.lightPitch), sp = std::sin(quality.lightPitch);
  return normalize(V4{cp * sy, sp, cp * cy, quality.lightW});
}

void App::rebuild() {
  version_++;
  lastChange_ = now;
  if (!world.empty() && !findNode(root, world)) world.clear();
  syncLinks(root, linkDefs_);  // an edit of one linked duplicate reaches the others
  // Isolate (view only): a copy of the tree with everything else hidden; picking and drawing use it
  for (auto it = isolate.begin(); it != isolate.end();) it = findNode(root, *it) ? std::next(it) : isolate.erase(it);
  if (!isolate.empty()) {
    viewRoot_ = root;
    std::unordered_set<std::string> keep;
    for (auto& id : isolate) {
      for (Node* n : pathTo(root, id)) keep.insert(n->id);           // ancestors (their transforms)
      if (Node* n = findNode(root, id)) walk(*n, [&](Node& m, Node*, int) { keep.insert(m.id); });
    }
    if (!world.empty()) for (Node* n : pathTo(root, world)) keep.insert(n->id);
    if (Node* w = world.empty() ? nullptr : findNode(root, world)) walk(*w, [&](Node& m, Node*, int) { keep.insert(m.id); });
    walk(viewRoot_, [&](Node& m, Node*, int) { if (!keep.count(m.id)) m.hidden = true; });
  }
  ev = std::make_unique<SceneEvaluator>(isolate.empty() ? root : viewRoot_, reg, world, space);
  updateFollowPaths();
}

// ============================================================ objects on world surfaces
void App::ensureRest(Node& n) {
  if (!n.anchor || n.anchor->hasRest) return;
  Anchor& a = *n.anchor;
  a.hasRest = true;
  a.restPos = n.xf.pos;
  a.restFrame = mul(a.basis, n.xf.rot);
  a.travel = a.heading = a.spin = 0;
}

void App::applySurfacePose(Node& n) {
  if (!n.anchor || !n.anchor->hasRest || !ev || !findNode(root, n.anchor->world)) return;
  Anchor& a = *n.anchor;
  auto f = worldField(*ev, a.world);
  // turn by heading (about local Y), walk travel along local X (geodesic, frame carried), roll by spin
  M4 F0 = rotateFrame(a.restFrame, 0, 2, a.heading * PI / 180);
  auto r = std::abs(a.travel) > 1e-12 ? shoot(f, a.restPos, F0, {1, 0, 0}, a.travel, quality.geoMaxStep * 0.5) : GeoResult{a.restPos, F0};
  M4 F = rotateFrame(r.frame, 1, 2, a.spin * PI / 180);
  n.xf.pos = r.pos;
  a.basis = F;
  n.xf.rot = identity();
  a.reach = curvatureRadius(f, r.pos, F);
}

void App::setRestFromPose(Node& n) {
  if (!n.anchor || !n.anchor->hasRest || !ev || !findNode(root, n.anchor->world)) return;
  Anchor& a = *n.anchor;
  auto f = worldField(*ev, a.world);
  M4 F1 = rotateFrame(mul(a.basis, n.xf.rot), 1, 2, -a.spin * PI / 180);
  auto r = std::abs(a.travel) > 1e-12 ? shoot(f, n.xf.pos, F1, {1, 0, 0}, -a.travel, quality.geoMaxStep * 0.5) : GeoResult{n.xf.pos, F1};
  a.restPos = r.pos;
  a.restFrame = rotateFrame(r.frame, 0, 2, -a.heading * PI / 180);
}

void App::updateFollowPaths() {
  // the geodesic through each follow-surface object's anchor along its local X, with the object's
  // frame carried along it: NPATH + 1 samples over the object's length (packNodes / the evaluator)
  if (!ev || space.curved()) return;
  bool any = false;
  walk(root, [&](Node& n, Node*, int) { if (n.anchor && n.anchor->follow) any = true; });
  if (!any) return;
  auto bounds = computeBounds(root, reg);
  auto fill = [&](Node& n) {
    Anchor& a = *n.anchor;
    a.path.clear();
    if (!a.follow || !findNode(root, a.world)) return;
    auto f = worldField(*ev, a.world);
    auto b = bounds.find(n.id);
    double len = b != bounds.end() && std::isfinite(b->second.r) ? std::max(0.2, (b->second.r - 0.1) / 1.5 * 1.1) : 5.0;
    double ds = 2 * len / NPATH, step = quality.geoMaxStep * 0.5;
    M4 F = mul(a.basis, n.xf.rot);
    std::vector<Anchor::PathPt> pts(NPATH + 1);
    auto put = [&](int k, const V4& p, const M4& fr) { pts[k] = {p, col(fr, 1), col(fr, 2), col(fr, 3)}; };
    int mid = NPATH / 2;
    put(mid, n.xf.pos, F);
    for (int dirn : {1, -1}) {
      V4 p = n.xf.pos;
      M4 fr = F;
      for (int k = 1; k <= mid; k++) {
        auto r = shoot(f, p, fr, {1, 0, 0}, dirn * ds, step);
        p = r.pos;
        fr = r.frame;
        put(mid + dirn * k, p, fr);
      }
    }
    a.path = pts;
    a.pathLen = len;
  };
  walk(root, [&](Node& n, Node*, int) { if (n.anchor) fill(n); });
  if (!isolate.empty())  // the drawn copy gets the same paths
    walk(viewRoot_, [&](Node& m, Node*, int) {
      if (Node* o = m.anchor ? findNode(root, m.id) : nullptr; o && o->anchor) { m.anchor->path = o->anchor->path; m.anchor->pathLen = o->anchor->pathLen; }
    });
}

void App::commit() {
  // a group whose last object was removed / moved out is deleted with it
  pruneEmptyGroups(root);
  if (autoKey) autoKeyChanges();
  pruneTracks(anim, root, lights);
  selection.erase(std::remove_if(selection.begin(), selection.end(), [&](auto& id) { return !findNode(root, id); }), selection.end());
  rebuild();
  evalRoot_ = root;
  evalLights_ = lights;
  evalRadius_ = space.radius;
  std::string s = snapshot();
  if (s != committed_) {
    history_.push_back(committed_);
    if (history_.size() > 200) history_.erase(history_.begin());
    future_.clear();
    committed_ = s;
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
    c.linkId.clear();  // a plain copy is independent (Duplicate linked keeps the link)
    size_t i = 0;
    while (i < p->children.size() && p->children[i].id != id) i++;
    out.push_back(c.id);
    p->children.insert(p->children.begin() + i + 1, std::move(c));
  }
  selection = out;
  commit();
  if (!out.empty()) startModal(Modal::Grab);
}

void App::duplicateLinked() {
  std::vector<std::string> out;
  for (auto& id : selection) {
    Node* p = findParent(root, id);
    Node* n = findNode(root, id);
    if (!p || !n) continue;
    if (n->linkId.empty()) n->linkId = newId("l");
    Node c = cloneFresh(*n);  // keeps the link id
    c.name += " (linked)";
    size_t i = 0;
    while (i < p->children.size() && p->children[i].id != id) i++;
    out.push_back(c.id);
    p->children.insert(p->children.begin() + i + 1, std::move(c));
  }
  selection = out;
  commit();
  if (!out.empty()) startModal(Modal::Grab);
}

void App::unlink(const std::string& id) {
  if (Node* n = findNode(root, id)) { n->linkId.clear(); commit(); }
}

int App::linkCount(const std::string& linkId) {
  int c = 0;
  if (!linkId.empty()) walk(root, [&](Node& n, Node*, int) { if (n.linkId == linkId) c++; });
  return c;
}

std::string App::copySelection() {
  nlohmann::json j = {{"format", "4dcad-clipboard"}, {"nodes", nlohmann::json::array()}};
  for (auto& id : selection) {
    Node* n = findNode(root, id);
    if (!n || id == root.id) continue;
    bool covered = false;
    for (auto& o : selection) if (o != id && isAncestor(root, o, id)) covered = true;
    if (covered) continue;  // copied with its parent
    Node c = *n;
    if (auto w = worldTransform(id)) c.xf = *w;  // pasted at the scene root: same place in the world
    j["nodes"].push_back(nlohmann::json::parse(nodeToJson(c)));
  }
  return j["nodes"].empty() ? "" : j.dump();
}

bool App::pasteNodes(const std::string& text) {
  nlohmann::json j;
  try { j = nlohmann::json::parse(text); } catch (...) { return false; }
  if (!j.is_object() || j.value("format", std::string()) != "4dcad-clipboard" || !j.contains("nodes")) return false;
  std::vector<std::string> out;
  for (auto& nj : j["nodes"]) {
    Node n;
    try { n = cloneFresh(nodeFromJson(nj.dump())); } catch (...) { continue; }
    // links and anchors refer to this scene only when their target exists here
    walk(n, [&](Node& m, Node*, int) {
      if (m.anchor && !findNode(root, m.anchor->world)) m.anchor.reset();
      if (!m.linkId.empty() && linkCount(m.linkId) == 0) m.linkId.clear();
    });
    out.push_back(n.id);
    root.children.push_back(std::move(n));
  }
  if (out.empty()) return false;
  selection = out;
  commit();
  return true;
}

void App::toggleIsolate() {
  if (!isolate.empty()) isolate.clear();
  else for (auto& id : selection) if (id != root.id) isolate.insert(id);
  rebuild();
}

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
  if (node.isHoneycomb()) {  // a tiling of this scene's space
    int h = firstHoneycomb(space.kind);
    if (node.poly.honey < 0 || node.poly.honey >= (int)honeycombs().size() || honeycombs()[node.poly.honey].space != space.kind) node.poly.honey = std::max(0, h);
    node.name = honeycombs()[node.poly.honey].name;
    node.mat.roughness = 0.4;
  }
  if (id.rfind("builtin-", 0) == 0 && !node.isHoneycomb()) {  // give built-in shapes a fresh colour
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
  if (space.curved()) {
    // in front of the camera (or on the surface clicked), as a child of the root
    V4 p;
    if (hit) {
      V4 q = hit->pos, d = cs::tnormalize(space.k(), cs::tangent(space.k(), hit->pos, -hit->dir));
      cs::geodesic(space, q, d, 1.0);  // back toward the camera, off the surface
      p = q;
    } else {
      V4 q = v.eye, d = dir;
      cs::geodesic(space, q, d, screen ? curvCam.distance : curvCam.distance);
      p = q;
    }
    double sRoot = root.xf.scale;
    M4 rootIso = cs::nodeIso(space, root.xf, 1.0);
    M4 local = mul(cs::inverse(space.k(), rootIso), cs::transvection(space.k(), cs::origin(), p));
    Transform xf = node.xf;
    cs::setFromIso(space, xf, mul(local, cs::rot3(node.xf.rot)), sRoot);
    node.xf.pos = xf.pos;
    node.xf.rot = xf.rot;
  } else if (geo) {
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

// ============================================================ rendering
std::vector<LightParam> App::frameLights() {
  std::vector<LightParam> out;
  for (auto& l : lights) {
    if (!l.enabled || out.size() >= (size_t)MAX_LIGHTS) continue;
    LightParam p;
    p.pos = lightWorldPos(l);
    for (int i = 0; i < 3; i++) p.color[i] = (float)(l.color[i] * l.intensity);
    p.range = (float)l.range;
    p.shadows = l.shadows;
    out.push_back(p);
  }
  if (quality.emissiveLights) {
    // emissive objects light their surroundings: a point light at the object's centre,
    // reaching a few times its size (brightest first, up to the light limit)
    auto bounds = computeBounds(isolate.empty() ? root : viewRoot_, reg);
    std::vector<std::pair<double, LightParam>> em;
    walk(isolate.empty() ? root : viewRoot_, [&](Node& n, Node*, int) {
      if (!n.isLeaf() || n.mat.emissive <= 0.05) return;
      for (Node* a : pathTo(isolate.empty() ? root : viewRoot_, n.id)) if (a->hidden) return;
      auto b = bounds.find(n.id);
      double r = b != bounds.end() && std::isfinite(b->second.r) ? b->second.r : 1.0;
      LightParam p;
      p.pos = nodeWorldPos(n.id);  // the object's origin (shapes are built around it)
      double k = n.mat.emissive * 1.2;
      for (int i = 0; i < 3; i++) p.color[i] = (float)(n.color[i] * k);
      p.range = (float)std::max(1.5, r * 6);
      p.shadows = true;  // fine although the light sits inside the object: shadow rays ignore emissive objects
      p.emissive = true;
      em.push_back({n.mat.emissive * (n.color[0] + n.color[1] + n.color[2]), p});
    });
    std::sort(em.begin(), em.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [w, p] : em) if (out.size() < (size_t)MAX_LIGHTS) out.push_back(p);
  }
  return out;
}

View App::ballView() const {
  FullCamera oc;
  oc.target = {0, 0, 0, 0};
  oc.yaw = thumbYaw;
  oc.pitch = thumbPitch;
  oc.distance = (space.kind == SpaceKind::Hyperbolic ? 3.2 : 4.8) * thumbZoom;
  return oc.view();
}

V4 App::toBall(const V4& p) {
  double w = 1 + p[3];
  return w > 1e-6 ? V4{p[0] / w, p[1] / w, p[2] / w, 0} : V4{0, 0, 0, 0};
}

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

// Curved scenes: the shader works camera-relative (eye = origin, identity frame); positions are
// given relative to the camera, the camera's isometry goes along for the sky and the grid.
void App::curvedParams(FrameParams& fp, const View& v, std::ostringstream& key) {
  double k = space.k();
  M4 inv = cs::inverse(k, v.frame);
  fp.space = space;
  fp.camIso = v.frame;
  fp.view = {cs::origin(), identity()};
  fp.geo = false;
  fp.axes = identity();
  // the sun: a point at infinity (hyperbolic) / a point a quarter turn away (spherical) in its direction
  V4 L = light();
  V4 d3 = normalize(V4{L[0], L[1], L[2], 0});
  V4 sun = mul(inv, V4{d3[0], d3[1], d3[2], k < 0 ? 1.0 : 0.0});
  if (k < 0 && sun[3] > 1e-9) sun = sun * (1.0 / sun[3]);
  fp.light = sun;
  for (auto& l : fp.lights) l.pos = cs::renormPoint(k, mul(inv, l.pos));
  for (auto& p : fp.debugPts) p = mul(inv, p);  // debug ray, camera-relative
  if (fp.markerCol[3] > 0) { fp.marker = cs::origin(); fp.markerFwd = {0, 0, 1, 0}; }
  key << "|c" << space.radius;
}

RenderProfile App::mainProfile() const {
  const QualitySettings& q = quality;
  RenderProfile prof;
  prof.resolution = q.resolutionScale;
  prof.maxSteps = q.maxSteps;
  prof.bounces = q.transparentLayers;
  prof.textures = prof.specular = prof.pointLights = true;
  prof.shadows = q.shadows;
  prof.pointShadows = q.pointShadows;
  prof.ao = q.ao;
  prof.aoSamples = 5;
  prof.shadowSteps = 64;
  return prof;
}

void App::renderFrame() {
  if (!renderer) return;
  if (job.active) return;  // an image / video is rendering: the viewport keeps its last picture
  View v = currentView();
  bool interacting = drag_ != Drag::None && drag_ != Drag::Click;
  bool moving = (now - lastChange_) < 0.25 || interacting || modal != Modal::None || mode == Mode::Explore;
  const QualitySettings& q = quality;
  // the fast profile in Explore (and, if chosen, while moving in Edit mode); else the main settings
  bool fast = mode == Mode::Explore || (moving && q.fastWhileMovingInEdit);
  RenderProfile prof;
  if (fast) prof = q.fast;
  else {
    prof = mainProfile();
    prof.resolution = quality.resolutionScale * (moving ? quality.motionScale : 1.0f);
  }
  float scale = std::clamp(prof.resolution, 0.1f, 2.0f);
  // progressive stochastic refinement while nothing changes (Edit mode, main settings)
  bool refine = q.stochastic && !moving && !fast && mode == Mode::Edit && q.viewportSamples > 1;
  std::ostringstream key;
  key.precision(9);
  for (double x : v.eye) key << x << ',';
  for (double x : v.frame) key << x << ',';
  key << version_ << '|' << geo << (int)mode << '|' << vpW << 'x' << vpH << '|' << scale << '|' << fov() << '|' << q.maxSteps << q.shadows << q.ao
      << q.geoMaxStep << q.geoProjectIters << q.maxDistFull << q.maxDistGeo << q.eps << q.stepScale << q.shadowK << q.shadowMax << q.aoStrength
      << q.fog << q.ambient << q.lightYaw << q.lightPitch << q.lightW << q.skyTop[0] << q.skyTop[1] << q.skyTop[2] << q.skyBottom[0]
      << q.skyBottom[1] << q.skyBottom[2] << q.grid << exploreSet.showOwnBody << exploreSet.bodyRadius << '|';
  for (auto& s : selection) key << s << ';';

  FrameParams fp;
  fp.view = v;
  fp.geo = geo && geoEdit.valid;
  if (inCameraView()) { bool cg = false; cameraView(activeCamera, &cg); fp.geo = cg; }
  fp.grid = mode == Mode::Edit && q.grid;
  fp.fovDeg = fov();
  fp.quality = &quality;
  fp.light = light();
  fp.skyCol = worldColor();
  fp.lights = frameLights();
  fp.prof = prof;
  for (auto& l : fp.lights) { for (double x : l.pos) key << (float)x << ','; key << l.color[0] << l.color[1] << l.color[2] << l.range << l.shadows; }
  key << prof.maxSteps << prof.bounces << prof.textures << prof.specular << prof.pointLights << prof.shadows << prof.pointShadows << prof.ao
      << prof.aoSamples << prof.shadowSteps << q.sunIntensity << q.relaxedMarching << refine << q.sunSize << q.lightRadius;
  if (inCameraView()) {  // depth of field of the camera looked through (stochastic only)
    Node* c = findNode(root, activeCamera);
    fp.aperture = (float)c->camAperture;
    fp.focus = (float)c->camFocus;
    key << fp.aperture << ',' << fp.focus;
  }
  // debug ray: re-traced from the view every frame unless frozen
  if (debugRay && ev) {
    if (!debugFrozen) debugTrace = traceRay(*ev, fp.geo, v.eye, col(v.frame, 2), quality);
    fp.debugPts = debugTrace.pts;
    {  // the start sits in (or next to) the eye: skip it, or it would cover the screen
      size_t k = 0;
      auto far = [&](const V4& p) { return space.curved() ? cs::distance(space, p, debugTrace.pts[0]) : length(p - debugTrace.pts[0]); };
      while (k + 2 < fp.debugPts.size() && far(fp.debugPts[k]) < 0.35) k++;
      fp.debugPts.erase(fp.debugPts.begin(), fp.debugPts.begin() + k);
    }
    fp.debugReason = (int)debugTrace.reason;
    for (auto& p : fp.debugPts) for (double x : p) key << (float)x << ',';
  }
  if (!fp.geo) fp.axes = editCam.axes();
  key << editCam.slider << ',';
  for (float x : fp.skyCol) key << x << ',';
  // the player's body in Explore: casts a shadow and can be seen when light wraps around a closed world
  float r = (float)exploreSet.bodyRadius;
  if (mode == Mode::Explore && exploreSet.showOwnBody) {
    fp.marker = v.eye;
    fp.markerFwd = col(v.frame, 2);
    fp.markerCol = {0.95f, 0.8f, 0.2f, r};
    fp.markerSkip = r * 3;  // the primary ray starts inside the body
  }
  if (space.curved()) curvedParams(fp, v, key);
  std::string k = key.str();
  fp.scale = scale;
  if (k != lastKey_) {
    if (k.substr(0, k.find('|')) != lastKey_.substr(0, lastKey_.find('|'))) lastChange_ = now;
    lastKey_ = k;
    fp.stochastic = refine;  // a new image: the first sample replaces what was there
    fp.sample = 0;
    fp.blend = 1;
    renderer->render(mainRT, (int)vpW, (int)vpH, isolate.empty() ? root : viewRoot_, reg, world, highlightSet(), fp);
    accumSamples = refine ? 1 : 0;
    if (renderer->compiling()) { lastKey_.clear(); accumSamples = 0; viewportCompiling = true; }  // not drawn yet: again next frame
    else viewportCompiling = false;
  } else if (refine && accumSamples < q.viewportSamples) {
    // the same view: add one more random sample to the running average
    fp.stochastic = true;
    fp.sample = accumSamples;
    fp.blend = 1.0f / (accumSamples + 1);
    renderer->render(mainRT, (int)vpW, (int)vpH, isolate.empty() ? root : viewRoot_, reg, world, highlightSet(), fp);
    accumSamples++;
  }
  if (space.curved() && editSet.showThumbnail) {
    // overview: the Poincare ball / stereographic view from outside, with a marker where you are
    thumbW = editSet.thumbSize;
    thumbH = editSet.thumbSize * 3 / 4;
    FrameParams tp;
    tp.space = space;
    tp.ballView = true;
    tp.camIso = identity();  // world coordinates (the ball shows the space around the origin)
    tp.quality = &quality;
    tp.fovDeg = 40;
    tp.view = ballView();
    V4 eb = toBall(v.eye), fb = toBall(along(v.eye, col(v.frame, 2) * (0.05 * space.R())));
    V4 dir = fb - eb;
    tp.marker = eb;
    tp.markerFwd = length(dir) > 1e-12 ? normalize(dir) : V4{0, 0, 1, 0};
    double br = std::max(0.012, 0.03 * (space.kind == SpaceKind::Hyperbolic ? (1 - dot(eb, eb)) : 1.0));
    tp.markerCol = {1.0f, 0.85f, 0.2f, (float)br};
    tp.scale = 1;
    std::ostringstream tk;
    tk << k << "|ball" << thumbYaw << thumbPitch << thumbZoom << thumbW << thumbH;
    if (tk.str() != lastThumbKey_) {
      lastThumbKey_ = tk.str();
      renderer->render(thumbRT, thumbW, thumbH, isolate.empty() ? root : viewRoot_, reg, world, {}, tp);
      if (renderer->compiling()) lastThumbKey_.clear();
    }
  }
  if (fp.geo && editSet.showThumbnail) {
    thumbW = editSet.thumbSize;
    thumbH = editSet.thumbSize * 3 / 4;
    View tv = thumbnailView();
    FrameParams tp;
    tp.view = tv;
    tp.geo = false;
    tp.grid = false;
    tp.fovDeg = 40;
    tp.quality = &quality;
    tp.light = light();
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
      renderer->render(thumbRT, thumbW, thumbH, isolate.empty() ? root : viewRoot_, reg, world, {}, tp);
      if (renderer->compiling()) lastThumbKey_.clear();
    }
  }
}

// ============================================================ settings (persisted per user)
namespace {
template <class T> void getKey(const nlohmann::json& o, const char* k, T& v) {
  if (o.contains(k)) try { v = o[k].get<T>(); } catch (...) {}
}

nlohmann::json qualityToJson(const QualitySettings& q) {
  nlohmann::json r = {{"resolutionScale", q.resolutionScale}, {"motionScale", q.motionScale}, {"maxSteps", q.maxSteps}, {"maxDistFull", q.maxDistFull},
                      {"maxDistGeo", q.maxDistGeo}, {"eps", q.eps}, {"stepScale", q.stepScale}, {"shadows", q.shadows}, {"shadowK", q.shadowK},
                      {"shadowMax", q.shadowMax}, {"ao", q.ao}, {"aoStrength", q.aoStrength}, {"fog", q.fog}, {"ambient", q.ambient},
                      {"lightYaw", q.lightYaw}, {"lightPitch", q.lightPitch}, {"lightW", q.lightW}, {"skyTop", q.skyTop}, {"skyBottom", q.skyBottom},
                      {"grid", q.grid}, {"geoMaxStep", q.geoMaxStep}, {"geoProjectIters", q.geoProjectIters}, {"sunIntensity", q.sunIntensity},
                      {"pointShadows", q.pointShadows}, {"emissiveLights", q.emissiveLights}, {"transparentLayers", q.transparentLayers},
                      {"relaxedMarching", q.relaxedMarching}, {"fastWhileMovingInEdit", q.fastWhileMovingInEdit},
                      {"stochastic", q.stochastic}, {"viewportSamples", q.viewportSamples}, {"frameLimit", q.frameLimit}, {"sunSize", q.sunSize}, {"lightRadius", q.lightRadius}};
  const RenderProfile& f = q.fast;
  r["fast"] = {{"resolution", f.resolution}, {"maxSteps", f.maxSteps}, {"bounces", f.bounces}, {"textures", f.textures},
               {"specular", f.specular}, {"pointLights", f.pointLights}, {"shadows", f.shadows}, {"pointShadows", f.pointShadows},
               {"ao", f.ao}, {"aoSamples", f.aoSamples}, {"shadowSteps", f.shadowSteps}};
  return r;
}

/** Sets the values present in r (others are left as they are). */
void qualityFromJson(const nlohmann::json& r, QualitySettings& q) {
  if (!r.is_object()) return;
  getKey(r, "resolutionScale", q.resolutionScale); getKey(r, "motionScale", q.motionScale); getKey(r, "maxSteps", q.maxSteps);
  getKey(r, "maxDistFull", q.maxDistFull); getKey(r, "maxDistGeo", q.maxDistGeo); getKey(r, "eps", q.eps); getKey(r, "stepScale", q.stepScale);
  getKey(r, "shadows", q.shadows); getKey(r, "shadowK", q.shadowK); getKey(r, "shadowMax", q.shadowMax); getKey(r, "ao", q.ao);
  getKey(r, "aoStrength", q.aoStrength); getKey(r, "fog", q.fog); getKey(r, "ambient", q.ambient); getKey(r, "lightYaw", q.lightYaw);
  getKey(r, "lightPitch", q.lightPitch); getKey(r, "lightW", q.lightW); getKey(r, "skyTop", q.skyTop); getKey(r, "skyBottom", q.skyBottom);
  getKey(r, "grid", q.grid); getKey(r, "geoMaxStep", q.geoMaxStep); getKey(r, "geoProjectIters", q.geoProjectIters);
  getKey(r, "sunIntensity", q.sunIntensity); getKey(r, "pointShadows", q.pointShadows); getKey(r, "emissiveLights", q.emissiveLights);
  getKey(r, "transparentLayers", q.transparentLayers); getKey(r, "relaxedMarching", q.relaxedMarching);
  getKey(r, "fastWhileMovingInEdit", q.fastWhileMovingInEdit);
  getKey(r, "frameLimit", q.frameLimit);
  getKey(r, "stochastic", q.stochastic); getKey(r, "viewportSamples", q.viewportSamples); getKey(r, "sunSize", q.sunSize); getKey(r, "lightRadius", q.lightRadius);
  auto& f = q.fast;
  if (r.contains("fast") && r["fast"].is_object()) {
    auto& o = r["fast"];
    getKey(o, "resolution", f.resolution); getKey(o, "maxSteps", f.maxSteps); getKey(o, "bounces", f.bounces); getKey(o, "textures", f.textures);
    getKey(o, "specular", f.specular); getKey(o, "pointLights", f.pointLights); getKey(o, "shadows", f.shadows); getKey(o, "pointShadows", f.pointShadows);
    getKey(o, "ao", f.ao); getKey(o, "aoSamples", f.aoSamples); getKey(o, "shadowSteps", f.shadowSteps);
  } else if (r.contains("exploreSimple")) {  // settings from before the fast profile: the old Explore switches
    bool simple = r.value("exploreSimple", true);
    f.textures = !simple;
    f.bounces = simple ? 0 : 1;
    getKey(r, "exploreShadows", f.shadows); getKey(r, "exploreAO", f.ao); getKey(r, "explorePointShadows", f.pointShadows);
  }
  q.maxSteps = std::clamp(q.maxSteps, 8, 2000);
  q.transparentLayers = std::clamp(q.transparentLayers, 0, 8);
  q.viewportSamples = std::clamp(q.viewportSamples, 1, 4096);
}

nlohmann::json outputToJson(const App::RenderSettings& o, bool paths) {
  nlohmann::json j = {{"width", o.width}, {"height", o.height}, {"aa", o.aa}, {"samples", o.samples}, {"stillFormat", o.stillFormat},
                      {"animFormat", o.animFormat}, {"sceneRange", o.sceneRange}, {"start", o.start}, {"end", o.end}, {"fps", o.fps},
                      {"jpegQuality", o.jpegQuality}, {"priority", o.priority}};
  if (paths) { j["stillPath"] = o.stillPath; j["animPath"] = o.animPath; j["ffmpeg"] = o.ffmpeg; }
  return j;
}

void outputFromJson(const nlohmann::json& r, App::RenderSettings& o) {
  if (!r.is_object()) return;
  getKey(r, "width", o.width); getKey(r, "height", o.height); getKey(r, "aa", o.aa); getKey(r, "samples", o.samples);
  getKey(r, "stillFormat", o.stillFormat); getKey(r, "animFormat", o.animFormat); getKey(r, "stillPath", o.stillPath);
  getKey(r, "animPath", o.animPath); getKey(r, "sceneRange", o.sceneRange); getKey(r, "start", o.start); getKey(r, "end", o.end);
  getKey(r, "fps", o.fps); getKey(r, "jpegQuality", o.jpegQuality); getKey(r, "ffmpeg", o.ffmpeg);
  getKey(r, "priority", o.priority);
  o.priority = std::clamp(o.priority, 0, 2);
  o.stillFormat = std::clamp(o.stillFormat, 0, 1);
  o.animFormat = std::clamp(o.animFormat, 0, 4);
  o.aa = std::clamp(o.aa, 1, 3);
  o.samples = std::clamp(o.samples, 1, 4096);
  o.width = std::clamp(o.width, 16, 8192);
  o.height = std::clamp(o.height, 16, 8192);
}
}  // namespace

// ------------------------------------------------------------ render presets
// A preset is all rendering settings (Settings > Rendering, incl. the fast profile, and the Render
// window's image / animation settings, without output paths). User presets are JSON files in
// <data>/presets; the built-in ones are read-only.
static std::filesystem::path presetDir() { return std::filesystem::path(dataDir()) / "presets"; }

static std::string presetFileName(const std::string& name) {
  std::string f;
  for (char c : name) f += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
  if (f.empty()) f = "preset";
  return f + ".json";
}

std::vector<App::RenderPreset> App::builtinPresets() {
  std::vector<RenderPreset> out;
  const char* names[3] = {"Fast", "Balanced", "Full"};
  for (int i = 0; i < 3; i++) {
    QualitySettings q;  // the defaults, with the matching profile's quality
    RenderProfile p = RenderProfile::preset((RenderProfile::Preset)i);
    q.resolutionScale = i == 0 ? 0.6f : i == 1 ? 0.8f : 1.0f;
    q.maxSteps = p.maxSteps;
    q.transparentLayers = p.bounces;
    q.shadows = p.shadows || i > 0;
    q.pointShadows = p.pointShadows;
    q.ao = p.ao;
    q.stochastic = i > 0;
    q.viewportSamples = i == 0 ? 8 : i == 1 ? 32 : 128;
    RenderSettings o;
    o.width = i == 0 ? 640 : i == 1 ? 1280 : 1920;
    o.height = i == 0 ? 360 : i == 1 ? 720 : 1080;
    o.aa = i == 0 ? 1 : 2;
    o.samples = i == 0 ? 1 : i == 1 ? 16 : 128;
    nlohmann::json j = {{"name", names[i]}, {"quality", qualityToJson(q)}, {"output", outputToJson(o, false)}};
    // the look (light, sky, fog) and the output format stay as they are
    for (const char* k : {"lightYaw", "lightPitch", "lightW", "skyTop", "skyBottom", "fog", "ambient", "sunIntensity", "grid"}) j["quality"].erase(k);
    for (const char* k : {"stillFormat", "animFormat", "sceneRange", "start", "end", "fps", "jpegQuality"}) j["output"].erase(k);
    out.push_back({names[i], true, j.dump(2)});
  }
  return out;
}

std::vector<App::RenderPreset> App::renderPresets() const {
  std::vector<RenderPreset> out = builtinPresets(), user;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(presetDir(), ec)) {
    if (e.path().extension() != ".json") continue;
    std::string text;
    if (!readFile(e.path().string(), text)) continue;
    try {
      auto j = nlohmann::json::parse(text);
      std::string name = j.value("name", e.path().stem().string());
      user.push_back({name, false, text});
    } catch (...) {}
  }
  std::sort(user.begin(), user.end(), [](auto& a, auto& b) { return a.name < b.name; });
  out.insert(out.end(), user.begin(), user.end());
  return out;
}

std::string App::currentPresetJson(const std::string& name) const {
  nlohmann::json j = {{"name", name}, {"quality", qualityToJson(quality)}, {"output", outputToJson(renderSet, false)}};
  return j.dump(2);
}

bool App::applyPreset(const std::string& json, std::string& err) {
  try {
    auto j = nlohmann::json::parse(json);
    if (!j.is_object() || (!j.contains("quality") && !j.contains("output"))) { err = "Not a render preset"; return false; }
    if (j.contains("quality")) qualityFromJson(j["quality"], quality);
    if (j.contains("output")) outputFromJson(j["output"], renderSet);
  } catch (const std::exception& e) { err = std::string("Bad preset: ") + e.what(); return false; }
  saveSettings();
  return true;
}

bool App::savePreset(const std::string& name, std::string& err) {
  if (name.empty()) { err = "Give the preset a name"; return false; }
  for (auto& p : builtinPresets()) if (p.name == name) { err = "\"" + name + "\" is a built-in preset"; return false; }
  std::error_code ec;
  std::filesystem::create_directories(presetDir(), ec);
  deletePreset(name);  // overwrite (also if its file had another name)
  if (!writeFile((presetDir() / presetFileName(name)).string(), currentPresetJson(name))) { err = "Cannot write the preset"; return false; }
  return true;
}

bool App::deletePreset(const std::string& name) {
  std::error_code ec;
  bool any = false;
  for (auto& e : std::filesystem::directory_iterator(presetDir(), ec)) {
    if (e.path().extension() != ".json") continue;
    std::string text;
    if (!readFile(e.path().string(), text)) continue;
    try {
      if (nlohmann::json::parse(text).value("name", e.path().stem().string()) == name) { std::filesystem::remove(e.path(), ec); any = true; }
    } catch (...) {}
  }
  return any;
}

bool App::renamePreset(const std::string& from, const std::string& to, std::string& err) {
  if (to.empty()) { err = "Give the preset a name"; return false; }
  if (from == to) return true;
  for (auto& p : renderPresets()) if (p.name == to) { err = "A preset named \"" + to + "\" exists"; return false; }
  for (auto& p : renderPresets()) {
    if (p.name != from || p.builtin) continue;
    try {
      auto j = nlohmann::json::parse(p.json);
      j["name"] = to;
      deletePreset(from);
      if (!writeFile((presetDir() / presetFileName(to)).string(), j.dump(2))) { err = "Cannot write the preset"; return false; }
      return true;
    } catch (...) { break; }
  }
  err = "No such preset";
  return false;
}

bool App::importPreset(const std::string& path, std::string& err, std::string* nameOut) {
  std::string text;
  if (!readFile(path, text)) { err = "Cannot read " + path; return false; }
  try {
    auto j = nlohmann::json::parse(text);
    if (!j.is_object() || (!j.contains("quality") && !j.contains("output"))) { err = "Not a render preset"; return false; }
    std::string base = j.value("name", std::filesystem::path(path).stem().string()), name = base;
    auto all = renderPresets();
    for (int n = 2; std::any_of(all.begin(), all.end(), [&](auto& p) { return p.name == name; }); n++) name = base + " " + std::to_string(n);
    j["name"] = name;
    std::error_code ec;
    std::filesystem::create_directories(presetDir(), ec);
    if (!writeFile((presetDir() / presetFileName(name)).string(), j.dump(2))) { err = "Cannot write the preset"; return false; }
    if (nameOut) *nameOut = name;
    return true;
  } catch (const std::exception& e) { err = std::string("Bad preset file: ") + e.what(); return false; }
}

bool App::exportPreset(const std::string& name, const std::string& path, std::string& err) {
  for (auto& p : renderPresets())
    if (p.name == name) {
      if (!writeFile(path, p.json)) { err = "Cannot write " + path; return false; }
      return true;
    }
  err = "No such preset";
  return false;
}

void App::saveSettings() {
  nlohmann::json j;
  j["lastFile"] = lastFile;
  j["activePreset"] = activePreset;
  j["render"] = qualityToJson(quality);
  j["render"]["backend"] = BACKEND_NAMES[(int)backendPref];
  const auto& e = editSet;
  j["edit"] = {{"fov", e.fov}, {"orbitSpeed", e.orbitSpeed}, {"panSpeed", e.panSpeed}, {"zoomSpeed", e.zoomSpeed}, {"rot4Speed", e.rot4Speed},
               {"showWSlider", e.showWSlider}, {"flySpeed", e.flySpeed}, {"lookSpeed", e.lookSpeed}, {"showThumbnail", e.showThumbnail},
               {"thumbSize", e.thumbSize}, {"autosaveSeconds", e.autosaveSeconds}, {"globalAxes", e.globalAxes}, {"snap", e.snap}, {"snapMove", e.snapMove},
               {"snapAngle", e.snapAngle}, {"snapScale", e.snapScale}};
  const auto& x = exploreSet;
  j["explore"] = {{"fov", x.fov}, {"lookSpeed", x.lookSpeed}, {"showOwnBody", x.showOwnBody}, {"bodyRadius", x.bodyRadius},
                  {"moveSpeed", x.moveSpeed}, {"turn4Speed", x.turn4Speed}, {"collide", x.collide}, {"geoMoveSpeed", x.geoMoveSpeed},
                  {"geoCollide", x.geoCollide}};
  j["output"] = outputToJson(renderSet, true);
  keys.toJson(j["keys"]);
  writeFile((std::filesystem::path(dataDir()) / "settings.json").string(), j.dump(2));
}

static Backend backendFromSettings(const std::string& settingsText) {
  try {
    auto j = nlohmann::json::parse(settingsText);
    std::string b = j.value("render", nlohmann::json::object()).value("backend", std::string("Auto"));
    for (int i = 0; i < 3; i++) if (b == BACKEND_NAMES[i]) return (Backend)i;
  } catch (...) {}
  return Backend::Auto;
}

Backend App::savedBackend() {
  std::string text;
  if (!readFile((std::filesystem::path(dataDir()) / "settings.json").string(), text)) return Backend::Auto;
  return backendFromSettings(text);
}

void App::loadSettings() {
  std::string text;
  if (!readFile((std::filesystem::path(dataDir()) / "settings.json").string(), text)) return;
  try {
    auto j = nlohmann::json::parse(text);
    lastFile = j.value("lastFile", std::string());
    activePreset = j.value("activePreset", std::string());
    auto get = [](const nlohmann::json& o, const char* k, auto& v) { if (o.contains(k)) v = o[k].get<std::decay_t<decltype(v)>>(); };
    if (j.contains("render")) {
      qualityFromJson(j["render"], quality);
      backendPref = backendFromSettings(text);
    }
    if (j.contains("edit")) {
      auto& r = j["edit"]; auto& e = editSet;
      get(r, "fov", e.fov); get(r, "orbitSpeed", e.orbitSpeed); get(r, "panSpeed", e.panSpeed); get(r, "zoomSpeed", e.zoomSpeed);
      get(r, "rot4Speed", e.rot4Speed); get(r, "showWSlider", e.showWSlider); get(r, "flySpeed", e.flySpeed); get(r, "lookSpeed", e.lookSpeed);
      get(r, "showThumbnail", e.showThumbnail); get(r, "thumbSize", e.thumbSize);
      get(r, "autosaveSeconds", e.autosaveSeconds); get(r, "globalAxes", e.globalAxes); get(r, "snap", e.snap); get(r, "snapMove", e.snapMove);
      get(r, "snapAngle", e.snapAngle); get(r, "snapScale", e.snapScale);
    }
    if (j.contains("explore")) {
      auto& r = j["explore"]; auto& x = exploreSet;
      get(r, "fov", x.fov); get(r, "lookSpeed", x.lookSpeed); get(r, "showOwnBody", x.showOwnBody); get(r, "bodyRadius", x.bodyRadius);
      get(r, "moveSpeed", x.moveSpeed); get(r, "turn4Speed", x.turn4Speed); get(r, "collide", x.collide); get(r, "geoMoveSpeed", x.geoMoveSpeed);
      get(r, "geoCollide", x.geoCollide);
    }
    if (j.contains("output")) outputFromJson(j["output"], renderSet);
    if (j.contains("keys")) keys.fromJson(j["keys"]);
  } catch (...) { /* bad settings file: keep defaults */ }
}

}  // namespace fd
