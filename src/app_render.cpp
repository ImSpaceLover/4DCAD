// App: what the viewport and thumbnail draw (frame parameters, lights, render threads).
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

// ============================================================ rendering
// How far a glowing object reaches from its origin, measured with the CPU evaluator: along 32
// directions, the outermost point that is inside this object (its bounding sphere is conservative,
// often twice as large; light sampling within a tight sphere wastes far fewer rays, above all in
// 4D where the share hitting the object falls with the cube of the ratio). A part missed here is
// still lit correctly by the paths that hit it, only with more noise.
double App::emitterRadius(const Node& n, const V4& centre, double bound) {
  if (emitRadiusVersion_ != version_) { emitRadius_.clear(); emitRadiusVersion_ = version_; }
  if (auto it = emitRadius_.find(n.id); it != emitRadius_.end()) return it->second;
  double best = 0;
  bool curved = space.curved();
  if (!ev) return bound;
  uint32_t seed = 12345;
  auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) * (1.0 / 16777216.0) * 2 - 1; };
  auto inside = [&](const V4& p) {
    Sample s = curved ? ev->evalCurved(p) : ev->evalFull(p);
    return s.d < 0 && s.node && s.node->id == n.id;
  };
  auto at = [&](const V4& dir, double r) {
    if (!curved) return centre + dir * r;
    V4 p = centre, d = dir;
    cs::geodesic(space, p, d, r);
    return p;
  };
  const int STEPS = 24;
  for (int k = 0; k < 32; k++) {
    V4 d{rnd(), rnd(), rnd(), curved ? 0.0 : rnd()};
    if (curved) d = cs::tnormalize(space.k(), cs::tangent(space.k(), centre, d));
    else d = length(d) > 1e-9 ? normalize(d) : V4{1, 0, 0, 0};
    // outermost sample inside the object, then bisection on the step beyond it
    double lo = -1;
    for (int i = STEPS; i >= 0; i--)
      if (inside(at(d, bound * i / STEPS))) { lo = bound * i / STEPS; break; }
    if (lo < 0) continue;
    double hi = std::min(bound, lo + bound / STEPS);
    for (int b = 0; b < 6; b++) { double m = (lo + hi) / 2; (inside(at(d, m)) ? lo : hi) = m; }
    best = std::max(best, hi);
  }
  double r = best > 0 ? std::min(bound, best * 1.08 + 1e-3) : bound;
  emitRadius_[n.id] = r;
  return r;
}

std::vector<LightParam> App::frameLights(bool pathTrace) {
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
  if (pathTrace) {
    // path tracing: every glowing object is an area light; the shader samples directions within its
    // bounding sphere and counts only those reaching its glowing surface (so its shape matters)
    Node& top = isolate.empty() ? root : viewRoot_;
    auto bounds = computeBounds(top, reg);
    std::vector<std::pair<double, LightParam>> em;
    walk(top, [&](Node& n, Node*, int) {
      if (!n.isLeaf() || n.mat.emissive <= 0 || n.mat.emissionStrength <= 0) return;
      auto path = pathTo(top, n.id);
      double sAnc = 1;
      for (Node* a : path) {
        if (a->hidden) return;
        if (a != &n) sAnc *= a->xf.scale;
      }
      auto b = bounds.find(n.id);
      if (b == bounds.end() || !std::isfinite(b->second.r)) return;  // unbounded (planes...): found by the paths alone
      V4 off = b->second.c - n.xf.pos;
      if (space.curved()) off[3] = 0;
      LightParam p;
      p.pos = nodeWorldPos(n.id);
      p.radius = (float)emitterRadius(n, p.pos, (b->second.r + length(off)) * sAnc);  // (measured; within the conservative bound)
      double k = n.mat.emissive * n.mat.emissionStrength;
      for (int i = 0; i < 3; i++) p.color[i] = (float)(n.color[i] * k);
      p.range = 1e9f;
      p.area = p.emissive = true;
      em.push_back({k * (n.color[0] + n.color[1] + n.color[2]) * p.radius * p.radius, p});
    });
    std::sort(em.begin(), em.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [w, p] : em) if (out.size() < (size_t)MAX_LIGHTS) out.push_back(p);
  } else if (quality.emissiveLights) {
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

void App::startRenderThreads(const std::function<std::unique_ptr<Renderer>()>& make, std::function<bool(bool)> viewCtx,
                             std::function<bool(bool)> thumbCtx) {
  viewThread = std::make_unique<RenderThread>(make, std::move(viewCtx));
  thumbThread = std::make_unique<RenderThread>(make, std::move(thumbCtx));
}

void App::stopRenderThreads() {
  viewThread.reset();
  thumbThread.reset();
}

uint64_t App::viewportTexture(bool& flip) {
  if (viewThread) { flip = viewThread->flipY(); return viewThread->acquire().tex; }
  if (!renderer) return 0;
  flip = renderer->flipY();
  return renderer->texture(mainRT);
}

uint64_t App::thumbnailTexture(bool& flip) {
  if (thumbThread) { flip = thumbThread->flipY(); return thumbThread->acquire().tex; }
  if (!renderer) return 0;
  flip = renderer->flipY();
  return renderer->texture(thumbRT);
}

std::shared_ptr<const Node> App::sceneSnapshot() {
  // the tree as drawn (isolation shows a reduced tree); copied once per scene change
  std::string k = std::to_string(version_) + "|";
  for (auto& s : isolate) k += s + ";";
  if (!snap_ || k != snapKey_) {
    snapKey_ = k;
    snap_ = std::make_shared<const Node>(isolate.empty() ? root : viewRoot_);
    snapReg_ = std::make_shared<const PrimitiveRegistry>(reg);
  }
  return snap_;
}

void App::postView(RenderThread& t, int w, int h, const std::unordered_set<std::string>& hl, const FrameParams& fp, int samples, bool stochastic) {
  RenderRequest r;
  r.root = sceneSnapshot();
  r.reg = snapReg_;
  r.world = world;
  r.highlight = hl;
  r.fp = fp;
  r.quality = viewQuality();
  r.w = w;
  r.h = h;
  r.samples = std::max(1, samples);
  r.stochastic = stochastic;
  t.post(std::move(r));
}

void App::renderFrame() {
  if (!renderer) return;
  if (job.active) return;  // an image / video is rendering: the viewport keeps its last picture
  View v = currentView();
  bool interacting = drag_ != Drag::None && drag_ != Drag::Click;
  bool moving = (now - lastChange_) < 0.25 || interacting || modal != Modal::None || mode == Mode::Explore;
  const QualitySettings& q = viewQuality();
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
  fp.quality = &q;
  fp.light = light();
  fp.skyCol = worldColor();
  // path tracing where the image is refined (the idle viewport); moving / fast views stay simple
  fp.pathTrace = q.lighting == 1 && refine;
  {  // the scene's content (see FrameParams::sceneVersion): edits and isolation
    std::string sv = std::to_string(version_);
    for (auto& s : isolate) sv += "|" + s;
    fp.sceneVersion = std::hash<std::string>()(sv) | 1;
  }
  fp.lights = frameLights(fp.pathTrace);
  fp.prof = prof;
  for (auto& l : fp.lights) { for (double x : l.pos) key << (float)x << ','; key << l.color[0] << l.color[1] << l.color[2] << l.range << l.shadows << l.radius; }
  key << prof.maxSteps << prof.bounces << prof.textures << prof.specular << prof.pointLights << prof.shadows << prof.pointShadows << prof.ao
      << prof.aoSamples << prof.shadowSteps << q.sunIntensity << q.relaxedMarching << refine << q.sunSize << q.lightRadius << '|' << q.lighting
      << q.skyIntensity << fp.pathTrace << q.exposure << q.tonemap << q.denoise << q.denoiseStrength << q.light4D;
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
    if (viewThread) postView(*viewThread, (int)vpW, (int)vpH, highlightSet(), fp, refine ? q.viewportSamples : 1, refine);  // (refined there)
    else {
      Node& vr = isolate.empty() ? root : viewRoot_;
      if (fp.pathTrace && q.denoise) {  // the denoiser's guide (first hit's colour and depth)
        FrameParams g = fp;
        g.ptGuide = true;
        renderer->render(mainGuideRT, (int)vpW, (int)vpH, vr, reg, world, highlightSet(), g);
      }
      renderer->render(fp.pathTrace ? mainAccRT : mainRT, (int)vpW, (int)vpH, vr, reg, world, highlightSet(), fp);
      if (fp.pathTrace && !renderer->compiling()) renderer->present(mainAccRT, q.denoise ? &mainGuideRT : nullptr, mainTmpRT, mainRT, postSettings(q, 1));
      accumSamples = refine ? 1 : 0;
      if (renderer->compiling()) { lastKey_.clear(); accumSamples = 0; viewportCompiling = true; }  // not drawn yet: again next frame
      else viewportCompiling = false;
    }
  } else if (refine && accumSamples < q.viewportSamples && !viewThread) {
    // the same view: add one more random sample to the running average
    fp.stochastic = true;
    fp.sample = accumSamples;
    fp.blend = 1.0f / (accumSamples + 1);
    renderer->render(fp.pathTrace ? mainAccRT : mainRT, (int)vpW, (int)vpH, isolate.empty() ? root : viewRoot_, reg, world, highlightSet(), fp);
    if (fp.pathTrace) renderer->present(mainAccRT, q.denoise ? &mainGuideRT : nullptr, mainTmpRT, mainRT, postSettings(q, accumSamples + 1));
    accumSamples++;
  }
  if (viewThread) {
    RenderThread::Image im = viewThread->acquire();
    accumSamples = refine ? im.samples : 0;
    viewportCompiling = viewThread->compiling();
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
    tp.sceneVersion = fp.sceneVersion;
    std::ostringstream tk;
    tk << k << "|ball" << thumbYaw << thumbPitch << thumbZoom << thumbW << thumbH;
    if (tk.str() != lastThumbKey_) {
      lastThumbKey_ = tk.str();
      if (thumbThread) postView(*thumbThread, thumbW, thumbH, {}, tp, 1, false);
      else {
        renderer->render(thumbRT, thumbW, thumbH, isolate.empty() ? root : viewRoot_, reg, world, {}, tp);
        if (renderer->compiling()) lastThumbKey_.clear();
      }
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
    tp.sceneVersion = fp.sceneVersion;
    std::ostringstream tk;
    tk << k << thumbYaw << thumbPitch << thumbZoom << thumbW << thumbH;
    if (tk.str() != lastThumbKey_) {
      lastThumbKey_ = tk.str();
      if (thumbThread) postView(*thumbThread, thumbW, thumbH, {}, tp, 1, false);
      else {
        renderer->render(thumbRT, thumbW, thumbH, isolate.empty() ? root : viewRoot_, reg, world, {}, tp);
        if (renderer->compiling()) lastThumbKey_.clear();
      }
    }
  }
}

}  // namespace fd
