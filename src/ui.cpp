// UI core: frame, keys, file actions, unsaved-changes prompts.
#include "ui_common.hpp"

namespace fd {

UI::UI(App& a, GLFWwindow* w) : app(a), win_(w) {
}

// ============================================================ frame
void UI::frame(double dt) {
  ImGuiIO& io = ImGui::GetIO();
  menuBar();
  toolbar();
  libraryPanel();
  treePanel();
  propertiesPanel();
  viewport(dt);
  timelinePanel();
  // I: insert keyframes for the selected objects
  if (app.keyMenuRequest) { ImGui::OpenPopup("Insert keyframe##menu"); app.keyMenuRequest = false; }
  if (ImGui::BeginPopup("Insert keyframe##menu")) {
    ImGui::TextDisabled("Insert keyframe");
    if (ImGui::MenuItem("Position / rotation / scale")) app.insertKeys(true);
    if (ImGui::MenuItem("All values")) app.insertKeys(false);
    ImGui::EndPopup();
  }
  if (showQuality_) qualityWindow();
  if (showHelp_) helpWindow();
  if (showSdf_) sdfWindow();
  if (app.showLights) lightsWindow();
  if (showRender_) renderWindow();
  pathPrompt();
  unsavedPrompt();
  recoveryPrompt();

  // toast
  if (app.now < app.toastUntil && !app.toastMsg.empty()) {
    ImVec2 ds = io.DisplaySize;
    ImGui::SetNextWindowPos({ds.x * 0.5f, ds.y - 40}, ImGuiCond_Always, {0.5f, 1});
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, app.toastError ? ImVec4(0.45f, 0.12f, 0.12f, 1) : ImVec4(0.2f, 0.23f, 0.28f, 1));
    ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(app.toastMsg.c_str());
    ImGui::End();
    ImGui::PopStyleColor();
  }
}

void UI::onKey(int key, int action, int mods) {
  if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
  bool ctrl = (mods & GLFW_MOD_CONTROL) || (mods & GLFW_MOD_SUPER);
  bool shift = mods & GLFW_MOD_SHIFT, alt = mods & GLFW_MOD_ALT;
  if (captureAction_ >= 0) {  // Settings > Keys: the next key press becomes the binding
    if (action != GLFW_PRESS || key == GLFW_KEY_UNKNOWN) return;
    bool isMod = key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT || key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL ||
                 key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT || key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER;
    Action a = (Action)captureAction_;
    bool held = actionInfo(a).held;
    if (isMod && !held) return;  // shortcuts: wait for the main key (modifiers are taken from it)
    KeyCombo c{key, held ? false : ctrl, held ? false : shift, held ? false : alt};
    app.keys.at(a)[captureSlot_] = c;
    captureAction_ = -1;
    app.saveSettings();
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput && !app.mouseCaptured) return;
  auto P = [&](Action a) { return app.keys.pressed(a, key, ctrl, shift, alt); };
  // keys over the timeline / graph editor act on keyframes
  if ((timelineHovered_ || graphHovered_) && action == GLFW_PRESS) {
    if (!ctrl && (key == GLFW_KEY_DELETE || key == GLFW_KEY_X)) { deleteSelectedKeys(); app.setFrame(app.anim.frame); app.commit(); return; }
    if (ctrl && key == GLFW_KEY_C) { copySelectedKeys(); return; }
    if (ctrl && key == GLFW_KEY_V) { pasteKeys(); return; }
    if (key == GLFW_KEY_HOME) { graphFit_ = true; resetView_ = true; return; }
  }
  if (action == GLFW_REPEAT && !(P(Action::Undo) || P(Action::Redo))) return;
  if (P(Action::Save)) { saveScene(); return; }
  if (P(Action::SaveAs)) { fileAction(2); return; }
  if (P(Action::Open)) { guarded([this] { fileAction(1); }); return; }
  if (P(Action::New)) { guarded([this] { app.newScene(); }); return; }
  if (P(Action::Copy)) { copy(); return; }
  if (P(Action::Paste)) { paste(); return; }
  if (P(Action::RenderImage) || P(Action::RenderAnimation)) { startRender(P(Action::RenderAnimation)); return; }
  app.handleKey(key, shift, ctrl, alt);
}

// ============================================================ file actions
void UI::fileAction(int action) {
  const char* titles[] = {"", "Open scene", "Save scene", "Import library item", "Export library", "Export object", "Export library object",
                          "Import render preset", "Export render preset"};
  std::string def;
  if (action == 2) def = app.currentFile.empty() ? "scene.4dscene.json" : app.currentFile;
  if (action == 4) def = "4dcad-library.json";
  if (action == 5) def = (app.selected() ? app.selected()->name : std::string("object")) + ".4dobj.json";
  if (action == 6) { auto* o = app.lib.find(exportPrimId_); def = (o ? o->name : std::string("object")) + ".4dobj.json"; }
  if (action == 8) def = presetSel_ + ".4dpreset.json";
  if (!nativeDialogsAvailable()) {
    pendingAction_ = action;
    std::snprintf(pathBuf_, sizeof pathBuf_, "%s", def.c_str());
    openPathPrompt_ = true;  // opened at the top level (pathPrompt), whichever window asked
    return;
  }
  std::string path = (action == 1 || action == 3 || action == 7) ? openFileDialog(titles[action]) : saveFileDialog(titles[action], def);
  if (!path.empty()) doFileAction(action, path);
}

void UI::doFileAction(int action, const std::string& path) {
  std::string err;
  switch (action) {
    case 1: if (app.loadSceneFile(path, err)) app.toast("Opened " + path); else app.toast(err, true); break;
    case 2: if (app.saveSceneFile(path, err)) app.toast("Saved " + path); else app.toast(err, true); break;
    case 3: {
      std::string text;
      if (!readFile(path, text)) { app.toast("Cannot read " + path, true); break; }
      try { app.toast("Imported " + app.lib.importText(text) + " into the library"); } catch (const std::exception& e) { app.toast(e.what(), true); }
      break;
    }
    case 4: writeFile(path, app.lib.exportText()) ? app.toast("Exported library") : app.toast("Cannot write " + path, true); break;
    case 5: {
      Node* n = app.selected();
      if (!n) break;
      writeFile(path, libraryObjectToJson(makeLibraryObject(*n, app.reg, n->name))) ? app.toast("Exported object") : app.toast("Cannot write", true);
      break;
    }
    case 7: {
      std::string name;
      if (app.importPreset(path, err, &name)) { presetSel_ = name; app.toast("Imported preset " + name); } else app.toast(err, true);
      break;
    }
    case 8: if (app.exportPreset(presetSel_, path, err)) app.toast("Exported preset " + presetSel_); else app.toast(err, true); break;
    case 6: {
      if (auto* o = app.lib.find(exportPrimId_)) writeFile(path, libraryObjectToJson(*o)) ? app.toast("Exported " + o->name) : app.toast("Cannot write", true);
      break;
    }
  }
}

void UI::pathPrompt() {
  if (openPathPrompt_) { ImGui::OpenPopup("File path"); openPathPrompt_ = false; }
  if (ImGui::BeginPopupModal("File path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Enter a file path:");
    ImGui::SetNextItemWidth(500);
    bool enter = ImGui::InputText("##path", pathBuf_, sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("OK") || enter) { doFileAction(pendingAction_, pathBuf_); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void UI::guarded(std::function<void()> action) {
  if (!app.dirty()) { action(); return; }
  pendingGuarded_ = std::move(action);
  openUnsaved_ = true;
}

void UI::saveScene() {
  if (app.currentFile.empty()) { fileAction(2); return; }
  std::string err;
  if (app.saveSceneFile(app.currentFile, err)) app.toast("Saved " + app.currentFile); else app.toast(err, true);
}

void UI::requestQuit() { guarded([this] { exitNow = true; }); }

void UI::copy() {
  std::string s = app.copySelection();
  if (s.empty()) return;
  glfwSetClipboardString(win_, s.c_str());
  app.toast("Copied " + std::to_string(app.selection.size()) + " object(s)");
}

void UI::paste() {
  const char* s = glfwGetClipboardString(win_);
  if (!s || !app.pasteNodes(s)) app.toast("The clipboard holds no 4DCAD objects", true);
}

void UI::recoveryPrompt() {
  if (app.pendingRecovery && !ImGui::IsPopupOpen("Recover unsaved work?")) ImGui::OpenPopup("Recover unsaved work?");
  if (ImGui::BeginPopupModal("Recover unsaved work?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    auto& r = *app.pendingRecovery;
    ImGui::TextUnformatted("4DCAD did not close normally last time.");
    ImGui::Text("Unsaved changes%s%s were autosaved%s%s.", r.file.empty() ? "" : " to ", r.file.empty() ? "" : r.file.c_str(),
                r.time.empty() ? "" : " at ", r.time.c_str());
    if (ImGui::Button("Recover")) {
      std::string err;
      if (!app.recover(err)) app.toast(err, true);
      else app.toast("Recovered: save to keep the changes");
      app.pendingRecovery.reset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) { app.clearRecovery(); app.pendingRecovery.reset(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
}

void UI::unsavedPrompt() {
  if (openUnsaved_) { ImGui::OpenPopup("Unsaved changes"); openUnsaved_ = false; }
  if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("The scene has unsaved changes%s%s.", app.currentFile.empty() ? "" : " in ", app.currentFile.c_str());
    ImGui::TextUnformatted("Save them first?");
    if (ImGui::Button("Save")) {
      ImGui::CloseCurrentPopup();
      std::string err;
      std::string path = app.currentFile.empty() ? saveFileDialog("Save scene", "scene.4dscene.json") : app.currentFile;
      if (!path.empty() && app.saveSceneFile(path, err)) { auto f = pendingGuarded_; pendingGuarded_ = nullptr; if (f) f(); }
      else if (!path.empty()) app.toast(err, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't save")) { ImGui::CloseCurrentPopup(); auto f = pendingGuarded_; pendingGuarded_ = nullptr; if (f) f(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); pendingGuarded_ = nullptr; }
    ImGui::EndPopup();
  }
}

}  // namespace fd
