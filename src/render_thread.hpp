// Render threads: the viewport and the thumbnail each render on their own thread, so a slow
// image never freezes the UI or the other view.
//
// The UI posts a RenderRequest (a snapshot of everything the image needs; the newest one wins)
// and shows the newest finished image. The thread renders into a private accumulation target
// (progressive samples average there), then copies each finished pass into one of three display
// images and publishes it. A display image is only overwritten once the UI frames that drew it
// have finished on the GPU (uiFrames()). Large images are drawn in tiles, each its own GPU
// submission, so the UI's frames get in between.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include "renderer.hpp"

namespace fd {

struct RenderRequest {
  std::shared_ptr<const Node> root;               // scene snapshot (shared, never changed)
  std::shared_ptr<const PrimitiveRegistry> reg;
  std::string world;
  std::unordered_set<std::string> highlight;
  FrameParams fp;            // fp.quality is replaced by &quality on the thread
  QualitySettings quality;
  int w = 1, h = 1;
  int samples = 1;           // > 1: progressive refinement (while no newer request arrives)
  bool stochastic = false;   // the first sample is stochastic too (refining)
};

class RenderThread {
 public:
  /** make: creates the thread's renderer (called on the thread, after context(true)).
   *  context(true / false): makes the thread's GL context current / releases it (empty: none). */
  RenderThread(std::function<std::unique_ptr<Renderer>()> make, std::function<bool(bool)> context);
  ~RenderThread();
  void post(RenderRequest r);
  struct Image {
    uint64_t tex = 0;  // ImTextureID (0: nothing yet)
    int w = 0, h = 0;
    int samples = 0;   // samples averaged in it
  };
  /** The newest finished image; marks it as used by the UI frame being recorded. */
  Image acquire();
  bool flipY() const { return flip_; }
  /** A shader is being compiled for this view. */
  bool compiling() const;
  /** Requests not shown yet (posted but not finished). */
  bool busy() const { return busy_; }
  /** The last render's shader failed (lastError() says why). */
  bool failed() const { return failed_; }
  std::string lastError();
  void stop();

 private:
  void loop();
  std::function<std::unique_ptr<Renderer>()> make_;
  std::function<bool(bool)> context_;
  std::thread th_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::optional<RenderRequest> next_;
  bool stop_ = false;
  // published images (guarded by mu_)
  static constexpr int SLOTS = 3;
  int pub_ = -1;
  Image img_[SLOTS];
  uint64_t lastUsed_[SLOTS] = {0, 0, 0};
  std::string error_;
  std::atomic<Renderer*> r_{nullptr};
  std::atomic<bool> flip_{false}, busy_{false}, failed_{false};
};

}  // namespace fd
