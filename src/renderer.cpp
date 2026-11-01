#include "renderer.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

#include "shaders.hpp"

namespace fd {

void RenderTarget::ensure(int nw, int nh) {
  nw = std::max(1, nw);
  nh = std::max(1, nh);
  if (fbo && nw == w && nh == h) return;
  if (!fbo) { gl::GenFramebuffers(1, &fbo); gl::GenTextures(1, &tex); }
  w = nw; h = nh;
  gl::BindTexture(GL_TEXTURE_2D, tex);
  gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderTarget::destroy() {
  if (fbo) { gl::DeleteFramebuffers(1, &fbo); gl::DeleteTextures(1, &tex); }
  fbo = tex = 0; w = h = 0;
}

Renderer::Renderer() {
  float tri[6] = {-1, -1, 3, -1, -1, 3};
  gl::GenVertexArrays(1, &vao_);
  gl::BindVertexArray(vao_);
  gl::GenBuffers(1, &vbo_);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
  gl::BufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
  gl::EnableVertexAttribArray(0);
  gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  gl::BindVertexArray(0);
}

Renderer::~Renderer() {
  for (auto& [k, p] : cache_) gl::DeleteProgram(p.id);
}

GLuint compileShader(GLenum type, const std::string& src, std::string& err) {
  GLuint sh = gl::CreateShader(type);
  const char* s = src.c_str();
  gl::ShaderSource(sh, 1, &s, nullptr);
  gl::CompileShader(sh);
  GLint ok = 0;
  gl::GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    gl::GetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log(std::max(1, len));
    gl::GetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
    err = log.data();
    gl::DeleteShader(sh);
    return 0;
  }
  return sh;
}

std::string Renderer::checkCustomCode(const std::string& code) {
  auto it = codeErrors_.find(code);
  if (it != codeErrors_.end()) return it->second;
  std::string src = std::string("#version 330 core\n") + sdfHelpersGLSL() + renameCustomCode(code, "_t") +
                    "\nout vec4 fragColor;\nvoid main() { fragColor = vec4(sdf_t(vec4(gl_FragCoord.xy, 0.0, 1.0))); }\n";
  std::string err;
  GLuint sh = compileShader(GL_FRAGMENT_SHADER, src, err);
  if (sh) gl::DeleteShader(sh);
  else if (err.empty()) err = "compile error";
  if (sh && code.find("sdf") == std::string::npos) err = "the code must define: float sdf(vec4 p)";
  codeErrors_[code] = err;
  if (!err.empty()) badCode_.insert(code);
  return err;
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
  GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SHADER, err);
  GLuint fs = vs ? compileShader(GL_FRAGMENT_SHADER, buildFragmentShader(p.cs.glsl, geo), err) : 0;
  if (!vs || !fs) {
    lastError = err;
    std::fprintf(stderr, "Shader error:\n%s\n", err.c_str());
    failed_.insert(key);
    return nullptr;
  }
  p.id = gl::CreateProgram();
  gl::AttachShader(p.id, vs);
  gl::AttachShader(p.id, fs);
  gl::LinkProgram(p.id);
  gl::DeleteShader(vs);
  gl::DeleteShader(fs);
  GLint ok = 0;
  gl::GetProgramiv(p.id, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096] = {0};
    gl::GetProgramInfoLog(p.id, sizeof log, nullptr, log);
    lastError = log;
    gl::DeleteProgram(p.id);
    failed_.insert(key);
    return nullptr;
  }
  lastCompileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  lastError.clear();
  while (lru_.size() >= 8) {
    auto& old = cache_[lru_.back()];
    gl::DeleteProgram(old.id);
    cache_.erase(lru_.back());
    lru_.pop_back();
  }
  lru_.push_front(key);
  return &(cache_[key] = std::move(p));
}

GLint Renderer::u(Program& p, const char* name) {
  auto it = p.loc.find(name);
  if (it != p.loc.end()) return it->second;
  GLint l = gl::GetUniformLocation(p.id, name);
  p.loc[name] = l;
  return l;
}

bool Renderer::render(RenderTarget& rt, int w, int h, Node& root, const PrimitiveRegistry& reg, const std::string& world,
                      const std::unordered_set<std::string>& highlight, const FrameParams& fp) {
  rt.ensure((int)(w * fp.scale), (int)(h * fp.scale));
  gl::BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
  gl::Viewport(0, 0, rt.w, rt.h);
  Program* p = get(root, reg, world, fp.geo);
  if (!p) {
    gl::ClearColor(0.3f, 0.05f, 0.05f, 1);
    gl::Clear(GL_COLOR_BUFFER_BIT);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    return false;
  }
  gl::UseProgram(p->id);
  auto pk = packUniforms(root, reg, p->cs, highlight);
  GLsizei N = (GLsizei)p->cs.nodeIds.size();
  gl::UniformMatrix4fv(u(*p, "u_rot"), N, GL_FALSE, pk.rot.data());
  gl::Uniform4fv(u(*p, "u_pos"), N, pk.pos.data());
  gl::Uniform4fv(u(*p, "u_par"), N, pk.par.data());
  gl::Uniform4fv(u(*p, "u_col"), N, pk.col.data());
  gl::Uniform4fv(u(*p, "u_bc"), N, pk.bc.data());
  gl::Uniform1fv(u(*p, "u_br"), N, pk.br.data());
  gl::Uniform4fv(u(*p, "u_mat"), N, pk.mat.data());
  gl::Uniform4fv(u(*p, "u_col2"), N, pk.col2.data());
  const QualitySettings& q = *fp.quality;
  gl::Uniform2f(u(*p, "u_res"), (float)rt.w, (float)rt.h);
  float eye[4], fr[16], light[4], mk[4];
  for (int i = 0; i < 4; i++) { eye[i] = (float)fp.view.eye[i]; light[i] = (float)fp.light[i]; mk[i] = (float)fp.marker[i]; }
  for (int i = 0; i < 16; i++) fr[i] = (float)fp.view.frame[i];
  gl::Uniform4fv(u(*p, "u_eye"), 1, eye);
  gl::UniformMatrix4fv(u(*p, "u_frame"), 1, GL_FALSE, fr);
  gl::Uniform1f(u(*p, "u_tanHalf"), (float)std::tan(fp.fovDeg * PI / 360));
  gl::Uniform1i(u(*p, "u_maxSteps"), q.maxSteps);
  gl::Uniform1f(u(*p, "u_maxDist"), fp.geo ? q.maxDistGeo : q.maxDistFull);
  gl::Uniform1f(u(*p, "u_eps"), q.eps);
  gl::Uniform1f(u(*p, "u_stepScale"), q.stepScale);
  gl::Uniform1f(u(*p, "u_shadowK"), q.shadowK);
  gl::Uniform1f(u(*p, "u_shadowMax"), q.shadowMax);
  gl::Uniform1f(u(*p, "u_aoStrength"), q.aoStrength);
  gl::Uniform1f(u(*p, "u_fog"), q.fog);
  gl::Uniform1f(u(*p, "u_ambient"), q.ambient);
  gl::Uniform3f(u(*p, "u_skyTop"), q.skyTop[0], q.skyTop[1], q.skyTop[2]);
  gl::Uniform3f(u(*p, "u_skyBottom"), q.skyBottom[0], q.skyBottom[1], q.skyBottom[2]);
  gl::Uniform1i(u(*p, "u_shadows"), q.shadows ? 1 : 0);
  gl::Uniform1i(u(*p, "u_ao"), q.ao ? 1 : 0);
  gl::Uniform1i(u(*p, "u_grid"), fp.grid ? 1 : 0);
  gl::Uniform1f(u(*p, "u_geoMaxStep"), q.geoMaxStep);
  gl::Uniform1i(u(*p, "u_geoIters"), q.geoProjectIters);
  gl::Uniform4fv(u(*p, "u_light"), 1, light);
  gl::Uniform4fv(u(*p, "u_marker"), 1, mk);
  gl::Uniform4fv(u(*p, "u_markerCol"), 1, fp.markerCol.data());
  float mf[4];
  for (int i = 0; i < 4; i++) mf[i] = (float)fp.markerFwd[i];
  gl::Uniform4fv(u(*p, "u_markerFwd"), 1, mf);
  gl::Uniform1f(u(*p, "u_markerSkip"), fp.markerSkip);
  gl::Uniform3f(u(*p, "u_skyCol"), fp.skyCol[0], fp.skyCol[1], fp.skyCol[2]);
  gl::Disable(GL_SCISSOR_TEST);
  gl::Disable(GL_BLEND);
  gl::Disable(GL_DEPTH_TEST);
  gl::BindVertexArray(vao_);
  gl::DrawArrays(GL_TRIANGLES, 0, 3);
  gl::BindVertexArray(0);
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

}  // namespace fd
