#pragma once
#include "app.hpp"
#include "imgui.h"

struct GLFWwindow;

namespace fd {

class UI {
 public:
  UI(App& app, GLFWwindow* win);
  /** Builds all ImGui windows for this frame and forwards viewport input to the app. */
  void frame(double dt);
  void onKey(int key, int action, int mods);
  void onScroll(double y) { wheel_ += y; }
  bool quitRequested = false;

 private:
  App& app;
  GLFWwindow* win_;
  double wheel_ = 0;
  double lastMx_ = 0, lastMy_ = 0;
  bool prevDown_[3] = {false, false, false};
  bool vpHovered_ = false;
  bool showHelp_ = false, showQuality_ = false, showBuilder_ = false;
  // builder state
  PrimitiveDef draft_;
  bool draftEditing_ = false;
  FullCamera previewCam_;
  float previewW_ = 0, previewZW_ = 0;
  RenderTarget previewRT_;
  // path prompt fallback (when no native file dialog is available)
  int pendingAction_ = 0;
  char pathBuf_[1024] = "";
  char renameBuf_[256] = "";
  std::string renameId_;

  void menuBar();
  void toolbar();
  void libraryPanel();
  void treePanel();
  void treeNode(Node& n, int depth);
  void propertiesPanel();
  void viewport(double dt);
  void overlays(ImDrawList* dl);
  void thumbnail();
  void builderWindow();
  void qualityWindow();
  void helpWindow();
  void pathPrompt();
  void openBuilder(const PrimitiveDef* base, bool editing);
  // file actions (1 open scene, 2 save scene, 3 import library, 4 export library, 5 export object, 6 export primitive)
  void fileAction(int action);
  void doFileAction(int action, const std::string& path);
  std::string exportPrimId_;
};

}  // namespace fd
