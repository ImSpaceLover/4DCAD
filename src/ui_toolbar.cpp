// Menu bar and toolbar.
#include "ui_common.hpp"

namespace fd {

// ============================================================ menu & toolbar
void UI::menuBar() {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New", "Ctrl+N")) guarded([this] { app.newScene(); });
    if (ImGui::MenuItem("Open...", "Ctrl+O")) guarded([this] { fileAction(1); });
    if (ImGui::MenuItem("Save", "Ctrl+S")) saveScene();
    if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) fileAction(2);
    if (ImGui::BeginMenu("Examples")) {
      for (auto& ex : EXAMPLES)
        if (ImGui::MenuItem(ex.name)) { auto mk = ex.make; guarded([this, mk] { app.loadScene(mk()); app.currentFile.clear(); }); }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Import object / library...")) fileAction(3);
    if (ImGui::MenuItem("Export library...")) fileAction(4);
    if (ImGui::MenuItem("Export selected object...", nullptr, false, app.selected() != nullptr)) fileAction(5);
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) requestQuit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, app.canUndo())) app.undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, app.canRedo())) app.redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", "Shift+D", false, !app.selection.empty())) app.duplicateSelection();
    if (ImGui::MenuItem("Delete", "X / Del", false, !app.selection.empty())) app.deleteSelection();
    if (ImGui::BeginMenu("Group selection as", !app.selection.empty())) {
      for (int i = 0; i < 6; i++) if (ImGui::MenuItem(BOOL_OP_LABELS[i])) app.groupSelection((BoolOp)i);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Edit mode", "Tab", app.mode == Mode::Edit)) app.setMode(Mode::Edit);
    if (ImGui::MenuItem("Explore mode", "Tab", app.mode == Mode::Explore)) app.setMode(Mode::Explore);
    ImGui::Separator();
    if (ImGui::MenuItem("Full 4D", "V", !app.geo)) app.setGeo(false);
    if (ImGui::MenuItem("Geodesic 3D", "V", app.geo)) app.setGeo(true);
    ImGui::Separator();
    ImGui::MenuItem("Outside thumbnail (Geodesic)", nullptr, &app.editSet.showThumbnail);
    ImGui::MenuItem("Show own body (Explore)", nullptr, &app.exploreSet.showOwnBody);
    if (ImGui::MenuItem("Reset 4D orientation", "Shift+W")) app.editCam.reset4();
    if (ImGui::MenuItem("Front / Right / Top / Along W", "1 3 7 9")) app.standardView(0);
    ImGui::Separator();
    ImGui::MenuItem("Settings...", nullptr, &showQuality_);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Help")) showHelp_ = !showHelp_;
  ImGui::EndMainMenuBar();
}

void UI::toolbar() {
  float y = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos({0, y});
  ImGui::SetNextWindowSize({ImGui::GetIO().DisplaySize.x, TOOLBAR_H});
  ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  if (toggleButton("Edit", app.mode == Mode::Edit, "Edit mode (Tab)")) app.setMode(Mode::Edit);
  ImGui::SameLine(0, 2);
  if (toggleButton("Explore", app.mode == Mode::Explore, "Explore mode (Tab)")) app.setMode(Mode::Explore);
  ImGui::SameLine(0, 16);
  if (toggleButton("Full 4D", !app.geo, "Full 4D view (V)")) app.setGeo(false);
  ImGui::SameLine(0, 2);
  if (toggleButton("Geodesic 3D", app.geo, "Geodesic 3D view (V): live on the world's surface")) app.setGeo(true);
  ImGui::SameLine(0, 16);
  // world selector
  std::string cur = "World: none";
  if (Node* w = app.world.empty() ? nullptr : findNode(app.root, app.world)) cur = "World: " + w->name;
  ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo("##world", cur.c_str())) {
    if (ImGui::Selectable("none", app.world.empty())) app.setWorld("");
    walk(app.root, [&](Node& n, Node*, int) {
      if (n.id == app.root.id || n.anchor || nodeDim(n, app.reg) < 4) return;
      ImGui::PushID(n.id.c_str());
      if (ImGui::Selectable(n.name.c_str(), n.id == app.world)) app.setWorld(n.id);
      ImGui::PopID();
    });
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("The 4D object whose 3D surface is the space of the Geodesic view");
  if (app.mode == Mode::Edit) {
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("Tool:");
    ImGui::SameLine();
    if (toggleButton("Select", app.tool == Tool::Select, "Q: click to select")) app.tool = Tool::Select;
    ImGui::SameLine(0, 2);
    if (toggleButton("Move", app.tool == Tool::Move, "W: gizmo arrows along the object's local X / Y / Z (kept inside the visible slice)")) app.tool = Tool::Move;
    ImGui::SameLine(0, 2);
    if (toggleButton("Rotate", app.tool == Tool::Rotate, "E: gizmo rings to rotate about the object's local X / Y / Z")) app.tool = Tool::Rotate;
    ImGui::SameLine(0, 10);
    auto& e = app.editSet;
    if (toggleButton("Local", !e.globalAxes, "L: move / rotate along the object's own axes")) e.globalAxes = false;
    ImGui::SameLine(0, 2);
    if (toggleButton("Global", e.globalAxes, "L: move / rotate along the world axes (on a world surface: the surface's directions)")) e.globalAxes = true;
    ImGui::SameLine(0, 10);
    if (toggleButton("Snap", e.snap, "Snap moves to the world grid, rotations and scale to steps (Settings > Edit mode).\nHold Ctrl while transforming to invert.")) e.snap = !e.snap;
  }
  ImGui::SameLine(0, 16);
  if (ImGui::Button("Undo")) app.undo();
  ImGui::SameLine(0, 2);
  if (ImGui::Button("Redo")) app.redo();
  ImGui::SameLine(0, 16);
  if (ImGui::Button("Settings")) showQuality_ = !showQuality_;
  if (app.dirty()) { ImGui::SameLine(); ImGui::TextColored({1, 0.75f, 0.3f, 1}, "unsaved"); }
  ImGui::SameLine();
  ImGui::TextDisabled("%.0f fps", app.fps);
  ImGui::End();
}

}  // namespace fd
