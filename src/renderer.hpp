// Renderer: compiles one ray-marching shader per (scene structure, view mode) and draws
// into offscreen render targets that the UI shows as images. The shared part (shader
// cache, node data, frame uniforms) lives here; backends (OpenGL, Vulkan) only create
// programs and targets and run the full-screen pass.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
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
  // path tracing: an emissive object as an area light, given by its bounding sphere (pos, radius);
  // shadow rays toward it count only where they reach a glowing surface, so it shines with its own shape
  bool area = false;
  float radius = 0;
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
  bool pathTrace = false;  // physically based path tracing (a separate shader; needs random samples)
  bool ptGuide = false;    // path tracing: the denoiser's guide image (first hit's colour and depth), no jitter
};

/** Full-screen post passes (path tracing). */
enum PostOp { POST_RESOLVE = 0, POST_ATROUS = 1, POST_COUNT };
/** How a path-traced image is shown. */
struct PostSettings {
  float exposure = 0;  // stops
  int tonemap = 1;     // 0 = clip, 1 = filmic (ACES)
  bool denoise = true;
  float strength = 1;
  int samples = 1;     // samples averaged so far (the denoiser does less as they add up)
};
inline PostSettings postSettings(const QualitySettings& q, int samples) {
  return {q.exposure, q.tonemap, q.denoise, q.denoiseStrength, samples};
}

/** The frame uniform block (std140; mirrors FRAME_BLOCK in shaders.hpp). */
struct FrameUniforms {
  float frame[16], axes[16];
  float eye[4], light[4], marker[4], markerCol[4], markerFwd[4];
  float skyTop[4], skyBottom[4], skyCol[4], dbgEndCol[4];
  float a[4], b[4], c[4], d[4];
  int32_t i[4], j[4], k[4], l[4];
  float e[4];  // aperture, focus distance, accumulation weight (backend blend), 0
  float f[4];  // sky intensity, 4D light like 3D (path tracing), 0, 0
  float lpos[MAX_LIGHTS][4], lcol[MAX_LIGHTS][4];
  float lext[MAX_LIGHTS][4];  // radius, area light (1) / point light (0), 0, 0
  float dbg[MAX_DEBUG_PTS][4];
};

/** UI frame numbers shared with the render threads: an image the UI drew in frame N may be
 *  overwritten once `completed` >= N (the GPU has finished that frame). */
struct UiFrameClock {
  std::atomic<uint64_t> current{1};    // the frame the UI is recording
  std::atomic<uint64_t> completed{0};  // the newest frame the GPU has finished
};
UiFrameClock& uiFrames();

class Renderer {
 public:
  virtual ~Renderer() = default;
  /** Renders into `rt`, resizing it to (w*scale, h*scale). Returns false if the shader failed.
   *  With background compiles on, a scene whose program is not ready yet is not drawn (the target
   *  keeps its last image) and compiling() is true: render again later. */
  bool render(RenderTarget& rt, int w, int h, Node& root, const PrimitiveRegistry& reg, const std::string& world,
              const std::unordered_set<std::string>& highlight, const FrameParams& fp);
  /** Test-compiles custom SDF code; returns the compiler error ("" if it is fine). Cached.
   *  With background compiles on, an unchecked code returns "" and is checked on the worker
   *  (wait: check it here and now). */
  std::string checkCustomCode(const std::string& code, bool wait = false);
  /** Background compiles (interactive use): programs are compiled on a worker thread while the
   *  last image stays on screen. Off (default): compiled at once (headless and offline renders). */
  void setAsync(bool on) { async_ = on; }
  bool async() const { return async_; }
  /** The last render() waited for a program that is still being compiled. */
  bool compiling() const { return compiling_; }
  /** Programs (and custom code checks) still being compiled. */
  int pendingCompiles() const { return (int)pending_.size() + (int)pendingCode_.size(); }
  /** The target's image for ImGui::Image (ImTextureID). */
  virtual uint64_t texture(const RenderTarget& rt) = 0;
  /** OpenGL images are stored bottom-up: the UI flips their texture coordinates. */
  virtual bool flipY() const = 0;
  virtual void destroy(RenderTarget& rt) = 0;
  /** Copies src into dst (resized to src's size). */
  virtual void copyTarget(const RenderTarget& src, RenderTarget& dst) = 0;
  /** A full-screen pass: dst (resized to a's size) = op(a, b) with parameters p[8].
   *  POST_RESOLVE: p0 = exposure factor, p1 = tonemap. POST_ATROUS: b = guide, p0 = step,
   *  p1 = colour sigma, p2 = colour-guide sigma, p3 = depth sigma (relative). */
  virtual void post(int op, const RenderTarget& a, const RenderTarget* b, RenderTarget& dst, const float* p) = 0;
  /** Path tracing accumulates linear light (averaging gamma-encoded samples would darken rare bright
   *  ones). This shows it: optional denoising (guide: see FrameParams::ptGuide; tmp: two scratch
   *  targets), exposure, tone mapping and gamma, into dst (display / saving). */
  void present(const RenderTarget& acc, const RenderTarget* guide, RenderTarget* tmp, RenderTarget& dst, const PostSettings& s);
  /** Waits until this renderer's drawing has finished (render threads, before publishing an image). */
  virtual void flushAndWait() { finish(); }
  /** Sends the queued drawing to the GPU without waiting (OpenGL: glFlush). */
  virtual void flush() {}
  /** A program is being compiled right now (synchronously, on the thread rendering). */
  bool compilingNow() const { return compilingNow_.load(); }
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
  /** Called on the compile worker thread before / after its jobs (OpenGL: makes a context of the
   *  share group current; false: no worker context, compiles stay on the calling thread).
   *  workerDone runs after each program so other contexts can use it (OpenGL: glFinish). */
  virtual bool workerBegin() { return true; }
  virtual void workerDone() {}
  virtual void workerEnd() {}
  /** Call first in the backend destructor: stops the compile worker (before its context goes). */
  void stopWorker();
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
  Program* get(Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo, SpaceKind space, bool ball, int pt, bool& pending);
  void insert(const std::string& key, Program p);

  // ---- background compiles: jobs run on worker_, results are applied on the calling thread
  struct Job {
    std::string key;     // program key ("" for a custom code check)
    std::string source;  // fragment shader
    std::string code;    // custom code (check jobs)
  };
  struct Result {
    Job job;
    void* handle = nullptr;
    std::string err;
    double ms = 0;
  };
  bool async_ = false, compiling_ = false;
  std::atomic<bool> compilingNow_{false};
  bool workerOk_ = true;  // false: the backend has no worker context
  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::vector<Result> done_;
  bool stop_ = false;
  std::unordered_map<std::string, CompiledScene> pending_;  // key -> its scene code (program being compiled)
  std::unordered_set<std::string> pendingCode_;
  void enqueue(Job j);
  void pollCompiles();
  void workerLoop();
  std::string codeCheckSource(const std::string& code);
  void applyCodeResult(const std::string& code, std::string err);
};

/** OpenGL 3.3 backend (needs a current context with loaded functions). */
std::unique_ptr<Renderer> makeGLRenderer(std::function<bool(bool)> workerContext = {});
/**   workerContext(true) makes a hidden context sharing with the main one current on the calling
 *    (worker) thread, workerContext(false) releases it; empty: no background compiles. */
#ifdef FD_HAVE_VULKAN
/** Vulkan backend (see render_vk.cpp); nullptr if no usable device. */
struct VulkanContext;
std::unique_ptr<Renderer> makeVulkanRenderer(VulkanContext& ctx, bool threaded = false);
/**   threaded: for a render thread (every draw is submitted and waited for on that thread). */
#endif

/** Packs the frame parameters into the uniform block. */
FrameUniforms frameUniforms(const FrameParams& fp, int w, int h);

}  // namespace fd
