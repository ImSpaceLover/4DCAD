// Render window: output settings, start / cancel, progress and a preview of the image being rendered.
#include "ui_common.hpp"

namespace fd {

void UI::startRender(bool animation) {
  showRender_ = true;
  std::string err;
  if (!app.startRender(animation, err)) app.toast(err, true);
  else app.saveSettings();
}

void UI::presetsWidget() {
  auto presets = app.renderPresets();
  if (presetSel_.empty()) presetSel_ = app.activePreset.empty() ? "Balanced" : app.activePreset;
  const App::RenderPreset* sel = nullptr;
  for (auto& p : presets) if (p.name == presetSel_) sel = &p;
  ImGui::PushID("presets");
  ImGui::SetNextItemWidth(220);
  std::string shown = sel ? presetSel_ + (sel->builtin ? "  (built-in)" : "") : std::string("(none)");
  if (ImGui::BeginCombo("##preset", shown.c_str())) {
    bool sep = false;
    for (auto& p : presets) {
      if (!p.builtin && !sep) { ImGui::Separator(); sep = true; }
      std::string label = p.name + (p.name == app.activePreset ? "  *" : "");
      if (ImGui::Selectable(label.c_str(), p.name == presetSel_)) presetSel_ = p.name;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        std::string err;
        if (app.applyPreset(p.json, err)) { app.activePreset = p.name; app.toast("Loaded preset " + p.name); } else app.toast(err, true);
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render presets: all rendering settings (quality, lighting, fast mode, samples,\n"
                                                "the Render window's resolution / anti-aliasing / formats). * = loaded");
  ImGui::SameLine();
  ImGui::BeginDisabled(!sel);
  if (ImGui::Button("Load")) {
    std::string err;
    if (app.applyPreset(sel->json, err)) { app.activePreset = sel->name; app.toast("Loaded preset " + sel->name); } else app.toast(err, true);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Save as...")) {
    std::snprintf(presetName_, sizeof presetName_, "%s", sel && !sel->builtin ? presetSel_.c_str() : "My preset");
    ImGui::OpenPopup("Save preset");
  }
  bool user = sel && !sel->builtin;
  ImGui::BeginDisabled(!user);
  if (ImGui::Button("Rename")) { std::snprintf(presetName_, sizeof presetName_, "%s", presetSel_.c_str()); ImGui::OpenPopup("Rename preset"); }
  ImGui::SameLine();
  if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete preset");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Import...")) fileAction(7);
  ImGui::SameLine();
  ImGui::BeginDisabled(!sel);
  if (ImGui::Button("Export...")) fileAction(8);
  ImGui::EndDisabled();
  if (sel && sel->builtin) { ImGui::SameLine(); ImGui::TextDisabled("(built-in: read-only)"); }

  auto namePopup = [&](const char* id, const char* button, auto&& action) {
    if (!ImGui::BeginPopup(id)) return;
    ImGui::TextUnformatted(id);
    ImGui::SetNextItemWidth(260);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##name", presetName_, sizeof presetName_, ImGuiInputTextFlags_EnterReturnsTrue);
    bool exists = false;
    for (auto& p : presets) if (p.name == presetName_) exists = true;
    if (exists && std::string(presetName_) != presetSel_) ImGui::TextDisabled("Replaces the preset of this name");
    if (ImGui::Button(button) || enter) { if (action()) ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  };
  namePopup("Save preset", "Save", [&] {
    std::string err, name = presetName_;
    if (!app.savePreset(name, err)) { app.toast(err, true); return false; }
    presetSel_ = app.activePreset = name;
    app.saveSettings();
    app.toast("Saved preset " + name);
    return true;
  });
  namePopup("Rename preset", "Rename", [&] {
    std::string err, name = presetName_;
    if (!app.renamePreset(presetSel_, name, err)) { app.toast(err, true); return false; }
    if (app.activePreset == presetSel_) app.activePreset = name;
    presetSel_ = name;
    app.saveSettings();
    return true;
  });
  if (ImGui::BeginPopup("Delete preset")) {
    ImGui::Text("Delete the preset \"%s\"?", presetSel_.c_str());
    if (ImGui::Button("Delete")) {
      app.deletePreset(presetSel_);
      if (app.activePreset == presetSel_) app.activePreset.clear();
      presetSel_.clear();
      app.saveSettings();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

void UI::renderWindow() {
  ImGui::SetNextWindowSize({460, 640}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Render", &showRender_)) { ImGui::End(); return; }
  auto& r = app.renderSet;
  bool busy = app.job.active;
  bool changed = false;

  // --- camera
  Node* cam = app.activeCamera.empty() ? nullptr : findNode(app.root, app.activeCamera);
  if (cam && cam->isCamera()) ImGui::Text("Camera: %s", cam->name.c_str());
  else ImGui::TextColored({1, 0.7f, 0.3f, 1}, "No active camera: the current view is rendered");

  ImGui::BeginDisabled(busy);
  ImGui::SeparatorText("Preset");
  presetsWidget();
  // --- image
  ImGui::SeparatorText("Image");
  int wh[2] = {r.width, r.height};
  if (ImGui::InputInt2("Resolution", wh)) { r.width = std::clamp(wh[0], 16, 8192); r.height = std::clamp(wh[1], 16, 8192); changed = true; }
  const char* presets[] = {"640 x 360", "1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160", "512 x 512", "1080 x 1080"};
  const int pw[] = {640, 1280, 1920, 2560, 3840, 512, 1080}, ph[] = {360, 720, 1080, 1440, 2160, 512, 1080};
  ImGui::SameLine();
  if (ImGui::Button("...")) ImGui::OpenPopup("res##presets");
  if (ImGui::BeginPopup("res##presets")) {
    for (int i = 0; i < 7; i++) if (ImGui::MenuItem(presets[i])) { r.width = pw[i]; r.height = ph[i]; changed = true; }
    ImGui::EndPopup();
  }
  const char* aaNames[] = {"Off", "2x (4 rays / pixel)", "3x (9 rays / pixel)"};
  int aa = std::clamp(r.aa, 1, 3) - 1;
  if (ImGui::Combo("Anti-aliasing", &aa, aaNames, 3)) { r.aa = aa + 1; changed = true; }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Renders at 2x / 3x the size and averages down");
  if (ImGui::SliderInt("Samples", &r.samples, 1, 512, "%d", ImGuiSliderFlags_Logarithmic)) changed = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Stochastic samples per pixel: soft shadows, rough reflections, glass\n"
                      "(reflect or refract at random), depth of field. 1 = the plain image.");
  ImGui::TextDisabled("Quality (steps, bounces, shadows, AO): Settings > Rendering");

  // --- still
  ImGui::SeparatorText("Still image");
  if (ImGui::Combo("Format##still", &r.stillFormat, App::STILL_FORMATS, 2)) changed = true;
  auto pathField = [&](const char* id, std::string& path, bool animation) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", path.c_str());
    bool native = nativeDialogsAvailable();
    ImGui::SetNextItemWidth(native ? -90 : -1);
    std::string hint = app.renderPath(animation);
    if (ImGui::InputTextWithHint(id, hint.c_str(), buf, sizeof buf)) { path = buf; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app.renderPath(animation).c_str());
    if (!native) return;
    ImGui::SameLine();
    ImGui::PushID(id);
    if (ImGui::Button("Browse")) {
      std::string p = saveFileDialog(animation ? "Save animation" : "Save image", app.renderPath(animation));
      if (!p.empty()) { path = p; changed = true; }
    }
    ImGui::PopID();
  };
  pathField("##stillpath", r.stillPath, false);
  if (r.stillFormat == 1 || r.animFormat == 1) if (ImGui::SliderInt("JPEG quality", &r.jpegQuality, 50, 100)) changed = true;

  // --- animation
  ImGui::SeparatorText("Animation");
  if (ImGui::Combo("Format##anim", &r.animFormat, App::ANIM_FORMATS, 5)) changed = true;
  if (r.animFormat >= 3) {
    if (ffmpegChecked_ != r.ffmpeg) { ffmpegChecked_ = r.ffmpeg; ffmpegOk_ = ffmpegAvailable(r.ffmpeg) ? 1 : 0; }
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", r.ffmpeg.c_str());
    if (ImGui::InputText("ffmpeg", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) { r.ffmpeg = buf; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The ffmpeg program: a command name on the PATH or a full path (Enter to check)");
    ImGui::SameLine();
    if (ffmpegOk_ == 1) ImGui::TextColored({0.4f, 0.9f, 0.4f, 1}, "found");
    else ImGui::TextColored({1, 0.4f, 0.4f, 1}, "not found");
  }
  pathField("##animpath", r.animPath, true);
  if (r.animFormat == 2) ImGui::TextDisabled("Frames are numbered: name_0001.png, ...");
  if (ImGui::Checkbox("Frame range and fps from the timeline", &r.sceneRange)) changed = true;
  if (r.sceneRange) {
    ImGui::TextDisabled("Frames %d - %d at %.4g fps", app.anim.start, app.anim.end, app.anim.fps);
  } else {
    int fr[2] = {r.start, r.end};
    if (ImGui::InputInt2("Frames", fr)) { r.start = fr[0]; r.end = std::max(fr[0], fr[1]); changed = true; }
    if (ImGui::InputDouble("FPS", &r.fps, 1, 5, "%.3g")) { r.fps = std::clamp(r.fps, 1.0, 240.0); changed = true; }
  }
  ImGui::EndDisabled();

  // --- go
  ImGui::Separator();
  if (!busy) {
    if (ImGui::Button("Render image")) startRender(false);
    ImGui::SameLine();
    if (ImGui::Button("Render animation")) startRender(true);
  } else {
    if (ImGui::Button("Cancel")) app.cancelRender();
  }
  const auto& j = app.job;
  if (busy) {
    char ov[96];
    if (j.animation) std::snprintf(ov, sizeof ov, "frame %d / %d, sample %d / %d", j.frame, j.last, j.sample, j.samples);
    else std::snprintf(ov, sizeof ov, "sample %d / %d", j.sample, j.samples);
    ImGui::ProgressBar((float)app.renderProgress(), {-1, 0}, ov);
  } else if (j.finished) {
    ImGui::TextWrapped("Saved %s (%.1f s)", j.path.c_str(), j.seconds);
  } else if (!j.error.empty()) {
    ImGui::TextColored({1, 0.5f, 0.4f, 1}, "%s", j.error.c_str());
  }
  // preview: the image being rendered (supersampled; shown scaled to the window)
  if (j.rt.id >= 0 && j.rt.w > 0) {
    uint64_t t = app.renderer->texture(j.rt);
    float avail = ImGui::GetContentRegionAvail().x;
    float s = std::min(avail / j.rt.w, std::max(80.f, ImGui::GetContentRegionAvail().y - 4) / j.rt.h);
    bool fl = app.renderer->flipY();
    if (t) ImGui::Image((ImTextureID)t, {j.rt.w * s, j.rt.h * s}, {0, fl ? 1.f : 0.f}, {1, fl ? 0.f : 1.f});
  }
  ImGui::End();
  if (changed || !showRender_) app.saveSettings();
}

}  // namespace fd
