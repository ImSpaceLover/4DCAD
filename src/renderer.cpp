#include "renderer.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fd {

// ---------------------------------------------------------------- shader disk cache
std::string Renderer::cachePath(const std::string& key, const char* ext) const {
  if (cacheDir_.empty()) return "";
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
  char name[40];
  std::snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)h, ext);
  return (std::filesystem::path(cacheDir_) / name).string();
}

bool Renderer::cacheRead(const std::string& path, std::vector<uint8_t>& out) const {
  if (path.empty()) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

void Renderer::cacheWrite(const std::string& path, const void* data, size_t bytes) const {
  if (path.empty() || !bytes) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(cacheDir_, ec);
  // written under a temporary name, then renamed: other threads / runs never read half a file
  static std::atomic<unsigned> counter{0};
  std::string tmp = path + ".tmp" + std::to_string(counter++);
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return;
    out.write((const char*)data, (std::streamsize)bytes);
  }
  fs::rename(tmp, path, ec);
  if (ec) fs::remove(tmp, ec);
  // keep the folder small: the oldest files go beyond 400
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  for (auto& e : fs::directory_iterator(cacheDir_, ec)) files.push_back({e.last_write_time(ec), e.path()});
  if (files.size() > 400) {
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + 400 < files.size(); i++) fs::remove(files[i].second, ec);
  }
}

UiFrameClock& uiFrames() {
  static UiFrameClock c;
  return c;
}

constexpr size_t PROGRAM_CACHE = 16;  // programs kept (values are node data: entries are real structures)

std::string Renderer::codeCheckSource(const std::string& code) {
  // a tiny fragment shader using the code (same prelude as the real shader)
  return shaderPrelude(shaderTarget(), false) + sdfHelpersGLSL() + guardLoops(renameCustomCode(code, "_t")) +
         "\nvoid main() { fragColor = vec4(sdf_t(vec4(v_uv, 0.0, 1.0))); }\n";
}

void Renderer::applyCodeResult(const std::string& code, std::string err) {
  if (err.empty() && code.find("sdf") == std::string::npos) err = "the code must define: float sdf(vec4 p)";
  codeErrors_[code] = err;
  if (!err.empty()) badCode_.insert(code);
}

std::string Renderer::checkCustomCode(const std::string& code, bool wait) {
  pollCompiles();
  auto it = codeErrors_.find(code);
  if (it != codeErrors_.end()) return it->second;
  if (async_ && workerOk_ && !wait) {  // checked in the background; the editor shows the result when it is there
    if (!pendingCode_.count(code)) { pendingCode_.insert(code); enqueue({"", codeCheckSource(code), code}); }
    return "";
  }
  applyCodeResult(code, compileCheck(codeCheckSource(code)));
  return codeErrors_[code];
}

void Renderer::clearPrograms() {
  for (auto& [k, p] : cache_) destroyProgram(p.handle);
  cache_.clear();
  lru_.clear();
}

// ---------------------------------------------------------------- background compiles
void Renderer::enqueue(Job j) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!worker_.joinable()) worker_ = std::thread([this] { workerLoop(); });
  jobs_.push_back(std::move(j));
  cv_.notify_all();
}

void Renderer::workerLoop() {
  bool ctx = workerBegin();
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
      if (stop_) break;
      j = std::move(jobs_.front());
      jobs_.pop_front();
    }
    Result r;
    r.job = std::move(j);
    if (!ctx) r.err = "\x01";  // no worker context: the calling thread compiles it
    else {
      auto t0 = std::chrono::steady_clock::now();
      if (r.job.key.empty()) r.err = compileCheck(r.job.source);
      else {
        r.handle = createProgram(r.job.source, r.err);
        if (r.handle) workerDone();
      }
      r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    std::lock_guard<std::mutex> lk(mu_);
    done_.push_back(std::move(r));
  }
  if (ctx) workerEnd();
}

void Renderer::stopWorker() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
    cv_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  for (auto& r : done_) if (r.handle) destroyProgram(r.handle);
  done_.clear();
  jobs_.clear();
}

void Renderer::insert(const std::string& key, Program p) {
  while (lru_.size() >= PROGRAM_CACHE) {
    destroyProgram(cache_[lru_.back()].handle);
    cache_.erase(lru_.back());
    lru_.pop_back();
  }
  lru_.push_front(key);
  cache_[key] = std::move(p);
}

void Renderer::pollCompiles() {
  std::vector<Result> rs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (done_.empty()) return;
    rs.swap(done_);
  }
  for (auto& r : rs) {
    if (r.err == "\x01") {  // the worker could not compile (no context): from now on, here
      workerOk_ = false;
      if (r.job.key.empty()) { applyCodeResult(r.job.code, compileCheck(r.job.source)); pendingCode_.erase(r.job.code); }
      else pending_.erase(r.job.key);  // asked again by the next render (synchronously)
      continue;
    }
    if (r.job.key.empty()) {
      applyCodeResult(r.job.code, r.err);
      pendingCode_.erase(r.job.code);
      continue;
    }
    auto pit = pending_.find(r.job.key);
    if (!r.handle) {
      lastError = r.err;
      std::fprintf(stderr, "Shader error:\n%s\n", r.err.c_str());
      failed_.insert(r.job.key);
    } else if (pit != pending_.end()) {
      lastCompileMs = r.ms;
      lastError.clear();
      Program p;
      p.handle = r.handle;
      p.cs = std::move(pit->second);
      insert(r.job.key, std::move(p));
    } else destroyProgram(r.handle);
    if (pit != pending_.end()) pending_.erase(pit);
  }
}

Renderer::Program* Renderer::get(Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo, SpaceKind space, bool ball,
                                 int pt, bool& pending) {
  pending = false;
  pollCompiles();
  if (space == SpaceKind::Flat) ball = false;
  if (space != SpaceKind::Flat) geo = false;
  // custom SDFs that do not compile are replaced by their bounding box (so their checks come first)
  bool codeWait = false;
  walk(root, [&](Node& n, Node*, int) {
    if (n.isCustom()) { checkCustomCode(n.code); if (pendingCode_.count(n.code)) codeWait = true; }
  });
  if (codeWait) { pending = true; return nullptr; }
  if (ball) pt = 0;
  std::string key = structureKey(root, reg, world, geo, space) + (ball ? "|ball" : "") + (pt == 1 ? "|pt" : pt == 2 ? "|ptg" : "");
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    lru_.remove(key);
    lru_.push_front(key);
    return &it->second;
  }
  if (failed_.count(key)) return nullptr;
  if (pending_.count(key)) { pending = true; return nullptr; }
  auto t0 = std::chrono::steady_clock::now();
  CompiledScene cs = compileScene(root, reg, world, geo, &badCode_, space);
  std::string src = buildFragmentShader(cs.glsl, geo, shaderTarget(), (int)space, ball, pt > 0, pt == 2);
  if (async_ && workerOk_) {
    pending_[key] = std::move(cs);
    enqueue({key, std::move(src), ""});
    pending = true;
    return nullptr;
  }
  Program p;
  p.cs = std::move(cs);
  std::string err;
  compilingNow_ = true;
  p.handle = createProgram(src, err);
  compilingNow_ = false;
  if (!p.handle) {
    lastError = err;
    std::fprintf(stderr, "Shader error:\n%s\n", err.c_str());
    failed_.insert(key);
    return nullptr;
  }
  lastCompileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  lastError.clear();
  insert(key, std::move(p));
  return &cache_[key];
}

void Renderer::present(const RenderTarget& acc, const RenderTarget* guide, RenderTarget* tmp, RenderTarget& dst, const PostSettings& s) {
  const RenderTarget* cur = &acc;
  int i = 0;
  if (s.denoise > 0 && guide && guide->id >= 0 && s.strength > 0) {
    // the colour tolerance shrinks as samples add up (less noise to remove)
    float sc = 0.9f * s.strength / std::sqrt((float)std::max(1, s.samples));
    if (s.denoise == 3 || s.denoise == 4) {
      // median of each 3x3 neighbourhood (by brightness, within the same surface): removes single
      // bright / dark pixels (fireflies) and keeps edges
      float p[8] = {1, 0, 0.12f, 0.04f, 0, 0, 0, 0};
      post(POST_MEDIAN, *cur, guide, tmp[i], p);
      cur = &tmp[i];
      i ^= 1;
    }
    if (s.denoise == 1 || s.denoise == 4) {
      // edge-aware a-trous wavelet filter: 5 passes with growing steps (1 .. 16 pixels)
      for (int step = 1; step <= 16; step *= 2) {
        float p[8] = {(float)step, sc, 0.12f, 0.04f, 0, 0, 0, 0};
        post(POST_ATROUS, *cur, guide, tmp[i], p);
        cur = &tmp[i];
        i ^= 1;
        sc *= 0.7f;
      }
    }
    if (s.denoise == 2) {
      // bilateral: one wide pass (radius 2.5 x strength pixels), stopped by colour, surface and depth
      // differences: smooth noise inside surfaces, less blur than the wavelet filter at fine detail
      float p[8] = {std::clamp(2.5f * s.strength, 1.f, 6.f), sc * 1.3f, 0.12f, 0.04f, 0, 0, 0, 0};
      post(POST_BILATERAL, *cur, guide, tmp[i], p);
      cur = &tmp[i];
      i ^= 1;
    }
  }
  float p[8] = {std::exp2(s.exposure), (float)s.tonemap, s.encoded ? 1.f : 0.f, 0, 0, 0, 0, 0};
  post(POST_RESOLVE, *cur, nullptr, dst, p);
}

static void put4(float* d, const V4& v) { for (int i = 0; i < 4; i++) d[i] = (float)v[i]; }
static void put3(float* d, const std::array<float, 3>& v, float w = 0) { d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = w; }

FrameUniforms frameUniforms(const FrameParams& fp, int w, int h) {
  FrameUniforms u;
  std::memset(&u, 0, sizeof u);
  const QualitySettings& q = *fp.quality;
  bool curved = fp.space.curved();
  for (int i = 0; i < 16; i++) { u.frame[i] = (float)fp.view.frame[i]; u.axes[i] = (float)(curved ? fp.camIso[i] : fp.axes[i]); }
  put4(u.eye, fp.view.eye);
  put4(u.light, fp.light);
  put4(u.marker, fp.marker);
  for (int i = 0; i < 4; i++) u.markerCol[i] = fp.markerCol[i];
  put4(u.markerFwd, fp.markerFwd);
  put3(u.skyTop, q.skyTop);
  put3(u.skyBottom, q.skyBottom);
  put3(u.skyCol, fp.skyCol);
  static const float rc[4][3] = {{1.0f, 0.25f, 0.2f}, {0.3f, 0.6f, 1.0f}, {1.0f, 0.9f, 0.2f}, {0.8f, 0.8f, 0.8f}};
  int r = std::clamp(fp.debugReason, 0, 3);
  for (int i = 0; i < 3; i++) u.dbgEndCol[i] = rc[r][i];
  float maxDist = fp.geo ? q.maxDistGeo : q.maxDistFull;
  // hyperbolic: coordinates grow like e^(d/R), floats give out beyond ~12 R (where anything is far
  // below a pixel anyway); spherical: a ray has gone all the way round after 2 pi R
  // (honeycombs fold far points through many reflections: they need a shorter reach)
  if (fp.space.kind == SpaceKind::Hyperbolic) maxDist = std::min(maxDist, (float)((fp.honeycomb ? 7 : 12) * fp.space.R()));
  if (fp.space.kind == SpaceKind::Spherical) maxDist = std::min(maxDist, (float)(2 * PI * fp.space.R()));
  float a[4] = {(float)w, (float)h, (float)std::tan(fp.fovDeg * PI / 360), maxDist};
  float b[4] = {q.eps, q.stepScale, q.shadowK, q.shadowMax};
  float c[4] = {q.aoStrength, q.fog, q.ambient, q.geoMaxStep};
  float d[4] = {fp.markerSkip, q.sunIntensity, q.sunSize, q.lightRadius};
  std::memcpy(u.a, a, sizeof a);
  std::memcpy(u.b, b, sizeof b);
  std::memcpy(u.c, c, sizeof c);
  std::memcpy(u.d, d, sizeof d);
  // point lights
  int nl = 0, mask = 0;
  for (auto& l : fp.lights) {
    if (nl >= MAX_LIGHTS) break;
    if (!l.area && !fp.prof.pointLights) continue;  // (area lights: path tracing, always on)
    put4(u.lpos[nl], l.pos);
    put3(u.lcol[nl], l.color, l.range);
    u.lext[nl][0] = l.area ? l.radius : q.lightRadius;
    u.lext[nl][1] = l.area ? 1.f : 0.f;
    // emissive objects' lights always cast shadows (they are what would shine through walls)
    if (l.shadows && (fp.prof.pointShadows || l.emissive)) mask |= 1 << nl;
    nl++;
  }
  // debug ray: resampled to at most MAX_DEBUG_PTS points
  int n = (int)std::min<size_t>(fp.debugPts.size(), MAX_DEBUG_PTS);
  for (int i = 0; i < n; i++) {
    size_t src = fp.debugPts.size() <= (size_t)MAX_DEBUG_PTS ? (size_t)i
                                                             : (size_t)std::llround((double)i * (fp.debugPts.size() - 1) / (MAX_DEBUG_PTS - 1));
    put4(u.dbg[i], fp.debugPts[src]);
  }
  const RenderProfile& p = fp.prof;
  int32_t ii[4] = {std::max(8, p.maxSteps), p.shadows ? 1 : 0, p.ao ? 1 : 0, fp.grid ? 1 : 0};
  int32_t jj[4] = {q.geoProjectIters, n >= 2 ? n : 0, nl, p.textures ? 1 : 0};
  int32_t kk[4] = {mask, std::clamp(p.bounces, 0, 8), q.relaxedMarching ? 1 : 0, std::clamp(p.aoSamples, 1, 5)};
  int32_t ll[4] = {std::clamp(p.shadowSteps, 4, 256), p.specular ? 1 : 0, fp.stochastic ? 1 : 0, fp.sample};
  float ee[4] = {fp.stochastic ? fp.aperture : 0.f, std::max(0.01f, fp.focus), std::clamp(fp.blend, 0.f, 1.f), (float)fp.space.R()};
  std::memcpy(u.e, ee, sizeof ee);
  u.f[0] = std::max(0.f, q.skyIntensity);
  u.f[1] = q.light4D == 1 ? 1.f : 0.f;
  u.f[2] = (float)std::clamp(q.debugView, 0, VIEW_COUNT - 1);
  u.f[3] = std::max(1.f, q.costScale);
  std::memcpy(u.i, ii, sizeof ii);
  std::memcpy(u.j, jj, sizeof jj);
  std::memcpy(u.k, kk, sizeof kk);
  std::memcpy(u.l, ll, sizeof ll);
  return u;
}

bool Renderer::render(RenderTarget& rt, int w, int h, Node& root, const PrimitiveRegistry& reg, const std::string& world,
                      const std::unordered_set<std::string>& highlight, const FrameParams& fp) {
  int rw = std::max(1, (int)(w * fp.scale)), rh = std::max(1, (int)(h * fp.scale));
  bool pending = false;
  Program* p = get(root, reg, world, fp.geo, fp.space.kind, fp.ballView, fp.pathTrace ? (fp.ptGuide ? 2 : 1) : 0, pending);
  compiling_ = pending;
  if (pending) {
    // the program is being compiled: keep the last image (a target that does not exist yet is
    // cleared once so there is something to show)
    if (rt.id < 0) draw(rt, rw, rh, nullptr, frameUniforms(fp, rw, rh), {}, -1);
    return true;
  }
  FrameParams fpx = fp;
  walk(root, [&](Node& n, Node*, int) { if (n.isHoneycomb() && !n.hidden) fpx.honeycomb = true; });
  FrameUniforms u = frameUniforms(fpx, rw, rh);
  if (!p) {
    draw(rt, rw, rh, nullptr, u, {}, 0);  // clears the target to an error colour
    return false;
  }
  // the node data: the same as for the previous draw when nothing it depends on changed (tiles of a
  // render, refinement samples, the guide pass): the scene, the program's node order, the
  // highlighted objects and, in curved spaces, the camera (the data is camera-relative there)
  uint64_t key = 0;
  if (fp.sceneVersion) {
    auto mix = [&](uint64_t v) { key ^= v + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2); };
    mix(fp.sceneVersion);
    mix((uint64_t)(uintptr_t)p);
    uint64_t hl = 0;
    for (auto& id : highlight) hl ^= std::hash<std::string>()(id);  // (order independent)
    mix(hl);
    mix((uint64_t)fp.space.kind);
    if (fp.space.curved()) {
      mix(std::hash<double>()(fp.space.radius));
      for (double x : fp.camIso) mix(std::hash<double>()(x));
    }
    mix(1);  // (never 0)
  }
  nodesChanged_ = !key || key != lastNodesKey_;
  if (nodesChanged_) {
    lastNodes_ = packNodes(root, reg, p->cs, highlight, &fp.space, &fp.camIso);
    lastNodesKey_ = key;
  }
  const std::vector<float>& nodes = lastNodes_;
  for (int i = 0; i < 4; i++) scissor_[i] = fp.tile[i];
  if (scissor_[2] > 0) {  // clamp to the target
    scissor_[0] = std::clamp(scissor_[0], 0, rw); scissor_[1] = std::clamp(scissor_[1], 0, rh);
    scissor_[2] = std::clamp(scissor_[2], 1, rw - scissor_[0]); scissor_[3] = std::clamp(scissor_[3], 1, rh - scissor_[1]);
  }
  draw(rt, rw, rh, p->handle, u, nodes, (int)(nodes.size() / (NODE_VEC4S * 4)));  // (+ extra rows: follow paths)
  return true;
}

}  // namespace fd
