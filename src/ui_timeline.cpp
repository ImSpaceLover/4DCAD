// Timeline (bottom panel): playback controls, keyframes of the selected objects, graph editor.
#include <cmath>
#include <map>

#include "ui_common.hpp"

namespace fd {

namespace {

ImU32 trackColor(const std::string& path) {
  if (path == "pos.x" || path.size() > 1 && path.substr(path.size() - 2) == ".r") return IM_COL32(230, 90, 90, 255);
  if (path == "pos.y" || path.size() > 1 && path.substr(path.size() - 2) == ".g") return IM_COL32(110, 210, 110, 255);
  if (path == "pos.z" || path.size() > 1 && path.substr(path.size() - 2) == ".b") return IM_COL32(90, 140, 240, 255);
  if (path == "pos.w") return IM_COL32(200, 110, 230, 255);
  unsigned h = 2166136261u;
  for (char c : path) h = (h ^ (unsigned char)c) * 16777619u;
  ImVec4 c;
  ImGui::ColorConvertHSVtoRGB((h % 360) / 360.f, 0.55f, 0.95f, c.x, c.y, c.z);
  return ImGui::GetColorU32({c.x, c.y, c.z, 1});
}

void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 edge) {
  ImVec2 p[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
  dl->AddConvexPolyFilled(p, 4, fill);
  dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, 1);
}

}  // namespace

std::string UI::targetName(const std::string& target) {
  if (target.rfind("light:", 0) == 0) {
    for (auto& l : app.lights) if ("light:" + l.id == target) return l.name;
    return "Light";
  }
  Node* n = findNode(app.root, target);
  return n ? n->name : target;
}

std::string UI::pathLabel(const std::string& target, const std::string& path) {
  if (target.rfind("light:", 0) == 0) {
    for (auto& v : animatableLightValues()) if (v.path == path) return v.label;
    return path;
  }
  if (Node* n = findNode(app.root, target))
    for (auto& v : animatableValues(*n)) if (v.path == path) return v.label;
  return path;
}

float UI::timelineHeight() const {
  if (app.mode != Mode::Edit || !showTimeline_) return ImGui::GetFrameHeightWithSpacing() + 10;
  return showGraph_ ? 380.f : 170.f;
}

void UI::timelinePanel() {
  ImGuiIO& io = ImGui::GetIO();
  Animation& A = app.anim;
  float h = timelineHeight();
  ImGui::SetNextWindowPos({LEFT_W, io.DisplaySize.y - h});
  ImGui::SetNextWindowSize({io.DisplaySize.x - LEFT_W - RIGHT_W, h});
  ImGui::Begin("##timeline", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
  bool animChanged = false, animCommit = false;
  auto K = [this](Action a) { return app.keys.text(a); };

  // ---------------------------------------------------------------- controls
  if (ImGui::ArrowButton("##tl", showTimeline_ ? ImGuiDir_Down : ImGuiDir_Up)) showTimeline_ = !showTimeline_;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(showTimeline_ ? "Hide the timeline" : "Show the timeline");
  ImGui::SameLine();
  if (ImGui::Button("|<")) { app.playing = false; app.setFrame(A.start); }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("First frame (%s)", K(Action::FrameStart).c_str());
  ImGui::SameLine(0, 2);
  if (ImGui::Button("<")) app.setFrame(std::round(A.frame) - 1);
  ImGui::SameLine(0, 2);
  if (toggleButton(app.playing ? " || " : " > ", app.playing)) app.playing = !app.playing;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play / pause (%s)", K(Action::PlayPause).c_str());
  ImGui::SameLine(0, 2);
  if (ImGui::Button(">")) app.setFrame(std::round(A.frame) + 1);
  ImGui::SameLine(0, 2);
  if (ImGui::Button(">|")) { app.playing = false; app.setFrame(A.end); }
  ImGui::SameLine();
  int fr = (int)std::round(A.frame);
  ImGui::SetNextItemWidth(70);
  if (ImGui::DragInt("##frame", &fr, 0.2f)) app.setFrame(fr);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Current frame");
  ImGui::SameLine();
  ImGui::TextDisabled("Range");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(55);
  if (ImGui::DragInt("##start", &A.start, 0.2f)) { A.end = std::max(A.end, A.start); animCommit = true; }
  ImGui::SameLine(0, 2);
  ImGui::SetNextItemWidth(55);
  if (ImGui::DragInt("##end", &A.end, 0.2f)) { A.start = std::min(A.start, A.end); animCommit = true; }
  ImGui::SameLine();
  float fps = (float)A.fps;
  ImGui::SetNextItemWidth(55);
  if (ImGui::DragFloat("fps", &fps, 0.1f, 1, 240, "%.0f")) { A.fps = std::clamp(fps, 1.f, 240.f); animCommit = true; }
  ImGui::SameLine(0, 14);
  ImGui::PushStyleColor(ImGuiCol_Text, app.autoKey ? ImVec4(1, 0.35f, 0.3f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
  if (toggleButton("Auto-key", app.autoKey)) app.autoKey = !app.autoKey;
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every change is keyed at the current frame");
  ImGui::SameLine(0, 2);
  if (ImGui::Button("Key")) ImGui::OpenPopup("keymenu");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Insert keyframes for the selected objects (%s)", K(Action::InsertKey).c_str());
  if (ImGui::BeginPopup("keymenu")) {
    if (ImGui::MenuItem("Position / rotation / scale")) app.insertKeys(true);
    if (ImGui::MenuItem("All values")) app.insertKeys(false);
    ImGui::EndPopup();
  }
  // interpolation of the selected keys
  int selKeys = 0, interp = -1;
  for (auto& t : A.tracks)
    for (auto& k : t.keys)
      if (k.selected) { selKeys++; interp = interp == -1 || interp == (int)k.interp ? (int)k.interp : -2; }
  ImGui::SameLine();
  ImGui::BeginDisabled(selKeys == 0);
  ImGui::SetNextItemWidth(120);
  const char* cur = interp >= 0 ? INTERP_NAMES[interp] : selKeys ? "(mixed)" : "Interpolation";
  if (ImGui::BeginCombo("##interp", cur)) {
    for (int i = 0; i < (int)Interp::Count; i++)
      if (ImGui::Selectable(INTERP_NAMES[i], i == interp)) {
        for (auto& t : A.tracks) for (auto& k : t.keys) if (k.selected) k.interp = (Interp)i;
        animCommit = true;
      }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the selected keys go over to the next key");
  ImGui::EndDisabled();
  ImGui::SameLine(0, 14);
  if (toggleButton("Camera view", app.viewThroughCamera)) app.viewThroughCamera = !app.viewThroughCamera;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the active camera's view (%s); navigating leaves it", K(Action::CameraView).c_str());
  ImGui::SameLine(0, 2);
  if (toggleButton("Graph", showGraph_)) { showGraph_ = !showGraph_; showTimeline_ = true; graphFit_ = true; }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Graph editor: value curves, keys and handles");

  if (!showTimeline_ || app.mode != Mode::Edit) {
    ImGui::End();
    if (animCommit) app.commit();
    return;
  }

  // ---------------------------------------------------------------- rows (selected objects, else everything animated)
  struct Row { std::string label; std::vector<int> tracks; bool summary; };
  std::vector<Row> rows;
  std::vector<std::string> targets;
  for (auto& id : app.selection) targets.push_back(id);
  if (targets.empty()) {
    for (auto& t : A.tracks) if (std::find(targets.begin(), targets.end(), t.target) == targets.end()) targets.push_back(t.target);
  }
  for (auto& tg : targets) {
    Row s{targetName(tg), {}, true};
    for (int i = 0; i < (int)A.tracks.size(); i++) if (A.tracks[i].target == tg) s.tracks.push_back(i);
    if (s.tracks.empty()) continue;
    rows.push_back(s);
    if (app.selection.size() <= 3)
      for (int i : s.tracks) rows.push_back({"   " + pathLabel(tg, A.tracks[i].path), {i}, false});
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float dopeH = showGraph_ ? 110.f : avail.y;
  const float nameW = 170, rulerH = 18, rowH = 18;
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImVec2 c0 = {o.x + nameW, o.y}, c1 = {o.x + avail.x, o.y + dopeH};
  if (tl1_ <= tl0_ || resetView_) { tl0_ = A.start - 2; tl1_ = A.end + 2; resetView_ = false; }
  auto X = [&](double f) { return (float)(c0.x + (f - tl0_) / (tl1_ - tl0_) * (c1.x - c0.x)); };
  auto F = [&](float x) { return tl0_ + (x - c0.x) / (c1.x - c0.x) * (tl1_ - tl0_); };
  dl->AddRectFilled(o, c1, IM_COL32(22, 24, 30, 255));
  dl->AddRectFilled({c0.x, o.y}, {c1.x, o.y + rulerH}, IM_COL32(34, 37, 45, 255));
  // shade outside the frame range
  dl->AddRectFilled({c0.x, o.y + rulerH}, {std::max(c0.x, X(A.start)), c1.y}, IM_COL32(0, 0, 0, 70));
  dl->AddRectFilled({std::min(c1.x, X(A.end)), o.y + rulerH}, c1, IM_COL32(0, 0, 0, 70));
  // ruler ticks
  double span = tl1_ - tl0_;
  int step = span > 2000 ? 200 : span > 800 ? 100 : span > 300 ? 50 : span > 120 ? 20 : span > 50 ? 10 : span > 20 ? 5 : 1;
  for (int f = (int)std::ceil(tl0_ / step) * step; f <= tl1_; f += step) {
    float x = X(f);
    dl->AddLine({x, o.y + rulerH - 5}, {x, c1.y}, IM_COL32(255, 255, 255, 18));
    char b[16];
    std::snprintf(b, sizeof b, "%d", f);
    dl->AddText({x + 2, o.y + 2}, IM_COL32(170, 170, 180, 255), b);
  }
  // names column + rows
  dl->PushClipRect(o, c1, true);
  for (size_t r = 0; r < rows.size(); r++) {
    float y = o.y + rulerH + 2 + r * rowH;
    if (y > c1.y - rowH) break;
    if (rows[r].summary) dl->AddRectFilled({o.x, y}, {c1.x, y + rowH}, IM_COL32(255, 255, 255, 10));
    dl->AddText({o.x + 4, y + 1}, rows[r].summary ? IM_COL32(230, 230, 235, 255) : IM_COL32(170, 170, 180, 255), rows[r].label.c_str());
  }
  dl->PopClipRect();

  // ---------------------------------------------------------------- interaction (one invisible button over the key area)
  ImGui::SetCursorScreenPos(c0);
  ImGui::InvisibleButton("##dope", {std::max(1.f, c1.x - c0.x), std::max(1.f, dopeH)}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                                                            ImGuiButtonFlags_MouseButtonMiddle);
  bool hovered = ImGui::IsItemHovered();
  timelineHovered_ = hovered;
  ImVec2 m = io.MousePos;
  // keys under the mouse: (track, key) pairs of the row / frame clicked
  auto keysAt = [&](size_t r, float mx) {
    std::vector<std::pair<int, int>> out;
    if (r >= rows.size()) return out;
    for (int ti : rows[r].tracks)
      for (int ki = 0; ki < (int)A.tracks[ti].keys.size(); ki++)
        if (std::abs(X(A.tracks[ti].keys[ki].frame) - mx) <= 6) out.push_back({ti, ki});
    return out;
  };
  size_t hoverRow = (size_t)std::max(0.f, (m.y - (o.y + rulerH + 2)) / rowH);
  bool onRuler = m.y < o.y + rulerH;
  if (hovered && ImGui::IsMouseClicked(0)) {
    if (onRuler) scrub_ = true;
    else {
      auto hit = keysAt(hoverRow, m.x);
      if (hit.empty()) {
        if (!io.KeyShift) for (auto& t : A.tracks) for (auto& k : t.keys) k.selected = false;
        scrub_ = true;  // empty space: scrub (Blender-like)
      } else {
        bool already = A.tracks[hit[0].first].keys[hit[0].second].selected;
        if (!io.KeyShift && !already) for (auto& t : A.tracks) for (auto& k : t.keys) k.selected = false;
        for (auto& [ti, ki] : hit) A.tracks[ti].keys[ki].selected = io.KeyShift && already ? false : true;
        keyDrag_ = true;
        dragX0_ = m.x;
        dragOrig_.clear();
        for (int ti = 0; ti < (int)A.tracks.size(); ti++)
          for (auto& k : A.tracks[ti].keys) if (k.selected) dragOrig_.push_back({&k, k.frame, k.value});
      }
    }
  }
  if (scrub_) {
    if (ImGui::IsMouseDown(0)) app.setFrame(std::round(std::clamp(F(m.x), (double)A.start - 1000, (double)A.end + 1000)));
    else scrub_ = false;
  }
  if (keyDrag_) {
    if (ImGui::IsMouseDown(0)) {
      double df = std::round(F(m.x) - F(dragX0_));
      for (auto& d : dragOrig_) d.key->frame = d.frame + df;
      for (auto& t : A.tracks) sortKeys(t);
      dragOrig_.clear();  // pointers moved by the sort: collect again
      for (auto& t : A.tracks) for (auto& k : t.keys) if (k.selected) dragOrig_.push_back({&k, k.frame - df, k.value});
      animChanged = true;
    } else {
      keyDrag_ = false;
      animCommit = true;
    }
  }
  // right-click a key: interpolation / delete
  if (hovered && ImGui::IsMouseClicked(1) && !onRuler) {
    auto hit = keysAt(hoverRow, m.x);
    if (!hit.empty()) {
      bool already = A.tracks[hit[0].first].keys[hit[0].second].selected;
      if (!already) for (auto& t : A.tracks) for (auto& k : t.keys) k.selected = false;
      for (auto& [ti, ki] : hit) A.tracks[ti].keys[ki].selected = true;
      ImGui::OpenPopup("keyctx");
    }
  }
  if (ImGui::BeginPopup("keyctx")) {
    ImGui::TextDisabled("Interpolation");
    for (int i = 0; i < (int)Interp::Count; i++)
      if (ImGui::MenuItem(INTERP_NAMES[i])) { for (auto& t : A.tracks) for (auto& k : t.keys) if (k.selected) k.interp = (Interp)i; animCommit = true; }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete keys", "X / Del")) { deleteSelectedKeys(); animCommit = true; }
    ImGui::EndPopup();
  }
  // wheel: zoom around the mouse; middle drag: pan
  if (hovered && io.MouseWheel != 0) {
    double f = F(m.x), k = std::exp(-io.MouseWheel * 0.15);
    tl0_ = f + (tl0_ - f) * k;
    tl1_ = f + (tl1_ - f) * k;
  }
  if (hovered && ImGui::IsMouseDragging(2)) {
    double d = io.MouseDelta.x / (c1.x - c0.x) * (tl1_ - tl0_);
    tl0_ -= d;
    tl1_ -= d;
  }

  // keys
  dl->PushClipRect(c0, c1, true);
  for (size_t r = 0; r < rows.size(); r++) {
    float y = o.y + rulerH + 2 + r * rowH + rowH / 2;
    if (y > c1.y - rowH / 2) break;
    std::map<long long, bool> drawn;  // one diamond per frame on summary rows
    for (int ti : rows[r].tracks)
      for (auto& k : A.tracks[ti].keys) {
        long long fk = std::llround(k.frame * 1000);
        if (drawn.count(fk) && !(k.selected && !drawn[fk])) continue;
        drawn[fk] = drawn[fk] || k.selected;
        ImU32 fill = k.selected ? IM_COL32(255, 170, 60, 255) : rows[r].summary ? IM_COL32(200, 200, 205, 255) : trackColor(A.tracks[ti].path);
        diamond(dl, {X(k.frame), y}, rows[r].summary ? 6.f : 5.f, fill, IM_COL32(20, 20, 20, 255));
      }
  }
  // current frame
  float cx = X(A.frame);
  dl->AddLine({cx, o.y}, {cx, c1.y}, IM_COL32(80, 150, 255, 255), 2);
  char fb[16];
  std::snprintf(fb, sizeof fb, "%d", (int)std::round(A.frame));
  ImVec2 ts = ImGui::CalcTextSize(fb);
  dl->AddRectFilled({cx - ts.x / 2 - 3, o.y}, {cx + ts.x / 2 + 3, o.y + rulerH - 1}, IM_COL32(80, 150, 255, 255), 3);
  dl->AddText({cx - ts.x / 2, o.y + 1}, IM_COL32(255, 255, 255, 255), fb);
  dl->PopClipRect();
  if (rows.empty())
    dl->AddText({c0.x + 10, o.y + rulerH + 8}, IM_COL32(140, 140, 150, 255),
                app.selection.empty() ? "Nothing is animated yet: select objects and press I (or Key) to insert keyframes"
                                      : "No keyframes on the selected objects: press I (or Key), or right-click a value in Properties");

  // ---------------------------------------------------------------- graph editor
  if (showGraph_) {
    ImGui::SetCursorScreenPos({o.x, o.y + dopeH + 4});
    graphEditor(rows.empty() ? std::vector<int>{} : [&] {
      std::vector<int> v;
      for (auto& r : rows) if (r.summary) for (int ti : r.tracks) if (!A.tracks[ti].rotation()) v.push_back(ti);
      return v;
    }(), animChanged, animCommit);
  }
  ImGui::End();
  if (animChanged) app.setFrame(A.frame);
  if (animCommit) app.commit();
}

void UI::graphEditor(const std::vector<int>& trackIds, bool& changed, bool& commit) {
  Animation& A = app.anim;
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float listW = 170;
  ImVec2 o = ImGui::GetCursorScreenPos();
  // track list (checkboxes)
  ImGui::BeginChild("##gtracks", {listW, avail.y}, ImGuiChildFlags_None);
  if (ImGui::SmallButton("Frame all")) graphFit_ = true;
  for (int ti : trackIds) {
    auto& t = A.tracks[ti];
    std::string key = t.target + "/" + t.path;
    bool vis = !graphHidden_.count(key);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(trackColor(t.path)));
    if (ImGui::Checkbox((targetName(t.target) + ": " + pathLabel(t.target, t.path) + "##" + key).c_str(), &vis)) {
      if (vis) graphHidden_.erase(key); else graphHidden_.insert(key);
    }
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();
  ImVec2 c0 = {o.x + listW + 4, o.y}, c1 = {o.x + avail.x, o.y + avail.y};
  std::vector<int> vis;
  for (int ti : trackIds) if (!graphHidden_.count(A.tracks[ti].target + "/" + A.tracks[ti].path)) vis.push_back(ti);
  if (graphFit_ && !vis.empty()) {
    double f0 = 1e18, f1 = -1e18, v0 = 1e18, v1 = -1e18;
    for (int ti : vis)
      for (auto& k : A.tracks[ti].keys) { f0 = std::min(f0, k.frame); f1 = std::max(f1, k.frame); v0 = std::min(v0, k.value); v1 = std::max(v1, k.value); }
    if (f1 - f0 < 2) { f0 -= 5; f1 += 5; }
    if (v1 - v0 < 1e-3) { v0 -= 1; v1 += 1; }
    double pf = (f1 - f0) * 0.08, pv = (v1 - v0) * 0.15;
    gx0_ = f0 - pf; gx1_ = f1 + pf; gy0_ = v0 - pv; gy1_ = v1 + pv;
    graphFit_ = false;
  }
  if (gx1_ <= gx0_) { gx0_ = A.start; gx1_ = A.end; }
  if (gy1_ <= gy0_) { gy0_ = -1; gy1_ = 1; }
  auto X = [&](double f) { return (float)(c0.x + (f - gx0_) / (gx1_ - gx0_) * (c1.x - c0.x)); };
  auto Y = [&](double v) { return (float)(c1.y - (v - gy0_) / (gy1_ - gy0_) * (c1.y - c0.y)); };
  auto Fx = [&](float x) { return gx0_ + (x - c0.x) / (c1.x - c0.x) * (gx1_ - gx0_); };
  auto Vy = [&](float y) { return gy0_ + (c1.y - y) / (c1.y - c0.y) * (gy1_ - gy0_); };
  dl->AddRectFilled(c0, c1, IM_COL32(26, 28, 34, 255));
  dl->PushClipRect(c0, c1, true);
  // grid
  auto niceStep = [](double span, int target) {
    double raw = span / target, p = std::pow(10.0, std::floor(std::log10(raw))), s = p;
    for (double m : {1.0, 2.0, 5.0, 10.0}) if (p * m >= raw) { s = p * m; break; }
    return s;
  };
  double sx = std::max(1.0, niceStep(gx1_ - gx0_, 10)), sy = niceStep(gy1_ - gy0_, 6);
  for (double f = std::ceil(gx0_ / sx) * sx; f <= gx1_; f += sx) {
    dl->AddLine({X(f), c0.y}, {X(f), c1.y}, IM_COL32(255, 255, 255, 14));
    char b[16];
    std::snprintf(b, sizeof b, "%g", std::abs(f) < 1e-9 ? 0.0 : f);
    dl->AddText({X(f) + 2, c1.y - 16}, IM_COL32(130, 130, 140, 255), b);
  }
  for (double v = std::ceil(gy0_ / sy) * sy; v <= gy1_; v += sy) {
    dl->AddLine({c0.x, Y(v)}, {c1.x, Y(v)}, IM_COL32(255, 255, 255, std::abs(v) < sy * 1e-3 ? 40 : 14));
    char b[24];
    std::snprintf(b, sizeof b, "%g", std::abs(v) < sy * 1e-6 ? 0.0 : v);
    dl->AddText({c0.x + 3, Y(v) - 14}, IM_COL32(130, 130, 140, 255), b);
  }
  dl->AddLine({X(A.frame), c0.y}, {X(A.frame), c1.y}, IM_COL32(80, 150, 255, 200), 1.5f);
  // curves
  for (int ti : vis) {
    auto& t = A.tracks[ti];
    ImU32 col = trackColor(t.path);
    std::vector<ImVec2> pts;
    for (float x = c0.x; x <= c1.x; x += 2) pts.push_back({x, Y(evalTrack(t, Fx(x)))});
    dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.6f);
  }
  // keys and handles (handles of selected bezier keys)
  struct Hit { Key* k; int part; };  // part: 0 key, 1 left handle, 2 right handle
  std::optional<Hit> hover;
  ImVec2 m = io.MousePos;
  for (int ti : vis)
    for (auto& k : A.tracks[ti].keys) {
      ImVec2 p = {X(k.frame), Y(k.value)};
      if (k.selected && k.interp == Interp::Bezier && !isSteppedPath(A.tracks[ti].path)) {
        ImVec2 hl = {X(k.frame + k.lx), Y(k.value + k.ly)}, hr = {X(k.frame + k.rx), Y(k.value + k.ry)};
        dl->AddLine(hl, p, IM_COL32(220, 220, 220, 160));
        dl->AddLine(p, hr, IM_COL32(220, 220, 220, 160));
        dl->AddCircleFilled(hl, 3.5f, IM_COL32(230, 230, 230, 255));
        dl->AddCircleFilled(hr, 3.5f, IM_COL32(230, 230, 230, 255));
        if (std::hypot(m.x - hl.x, m.y - hl.y) < 6) hover = Hit{&k, 1};
        if (std::hypot(m.x - hr.x, m.y - hr.y) < 6) hover = Hit{&k, 2};
      }
      dl->AddCircleFilled(p, 4.5f, k.selected ? IM_COL32(255, 170, 60, 255) : IM_COL32(15, 15, 15, 255));
      dl->AddCircle(p, 4.5f, trackColor(A.tracks[ti].path), 0, 1.5f);
      if (!hover && std::hypot(m.x - p.x, m.y - p.y) < 6) hover = Hit{&k, 0};
    }
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(c0);
  ImGui::InvisibleButton("##graph", {std::max(1.f, c1.x - c0.x), std::max(1.f, c1.y - c0.y)},
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  bool hov = ImGui::IsItemHovered();
  graphHovered_ = hov;
  if (hov && ImGui::IsMouseClicked(0)) {
    if (hover && hover->part == 0) {
      if (!io.KeyShift && !hover->k->selected) for (auto& t : A.tracks) for (auto& k : t.keys) k.selected = false;
      hover->k->selected = io.KeyShift ? !hover->k->selected : true;
      gDrag_ = 1;
    } else if (hover) {
      gDrag_ = 1 + hover->part;
      gHandleKey_ = hover->k;
    } else {
      if (!io.KeyShift) for (auto& t : A.tracks) for (auto& k : t.keys) k.selected = false;
      gDrag_ = 0;
    }
    gx0Drag_ = Fx(m.x);
    gy0Drag_ = Vy(m.y);
    dragOrig_.clear();
    for (auto& t : A.tracks) for (auto& k : t.keys) if (k.selected) dragOrig_.push_back({&k, k.frame, k.value});
  }
  if (gDrag_ && ImGui::IsMouseDown(0) && ImGui::IsMouseDragging(0, 2)) {
    double df = Fx(m.x) - gx0Drag_, dv = Vy(m.y) - gy0Drag_;
    if (gDrag_ == 1) {  // move keys (frames snap to whole frames)
      for (auto& d : dragOrig_) { d.key->frame = d.frame + std::round(df); d.key->value = d.value + dv; }
      for (auto& t : A.tracks) {
        sortKeys(t);
      }
      dragOrig_.clear();
      for (auto& t : A.tracks) for (auto& k : t.keys) if (k.selected) dragOrig_.push_back({&k, k.frame - std::round(df), k.value - dv});
    } else if (gHandleKey_) {  // move a handle: the other one stays aligned (smooth curve)
      Key& k = *gHandleKey_;
      double hx = Fx(m.x) - k.frame, hy = Vy(m.y) - k.value;
      if (gDrag_ == 2) { k.lx = std::min(hx, -1e-3); k.ly = hy; double l = std::hypot(k.rx, k.ry), s = std::hypot(k.lx, k.ly); if (s > 1e-9) { k.rx = -k.lx / s * l; k.ry = -k.ly / s * l; } }
      else { k.rx = std::max(hx, 1e-3); k.ry = hy; double l = std::hypot(k.lx, k.ly), s = std::hypot(k.rx, k.ry); if (s > 1e-9) { k.lx = -k.rx / s * l; k.ly = -k.ry / s * l; } }
      k.autoHandles = false;
      for (auto& t : A.tracks) autoHandles(t);
    }
    changed = true;
  }
  if (gDrag_ && !ImGui::IsMouseDown(0)) {
    if (changed || ImGui::IsMouseReleased(0)) commit = true;
    gDrag_ = 0;
    gHandleKey_ = nullptr;
  }
  // wheel zoom (Ctrl: values), middle drag pan
  if (hov && io.MouseWheel != 0) {
    double k = std::exp(-io.MouseWheel * 0.15);
    if (io.KeyCtrl) { double v = Vy(m.y); gy0_ = v + (gy0_ - v) * k; gy1_ = v + (gy1_ - v) * k; }
    else { double f = Fx(m.x); gx0_ = f + (gx0_ - f) * k; gx1_ = f + (gx1_ - f) * k; }
  }
  if (hov && ImGui::IsMouseDragging(2)) {
    double dfx = io.MouseDelta.x / (c1.x - c0.x) * (gx1_ - gx0_), dvy = io.MouseDelta.y / (c1.y - c0.y) * (gy1_ - gy0_);
    gx0_ -= dfx; gx1_ -= dfx; gy0_ += dvy; gy1_ += dvy;
  }
  if (vis.empty()) dl->AddText({c0.x + 10, c0.y + 10}, IM_COL32(140, 140, 150, 255), "No value curves (rotations are shown in the timeline only)");
}

void UI::deleteSelectedKeys() {
  for (auto& t : app.anim.tracks) {
    t.keys.erase(std::remove_if(t.keys.begin(), t.keys.end(), [](const Key& k) { return k.selected; }), t.keys.end());
    sortKeys(t);
  }
}

void UI::copySelectedKeys() {
  keyClipboard_.clear();
  double first = 1e18;
  for (auto& t : app.anim.tracks) for (auto& k : t.keys) if (k.selected) first = std::min(first, k.frame);
  for (auto& t : app.anim.tracks)
    for (auto& k : t.keys)
      if (k.selected) { Key c = k; c.frame -= first; c.selected = false; keyClipboard_.push_back({t.target, t.path, c}); }
  if (!keyClipboard_.empty()) app.toast("Copied " + std::to_string(keyClipboard_.size()) + " keys");
}

void UI::pasteKeys() {
  double at = std::round(app.anim.frame);
  for (auto& [target, path, k] : keyClipboard_) {
    Track* t = findTrack(app.anim, target, path);
    if (!t) { app.anim.tracks.push_back({target, path, {}}); t = &app.anim.tracks.back(); }
    Key c = k;
    c.frame += at;
    t->keys.erase(std::remove_if(t->keys.begin(), t->keys.end(), [&](const Key& x) { return std::abs(x.frame - c.frame) < 1e-6; }), t->keys.end());
    t->keys.push_back(c);
    sortKeys(*t);
  }
  if (!keyClipboard_.empty()) { app.setFrame(app.anim.frame); app.commit(); }
}

}  // namespace fd
