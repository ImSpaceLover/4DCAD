// 4DCAD entry point: window, OpenGL context, ImGui, main loop.
// Command line (mainly for automated testing):
//   4DCAD [scene.json] [--example playground|planet] [--geo] [--explore] [--gl | --vulkan]
//         [--select NAME] [--tool move|rotate] [--size WxH] [--screenshot out.ppm --frames N]
//         [--debug-ray] (debug ray on, frozen after the first frame) [--strafe D] [--turn DEG] (then move / turn)
//         [--render out.png|.jpg|.gif|.avi|.mp4|.webm [--animation] [--render-size WxH] [--samples N] [--aa N]]
//         (renders like the Render window, from the active camera, then quits)
#include "version.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app.hpp"
#include "examples.hpp"
#include "gl.hpp"
#include "renderer.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#ifdef FD_HAVE_VULKAN
#include "imgui_impl_vulkan.h"
#include "vk_context.hpp"
#endif
#include "ui.hpp"

using namespace fd;

static UI* g_ui = nullptr;

// rgba: top row first
static void writePPM(const std::string& path, int w, int h, const std::vector<uint8_t>& px) {
  if (px.size() < (size_t)w * h * 4) { std::fprintf(stderr, "screenshot: no pixels\n"); return; }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) std::fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
  std::fclose(f);
}

static void loadFonts(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  const char* candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                              "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"};
  for (const char* c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c, ec)) {
      static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0370, 0x03FF, 0x0400, 0x044F, 0x2000, 0x206F, 0x2190, 0x21FF, 0};
      if (io.Fonts->AddFontFromFileTTF(c, 16.0f * scale, nullptr, ranges)) return;
    }
  }
  ImFontConfig cfg;
  cfg.SizePixels = 13.0f * scale;
  io.Fonts->AddFontDefault(&cfg);
}

int main(int argc, char** argv) {
  std::string sceneArg, example, screenshot;
  std::string selectName, toolName, openWindow;
  bool startGeo = false, startExplore = false, debugRayArg = false, forceAsync = false, forceThreads = false;
  int lightOverride = -1;
  std::vector<std::pair<std::string, double>> ptOverrides;
  double skyOverride = -1, sunOverride = -1, ambientOverride = -1, fogOverride = -1;
  double turnDeg = 0, strafe = 0;
  Backend forceBackend = Backend::Auto;
  int frames = 30, W = 1600, H = 900;
  int debugViewArg = 0;
  std::string renderOut, newSpace;
  bool renderAnim = false;
  int renderW = 0, renderH = 0, renderSamples = -1, renderAA = -1;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--version" || a == "-v") {
      std::printf("4DCAD %s%s%s\n", FD_VERSION, FD_GIT_COMMIT[0] ? " (" : "", FD_GIT_COMMIT[0] ? FD_GIT_COMMIT ")" : "");
      return 0;
    }
    if (a == "--example" && i + 1 < argc) example = argv[++i];
    else if (a == "--view" && i + 1 < argc) debugViewArg = std::atoi(argv[++i]);  // debug view (testing): 1 colour .. 9
    else if (a == "--geo") startGeo = true;
    else if (a == "--select" && i + 1 < argc) selectName = argv[++i];
    else if (a == "--tool" && i + 1 < argc) toolName = argv[++i];
    else if (a == "--open" && i + 1 < argc) openWindow = argv[++i];
    else if (a == "--explore") startExplore = true;
    else if (a == "--debug-ray") debugRayArg = true;
    else if (a == "--turn" && i + 1 < argc) turnDeg = std::atof(argv[++i]);
    else if (a == "--strafe" && i + 1 < argc) strafe = std::atof(argv[++i]);
    else if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
    else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--render" && i + 1 < argc) renderOut = argv[++i];
    else if (a == "--new" && i + 1 < argc) newSpace = argv[++i];
    else if (a == "--animation") renderAnim = true;
    else if (a == "--render-size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &renderW, &renderH);
    else if (a == "--samples" && i + 1 < argc) renderSamples = std::atoi(argv[++i]);
    else if (a == "--aa" && i + 1 < argc) renderAA = std::atoi(argv[++i]);
    else if (a == "--gl") forceBackend = Backend::OpenGL;
    else if (a == "--async") forceAsync = true;
    else if (a == "--threads") forceThreads = true;  // render threads even for --screenshot (testing)
    // lighting overrides for this run (testing; not saved): --path-trace, --simple-lighting, --sky-intensity X, --sun X, --ambient X, --fog X
    else if (a == "--path-trace") lightOverride = 1;
    else if (a == "--simple-lighting") lightOverride = 0;
    else if (a == "--sky-intensity" && i + 1 < argc) skyOverride = std::atof(argv[++i]);
    else if (a == "--sun" && i + 1 < argc) sunOverride = std::atof(argv[++i]);
    else if (a == "--ambient" && i + 1 < argc) ambientOverride = std::atof(argv[++i]);
    else if (a == "--fog" && i + 1 < argc) fogOverride = std::atof(argv[++i]);
    // path tracing display: --denoise 0|1, --exposure STOPS, --tonemap 0|1, --light4d 0|1 (physical / like 3D)
    else if ((a == "--denoise" || a == "--exposure" || a == "--tonemap" || a == "--light4d" || a == "--res" || a == "--upscale") && i + 1 < argc) ptOverrides.push_back({a, std::atof(argv[++i])});  // background compiles even for --screenshot (testing)
    else if (a == "--vulkan") forceBackend = Backend::Vulkan;
    else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &W, &H);
    else if (a[0] != '-') sceneArg = a;
  }

  if (!glfwInit()) { std::fprintf(stderr, "Failed to initialise GLFW\n"); return 1; }
  // graphics API: --gl / --vulkan, else the setting (Auto tries Vulkan first, falling back to OpenGL)
  Backend want = forceBackend != Backend::Auto ? forceBackend : App::savedBackend();
  GLFWwindow* win = nullptr;
  bool useVulkan = false;
  std::string vkErr;
#ifdef FD_HAVE_VULKAN
  VulkanContext vk;
  if (want != Backend::OpenGL) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    win = glfwCreateWindow(W, H, "4DCAD " FD_VERSION_SHORT, nullptr, nullptr);
    vk.pipelineCachePath = (std::filesystem::path(dataDir()) / "shadercache" / "vulkan-pipelines.bin").string();
    if (win && vk.init(win, vkErr)) useVulkan = true;
    else {
      std::fprintf(stderr, "Vulkan not available (%s): using OpenGL\n", vkErr.c_str());
      vk.shutdown();
      if (win) glfwDestroyWindow(win);
      win = nullptr;
    }
  }
#else
  if (want == Backend::Vulkan) std::fprintf(stderr, "This build has no Vulkan renderer: using OpenGL\n");
#endif
  if (!useVulkan) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    win = glfwCreateWindow(W, H, "4DCAD " FD_VERSION_SHORT, nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "Failed to create an OpenGL 3.3 window (update your graphics driver)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (const char* missing = gl::load((void* (*)(const char*))glfwGetProcAddress)) {
      std::fprintf(stderr, "OpenGL function missing: %s\n", missing);
      return 1;
    }
  }

  App app;
  UI* ui = nullptr;
  // our callbacks first; ImGui chains to them
  glfwSetKeyCallback(win, [](GLFWwindow*, int key, int, int action, int mods) { if (g_ui) g_ui->onKey(key, action, mods); });
  glfwSetScrollCallback(win, [](GLFWwindow*, double, double y) { if (g_ui) g_ui->onScroll(y); });
  // closing the window asks about unsaved changes first
  glfwSetWindowCloseCallback(win, [](GLFWwindow* w) { if (g_ui) { glfwSetWindowShouldClose(w, GLFW_FALSE); g_ui->requestQuit(); } });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // panel layout (docking): saved in the settings folder
  static std::string iniPath = (std::filesystem::path(dataDir()) / "layout.ini").string();
  io.IniFilename = iniPath.c_str();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  float xs = 1, ys = 1;
  glfwGetWindowContentScale(win, &xs, &ys);
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0;
  st.FrameRounding = 3;
  st.ScaleAllSizes(xs);
  loadFonts(xs);
  std::unique_ptr<Renderer> renderer;
  GLFWwindow* glWorker = nullptr;
  GLFWwindow* glThreadWin[2] = {nullptr, nullptr};  // contexts of the viewport / thumbnail render threads
#ifdef FD_HAVE_VULKAN
  if (useVulkan) {
    ImGui_ImplGlfw_InitForVulkan(win, true);
    if (!vk.initImGui()) { std::fprintf(stderr, "ImGui Vulkan setup failed\n"); return 1; }
    renderer = makeVulkanRenderer(vk);
    if (!renderer) { std::fprintf(stderr, "Vulkan renderer setup failed\n"); return 1; }
  }
#endif
  if (!useVulkan) {
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    // a hidden window whose context shares objects with the main one: shaders compile on a
    // worker thread with it (the UI keeps running)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glWorker = glfwCreateWindow(16, 16, "4DCAD compiler", nullptr, win);
    glfwDefaultWindowHints();
    GLFWwindow* wk = glWorker;
    renderer = makeGLRenderer(wk ? std::function<bool(bool)>([wk](bool on) { glfwMakeContextCurrent(on ? wk : nullptr); return true; })
                                 : std::function<bool(bool)>());
  }
  // interactive use compiles in the background; screenshots and command-line renders wait for it
  renderer->setAsync(forceAsync || (screenshot.empty() && renderOut.empty()));
  std::printf("%s\n", renderer->device.c_str());
  // compiled shaders are kept between runs (settings folder)
  std::string shaderCache = (std::filesystem::path(dataDir()) / "shadercache").string();
  renderer->setCacheDir(shaderCache);

  app.init(std::move(renderer));
  // the viewport and the thumbnail render on their own threads (interactive use; headless runs
  // render on this thread so screenshots are deterministic)
  if (forceThreads || (screenshot.empty() && renderOut.empty())) {
    std::function<std::unique_ptr<Renderer>()> make;
    std::function<bool(bool)> ctx[2];
#ifdef FD_HAVE_VULKAN
    if (useVulkan) make = [&vk, shaderCache] { auto r = makeVulkanRenderer(vk, true); if (r) r->setCacheDir(shaderCache); return r; };
#endif
    if (!useVulkan) {
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
      glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
      glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
      for (int i = 0; i < 2; i++) {
        glThreadWin[i] = glfwCreateWindow(16, 16, "4DCAD render", nullptr, win);
        GLFWwindow* w = glThreadWin[i];
        if (w) ctx[i] = [w](bool on) { glfwMakeContextCurrent(on ? w : nullptr); return true; };
      }
      glfwDefaultWindowHints();
      make = [shaderCache] { auto r = makeGLRenderer(); r->setCacheDir(shaderCache); return r; };
    }
    if (useVulkan || (glThreadWin[0] && glThreadWin[1])) app.startRenderThreads(make, ctx[0], ctx[1]);
  }
  ui = new UI(app, win);
  g_ui = ui;
  std::string err;
  if (!sceneArg.empty() && !app.loadSceneFile(sceneArg, err)) app.toast(err, true);
  for (auto& ex : EXAMPLES) if (example == ex.id) app.loadScene(ex.make());
  app.quality.debugView = debugViewArg;
  if (!newSpace.empty()) {  // --new hyperbolic[:R] | spherical[:R] | flat
    std::string kind = newSpace.substr(0, newSpace.find(':'));
    double r = newSpace.find(':') != std::string::npos ? std::atof(newSpace.substr(newSpace.find(':') + 1).c_str()) : 8;
    app.newScene(kind == "hyperbolic" ? SpaceKind::Hyperbolic : kind == "spherical" ? SpaceKind::Spherical : SpaceKind::Flat, r);
  }
  if (startGeo) app.setGeo(true);
  QualitySettings savedQuality = app.quality;
  bool qualityOverride = lightOverride >= 0 || skyOverride >= 0 || sunOverride >= 0 || ambientOverride >= 0 || fogOverride >= 0 || !ptOverrides.empty();
  for (auto& [k, v] : ptOverrides) {
    if (k == "--denoise") app.quality.denoise = std::clamp((int)v, 0, 4);
    if (k == "--exposure") app.quality.exposure = (float)v;
    if (k == "--tonemap") app.quality.tonemap = (int)v;
    if (k == "--light4d") app.quality.light4D = (int)v;
    if (k == "--res") app.quality.resolutionScale = app.quality.fast.resolution = (float)v;  // (testing: both profiles)
    if (k == "--upscale") app.quality.upscale = app.quality.fast.upscale = std::clamp((int)v, 0, 1);
  }
  if (lightOverride >= 0) app.quality.lighting = lightOverride;
  if (skyOverride >= 0) app.quality.skyIntensity = (float)skyOverride;
  if (sunOverride >= 0) app.quality.sunIntensity = (float)sunOverride;
  if (ambientOverride >= 0) app.quality.ambient = (float)ambientOverride;
  if (fogOverride >= 0) app.quality.fog = (float)fogOverride;
  if (!selectName.empty()) walk(app.root, [&](Node& n, Node*, int) { if (n.name == selectName && app.selection.empty()) app.selection = {n.id}; });
  if (toolName == "move") app.tool = Tool::Move;
  if (toolName == "rotate") app.tool = Tool::Rotate;
  if (!openWindow.empty()) ui->debugOpen(openWindow);
  if (startExplore) app.setMode(Mode::Explore);
  app.debugRay = debugRayArg;
  // --render: one offline render with these settings (the saved Render settings are left alone)
  App::RenderSettings savedRenderSet = app.renderSet;
  bool renderStarted = false;
  if (!renderOut.empty()) {
    auto& r = app.renderSet;
    std::string ext = std::filesystem::path(renderOut).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (renderW > 0 && renderH > 0) { r.width = renderW; r.height = renderH; }
    if (renderSamples > 0) r.samples = renderSamples;
    if (renderAA > 0) r.aa = renderAA;
    r.sceneRange = true;
    r.stillFormat = (ext == ".jpg" || ext == ".jpeg") ? 1 : 0;
    r.animFormat = ext == ".avi" ? 1 : ext == ".mp4" ? 3 : ext == ".webm" ? 4 : ext == ".png" ? 2 : 0;
    if (ext == ".gif" || ext == ".avi" || ext == ".mp4" || ext == ".webm") renderAnim = true;
    (renderAnim ? r.animPath : r.stillPath) = renderOut;
  }

  auto t0 = std::chrono::steady_clock::now();
  double last = 0;
  int frame = 0;
  bool frameStats = std::getenv("FD_FRAME_STATS") != nullptr;  // prints UI frame time statistics at exit
  std::vector<double> frameTimes;
  while (!ui->exitNow) {
    glfwPollEvents();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double rawDt = t - last, dt = std::min(0.1, rawDt);
    last = t;
    app.now = t;
    if (frameStats && frame > 2) frameTimes.push_back(rawDt);
    app.fps = app.fps * 0.9 + (dt > 0 ? 1 / dt : 0) * 0.1;
    app.lastDt_ = dt;  // (dynamic resolution reacts to the raw frame time)
    // explore mouse capture
    if (app.wantCapture && !screenshot.empty()) app.wantCapture = false;
    if (app.wantCapture) { glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED); app.mouseCaptured = true; app.wantCapture = false; }
    if (app.wantRelease || (app.mouseCaptured && app.mode != Mode::Explore && !app.flying())) {
      glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      app.mouseCaptured = false;
      app.wantRelease = false;
    }
    if (app.mouseCaptured) io.ConfigFlags |= ImGuiConfigFlags_NoMouse; else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

#ifdef FD_HAVE_VULKAN
    if (useVulkan && !vk.beginFrame(win)) {  // minimised or the swapchain is being rebuilt
      glfwWaitEventsTimeout(0.05);
      continue;
    }
    if (useVulkan) ImGui_ImplVulkan_NewFrame();
#endif
    if (!useVulkan) ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    app.tickAnimation(dt);
    if (!renderOut.empty() && !renderStarted && frame >= 1) {
      std::string rerr;
      if (!app.startRender(renderAnim, rerr)) { std::fprintf(stderr, "render: %s\n", rerr.c_str()); break; }
      renderStarted = true;
    }
    app.renderStep(app.renderBudget());  // offline render (Render window): a slice of work per frame (priority)
    ui->frame(dt);
    app.renderFrame();  // records / draws the scene into the offscreen targets
    app.autosaveTick();
    ImGui::Render();
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(win, &fw, &fh);
    // headless runs with render threads wait until their images are finished before capturing
    bool threadsBusy = (app.viewThread && app.viewThread->busy()) || (app.thumbThread && app.thumbThread->busy());
    bool shoot = !screenshot.empty() && frame + 1 >= frames && !threadsBusy;
    std::vector<uint8_t> px;
#ifdef FD_HAVE_VULKAN
    if (useVulkan) {
      vk.endFrame(ImGui::GetDrawData(), shoot ? &px : nullptr);
      fw = (int)vk.extent.width;
      fh = (int)vk.extent.height;
    }
#endif
    if (!useVulkan) {
      gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
      gl::Viewport(0, 0, fw, fh);
      gl::ClearColor(0.09f, 0.1f, 0.12f, 1);
      gl::Clear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      {  // UI frame clock for the render threads: a fence per frame, polled without waiting
        static std::deque<std::pair<uint64_t, GLsync>> fences;
        fences.push_back({uiFrames().current.load(), gl::FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
        uiFrames().current++;
        gl::Flush();
        while (!fences.empty()) {
          GLenum r = gl::ClientWaitSync(fences.front().second, 0, 0);
          if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) break;
          uiFrames().completed = fences.front().first;
          gl::DeleteSync(fences.front().second);
          fences.pop_front();
        }
      }
      if (shoot) {
        px.resize((size_t)fw * fh * 4);
        gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl::ReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        // OpenGL rows are bottom-up
        std::vector<uint8_t> flipped(px.size());
        for (int y = 0; y < fh; y++) std::memcpy(&flipped[(size_t)y * fw * 4], &px[(size_t)(fh - 1 - y) * fw * 4], (size_t)fw * 4);
        px.swap(flipped);
      }
    }
    if (debugRayArg && frame == 1 && !app.debugFrozen) {  // freeze the ray, then look at it from the side
      app.debugFrozen = true;
      double pxl = turnDeg * PI / 180 / 0.003;
      if (app.curved()) {
        if (strafe != 0) app.curvCam.move(app.space, {strafe, 0, 0, 0}, nullptr);
        app.curvCam.look(app.space, pxl, 0);
      } else if (app.geo && app.geoEdit.valid) {
        if (strafe != 0) app.geoEdit.move(worldField(*app.ev), {strafe, 0, 0}, app.quality.geoMaxStep * 0.5, nullptr);
        app.geoEdit.look(pxl, 0);
      } else {
        if (strafe != 0) app.editCam.move({strafe, 0, 0, 0}, nullptr);
        app.editCam.look(pxl, 0);
      }
    }
    if (renderStarted && !app.job.active) {
      if (app.job.finished) std::printf("rendered %s (%.1f s)\n", app.job.path.c_str(), app.job.seconds);
      else std::fprintf(stderr, "render failed: %s\n", app.job.error.c_str());
      break;
    }
    if (!renderOut.empty()) { frame++; if (!useVulkan) glfwSwapBuffers(win); continue; }
    if (!screenshot.empty() && shoot && ++frame >= frames) {
      writePPM(screenshot, fw, fh, px);
      break;
    }
    if (!screenshot.empty() && !shoot && frame + 1 < frames) frame++;
    if (!useVulkan) glfwSwapBuffers(win);
    if (app.quality.frameLimit > 0 && screenshot.empty()) {  // Settings > Rendering > Frame rate limit
      double left = 1.0 / app.quality.frameLimit - (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - t);
      if (left > 0.001) glfwWaitEventsTimeout(left);
    }
  }
  if (frameStats && !frameTimes.empty()) {
    std::sort(frameTimes.begin(), frameTimes.end());
    std::printf("UI frames: %zu, median %.1f ms, 95%% %.1f ms, max %.1f ms\n", frameTimes.size(), frameTimes[frameTimes.size() / 2] * 1000,
                frameTimes[frameTimes.size() * 95 / 100] * 1000, frameTimes.back() * 1000);
  }
  if (!renderOut.empty()) app.renderSet = savedRenderSet;
  if (qualityOverride) app.quality = savedQuality;  // (the overrides are not saved)
  app.saveSettings();
  if (screenshot.empty() && renderOut.empty()) app.clearRecovery();  // quitting normally (unsaved changes were already asked about)
  g_ui = nullptr;
  delete ui;
  app.stopRenderThreads();  // (before their renderers' devices / contexts go)
  app.renderer->destroy(app.mainRT);
  app.renderer->destroy(app.thumbRT);
  app.renderer->destroy(app.job.rt);
  app.renderer->destroy(app.job.acc);
  app.renderer->destroy(app.mainAccRT);
  app.renderer->destroy(app.mainGuideRT);
  app.renderer->destroy(app.job.guide);
  for (int i = 0; i < 2; i++) { app.renderer->destroy(app.mainTmpRT[i]); app.renderer->destroy(app.job.tmp[i]); }
  app.renderer.reset();
#ifdef FD_HAVE_VULKAN
  if (useVulkan) {
    vkDeviceWaitIdle(vk.device);
    ImGui_ImplVulkan_Shutdown();
  }
#endif
  if (!useVulkan) ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
#ifdef FD_HAVE_VULKAN
  if (useVulkan) vk.shutdown();
#endif
  if (glWorker) glfwDestroyWindow(glWorker);
  for (auto* w : glThreadWin) if (w) glfwDestroyWindow(w);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
