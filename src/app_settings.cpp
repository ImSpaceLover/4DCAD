// App: per-user settings and render presets (JSON).
#include "app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "examples.hpp"

namespace fd {

// ============================================================ settings (persisted per user)
namespace {
template <class T> void getKey(const nlohmann::json& o, const char* k, T& v) {
  if (o.contains(k)) try { v = o[k].get<T>(); } catch (...) {}
}

nlohmann::json qualityToJson(const QualitySettings& q) {
  nlohmann::json r = {{"resolutionScale", q.resolutionScale}, {"motionScale", q.motionScale}, {"maxSteps", q.maxSteps}, {"maxDistFull", q.maxDistFull},
                      {"maxDistGeo", q.maxDistGeo}, {"eps", q.eps}, {"stepScale", q.stepScale}, {"shadows", q.shadows}, {"shadowK", q.shadowK},
                      {"shadowMax", q.shadowMax}, {"ao", q.ao}, {"aoStrength", q.aoStrength}, {"fog", q.fog}, {"ambient", q.ambient},
                      {"lightYaw", q.lightYaw}, {"lightPitch", q.lightPitch}, {"lightW", q.lightW}, {"skyTop", q.skyTop}, {"skyBottom", q.skyBottom},
                      {"grid", q.grid}, {"geoMaxStep", q.geoMaxStep}, {"geoProjectIters", q.geoProjectIters}, {"sunIntensity", q.sunIntensity},
                      {"pointShadows", q.pointShadows}, {"emissiveLights", q.emissiveLights}, {"transparentLayers", q.transparentLayers},
                      {"relaxedMarching", q.relaxedMarching}, {"fastWhileMovingInEdit", q.fastWhileMovingInEdit},
                      {"stochastic", q.stochastic}, {"viewportSamples", q.viewportSamples}, {"frameLimit", q.frameLimit}, {"sunSize", q.sunSize}, {"lightRadius", q.lightRadius},
                      {"lighting", q.lighting}, {"skyIntensity", q.skyIntensity}, {"exposure", q.exposure}, {"tonemap", q.tonemap},
                      {"denoise", q.denoise}, {"denoiseStrength", q.denoiseStrength}, {"light4D", q.light4D}};
  const RenderProfile& f = q.fast;
  r["fast"] = {{"resolution", f.resolution}, {"maxSteps", f.maxSteps}, {"bounces", f.bounces}, {"textures", f.textures},
               {"specular", f.specular}, {"pointLights", f.pointLights}, {"shadows", f.shadows}, {"pointShadows", f.pointShadows},
               {"ao", f.ao}, {"aoSamples", f.aoSamples}, {"shadowSteps", f.shadowSteps}};
  return r;
}

/** Sets the values present in r (others are left as they are). */
void qualityFromJson(const nlohmann::json& r, QualitySettings& q) {
  if (!r.is_object()) return;
  getKey(r, "resolutionScale", q.resolutionScale); getKey(r, "motionScale", q.motionScale); getKey(r, "maxSteps", q.maxSteps);
  getKey(r, "maxDistFull", q.maxDistFull); getKey(r, "maxDistGeo", q.maxDistGeo); getKey(r, "eps", q.eps); getKey(r, "stepScale", q.stepScale);
  getKey(r, "shadows", q.shadows); getKey(r, "shadowK", q.shadowK); getKey(r, "shadowMax", q.shadowMax); getKey(r, "ao", q.ao);
  getKey(r, "aoStrength", q.aoStrength); getKey(r, "fog", q.fog); getKey(r, "ambient", q.ambient); getKey(r, "lightYaw", q.lightYaw);
  getKey(r, "lightPitch", q.lightPitch); getKey(r, "lightW", q.lightW); getKey(r, "skyTop", q.skyTop); getKey(r, "skyBottom", q.skyBottom);
  getKey(r, "grid", q.grid); getKey(r, "geoMaxStep", q.geoMaxStep); getKey(r, "geoProjectIters", q.geoProjectIters);
  getKey(r, "sunIntensity", q.sunIntensity); getKey(r, "pointShadows", q.pointShadows); getKey(r, "emissiveLights", q.emissiveLights);
  getKey(r, "transparentLayers", q.transparentLayers); getKey(r, "relaxedMarching", q.relaxedMarching);
  getKey(r, "fastWhileMovingInEdit", q.fastWhileMovingInEdit);
  getKey(r, "frameLimit", q.frameLimit);
  getKey(r, "stochastic", q.stochastic); getKey(r, "viewportSamples", q.viewportSamples); getKey(r, "sunSize", q.sunSize); getKey(r, "lightRadius", q.lightRadius);
  getKey(r, "lighting", q.lighting); getKey(r, "skyIntensity", q.skyIntensity);
  getKey(r, "exposure", q.exposure); getKey(r, "tonemap", q.tonemap); getKey(r, "denoise", q.denoise);
  getKey(r, "denoiseStrength", q.denoiseStrength); getKey(r, "light4D", q.light4D);
  q.tonemap = std::clamp(q.tonemap, 0, 1);
  q.light4D = std::clamp(q.light4D, 0, 1);
  q.lighting = std::clamp(q.lighting, 0, 1);
  auto& f = q.fast;
  if (r.contains("fast") && r["fast"].is_object()) {
    auto& o = r["fast"];
    getKey(o, "resolution", f.resolution); getKey(o, "maxSteps", f.maxSteps); getKey(o, "bounces", f.bounces); getKey(o, "textures", f.textures);
    getKey(o, "specular", f.specular); getKey(o, "pointLights", f.pointLights); getKey(o, "shadows", f.shadows); getKey(o, "pointShadows", f.pointShadows);
    getKey(o, "ao", f.ao); getKey(o, "aoSamples", f.aoSamples); getKey(o, "shadowSteps", f.shadowSteps);
  } else if (r.contains("exploreSimple")) {  // settings from before the fast profile: the old Explore switches
    bool simple = r.value("exploreSimple", true);
    f.textures = !simple;
    f.bounces = simple ? 0 : 1;
    getKey(r, "exploreShadows", f.shadows); getKey(r, "exploreAO", f.ao); getKey(r, "explorePointShadows", f.pointShadows);
  }
  q.maxSteps = std::clamp(q.maxSteps, 8, 2000);
  q.transparentLayers = std::clamp(q.transparentLayers, 0, 8);
  q.viewportSamples = std::clamp(q.viewportSamples, 1, 4096);
}

nlohmann::json outputToJson(const App::RenderSettings& o, bool paths) {
  nlohmann::json j = {{"width", o.width}, {"height", o.height}, {"aa", o.aa}, {"samples", o.samples}, {"stillFormat", o.stillFormat},
                      {"animFormat", o.animFormat}, {"sceneRange", o.sceneRange}, {"start", o.start}, {"end", o.end}, {"fps", o.fps},
                      {"jpegQuality", o.jpegQuality}, {"priority", o.priority}};
  if (paths) { j["stillPath"] = o.stillPath; j["animPath"] = o.animPath; j["ffmpeg"] = o.ffmpeg; }
  return j;
}

void outputFromJson(const nlohmann::json& r, App::RenderSettings& o) {
  if (!r.is_object()) return;
  getKey(r, "width", o.width); getKey(r, "height", o.height); getKey(r, "aa", o.aa); getKey(r, "samples", o.samples);
  getKey(r, "stillFormat", o.stillFormat); getKey(r, "animFormat", o.animFormat); getKey(r, "stillPath", o.stillPath);
  getKey(r, "animPath", o.animPath); getKey(r, "sceneRange", o.sceneRange); getKey(r, "start", o.start); getKey(r, "end", o.end);
  getKey(r, "fps", o.fps); getKey(r, "jpegQuality", o.jpegQuality); getKey(r, "ffmpeg", o.ffmpeg);
  getKey(r, "priority", o.priority);
  o.priority = std::clamp(o.priority, 0, 2);
  o.stillFormat = std::clamp(o.stillFormat, 0, 1);
  o.animFormat = std::clamp(o.animFormat, 0, 4);
  o.aa = std::clamp(o.aa, 1, 3);
  o.samples = std::clamp(o.samples, 1, 4096);
  o.width = std::clamp(o.width, 16, 8192);
  o.height = std::clamp(o.height, 16, 8192);
}
}  // namespace

// ------------------------------------------------------------ render presets
// A preset is all rendering settings (Settings > Rendering, incl. the fast profile, and the Render
// window's image / animation settings, without output paths). User presets are JSON files in
// <data>/presets; the built-in ones are read-only.
static std::filesystem::path presetDir() { return std::filesystem::path(dataDir()) / "presets"; }

static std::string presetFileName(const std::string& name) {
  std::string f;
  for (char c : name) f += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
  if (f.empty()) f = "preset";
  return f + ".json";
}

std::vector<App::RenderPreset> App::builtinPresets() {
  std::vector<RenderPreset> out;
  const char* names[3] = {"Fast", "Balanced", "Full"};
  for (int i = 0; i < 3; i++) {
    QualitySettings q;  // the defaults, with the matching profile's quality
    RenderProfile p = RenderProfile::preset((RenderProfile::Preset)i);
    q.resolutionScale = i == 0 ? 0.6f : i == 1 ? 0.8f : 1.0f;
    q.maxSteps = p.maxSteps;
    q.transparentLayers = p.bounces;
    q.shadows = p.shadows || i > 0;
    q.pointShadows = p.pointShadows;
    q.ao = p.ao;
    q.stochastic = i > 0;
    q.viewportSamples = i == 0 ? 8 : i == 1 ? 32 : 128;
    q.lighting = i == 2 ? 1 : 0;  // Full: path traced
    RenderSettings o;
    o.width = i == 0 ? 640 : i == 1 ? 1280 : 1920;
    o.height = i == 0 ? 360 : i == 1 ? 720 : 1080;
    o.aa = i == 0 ? 1 : 2;
    o.samples = i == 0 ? 1 : i == 1 ? 16 : 128;
    nlohmann::json j = {{"name", names[i]}, {"quality", qualityToJson(q)}, {"output", outputToJson(o, false)}};
    // the look (light, sky, fog) and the output format stay as they are
    for (const char* k : {"lightYaw", "lightPitch", "lightW", "skyTop", "skyBottom", "fog", "ambient", "sunIntensity", "grid", "skyIntensity", "exposure", "tonemap"}) j["quality"].erase(k);
    for (const char* k : {"stillFormat", "animFormat", "sceneRange", "start", "end", "fps", "jpegQuality"}) j["output"].erase(k);
    out.push_back({names[i], true, j.dump(2)});
  }
  return out;
}

std::vector<App::RenderPreset> App::renderPresets() const {
  std::vector<RenderPreset> out = builtinPresets(), user;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(presetDir(), ec)) {
    if (e.path().extension() != ".json") continue;
    std::string text;
    if (!readFile(e.path().string(), text)) continue;
    try {
      auto j = nlohmann::json::parse(text);
      std::string name = j.value("name", e.path().stem().string());
      user.push_back({name, false, text});
    } catch (...) {}
  }
  std::sort(user.begin(), user.end(), [](auto& a, auto& b) { return a.name < b.name; });
  out.insert(out.end(), user.begin(), user.end());
  return out;
}

std::string App::currentPresetJson(const std::string& name) const {
  nlohmann::json j = {{"name", name}, {"quality", qualityToJson(quality)}, {"output", outputToJson(renderSet, false)}};
  return j.dump(2);
}

bool App::applyPreset(const std::string& json, std::string& err) {
  try {
    auto j = nlohmann::json::parse(json);
    if (!j.is_object() || (!j.contains("quality") && !j.contains("output"))) { err = "Not a render preset"; return false; }
    if (j.contains("quality")) qualityFromJson(j["quality"], quality);
    if (j.contains("output")) outputFromJson(j["output"], renderSet);
  } catch (const std::exception& e) { err = std::string("Bad preset: ") + e.what(); return false; }
  saveSettings();
  return true;
}

bool App::savePreset(const std::string& name, std::string& err) {
  if (name.empty()) { err = "Give the preset a name"; return false; }
  for (auto& p : builtinPresets()) if (p.name == name) { err = "\"" + name + "\" is a built-in preset"; return false; }
  std::error_code ec;
  std::filesystem::create_directories(presetDir(), ec);
  deletePreset(name);  // overwrite (also if its file had another name)
  if (!writeFile((presetDir() / presetFileName(name)).string(), currentPresetJson(name))) { err = "Cannot write the preset"; return false; }
  return true;
}

bool App::deletePreset(const std::string& name) {
  std::error_code ec;
  bool any = false;
  for (auto& e : std::filesystem::directory_iterator(presetDir(), ec)) {
    if (e.path().extension() != ".json") continue;
    std::string text;
    if (!readFile(e.path().string(), text)) continue;
    try {
      if (nlohmann::json::parse(text).value("name", e.path().stem().string()) == name) { std::filesystem::remove(e.path(), ec); any = true; }
    } catch (...) {}
  }
  return any;
}

bool App::renamePreset(const std::string& from, const std::string& to, std::string& err) {
  if (to.empty()) { err = "Give the preset a name"; return false; }
  if (from == to) return true;
  for (auto& p : renderPresets()) if (p.name == to) { err = "A preset named \"" + to + "\" exists"; return false; }
  for (auto& p : renderPresets()) {
    if (p.name != from || p.builtin) continue;
    try {
      auto j = nlohmann::json::parse(p.json);
      j["name"] = to;
      deletePreset(from);
      if (!writeFile((presetDir() / presetFileName(to)).string(), j.dump(2))) { err = "Cannot write the preset"; return false; }
      return true;
    } catch (...) { break; }
  }
  err = "No such preset";
  return false;
}

bool App::importPreset(const std::string& path, std::string& err, std::string* nameOut) {
  std::string text;
  if (!readFile(path, text)) { err = "Cannot read " + path; return false; }
  try {
    auto j = nlohmann::json::parse(text);
    if (!j.is_object() || (!j.contains("quality") && !j.contains("output"))) { err = "Not a render preset"; return false; }
    std::string base = j.value("name", std::filesystem::path(path).stem().string()), name = base;
    auto all = renderPresets();
    for (int n = 2; std::any_of(all.begin(), all.end(), [&](auto& p) { return p.name == name; }); n++) name = base + " " + std::to_string(n);
    j["name"] = name;
    std::error_code ec;
    std::filesystem::create_directories(presetDir(), ec);
    if (!writeFile((presetDir() / presetFileName(name)).string(), j.dump(2))) { err = "Cannot write the preset"; return false; }
    if (nameOut) *nameOut = name;
    return true;
  } catch (const std::exception& e) { err = std::string("Bad preset file: ") + e.what(); return false; }
}

bool App::exportPreset(const std::string& name, const std::string& path, std::string& err) {
  for (auto& p : renderPresets())
    if (p.name == name) {
      if (!writeFile(path, p.json)) { err = "Cannot write " + path; return false; }
      return true;
    }
  err = "No such preset";
  return false;
}

void App::saveSettings() {
  nlohmann::json j;
  j["lastFile"] = lastFile;
  j["activePreset"] = activePreset;
  j["render"] = qualityToJson(quality);
  j["render"]["backend"] = BACKEND_NAMES[(int)backendPref];
  const auto& e = editSet;
  j["edit"] = {{"fov", e.fov}, {"orbitSpeed", e.orbitSpeed}, {"panSpeed", e.panSpeed}, {"zoomSpeed", e.zoomSpeed}, {"rot4Speed", e.rot4Speed},
               {"showWSlider", e.showWSlider}, {"flySpeed", e.flySpeed}, {"lookSpeed", e.lookSpeed}, {"showThumbnail", e.showThumbnail},
               {"thumbSize", e.thumbSize}, {"autosaveSeconds", e.autosaveSeconds}, {"globalAxes", e.globalAxes}, {"snap", e.snap}, {"snapMove", e.snapMove},
               {"snapAngle", e.snapAngle}, {"snapScale", e.snapScale}};
  const auto& x = exploreSet;
  j["explore"] = {{"fov", x.fov}, {"lookSpeed", x.lookSpeed}, {"showOwnBody", x.showOwnBody}, {"bodyRadius", x.bodyRadius},
                  {"moveSpeed", x.moveSpeed}, {"turn4Speed", x.turn4Speed}, {"collide", x.collide}, {"geoMoveSpeed", x.geoMoveSpeed},
                  {"geoCollide", x.geoCollide}};
  j["output"] = outputToJson(renderSet, true);
  keys.toJson(j["keys"]);
  writeFile((std::filesystem::path(dataDir()) / "settings.json").string(), j.dump(2));
}

static Backend backendFromSettings(const std::string& settingsText) {
  try {
    auto j = nlohmann::json::parse(settingsText);
    std::string b = j.value("render", nlohmann::json::object()).value("backend", std::string("Auto"));
    for (int i = 0; i < 3; i++) if (b == BACKEND_NAMES[i]) return (Backend)i;
  } catch (...) {}
  return Backend::Auto;
}

Backend App::savedBackend() {
  std::string text;
  if (!readFile((std::filesystem::path(dataDir()) / "settings.json").string(), text)) return Backend::Auto;
  return backendFromSettings(text);
}

void App::loadSettings() {
  std::string text;
  if (!readFile((std::filesystem::path(dataDir()) / "settings.json").string(), text)) return;
  try {
    auto j = nlohmann::json::parse(text);
    lastFile = j.value("lastFile", std::string());
    activePreset = j.value("activePreset", std::string());
    auto get = [](const nlohmann::json& o, const char* k, auto& v) { if (o.contains(k)) v = o[k].get<std::decay_t<decltype(v)>>(); };
    if (j.contains("render")) {
      qualityFromJson(j["render"], quality);
      backendPref = backendFromSettings(text);
    }
    if (j.contains("edit")) {
      auto& r = j["edit"]; auto& e = editSet;
      get(r, "fov", e.fov); get(r, "orbitSpeed", e.orbitSpeed); get(r, "panSpeed", e.panSpeed); get(r, "zoomSpeed", e.zoomSpeed);
      get(r, "rot4Speed", e.rot4Speed); get(r, "showWSlider", e.showWSlider); get(r, "flySpeed", e.flySpeed); get(r, "lookSpeed", e.lookSpeed);
      get(r, "showThumbnail", e.showThumbnail); get(r, "thumbSize", e.thumbSize);
      get(r, "autosaveSeconds", e.autosaveSeconds); get(r, "globalAxes", e.globalAxes); get(r, "snap", e.snap); get(r, "snapMove", e.snapMove);
      get(r, "snapAngle", e.snapAngle); get(r, "snapScale", e.snapScale);
    }
    if (j.contains("explore")) {
      auto& r = j["explore"]; auto& x = exploreSet;
      get(r, "fov", x.fov); get(r, "lookSpeed", x.lookSpeed); get(r, "showOwnBody", x.showOwnBody); get(r, "bodyRadius", x.bodyRadius);
      get(r, "moveSpeed", x.moveSpeed); get(r, "turn4Speed", x.turn4Speed); get(r, "collide", x.collide); get(r, "geoMoveSpeed", x.geoMoveSpeed);
      get(r, "geoCollide", x.geoCollide);
    }
    if (j.contains("output")) outputFromJson(j["output"], renderSet);
    if (j.contains("keys")) keys.fromJson(j["keys"]);
  } catch (...) { /* bad settings file: keep defaults */ }
}

}  // namespace fd
