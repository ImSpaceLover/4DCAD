// Offline renders (Render window): a still image or the animation, from the active camera,
// supersampled and refined with stochastic samples, then written as PNG / JPEG / GIF / AVI /
// PNG sequence / MP4 / WebM. The job runs a little every frame so the app stays usable; it
// renders a snapshot of the scene taken at the start.
#include <chrono>
#include <filesystem>

#include "app.hpp"

namespace fd {

namespace {
constexpr int TILE = 256;
double wallClock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
const char* STILL_EXT[2] = {".png", ".jpg"};
const char* ANIM_EXT[5] = {".gif", ".avi", ".png", ".mp4", ".webm"};
}  // namespace

std::string App::renderPath(bool animation) const {
  const RenderSettings& r = renderSet;
  std::string p = animation ? r.animPath : r.stillPath;
  std::string ext = animation ? ANIM_EXT[std::clamp(r.animFormat, 0, 4)] : STILL_EXT[std::clamp(r.stillFormat, 0, 1)];
  namespace fs = std::filesystem;
  if (p.empty()) {
    fs::path base;
    if (!currentFile.empty()) {
      fs::path f(currentFile);
      std::string stem = f.filename().string();
      if (size_t d = stem.find('.'); d != std::string::npos) stem = stem.substr(0, d);  // scene.4dscene.json -> scene
      base = f.parent_path() / (stem + (animation ? "_animation" : "_render"));
    } else {
      base = fs::path(dataDir()) / "renders" / (animation ? "animation" : "render");
    }
    return base.string() + ext;
  }
  fs::path f(p);
  if (f.extension().string() != ext) f.replace_extension(ext);  // the extension follows the format
  return f.string();
}

double App::renderProgress() const {
  if (!job.active) return job.finished ? 1.0 : 0.0;
  double frames = job.last - job.first + 1;
  int tx = (job.w + TILE - 1) / TILE, ty = (job.h + TILE - 1) / TILE;
  double done = (job.frame - job.first) + (job.sample + (double)job.tile / std::max(1, tx * ty)) / std::max(1, job.samples);
  return std::clamp(done / std::max(1.0, frames), 0.0, 1.0);
}

bool App::startRender(bool animation, std::string& err) {
  if (!renderer) { err = "No renderer"; return false; }
  if (job.active) { err = "A render is already running"; return false; }
  const RenderSettings& r = renderSet;
  RenderJob& j = job;
  j.writer.reset();
  j.animation = animation;
  j.finished = false;
  j.error.clear();
  j.aa = std::clamp(r.aa, 1, 3);
  j.samples = std::clamp(r.samples, 1, 4096);
  int W = std::clamp(r.width, 16, 8192), H = std::clamp(r.height, 16, 8192);
  if (animation && (r.animFormat == 3 || r.animFormat == 4)) { W += W & 1; H += H & 1; }  // video codecs want even sizes
  j.w = W * j.aa;
  j.h = H * j.aa;
  j.fps = r.sceneRange ? anim.fps : r.fps;
  j.first = r.sceneRange ? anim.start : r.start;
  j.last = r.sceneRange ? anim.end : r.end;
  if (!animation) j.first = j.last = (int)std::lround(anim.frame);
  if (j.last < j.first) { err = "The last frame is before the first"; return false; }
  j.path = renderPath(animation);
  std::error_code ec;
  std::filesystem::path parent = std::filesystem::path(j.path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  if (animation) {
    switch (r.animFormat) {
      case 0: j.writer = makeGifWriter(j.path, W, H, j.fps); break;
      case 1: j.writer = makeAviWriter(j.path, W, H, j.fps, r.jpegQuality); break;
      case 2: j.writer = makePngSequence(j.path, j.first, W, H); break;
      default:
        if (!ffmpegAvailable(r.ffmpeg)) { err = "ffmpeg was not found (set its path in the Render window)"; return false; }
        j.writer = makeFfmpegWriter(r.ffmpeg, j.path, W, H, j.fps, r.animFormat == 3 ? "h264" : "vp9");
    }
    if (!j.writer || !j.writer->error.empty()) { err = j.writer ? j.writer->error : "cannot create the output"; j.writer.reset(); return false; }
  }
  j.camera = activeCamera;
  if (Node* c = findNode(root, activeCamera); !c || !c->isCamera()) j.camera.clear();
  j.usedCamera = !j.camera.empty();
  j.sceneRoot = root;
  j.sceneLights = lights;
  j.sceneAnim = anim;
  j.world = world;
  j.space = space;
  j.quality = quality;
  j.frame = j.first;
  j.sample = 0;
  j.tile = 0;
  j.prepared = false;
  j.started = wallClock();
  j.active = true;
  return true;
}

void App::cancelRender() {
  if (!job.active) return;
  if (job.writer) job.writer->close();  // keeps what was rendered so far (a shorter animation)
  job.writer.reset();
  job.active = false;
  job.error = "Cancelled";
  toast("Render cancelled");
}

// The scene at the job's current frame: evaluated on the snapshot (the live scene is swapped out
// meanwhile so the usual animation code, including surface re-projection, can be used).
void App::prepareJobFrame() {
  RenderJob& j = job;
  Node liveRoot = std::move(root);
  std::vector<PointLight> liveLights = std::move(lights);
  Animation liveAnim = std::move(anim);
  std::string liveWorld = world;
  Space liveSpace = space;
  space = j.space;
  auto liveIsolate = std::move(isolate);
  isolate.clear();
  root = j.sceneRoot;
  lights = j.sceneLights;
  anim = j.sceneAnim;
  world = j.world;
  anim.frame = j.frame;
  if (j.animation && !anim.empty()) applyAnim();
  else rebuild();

  FrameParams fp;
  bool cg = false;
  double f = fov();
  View v;
  fp.axes = editCam.axes();
  if (!j.camera.empty() && findNode(root, j.camera)) {
    v = cameraView(j.camera, &cg, &f);
    Node* c = findNode(root, j.camera);
    fp.aperture = (float)c->camAperture;
    fp.focus = (float)c->camFocus;
    if (!cg) fp.axes = FullCamera::axesFor(c->camSlider);
  } else {
    v = currentView();
    cg = geo && geoEdit.valid;
  }
  fp.view = v;
  fp.geo = cg && !world.empty();
  fp.grid = false;
  fp.fovDeg = f;
  fp.quality = &j.quality;
  {  // light direction and lights from the snapshot's settings
    QualitySettings live = quality;
    quality = j.quality;
    fp.light = light();
    fp.skyCol = worldColor();
    fp.pathTrace = j.quality.lighting == 1;
    fp.lights = frameLights(fp.pathTrace);
    fp.prof = mainProfile();
    quality = live;
  }
  fp.prof.resolution = 1;
  fp.scale = 1;
  if (space.curved()) { std::ostringstream unused; curvedParams(fp, v, unused); }
  j.fp = fp;
  j.frameRoot = root;

  root = std::move(liveRoot);
  lights = std::move(liveLights);
  anim = std::move(liveAnim);
  world = liveWorld;
  space = liveSpace;
  isolate = std::move(liveIsolate);
  rebuild();
  j.prepared = true;
}

void App::finishJobFrame() {
  RenderJob& j = job;
  Pixels px;
  if (!renderer->readPixels(j.rt, px) || px.size() != (size_t)j.w * j.h * 4) {
    j.error = "Could not read the rendered image";
    if (j.writer) j.writer->close();
    j.writer.reset();
    j.active = false;
    toast(j.error, true);
    return;
  }
  int W = j.w / j.aa, H = j.h / j.aa;
  if (j.aa > 1) px = downscale(px, j.w, j.h, j.aa);
  bool ok = true;
  if (!j.animation) {
    ok = renderSet.stillFormat == 1 ? writeJPEG(j.path, W, H, px, renderSet.jpegQuality) : writePNG(j.path, W, H, px);
    if (!ok) j.error = "Cannot write " + j.path;
  } else {
    ok = j.writer->add(px);
    if (!ok) j.error = j.writer->error;
  }
  j.frame++;
  j.sample = 0;
  j.prepared = false;
  if (ok && j.frame <= j.last) return;
  if (ok && j.writer && !j.writer->close()) { ok = false; j.error = j.writer->error; }
  j.writer.reset();
  j.active = false;
  j.finished = ok;
  j.seconds = wallClock() - j.started;
  if (ok) toast("Saved " + j.path);
  else toast(j.error, true);
}

void App::renderStep(double budget) {
  if (!job.active || !renderer) return;
  RenderJob& j = job;
  double t0 = wallClock();
  renderer->setImmediate(true);
  int tx = (j.w + TILE - 1) / TILE, ty = (j.h + TILE - 1) / TILE;
  while (j.active) {
    if (!j.prepared) prepareJobFrame();
    FrameParams& fp = j.fp;
    fp.stochastic = j.samples > 1;
    fp.sample = j.sample;
    fp.blend = 1.0f / (j.sample + 1);
    // one tile per draw: short GPU jobs keep the PC responsive (and below the driver's watchdog)
    fp.tile[0] = (j.tile % tx) * TILE;
    fp.tile[1] = (j.tile / tx) * TILE;
    fp.tile[2] = fp.tile[3] = TILE;
    bool ok = renderer->render(j.rt, j.w, j.h, j.frameRoot, reg, j.world, {}, fp);
    fp.tile[2] = 0;
    if (renderer->compiling()) break;  // this frame's program is still being compiled: try again next UI frame
    if (!ok) {
      j.error = "Shader error: " + renderer->lastError;
      if (j.writer) j.writer->close();
      j.writer.reset();
      j.active = false;
      toast(j.error, true);
      break;
    }
    renderer->finish();
    if (++j.tile >= tx * ty) {
      j.tile = 0;
      if (++j.sample >= j.samples) finishJobFrame();
    }
    if (wallClock() - t0 > budget) break;
  }
  renderer->setImmediate(false);
}

}  // namespace fd
