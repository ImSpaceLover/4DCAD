// OpenGL 3.3 backend: programs, render targets (FBO + texture), frame uniform buffer and
// the node data texture (one RGBA32F row of 12 texels per node).
#include <cstdio>
#include <vector>

#include "gl.hpp"
#include "renderer.hpp"

namespace fd {

namespace {

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
    if (err.empty()) err = "compile error";
    gl::DeleteShader(sh);
    return 0;
  }
  return sh;
}

struct GLProgram {
  GLuint id = 0;
  GLint nodesLoc = -1;
};

struct GLTarget {
  GLuint fbo = 0, tex = 0;
  int w = 0, h = 0;
};

class GLRenderer : public Renderer {
 public:
  explicit GLRenderer(std::function<bool(bool)> wc) : workerCtx_(std::move(wc)) {
    gl::GenVertexArrays(1, &vao_);  // the full-screen triangle needs no vertex data
    gl::GenBuffers(1, &ubo_);
    gl::BindBuffer(GL_UNIFORM_BUFFER, ubo_);
    gl::BufferData(GL_UNIFORM_BUFFER, sizeof(FrameUniforms), nullptr, GL_DYNAMIC_DRAW);
    gl::BindBuffer(GL_UNIFORM_BUFFER, 0);
    gl::GenTextures(1, &nodeTex_);
    gl::BindTexture(GL_TEXTURE_2D, nodeTex_);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const GLubyte* r = gl::GetString(GL_RENDERER);
    const GLubyte* v = gl::GetString(GL_VERSION);
    device = std::string("OpenGL ") + (v ? (const char*)v : "") + " | " + (r ? (const char*)r : "");
  }
  ~GLRenderer() override {
    stopWorker();
    clearPrograms();
    for (auto& t : targets_) if (t.fbo) { gl::DeleteFramebuffers(1, &t.fbo); gl::DeleteTextures(1, &t.tex); }
    gl::DeleteTextures(1, &nodeTex_);
    gl::DeleteBuffers(1, &ubo_);
    gl::DeleteVertexArrays(1, &vao_);
  }

  uint64_t texture(const RenderTarget& rt) override { return rt.id >= 0 ? targets_[rt.id].tex : 0; }
  bool flipY() const override { return true; }
  const char* backendName() const override { return "OpenGL"; }
  void finish() override { gl::Finish(); }
  void flush() override { gl::Flush(); }
  void copyTarget(const RenderTarget& src, RenderTarget& dst) override {
    if (src.id < 0) return;
    GLTarget& s = targets_[src.id];
    GLTarget& d = ensure(dst, s.w, s.h);
    GLTarget& s2 = targets_[src.id];  // (ensure may have grown the list)
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, s2.fbo);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fbo);
    gl::BlitFramebuffer(0, 0, s2.w, s2.h, 0, 0, d.w, d.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  }

 protected:
  // background compiles: the worker thread uses a hidden context of the same share group
  // (programs are shared objects); glFinish makes a linked program usable by the other contexts
  bool workerBegin() override { return workerCtx_ && workerCtx_(true); }
  void workerDone() override { gl::Finish(); }
  void workerEnd() override { if (workerCtx_) workerCtx_(false); }

 public:
  bool readPixels(const RenderTarget& rt, Pixels& out) override {
    if (rt.id < 0 || rt.id >= (int)targets_.size() || !targets_[rt.id].fbo) return false;
    GLTarget& t = targets_[rt.id];
    std::vector<float> f((size_t)t.w * t.h * 4);
    gl::BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl::ReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_FLOAT, f.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    out.resize(f.size());
    for (int y = 0; y < t.h; y++)  // OpenGL rows are bottom-up
      for (int i = 0; i < t.w * 4; i++)
        out[(size_t)y * t.w * 4 + i] = (uint8_t)std::lround(std::clamp(f[(size_t)(t.h - 1 - y) * t.w * 4 + i], 0.f, 1.f) * 255);
    return true;
  }

  void destroy(RenderTarget& rt) override {
    if (rt.id < 0) return;
    GLTarget& t = targets_[rt.id];
    if (t.fbo) { gl::DeleteFramebuffers(1, &t.fbo); gl::DeleteTextures(1, &t.tex); }
    t = GLTarget{};
    rt = RenderTarget{};
  }

 protected:
  ShaderTarget shaderTarget() const override { return ShaderTarget::OpenGL; }

  void* createProgram(const std::string& fs, std::string& err) override {
    GLuint v = compileShader(GL_VERTEX_SHADER, vertexShader(ShaderTarget::OpenGL), err);
    GLuint f = v ? compileShader(GL_FRAGMENT_SHADER, fs, err) : 0;
    if (!v || !f) { if (v) gl::DeleteShader(v); return nullptr; }
    GLuint id = gl::CreateProgram();
    gl::AttachShader(id, v);
    gl::AttachShader(id, f);
    gl::LinkProgram(id);
    gl::DeleteShader(v);
    gl::DeleteShader(f);
    GLint ok = 0;
    gl::GetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[4096] = {0};
      gl::GetProgramInfoLog(id, sizeof log, nullptr, log);
      err = log;
      gl::DeleteProgram(id);
      return nullptr;
    }
    GLuint block = gl::GetUniformBlockIndex(id, "Frame");
    if (block != GL_INVALID_INDEX) gl::UniformBlockBinding(id, block, 0);
    auto* p = new GLProgram{id, gl::GetUniformLocation(id, "u_nodes")};
    return p;
  }
  void destroyProgram(void* h) override {
    auto* p = (GLProgram*)h;
    if (!p) return;
    gl::DeleteProgram(p->id);
    delete p;
  }
  std::string compileCheck(const std::string& fs) override {
    std::string err;
    GLuint sh = compileShader(GL_FRAGMENT_SHADER, fs, err);
    if (sh) gl::DeleteShader(sh);
    return err;
  }

  void draw(RenderTarget& rt, int w, int h, void* prog, const FrameUniforms& u, const std::vector<float>& nodes, int nodeCount) override {
    GLTarget& t = ensure(rt, w, h);
    gl::BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    gl::Viewport(0, 0, t.w, t.h);
    if (!prog) {  // shader error (dark red) / being compiled (nodeCount < 0: dark grey)
      if (nodeCount < 0) gl::ClearColor(0.08f, 0.08f, 0.09f, 1);
      else gl::ClearColor(0.3f, 0.05f, 0.05f, 1);
      gl::Clear(GL_COLOR_BUFFER_BIT);
      gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
      return;
    }
    auto* p = (GLProgram*)prog;
    gl::UseProgram(p->id);
    gl::BindBuffer(GL_UNIFORM_BUFFER, ubo_);
    gl::BufferData(GL_UNIFORM_BUFFER, sizeof(FrameUniforms), &u, GL_DYNAMIC_DRAW);
    gl::BindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);
    gl::ActiveTexture(GL_TEXTURE1);
    gl::BindTexture(GL_TEXTURE_2D, nodeTex_);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, NODE_VEC4S, std::max(1, nodeCount), 0, GL_RGBA, GL_FLOAT, nodes.data());
    gl::Uniform1i(p->nodesLoc, 1);
    gl::Disable(GL_DEPTH_TEST);
    if (scissor_[2] > 0) {  // one tile (OpenGL rows are bottom-up)
      gl::Enable(GL_SCISSOR_TEST);
      gl::Scissor(scissor_[0], t.h - scissor_[1] - scissor_[3], scissor_[2], scissor_[3]);
    } else gl::Disable(GL_SCISSOR_TEST);
    // progressive sampling: the new sample is blended into the running average (weight u.e[2])
    float weight = u.e[2];
    if (weight < 0.9999f) {
      gl::Enable(GL_BLEND);
      gl::BlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
      gl::BlendColor(0, 0, 0, weight);
    } else gl::Disable(GL_BLEND);
    gl::BindVertexArray(vao_);
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::Disable(GL_BLEND);
    gl::Disable(GL_SCISSOR_TEST);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  }

 private:
  GLuint vao_ = 0, ubo_ = 0, nodeTex_ = 0;
  std::function<bool(bool)> workerCtx_;
  std::vector<GLTarget> targets_;

  GLTarget& ensure(RenderTarget& rt, int w, int h) {
    if (rt.id < 0) { rt.id = (int)targets_.size(); targets_.push_back({}); }
    GLTarget& t = targets_[rt.id];
    if (t.fbo && t.w == w && t.h == h) return t;
    if (!t.fbo) { gl::GenFramebuffers(1, &t.fbo); gl::GenTextures(1, &t.tex); }
    t.w = w; t.h = h;
    rt.w = w; rt.h = h;
    gl::BindTexture(GL_TEXTURE_2D, t.tex);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);  // float: averages many samples
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    return t;
  }
};

}  // namespace

std::unique_ptr<Renderer> makeGLRenderer(std::function<bool(bool)> workerContext) {
  return std::make_unique<GLRenderer>(std::move(workerContext));
}

}  // namespace fd
