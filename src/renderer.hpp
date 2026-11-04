// Renderer: compiles one ray-marching shader per (scene structure, view mode) and draws
// into offscreen render targets that the UI shows as images. The shared part (shader
// cache, node data, frame uniforms) lives here; backends (OpenGL, Vulkan) only create
// programs and targets and run the full-screen pass.
#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "camera.hpp"
#include "codegen.hpp"
#include "media.hpp"
#include "shaders.hpp"

namespace fd {

enum class Backend { Auto, OpenGL, Vulkan };
inline const char* BACKEND_NAMES[3] = {"Auto", "OpenGL", "Vulkan"};

/** An offscreen image owned by the renderer (id = backend slot, -1 = not created yet). */
struct RenderTarget {
  int id = -1;
  int w = 0, h = 0;
};

/** A point light as the shader sees it (world position, rgb * intensity, range). */
struct LightParam {
  V4 pos;
  std::array<float, 3> color;
  float range = 10;
  bool shadows = true;
  bool emissive = false;  // an emissive object's light: casts shadows even when point-light shadows are off
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
  M4 axes = identity();  // world axes shown as display X, Y, Z and on the slider (FullCamera::axes)
  std::vector<V4> debugPts;  // debug ray polyline (drawn as a glowing tube, end marker coloured by reason)
  int debugReason = 0;
  std::vector<LightParam> lights;  // point lights (at most MAX_LIGHTS are used)
  RenderProfile prof;  // what to draw and how hard to work (main settings or the fast profile)
  // stochastic sampling (progressive): sample index, weight of this sample in the running average
  bool stochastic = false;
  int sample = 0;
  float blend = 1;  // 1 = replace the image, 1/(n+1) = add the (n+1)-th sample
  float aperture = 0, focus = 5;  // depth of field (stochastic only; cameras)
  // curved spaces: the shader works camera-relative (view = origin + identity frame); camIso is the
  // camera's isometry (world <- camera), light / marker positions are camera-relative model points
  Space space;
  M4 camIso = identity();
  bool honeycomb = false;  // set by render(): the scene has a honeycomb
  // only this rectangle of the target is drawn (pixels, top-left origin; w = 0: everything).
  // Offline renders draw in tiles so no single GPU job runs long on slow machines.
  int tile[4] = {0, 0, 0, 0};
  bool ballView = false;   // curved: the Poincare ball / stereographic overview (view = outside camera in the ball)
};

/** The frame uniform block (std140; mirrors FRAME_BLOCK in shaders.hpp). */
struct FrameUniforms {
  float frame[16], axes[16];
  float eye[4], light[4], marker[4], markerCol[4], markerFwd[4];
  float skyTop[4], skyBottom[4], skyCol[4], dbgEndCol[4];
  float a[4], b[4], c[4], d[4];
  int32_t i[4], j[4], k[4], l[4];
  float e[4];  // aperture, focus distance, accumulation weight (backend blend), 0
  float lpos[MAX_LIGHTS][4], lcol[MAX_LIGHTS][4];
  float dbg[MAX_DEBUG_PTS][4];
};

class Renderer {
 public:
  virtual ~Renderer() = default;
  /** Renders into `rt`, resizing it to (w*scale, h*scale). Returns false if the shader failed. */
  bool render(RenderTarget& rt, int w, int h, Node& root, const PrimitiveRegistry& reg, const std::string& world,
              const std::unordered_set<std::string>& highlight, const FrameParams& fp);
  /** Test-compiles custom SDF code; returns the compiler error ("" if it is fine). Cached. */
  std::string checkCustomCode(const std::string& code);
  /** The target's image for ImGui::Image (ImTextureID). */
  virtual uint64_t texture(const RenderTarget& rt) = 0;
  /** OpenGL images are stored bottom-up: the UI flips their texture coordinates. */
  virtual bool flipY() const = 0;
  virtual void destroy(RenderTarget& rt) = 0;
  /** The target's image as RGBA8, top row first (after its last render has finished). */
  virtual bool readPixels(const RenderTarget& rt, Pixels& rgba) = 0;
  /** Offline renders: each draw is executed at once (Vulkan; OpenGL draws immediately anyway). */
  virtual void setImmediate(bool) {}
  /** Waits until all drawing has finished (offline renders measure their time with it). */
  virtual void finish() {}
  virtual const char* backendName() const = 0;
  std::string device;  // GPU / driver name
  std::string lastError;
  double lastCompileMs = 0;

 protected:
  virtual ShaderTarget shaderTarget() const = 0;
  /** Compiles + links a full-screen program; returns a backend handle (nullptr + err on failure). */
  virtual void* createProgram(const std::string& fragmentSource, std::string& err) = 0;
  virtual void destroyProgram(void* program) = 0;
  /** Compiles a fragment shader only (custom code check); returns the error or "". */
  virtual std::string compileCheck(const std::string& fragmentSource) = 0;
  virtual void draw(RenderTarget& rt, int w, int h, void* program, const FrameUniforms& u, const std::vector<float>& nodes, int nodeCount) = 0;
  int scissor_[4] = {0, 0, 0, 0};  // the current draw's rectangle (x, y top-left, w, h; w = 0: all)
  /** Call from the backend destructor: frees all cached programs. */
  void clearPrograms();

 private:
  struct Program {
    void* handle = nullptr;
    CompiledScene cs;
  };
  std::unordered_map<std::string, Program> cache_;
  std::list<std::string> lru_;
  std::unordered_set<std::string> failed_;
  std::unordered_set<std::string> badCode_;
  std::unordered_map<std::string, std::string> codeErrors_;
  Program* get(Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo, SpaceKind space, bool ball = false);
};

/** OpenGL 3.3 backend (needs a current context with loaded functions). */
std::unique_ptr<Renderer> makeGLRenderer();
#ifdef FD_HAVE_VULKAN
/** Vulkan backend (see render_vk.cpp); nullptr if no usable device. */
struct VulkanContext;
std::unique_ptr<Renderer> makeVulkanRenderer(VulkanContext& ctx);
#endif

/** Packs the frame parameters into the uniform block. */
FrameUniforms frameUniforms(const FrameParams& fp, int w, int h);

}  // namespace fd
