// Settings, SDF code and help windows.
#include "ui_common.hpp"

namespace fd {

// ============================================================ quality / help
void UI::qualityWindow() {
  ImGui::SetNextWindowSize({430, 560}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings", &showQuality_)) { ImGui::End(); return; }
  auto F = [](const char* label, double& v, float lo, float hi, const char* fmt = "%.2f", const char* tip = nullptr) {
    float f = (float)v;
    if (ImGui::SliderFloat(label, &f, lo, hi, fmt)) v = f;
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  auto Ff = [](const char* label, float& v, float lo, float hi, const char* fmt = "%.2f", const char* tip = nullptr) {
    ImGui::SliderFloat(label, &v, lo, hi, fmt);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  if (ImGui::BeginTabBar("settings")) {
    if (ImGui::BeginTabItem("Edit mode")) {
      auto& e = app.editSet;
      F("Field of view", e.fov, 30, 110, "%.0f deg");
      F("Autosave every", e.autosaveSeconds, 0, 600, e.autosaveSeconds <= 0 ? "off" : "%.0f s",
        "Unsaved changes are copied to a recovery file; after a crash you are asked whether to recover them");
      ImGui::SeparatorText("Full 4D");
      F("Orbit speed", e.orbitSpeed, 0.1f, 4);
      F("Pan speed", e.panSpeed, 0.1f, 4);
      F("Zoom speed", e.zoomSpeed, 0.1f, 4);
      F("Turn into W speed", e.rot4Speed, 0.1f, 4, "%.2f", "Alt+drag rotation into the 4th dimension");
      ImGui::Checkbox("W slider", &e.showWSlider);
      ImGui::Checkbox("Ground grid", &app.quality.grid);
      ImGui::SeparatorText("Transforming");
      ImGui::Checkbox("Global axes (L)", &e.globalAxes);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gizmo and G / R axis keys use the world axes instead of the object's own axes");
      ImGui::Checkbox("Snapping", &e.snap);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold Ctrl while transforming to invert");
      F("Grid step", e.snapMove, 0.01f, 5, "%.2f", "Moved objects land on multiples of this (on a world surface: the distance moved)");
      F("Angle step", e.snapAngle, 1, 90, "%.0f deg");
      F("Scale step", e.snapScale, 0.01f, 1, "%.2f");
      ImGui::SeparatorText("Geodesic 3D");
      F("Fly speed", e.flySpeed, 0.2f, 30, "%.1f", "Hold RMB + WASD (the wheel also changes it)");
      F("Look speed", e.lookSpeed, 0.1f, 4);
      ImGui::Checkbox("Outside thumbnail", &e.showThumbnail);
      ImGui::SliderInt("Thumbnail size", &e.thumbSize, 160, 600);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Explore mode")) {
      auto& x = app.exploreSet;
      F("Field of view", x.fov, 30, 110, "%.0f deg");
      F("Look speed", x.lookSpeed, 0.1f, 4);
      ImGui::Checkbox("Show own body", &x.showOwnBody);
      F("Body radius", x.bodyRadius, 0.05f, 1, "%.2f", "Also the collision radius");
      ImGui::SeparatorText("Full 4D");
      F("Move speed", x.moveSpeed, 0.2f, 30, "%.1f", "The wheel also changes it");
      F("Turn into W speed", x.turn4Speed, 0.1f, 4, "%.2f", "Z/X and C/V");
      ImGui::Checkbox("Collisions", &x.collide);
      ImGui::SeparatorText("Geodesic 3D");
      F("Move speed##g", x.geoMoveSpeed, 0.2f, 30, "%.1f");
      ImGui::Checkbox("Collisions##g", &x.geoCollide);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Rendering")) {
      auto& q = app.quality;
      ImGui::SeparatorText("Presets");
      presetsWidget();
      ImGui::SeparatorText("Performance");
      Ff("Resolution", q.resolutionScale, 0.25f, 2.f, "%.2fx");
      Ff("While moving", q.motionScale, 0.2f, 1.f, "%.2fx", "Resolution factor while the camera or objects move");
      ImGui::SeparatorText("Ray marching");
      ImGui::SliderInt("Max steps", &q.maxSteps, 32, 1024);
      Ff("Max distance (4D)", q.maxDistFull, 5, 500, "%.0f", "Rays travelling further than this show the sky colour");
      Ff("Max distance (geodesic)", q.maxDistGeo, 5, 300, "%.0f", "Same, measured along the world surface");
      Ff("Hit precision", q.eps, 0.0002f, 0.02f, "%.4f", "Distance at which a ray counts as hitting a surface");
      Ff("Step scale", q.stepScale, 0.3f, 1.f, "%.2f", "Below 1: smaller, safer steps for inexact SDFs (custom code, heavy modifiers)");
      ImGui::SeparatorText("Lighting");
      ImGui::Checkbox("Soft shadows", &q.shadows);
      Ff("Shadow sharpness", q.shadowK, 2, 64, "%.0f");
      Ff("Shadow distance", q.shadowMax, 1, 50, "%.0f");
      ImGui::Checkbox("Ambient occlusion", &q.ao);
      Ff("AO strength", q.aoStrength, 0, 4);
      Ff("Ambient light", q.ambient, 0, 1);
      Ff("Light direction", q.lightYaw, -3.14f, 3.14f, "%.2f");
      Ff("Light height", q.lightPitch, -1.5f, 1.5f, "%.2f");
      Ff("Light W", q.lightW, -1, 1, "%.2f", "Component of the light direction along the 4th axis");
      Ff("Sun intensity", q.sunIntensity, 0, 3);
      ImGui::Checkbox("Point light shadows", &q.pointShadows);
      ImGui::Checkbox("Emissive objects light the scene", &q.emissiveLights);
      ImGui::SliderInt("Bounces", &q.transparentLayers, 0, 6);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reflections / refractions followed per pixel (0 = mirrors and glass off)");
      ImGui::Checkbox("Over-relaxed marching", &q.relaxedMarching);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Longer steps with a safety check: usually faster (Full 4D)");
      ImGui::SeparatorText("Stochastic sampling");
      ImGui::Checkbox("Refine the view when still", &q.stochastic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While nothing moves, random samples are averaged: soft shadows, glass that reflects and refracts by\n"
                          "Fresnel, blurry reflections of rough mirrors, anti-aliasing, depth of field (cameras)");
      ImGui::BeginDisabled(!q.stochastic);
      ImGui::SliderInt("Samples##vp", &q.viewportSamples, 2, 1024);
      ImGui::EndDisabled();
      Ff("Sun size", q.sunSize, 0, 0.3f, "%.3f rad", "Angular size of the sun: softer shadows");
      Ff("Light radius", q.lightRadius, 0, 2, "%.2f", "Point lights are balls of this size: softer shadows");
      ImGui::SeparatorText("Fast mode (Explore)");
      {
        auto& f = q.fast;
        ImGui::TextDisabled("Preset:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fast")) f = RenderProfile::preset(RenderProfile::Fast);
        ImGui::SameLine();
        if (ImGui::SmallButton("Balanced")) f = RenderProfile::preset(RenderProfile::Balanced);
        ImGui::SameLine();
        if (ImGui::SmallButton("Full")) f = RenderProfile::preset(RenderProfile::Full);
        ImGui::Checkbox("Also while moving in Edit mode", &q.fastWhileMovingInEdit);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use these settings while the camera or objects move in Edit mode too (instead of \"While moving\")");
        Ff("Resolution##fast", f.resolution, 0.2f, 1.5f, "%.2fx");
        ImGui::SliderInt("Max steps##fast", &f.maxSteps, 16, 512);
        ImGui::SliderInt("Bounces##fast", &f.bounces, 0, 6);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reflections / refractions per pixel (0: mirrors and glass look like plain surfaces)");
        ImGui::Checkbox("Textures##fast", &f.textures);
        ImGui::SameLine(160);
        ImGui::Checkbox("Highlights##fast", &f.specular);
        ImGui::Checkbox("Point lights##fast", &f.pointLights);
        ImGui::SameLine(160);
        ImGui::Checkbox("Sun shadows##fast", &f.shadows);
        ImGui::BeginDisabled(!f.pointLights);
        ImGui::Checkbox("Point light shadows##fast", &f.pointShadows);
        ImGui::EndDisabled();
        ImGui::SameLine(160);
        ImGui::Checkbox("Ambient occlusion##fast", &f.ao);
        ImGui::BeginDisabled(!f.ao);
        ImGui::SliderInt("AO samples##fast", &f.aoSamples, 1, 5);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!f.shadows && !(f.pointLights && f.pointShadows));
        ImGui::SliderInt("Shadow ray steps##fast", &f.shadowSteps, 8, 128);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fewer = faster, shadows of far objects may be missed");
        ImGui::EndDisabled();
      }
      ImGui::SeparatorText("Sky / fog");
      ImGui::ColorEdit3("Sky top (4D)", q.skyTop.data());
      ImGui::ColorEdit3("Sky bottom (4D)", q.skyBottom.data());
      Ff("Fog", q.fog, 0, 4);
      ImGui::SeparatorText("Geodesic rays");
      Ff("Max step", q.geoMaxStep, 0.02f, 1.f, "%.2f", "Smaller = more accurate light paths on strongly curved worlds (slower)");
      ImGui::SliderInt("Projections", &q.geoProjectIters, 1, 4);
      ImGui::SeparatorText("Backend");
      int be = (int)app.backendPref;
      if (ImGui::Combo("Graphics API", &be, BACKEND_NAMES, 3)) { app.backendPref = (Backend)be; app.saveSettings(); }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applied on the next start. Auto: Vulkan if available, else OpenGL");
      ImGui::TextDisabled("In use: %s", app.renderer->device.c_str());
      if (!app.renderer->lastError.empty()) ImGui::TextColored({1, 0.5f, 0.4f, 1}, "Last shader error:\n%.300s", app.renderer->lastError.c_str());
      ImGui::TextDisabled("Last shader compile: %.0f ms", app.renderer->lastCompileMs);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Keys")) {
      keysTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (ImGui::Button("Reset to defaults")) { app.quality = QualitySettings{}; app.editSet = EditSettings{}; app.exploreSet = ExploreSettings{}; }
  ImGui::End();
  if (!showQuality_) app.saveSettings();
}

void UI::lightsWindow() {
  ImGui::SetNextWindowSize({380, 420}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Lights", &app.showLights)) { ImGui::End(); return; }
  bool done = false, live = false;
  auto track = [&] { if (ImGui::IsItemEdited()) live = true; if (ImGui::IsItemDeactivatedAfterEdit()) done = true; };
  if (ImGui::Button("+ Point light")) {
    PointLight l;
    l.id = newId("l");
    l.name = "Light " + std::to_string(app.lights.size() + 1);
    l.pos = app.currentView().eye;  // where you are
    app.lights.push_back(l);
    done = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%d / %d (with emissive objects)", (int)app.frameLights().size(), MAX_LIGHTS);
  int del = -1;
  for (size_t i = 0; i < app.lights.size(); i++) {
    auto& l = app.lights[i];
    ImGui::PushID((int)i);
    if (ImGui::CollapsingHeader((l.name + "###h").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      if (ImGui::Checkbox("On", &l.enabled)) done = true;
      ImGui::SameLine();
      if (ImGui::Checkbox("Shadows", &l.shadows)) done = true;
      ImGui::SameLine();
      if (ImGui::SmallButton("To camera")) { l.pos = app.currentView().eye; done = true; }
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) del = (int)i;
      ImGui::SameLine();
      if (ImGui::SmallButton("Key")) { keyLight(app.anim, l, std::round(app.anim.frame)); done = true; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key every value of this light at the current frame");
      std::string lt = "light:" + l.id;
      float p[4] = {(float)l.pos[0], (float)l.pos[1], (float)l.pos[2], (float)l.pos[3]};
      if (ImGui::DragFloat4("Position XYZW", p, 0.02f)) for (int k = 0; k < 4; k++) l.pos[k] = p[k];
      keyable(lt, {"pos.x", "pos.y", "pos.z", "pos.w"});
      track();
      float c[3] = {(float)l.color[0], (float)l.color[1], (float)l.color[2]};
      if (ImGui::ColorEdit3("Colour", c)) for (int k = 0; k < 3; k++) l.color[k] = c[k];
      keyable(lt, {"color.r", "color.g", "color.b"});
      track();
      float in = (float)l.intensity, rg = (float)l.range;
      if (ImGui::DragFloat("Intensity", &in, 0.02f, 0, 20)) l.intensity = std::max(0.f, in);
      keyable(lt, {"intensity"});
      track();
      if (ImGui::DragFloat("Range", &rg, 0.05f, 0.1f, 500)) l.range = std::max(0.1f, rg);
      keyable(lt, {"range"});
      track();
    }
    ImGui::PopID();
  }
  if (del >= 0) { app.lights.erase(app.lights.begin() + del); done = true; }
  ImGui::End();
  if (done) app.commit();
  else if (live) app.rebuild();
}

void UI::keysTab() {
  auto& K = app.keys;
  if (ImGui::Button("Reset all to defaults")) { K.resetDefaults(); captureAction_ = -1; app.saveSettings(); }
  ImGui::SameLine();
  ImGui::TextDisabled("Click a key to change it, right-click to clear");
  if (captureAction_ >= 0) {
    ImGui::TextColored({1, 0.8f, 0.3f, 1}, "Press a key for \"%s\"...", actionInfo((Action)captureAction_).label);
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel")) captureAction_ = -1;
  }
  ImGui::BeginChild("keys");
  const char* group = "";
  for (int i = 0; i < (int)Action::Count; i++) {
    Action a = (Action)i;
    const ActionInfo& info = actionInfo(a);
    if (std::strcmp(group, info.group) != 0) {
      group = info.group;
      ImGui::SeparatorText(group);
    }
    ImGui::PushID(i);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(info.label);
    for (int slot = 0; slot < 2; slot++) {
      ImGui::SameLine(slot == 0 ? 210.f : 330.f);
      ImGui::PushID(slot);
      bool waiting = captureAction_ == i && captureSlot_ == slot;
      std::string t = waiting ? "..." : KeyBindings::comboText(K.at(a)[slot]);
      if (t.empty()) t = "-";
      if (ImGui::Button((t + "###k").c_str(), {112, 0})) { captureAction_ = i; captureSlot_ = slot; }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { K.at(a)[slot] = KeyCombo{}; app.saveSettings(); }
      ImGui::PopID();
    }
    auto cf = K.conflicts(a);
    if (!cf.empty()) {
      ImGui::SameLine();
      ImGui::TextColored({1, 0.45f, 0.35f, 1}, "!");
      if (ImGui::IsItemHovered()) {
        std::string s = "Same key as:";
        for (Action o : cf) s += std::string("\n  ") + actionInfo(o).label;
        ImGui::SetTooltip("%s", s.c_str());
      }
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void UI::sdfWindow() {
  ImGui::SetNextWindowSize({620, 520}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("SDF code", &showSdf_)) { ImGui::End(); return; }
  Node* n = findNode(app.root, sdfNodeId_);
  if (!n) { ImGui::TextDisabled("Select an object and press 'View SDF code'."); ImGui::End(); return; }
  std::string code = standaloneGLSL(*n, app.reg);
  ImGui::Text("\"%s\" as a GLSL signed distance function (read-only)", n->name.c_str());
  if (ImGui::Button("Copy to clipboard")) ImGui::SetClipboardText(code.c_str());
  ImGui::SameLine();
  if (ImGui::Button("Create editable custom SDF from it")) {
    Node c = makeCustom(n->name + " (code)", code);
    c.xf = n->xf;
    c.anchor = n->anchor;
    c.mods.clear();
    auto b = computeBounds(*n, app.reg);
    c.codeBound = std::isfinite(b[n->id].r) ? b[n->id].r / std::max(0.01, n->xf.scale) : 5.0;
    if (n->isLeaf()) { c.color = n->color; c.mat = n->mat; }
    std::string id = c.id;
    app.root.children.push_back(std::move(c));
    app.selection = {id};
    app.commit();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shapes are a point + modifiers; this is the equivalent GLSL.\nCustom SDFs can use: opExtrude(d, h), opRadial(v, r, n, start, step), sU/sI/sS(a, b, k), INF.");
  ImGui::InputTextMultiline("##sdf", code.data(), code.size() + 1, {-1, -1}, ImGuiInputTextFlags_ReadOnly);
  ImGui::End();
}

void UI::helpWindow() {
  ImGui::SetNextWindowSize({640, 560}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Help", &showHelp_)) { ImGui::End(); return; }
  ImGui::SeparatorText("Modes");
  ImGui::TextWrapped("Tab: Edit / Explore.  V: Full 4D / Geodesic 3D (needs a world object: World menu in the toolbar, or Properties > Use as world).");
  ImGui::SeparatorText("Edit, Full 4D (Blender-like)");
  ImGui::BulletText("RMB/MMB drag: orbit | MMB click on an object: set the centre of rotation (click empty space to reset)");
  ImGui::BulletText("Shift+drag: pan | Ctrl+drag or wheel: zoom | Alt+drag: rotate the view into W");
  ImGui::BulletText("W slider (left edge of the viewport): moves the visible 3D slice along the camera's 4th axis");
  ImGui::BulletText("1 / 3 / 7 / 9: front / right / top / along W | Shift+W: reset 4D orientation");
  ImGui::SeparatorText("Edit, Geodesic 3D (game-like)");
  ImGui::BulletText("Hold RMB: mouse look + WASD fly along the surface, Space/Shift up/down, Q/E roll; wheel = fly speed");
  ImGui::SeparatorText("Editing");
  ImGui::BulletText("Tools: Q select, W move, E rotate. The gizmo always shows the object's local X/Y/Z, kept inside the visible slice");
  ImGui::BulletText("Move: drag an arrow to move along that local axis, the centre square (or the object) to move in the screen plane");
  ImGui::BulletText("Rotate: drag a ring to rotate about a local axis. Orbiting has no angle limit");
  ImGui::BulletText("G grab, R rotate, S scale; then X/Y/Z = local axis; Enter/click confirm, Esc cancel");
  ImGui::BulletText("L: local / global axes for the gizmo and G / R. Snap (toolbar): grid / angle / scale steps; hold Ctrl to invert while transforming");
  ImGui::BulletText("Several objects selected: Properties edits all of them (position / rotation / scale relatively). Groups take their first object as origin");
  ImGui::BulletText("Click: select the object | Ctrl+click: its group | Shift+click: multi | A all | Esc none | Shift+D duplicate | X/Del delete | Ctrl+G group | F focus");
  ImGui::BulletText("Properties > View SDF code shows any object as GLSL; Custom SDF objects (4D shapes folder) take your own GLSL");
  ImGui::BulletText("Shapes are a point plus modifiers: Extrude / Revolve add the next axis (X, Y, Z, W); Mirror, Repeat, Radial repeat copy it");
  ImGui::SeparatorText("Explore (game-like)");
  ImGui::BulletText("Edit and Explore share one camera: switching never moves you. Click to capture the mouse, Esc releases (again: back to Edit)");
  ImGui::BulletText("WASD, Space/Shift up/down, wheel = speed. Full 4D: R/F along the slider axis (W), Q/E roll, Z/X turn into W, C/V sideways in W, Backspace reset 4D orientation");
  ImGui::BulletText("Your body (yellow sphere) is part of the scene: it casts a shadow, and on closed worlds you can see yourself");
  ImGui::SeparatorText("Library");
  ImGui::BulletText("Built-in folders are read-only; saved objects go to Custom. '+ Folder' adds folders; right-click to rename / move / delete");
  ImGui::BulletText("Drag items into the viewport, onto tree rows, or onto a folder. Drag tree rows onto a folder to save them there");
  ImGui::End();
}

}  // namespace fd
