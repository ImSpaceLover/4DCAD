#include "render_thread.hpp"

#include <algorithm>
#include <chrono>

namespace fd {

namespace {
constexpr int TILE = 320;  // images larger than this are drawn tile by tile (one GPU submission each)
}

RenderThread::RenderThread(std::function<std::unique_ptr<Renderer>()> make, std::function<bool(bool)> context)
    : make_(std::move(make)), context_(std::move(context)) {
  th_ = std::thread([this] { loop(); });
}

RenderThread::~RenderThread() { stop(); }

void RenderThread::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void RenderThread::post(RenderRequest r) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    next_ = std::move(r);
  }
  busy_ = true;
  cv_.notify_all();
}

RenderThread::Image RenderThread::acquire() {
  std::lock_guard<std::mutex> lk(mu_);
  if (pub_ < 0) return {};
  lastUsed_[pub_] = uiFrames().current;
  return img_[pub_];
}

bool RenderThread::compiling() const {
  Renderer* r = r_.load();
  return r && r->compilingNow();
}

std::string RenderThread::lastError() {
  std::lock_guard<std::mutex> lk(mu_);
  return error_;
}

void RenderThread::loop() {
  if (context_ && !context_(true)) return;
  std::unique_ptr<Renderer> r = make_();
  if (!r) {
    if (context_) context_(false);
    return;
  }
  flip_ = r->flipY();
  r_ = r.get();
  RenderTarget acc, disp[SLOTS], guide, tmp[2];  // (guide, tmp: path tracing's denoiser)
  RenderRequest cur;
  bool have = false;
  int sample = 0;
  Node root;
  const Node* snap = nullptr;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_ || next_.has_value() || (have && sample < cur.samples); });
      if (stop_) break;
      if (next_) {
        cur = std::move(*next_);
        next_.reset();
        have = true;
        sample = 0;
      }
    }
    if (cur.root.get() != snap) {  // a new scene snapshot: the thread's own copy (packNodes needs a mutable tree)
      root = *cur.root;
      snap = cur.root.get();
    }
    FrameParams fp = cur.fp;
    fp.quality = &cur.quality;
    fp.stochastic = sample > 0 || cur.stochastic;
    fp.sample = sample;
    fp.blend = 1.0f / (sample + 1);
    int rw = std::max(1, (int)(cur.w * fp.scale)), rh = std::max(1, (int)(cur.h * fp.scale));
    bool ok = true;
    // refinement passes (the view is not changing) leave the GPU idle for as long as they used it,
    // so the UI and the other view always get their turn; a new view is drawn at full speed
    bool throttle = sample > 0;
    auto pass = [&]() {
      auto t0 = std::chrono::steady_clock::now();
      ok = r->render(acc, cur.w, cur.h, root, *cur.reg, cur.world, cur.highlight, fp);
      if (throttle) {
        r->flushAndWait();
        auto used = std::chrono::steady_clock::now() - t0;
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, used, [&] { return stop_ || next_.has_value(); });  // (a new request ends the pause)
      } else r->flush();  // each tile goes to the GPU on its own
    };
    bool denoise = fp.pathTrace && cur.quality.denoise;
    if (denoise && sample == 0) {  // the denoiser's guide (first hit's colour and depth): one cheap pass
      FrameParams g = fp;
      g.ptGuide = true;
      g.blend = 1;
      r->render(guide, cur.w, cur.h, root, *cur.reg, cur.world, cur.highlight, g);
    }
    if ((long long)rw * rh <= (long long)TILE * TILE * 2) pass();
    else
      for (int y = 0; y < rh && ok; y += TILE)
        for (int x = 0; x < rw && ok; x += TILE) {
          fp.tile[0] = x; fp.tile[1] = y; fp.tile[2] = TILE; fp.tile[3] = TILE;
          pass();
        }
    failed_ = !ok;
    if (!ok) {
      std::lock_guard<std::mutex> lk(mu_);
      error_ = r->lastError;
    }
    // a display image no UI frame in flight still uses (normally one is free at once)
    int s = -1;
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) break;
        uint64_t done = uiFrames().completed;
        for (int i = 0; i < SLOTS && s < 0; i++)
          if (i != pub_ && lastUsed_[i] <= done) s = i;
      }
      if (s >= 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (s < 0) break;
    if (cur.fp.pathTrace)  // linear average -> (denoised,) tone mapped, gamma-encoded
      r->present(acc, denoise ? &guide : nullptr, tmp, disp[s], postSettings(cur.quality, sample + 1));
    else r->copyTarget(acc, disp[s]);
    r->flushAndWait();  // finished before the UI may sample it
    uint64_t tex = r->texture(disp[s]);
    {
      std::lock_guard<std::mutex> lk(mu_);
      img_[s] = {tex, disp[s].w, disp[s].h, sample + 1};
      pub_ = s;
      if (!next_) busy_ = false;
    }
    sample++;
  }
  r_ = nullptr;
  r->destroy(acc);
  r->destroy(guide);
  for (auto& t : tmp) r->destroy(t);
  for (auto& d : disp) r->destroy(d);
  r.reset();
  if (context_) context_(false);
}

}  // namespace fd
