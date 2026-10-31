// 4DCAD entry point: window, OpenGL context, ImGui, main loop.
// Command line (mainly for automated testing):
//   4DCAD [scene.json] [--example playground|planet] [--geo] [--explore]
//         [--select NAME] [--tool move|rotate] [--size WxH] [--screenshot out.ppm --frames N]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app.hpp"
#include "examples.hpp"
#include "gl.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ui.hpp"

using namespace fd;

static UI* g_ui = nullptr;

static void writePPM(const std::string& path, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 4);
  gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
  gl::ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--)
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
  std::string selectName, toolName;
  bool startGeo = false, startExplore = false;
  int frames = 30, W = 1600, H = 900;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--example" && i + 1 < argc) example = argv[++i];
    else if (a == "--geo") startGeo = true;
    else if (a == "--select" && i + 1 < argc) selectName = argv[++i];
    else if (a == "--tool" && i + 1 < argc) toolName = argv[++i];
    else if (a == "--explore") startExplore = true;
    else if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
    else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &W, &H);
    else if (a[0] != '-') sceneArg = a;
  }

  if (!glfwInit()) { std::fprintf(stderr, "Failed to initialise GLFW\n"); return 1; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  GLFWwindow* win = glfwCreateWindow(W, H, "4DCAD", nullptr, nullptr);
  if (!win) { std::fprintf(stderr, "Failed to create an OpenGL 3.3 window (update your graphics driver)\n"); glfwTerminate(); return 1; }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  if (const char* missing = gl::load((void* (*)(const char*))glfwGetProcAddress)) {
    std::fprintf(stderr, "OpenGL function missing: %s\n", missing);
    return 1;
  }
  std::printf("OpenGL: %s | %s\n", (const char*)gl::GetString(GL_VERSION), (const char*)gl::GetString(GL_RENDERER));

  App app;
  UI* ui = nullptr;
  // our callbacks first; ImGui chains to them
  glfwSetKeyCallback(win, [](GLFWwindow*, int key, int, int action, int mods) { if (g_ui) g_ui->onKey(key, action, mods); });
  glfwSetScrollCallback(win, [](GLFWwindow*, double, double y) { if (g_ui) g_ui->onScroll(y); });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  float xs = 1, ys = 1;
  glfwGetWindowContentScale(win, &xs, &ys);
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0;
  st.FrameRounding = 3;
  st.ScaleAllSizes(xs);
  loadFonts(xs);
  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 330");

  app.init();
  ui = new UI(app, win);
  g_ui = ui;
  std::string err;
  if (!sceneArg.empty() && !app.loadSceneFile(sceneArg, err)) app.toast(err, true);
  for (auto& ex : EXAMPLES) if (example == ex.id) app.loadScene(ex.make());
  if (startGeo) app.setGeo(true);
  if (!selectName.empty()) walk(app.root, [&](Node& n, Node*, int) { if (n.name == selectName && app.selection.empty()) app.selection = {n.id}; });
  if (toolName == "move") app.tool = Tool::Move;
  if (toolName == "rotate") app.tool = Tool::Rotate;
  if (startExplore) app.setMode(Mode::Explore);

  auto t0 = std::chrono::steady_clock::now();
  double last = 0;
  int frame = 0;
  while (!glfwWindowShouldClose(win) && !ui->quitRequested) {
    glfwPollEvents();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double dt = std::min(0.1, t - last);
    last = t;
    app.now = t;
    app.fps = app.fps * 0.9 + (dt > 0 ? 1 / dt : 0) * 0.1;
    // explore mouse capture
    if (app.wantCapture && !screenshot.empty()) app.wantCapture = false;
    if (app.wantCapture) { glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED); app.mouseCaptured = true; app.wantCapture = false; }
    if (app.wantRelease || (app.mouseCaptured && app.mode != Mode::Explore && !app.flying())) {
      glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      app.mouseCaptured = false;
      app.wantRelease = false;
    }
    if (app.mouseCaptured) io.ConfigFlags |= ImGuiConfigFlags_NoMouse; else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ui->frame(dt);
    app.renderFrame();
    ImGui::Render();
    int fw, fh;
    glfwGetFramebufferSize(win, &fw, &fh);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::Viewport(0, 0, fw, fh);
    gl::ClearColor(0.09f, 0.1f, 0.12f, 1);
    gl::Clear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!screenshot.empty() && ++frame >= frames) {
      writePPM(screenshot, fw, fh);
      break;
    }
    glfwSwapBuffers(win);
  }
  app.autosave();
  g_ui = nullptr;
  delete ui;
  app.mainRT.destroy();
  app.thumbRT.destroy();
  app.renderer.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
