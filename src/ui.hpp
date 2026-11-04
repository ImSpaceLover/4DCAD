#pragma once
#include "app.hpp"
#include "imgui.h"

#include <functional>
#include <set>
#include <tuple>

struct GLFWwindow;

namespace fd {

class UI {
 public:
  UI(App& app, GLFWwindow* win);
  /** Builds all ImGui windows for this frame and forwards viewport input to the app. */
  void frame(double dt);
  void onKey(int key, int action, int mods);
  void onScroll(double y) { wheel_ += y; }
  bool exitNow = false;  // set once quitting is confirmed
  void requestQuit();    // asks to save unsaved changes first
  void debugOpen(const std::string& what) { if (what == "settings") showQuality_ = true; if (what == "graph") showGraph_ = true; if (what == "render") showRender_ = true; if (what == "new") openNewScene_ = true; if (what == "sdf") { showSdf_ = true; if (app.selected()) sdfNodeId_ = app.selected()->id; } }

 private:
  App& app;
  GLFWwindow* win_;
  double wheel_ = 0;
  double lastMx_ = 0, lastMy_ = 0;
  bool prevDown_[3] = {false, false, false};
  bool vpHovered_ = false;
  bool showHelp_ = false, showQuality_ = false;
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
  void cameraProperties(Node& n, bool& live, bool& done);
  void viewport(double dt);
  void overlays(ImDrawList* dl);
  void thumbnail();
  void qualityWindow();
  void sdfWindow();
  void unsavedPrompt();
  void recoveryPrompt();
  void copy();   // selection -> system clipboard
  void paste();  // system clipboard -> scene
  char treeFilter_[128] = "";
  int treeOpenAll_ = -1;  // 0 collapse all, 1 expand all (for one frame)
  bool treeMatches(const Node& n);  // after a crash: recover or discard the autosaved changes
  void guarded(std::function<void()> action);  // runs action, asking about unsaved changes first
  void saveScene();
  bool showSdf_ = false, openUnsaved_ = false;
  std::string sdfNodeId_, codeEditId_, codeBuf_, codeErr_;
  std::function<void()> pendingGuarded_;
  void helpWindow();
  void pathPrompt();
  // file actions (1 open scene, 2 save scene, 3 import library, 4 export library, 5 export object, 6 export primitive)
  void fileAction(int action);
  void doFileAction(int action, const std::string& path);
  std::string exportPrimId_;
  int captureAction_ = -1, captureSlot_ = 0;  // Settings > Keys: waiting for a key press
  void keysTab();
  void lightsWindow();
  void renderWindow();
  void startRender(bool animation);
  bool showRender_ = false;
  void presetsWidget();       // render presets (Settings > Rendering and the Render window)
  std::string presetSel_;     // preset chosen in the list
  char presetName_[128] = {};  // Save as / Rename
  bool openPathPrompt_ = false;
  void newSceneDialog();
  bool openNewScene_ = false;
  int newKind_ = 1;
  float newRadius_ = 8;
  int ffmpegOk_ = -1;  // -1 = not checked, 0 / 1
  std::string ffmpegChecked_;
  // timeline + graph editor (ui_timeline.cpp)
  void timelinePanel();
  void graphEditor(const std::vector<int>& trackIds, bool& changed, bool& commit);
  // dockable panels (Window menu) and the dock space under the toolbar
  bool showLibrary_ = true, showScene_ = true, showProperties_ = true, showTimelineWin_ = true;
  bool resetLayout_ = false;
  void dockSpace();
  std::string targetName(const std::string& target);
  std::string pathLabel(const std::string& target, const std::string& path);
  void deleteSelectedKeys();
  void copySelectedKeys();
  void pasteKeys();
  /** Per-value keying in Properties: a diamond next to the last widget, right-click menu. */
  void keyable(const std::string& target, const std::vector<std::string>& paths);
  bool showTimeline_ = true, showGraph_ = false, graphFit_ = true, resetView_ = true;
  double tl0_ = 0, tl1_ = 0;           // visible frame range
  bool scrub_ = false, keyDrag_ = false, timelineHovered_ = false, graphHovered_ = false;
  float dragX0_ = 0;
  struct DragKey { Key* key; double frame, value; };
  std::vector<DragKey> dragOrig_;
  double gx0_ = 0, gx1_ = 0, gy0_ = 0, gy1_ = 0, gx0Drag_ = 0, gy0Drag_ = 0;
  int gDrag_ = 0;
  Key* gHandleKey_ = nullptr;
  std::set<std::string> graphHidden_;
  std::vector<std::tuple<std::string, std::string, Key>> keyClipboard_;
};

}  // namespace fd
