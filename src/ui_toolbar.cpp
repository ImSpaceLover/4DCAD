// Menu bar and toolbar.
#include "ui_common.hpp"

namespace fd {

// ============================================================ menu & toolbar
void UI::menuBar() {
  if (!ImGui::BeginMainMenuBar()) return;
  auto K = [this](Action a) { return app.keys.text(a); };  // current binding, shown in the menus
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New...", K(Action::New).c_str())) guarded([this] { openNewScene_ = true; });
    if (ImGui::MenuItem("Open...", K(Action::Open).c_str())) guarded([this] { fileAction(1); });
    if (ImGui::MenuItem("Save", K(Action::Save).c_str())) saveScene();
    if (ImGui::MenuItem("Save as...", K(Action::SaveAs).c_str())) fileAction(2);
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
    bool any = !app.selection.empty();
    if (ImGui::MenuItem("Undo", K(Action::Undo).c_str(), false, app.canUndo())) app.undo();
    if (ImGui::MenuItem("Redo", K(Action::Redo).c_str(), false, app.canRedo())) app.redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", K(Action::Copy).c_str(), false, any)) copy();
    if (ImGui::MenuItem("Paste", K(Action::Paste).c_str())) paste();
    if (ImGui::MenuItem("Duplicate", K(Action::Duplicate).c_str(), false, any)) app.duplicateSelection();
    if (ImGui::MenuItem("Duplicate linked", K(Action::DuplicateLinked).c_str(), false, any)) app.duplicateLinked();
    if (ImGui::MenuItem("Delete", K(Action::Delete).c_str(), false, any)) app.deleteSelection();
    if (ImGui::MenuItem(app.isolate.empty() ? "Isolate selection" : "Show all (end isolation)", K(Action::Isolate).c_str(), false,
                        any || !app.isolate.empty()))
      app.toggleIsolate();
    if (ImGui::BeginMenu("Group selection as", !app.selection.empty())) {
      for (int i = 0; i < 6; i++) if (ImGui::MenuItem(BOOL_OP_LABELS[i])) app.groupSelection((BoolOp)i);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Edit mode", K(Action::ToggleMode).c_str(), app.mode == Mode::Edit)) app.setMode(Mode::Edit);
    if (ImGui::MenuItem("Explore mode", K(Action::ToggleMode).c_str(), app.mode == Mode::Explore)) app.setMode(Mode::Explore);
    ImGui::Separator();
    if (ImGui::MenuItem("Full 4D", K(Action::ToggleView).c_str(), !app.geo)) app.setGeo(false);
    if (ImGui::MenuItem("Geodesic 3D", K(Action::ToggleView).c_str(), app.geo)) app.setGeo(true);
    ImGui::Separator();
    ImGui::MenuItem("Outside thumbnail (Geodesic)", nullptr, &app.editSet.showThumbnail);
    ImGui::MenuItem("Show own body (Explore)", nullptr, &app.exploreSet.showOwnBody);
    if (ImGui::MenuItem("Reset 4D orientation", K(Action::Reset4D).c_str())) app.editCam.reset4();
    if (ImGui::BeginMenu("Slider axis", !app.geo)) {
      for (int k = 0; k < 4; k++) {
        char lb[24];
        std::snprintf(lb, sizeof lb, "%c on the slider", AXIS[k]);
        if (ImGui::MenuItem(lb, nullptr, app.editCam.slider == k)) app.setSliderAxis(k);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Standard view")) {
      if (ImGui::MenuItem("Front", K(Action::ViewFront).c_str())) app.standardView(0);
      if (ImGui::MenuItem("Right", K(Action::ViewRight).c_str())) app.standardView(1);
      if (ImGui::MenuItem("Top", K(Action::ViewTop).c_str())) app.standardView(2);
      if (ImGui::MenuItem("Along the slider axis", K(Action::ViewAlongW).c_str())) app.standardView(3);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Cameras")) {
      if (ImGui::MenuItem("Add camera at the view")) app.addCamera();
      Node* sel = app.selected();
      bool selCam = sel && sel->isCamera();
      if (ImGui::MenuItem("Look through selected camera", K(Action::LookThrough).c_str(), false, selCam)) app.lookThrough(sel->id);
      if (ImGui::MenuItem("Selected camera to the view", K(Action::CameraToView).c_str(), false, selCam)) app.cameraToView(sel->id);
      std::vector<std::pair<std::string, std::string>> cams;
      walk(app.root, [&](Node& n, Node*, int) { if (n.isCamera()) cams.push_back({n.id, n.name}); });
      if (!cams.empty()) ImGui::SeparatorText("Look through");
      for (auto& [id, name] : cams) {
        ImGui::PushID(id.c_str());
        std::string label = name + (id == app.activeCamera ? "  (active)" : "");
        if (ImGui::MenuItem(label.c_str())) app.lookThrough(id);
        ImGui::PopID();
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::MenuItem("Lights...", nullptr, &app.showLights);
    ImGui::MenuItem("Settings...", nullptr, &showQuality_);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Render")) {
    std::string k1 = app.keys.text(Action::RenderImage), k2 = app.keys.text(Action::RenderAnimation);
    if (ImGui::MenuItem("Render image", k1.c_str(), false, !app.job.active)) startRender(false);
    if (ImGui::MenuItem("Render animation", k2.c_str(), false, !app.job.active)) startRender(true);
    ImGui::Separator();
    ImGui::MenuItem("Render window...", nullptr, &showRender_);
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
  if (app.curved()) {
    // curved 3D space: its kind and curvature radius (the space bends around unchanged coordinates)
    ImGui::TextUnformatted(SPACE_NAMES[(int)app.space.kind]);
    ImGui::SameLine();
    float r = (float)app.space.radius;
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("##curvR", &r, 0.02f * std::max(1.f, r), 0.2f, 1000.f, "R = %.2f", ImGuiSliderFlags_Logarithmic)) app.setCurvatureRadius(r);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.commit();
    keyable("scene", {"space.radius"});
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Right-click: keyframes (the curvature can be animated).\nCurvature radius: curvature = %s1 / R^2 (%s).\nSmaller = more strongly curved; objects keep their coordinates.",
                        app.space.kind == SpaceKind::Spherical ? "+" : "-", app.space.kind == SpaceKind::Spherical ? "spherical" : "hyperbolic");
  }
  if (!app.curved()) {
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
  }
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
