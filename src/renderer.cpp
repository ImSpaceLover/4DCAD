#include "renderer.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fd {

std::string Renderer::checkCustomCode(const std::string& code) {
  auto it = codeErrors_.find(code);
  if (it != codeErrors_.end()) return it->second;
  // a tiny fragment shader using the code (same prelude as the real shader)
  std::string src = shaderPrelude(shaderTarget(), false) + sdfHelpersGLSL() + renameCustomCode(code, "_t") +
                    "\nvoid main() { fragColor = vec4(sdf_t(vec4(v_uv, 0.0, 1.0))); }\n";
  std::string err = compileCheck(src);
  if (err.empty() && code.find("sdf") == std::string::npos) err = "the code must define: float sdf(vec4 p)";
  codeErrors_[code] = err;
  if (!err.empty()) badCode_.insert(code);
  return err;
}

void Renderer::clearPrograms() {
  for (auto& [k, p] : cache_) destroyProgram(p.handle);
  cache_.clear();
  lru_.clear();
}

Renderer::Program* Renderer::get(Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo) {
  // custom SDFs that do not compile are replaced by their bounding box
  walk(root, [&](Node& n, Node*, int) { if (n.isCustom()) checkCustomCode(n.code); });
  std::string key = structureKey(root, reg, world, geo);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    lru_.remove(key);
    lru_.push_front(key);
    return &it->second;
  }
  if (failed_.count(key)) return nullptr;
  auto t0 = std::chrono::steady_clock::now();
  Program p;
  p.cs = compileScene(root, reg, world, geo, &badCode_);
  std::string err;
  p.handle = createProgram(buildFragmentShader(p.cs.glsl, geo, shaderTarget()), err);
  if (!p.handle) {
    lastError = err;
    std::fprintf(stderr, "Shader error:\n%s\n", err.c_str());
    failed_.insert(key);
    return nullptr;
  }
  lastCompileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  lastError.clear();
  while (lru_.size() >= 8) {
    destroyProgram(cache_[lru_.back()].handle);
    cache_.erase(lru_.back());
    lru_.pop_back();
  }
  lru_.push_front(key);
  return &(cache_[key] = std::move(p));
}

static void put4(float* d, const V4& v) { for (int i = 0; i < 4; i++) d[i] = (float)v[i]; }
static void put3(float* d, const std::array<float, 3>& v, float w = 0) { d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = w; }

FrameUniforms frameUniforms(const FrameParams& fp, int w, int h) {
  FrameUniforms u;
  std::memset(&u, 0, sizeof u);
  const QualitySettings& q = *fp.quality;
  for (int i = 0; i < 16; i++) { u.frame[i] = (float)fp.view.frame[i]; u.axes[i] = (float)fp.axes[i]; }
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
  float a[4] = {(float)w, (float)h, (float)std::tan(fp.fovDeg * PI / 360), fp.geo ? q.maxDistGeo : q.maxDistFull};
  float b[4] = {q.eps, q.stepScale, q.shadowK, q.shadowMax};
  float c[4] = {q.aoStrength, q.fog, q.ambient, q.geoMaxStep};
  float d[4] = {fp.markerSkip, q.sunIntensity, 0, 0};
  std::memcpy(u.a, a, sizeof a);
  std::memcpy(u.b, b, sizeof b);
  std::memcpy(u.c, c, sizeof c);
  std::memcpy(u.d, d, sizeof d);
  // point lights
  int nl = 0, mask = 0;
  for (auto& l : fp.lights) {
    if (nl >= MAX_LIGHTS) break;
    put4(u.lpos[nl], l.pos);
    put3(u.lcol[nl], l.color, l.range);
    if (l.shadows && fp.pointShadows) mask |= 1 << nl;
    nl++;
  }
  // debug ray: resampled to at most MAX_DEBUG_PTS points
  int n = (int)std::min<size_t>(fp.debugPts.size(), MAX_DEBUG_PTS);
  for (int i = 0; i < n; i++) {
    size_t src = fp.debugPts.size() <= (size_t)MAX_DEBUG_PTS ? (size_t)i
                                                             : (size_t)std::llround((double)i * (fp.debugPts.size() - 1) / (MAX_DEBUG_PTS - 1));
    put4(u.dbg[i], fp.debugPts[src]);
  }
  int32_t ii[4] = {q.maxSteps, (q.shadows && fp.shadows) ? 1 : 0, (q.ao && fp.ao) ? 1 : 0, fp.grid ? 1 : 0};
  int32_t jj[4] = {q.geoProjectIters, n >= 2 ? n : 0, nl, fp.simple ? 1 : 0};
  int32_t kk[4] = {mask, fp.simple ? 0 : q.transparentLayers, q.relaxedMarching ? 1 : 0, 0};
  std::memcpy(u.i, ii, sizeof ii);
  std::memcpy(u.j, jj, sizeof jj);
  std::memcpy(u.k, kk, sizeof kk);
  return u;
}

bool Renderer::render(RenderTarget& rt, int w, int h, Node& root, const PrimitiveRegistry& reg, const std::string& world,
                      const std::unordered_set<std::string>& highlight, const FrameParams& fp) {
  int rw = std::max(1, (int)(w * fp.scale)), rh = std::max(1, (int)(h * fp.scale));
  Program* p = get(root, reg, world, fp.geo);
  FrameUniforms u = frameUniforms(fp, rw, rh);
  if (!p) {
    draw(rt, rw, rh, nullptr, u, {}, 0);  // clears the target to an error colour
    return false;
  }
  std::vector<float> nodes = packNodes(root, reg, p->cs, highlight);
  draw(rt, rw, rh, p->handle, u, nodes, (int)p->cs.nodeIds.size());
  return true;
}

}  // namespace fd
