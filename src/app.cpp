#include "app.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "defs.hpp"
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
  if (texMode.active) return;  // (the texture editing mode's scene is not the user's scene)
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
  // old files animated the rounding / shell fields: those are the Round / Onion modifiers now
  for (auto& t : anim.tracks) {
    if (t.path != "round" && t.path != "shell") continue;
    Node* n = findNode(root, t.target);
    if (!n) continue;
    const char* id = t.path == "round" ? "round" : "onion";
    int k = -1;
    for (int i = 0; i < (int)n->mods.size(); i++) if (n->mods[i].type == MOD_DEF && n->mods[i].def == id) k = i;
    if (k < 0) { n->mods.push_back(makeModifier(*defs().modifier(id))); k = (int)n->mods.size() - 1; }
    t.path = "mods." + std::to_string(k) + ".p." + (t.path == "round" ? "radius" : "thickness");
  }
  // ... and the one colour texture's scale / strength / colour: the colour slot's now
  for (auto& t : anim.tracks) {
    Node* n = findNode(root, t.target);
    if (!n || !n->isLeaf() || !n->mat.tex[CH_COLOR].used()) continue;
    if (t.path == "mat.texScale") t.path = "tex.color.scale";
    else if (t.path == "mat.texStrength") t.path = "tex.color.strength";
    else if (t.path.rfind("mat.color2.", 0) == 0) t.path = "tex.color.color2." + t.path.substr(11);
  }
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
  undoBytes_ = 0;
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
    f.root.children[0].mat.tex[CH_COLOR].map.scale = 1;
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
    clearFuture();
    history_.push_back(diffTo(committed_, s));
    undoBytes_ += history_.back().bytes();
    // the oldest steps go first when there are too many / they take too much memory
    while (!history_.empty() && (history_.size() > UNDO_MAX_STEPS || undoBytes_ > UNDO_MAX_BYTES)) {
      undoBytes_ -= history_.front().bytes();
      history_.pop_front();
    }
    committed_ = s;
  }
}

App::UndoStep App::diffTo(const std::string& target, const std::string& from) {
  // common prefix and suffix (not overlapping), the rest of target is stored
  size_t n = std::min(target.size(), from.size()), pre = 0, suf = 0;
  while (pre < n && target[pre] == from[pre]) pre++;
  while (suf < n - pre && target[target.size() - 1 - suf] == from[from.size() - 1 - suf]) suf++;
  UndoStep d;
  d.pre = pre;
  d.suf = suf;
  d.mid = target.substr(pre, target.size() - pre - suf);
  return d;
}

std::string App::applyStep(const UndoStep& d, const std::string& from) {
  return from.substr(0, d.pre) + d.mid + from.substr(from.size() - d.suf);
}

void App::clearFuture() {
  for (auto& f : future_) undoBytes_ -= f.bytes();
  future_.clear();
}

void App::undo() {
  if (history_.empty()) return;
  std::string prev = applyStep(history_.back(), committed_);
  undoBytes_ -= history_.back().bytes();
  history_.pop_back();
  future_.push_back(diffTo(committed_, prev));
  undoBytes_ += future_.back().bytes();
  committed_ = std::move(prev);
  restore(committed_);
  selection.erase(std::remove_if(selection.begin(), selection.end(), [&](auto& id) { return !findNode(root, id); }), selection.end());
  rebuild();
}
void App::redo() {
  if (future_.empty()) return;
  std::string next = applyStep(future_.back(), committed_);
  undoBytes_ -= future_.back().bytes();
  future_.pop_back();
  history_.push_back(diffTo(committed_, next));
  undoBytes_ += history_.back().bytes();
  committed_ = std::move(next);
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

// ============================================================ placement
int App::itemDim(const std::string& id) {
  const LibraryObject* o = lib.find(id);
  return o ? nodeDim(o->node, reg) : 0;
}

bool App::canPlace(const std::string& id, std::string* reason) {
  if (mode != Mode::Edit) { if (reason) *reason = "Switch to Edit mode to place objects"; return false; }
  if (geo && itemDim(id) > 3) { if (reason) *reason = "Geodesic view: only 3D (or lower) objects can be placed on the world surface"; return false; }
  if (const LibraryObject* o = lib.find(id); o && space.curved() && o->node.isPoly() && polyIs4D(o->node.poly.type)) {
    if (reason) *reason = "4D polytopes exist in flat 4D scenes (curved spaces are 3D)";
    return false;
  }
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
  if (texMode.active)  // texture editing mode: objects have one colour and start pure white (masks 0..1)
    walk(node, [&](Node& m, Node*, int) { m.color = {1, 1, 1}; for (int ch = 1; ch < CH_COUNT; ch++) m.mat.tex[ch] = TexSlot{}; });
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


}  // namespace fd
