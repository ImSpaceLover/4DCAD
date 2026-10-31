// OpenGL renderer: compiles one ray-marching program per (scene structure, view
// mode) and draws into offscreen render targets that the UI displays as images.
#pragma once
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "camera.hpp"
#include "codegen.hpp"
#include "gl.hpp"

namespace fd {

struct RenderTarget {
  GLuint fbo = 0, tex = 0;
  int w = 0, h = 0;
  void ensure(int w, int h);
  void destroy();
};

struct FrameParams {
  View view;
  bool geo = false;
  bool grid = true;
  double fovDeg = 60;
  const QualitySettings* quality = nullptr;
  float scale = 1;  // resolution scale for this frame
  V4 light{0.5, 0.8, -0.35, 0.25};
  V4 marker{0, 0, 0, 0};
  std::array<float, 4> markerCol{0, 0, 0, 0};  // rgb, radius (0 = hidden)
  V4 markerFwd{0, 0, 1, 0};                     // facing direction (draws a small "nose")
  float markerSkip = 0;                         // primary rays ignore the body for t < skip (first person)
  std::array<float, 3> skyCol{0.35f, 0.4f, 0.55f};  // Geodesic sky = world surface colour
};

class Renderer {
 public:
  Renderer();
  ~Renderer();
  /** Renders into `rt`, resizing it to (w*scale, h*scale). Returns false if the shader failed. */
  bool render(RenderTarget& rt, int w, int h, Node& root, const PrimitiveRegistry& reg, const std::string& world,
              const std::unordered_set<std::string>& highlight, const FrameParams& fp);
  std::string lastError;
  double lastCompileMs = 0;

 private:
  struct Program {
    GLuint id = 0;
    CompiledScene cs;
    std::unordered_map<std::string, GLint> loc;
  };
  std::unordered_map<std::string, Program> cache_;
  std::list<std::string> lru_;
  std::unordered_set<std::string> failed_;
  GLuint vao_ = 0, vbo_ = 0;
  Program* get(Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo);
  GLint u(Program& p, const char* name);
};

}  // namespace fd
