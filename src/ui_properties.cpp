// Properties panel (single and multi-object editing, modifiers).
#include "ui_common.hpp"
#include "defs.hpp"
#include <functional>
#include "textures.hpp"

namespace fd {

// ============================================================ properties
void UI::keyable(const std::string& target, const std::vector<std::string>& paths) {
  const Animation& A = app.anim;
  double fr = std::round(A.frame);
  bool keyed = false, animated = false;
  for (auto& p : paths) { keyed |= hasKeyAt(A, target, p, fr); animated |= isAnimated(A, target, p); }
  ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
  if (animated) {  // a diamond left of the value: filled = key on this frame, hollow = animated
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = {mn.x - 5, (mn.y + mx.y) / 2};
    ImVec2 q[4] = {{c.x, c.y - 4}, {c.x + 4, c.y}, {c.x, c.y + 4}, {c.x - 4, c.y}};
    if (keyed) dl->AddConvexPolyFilled(q, 4, IM_COL32(255, 200, 60, 255));
    else dl->AddPolyline(q, 4, IM_COL32(120, 220, 120, 255), ImDrawFlags_Closed, 1.5f);
  }
  if (app.texMode.active) {  // exposed values: a small "P" right of the value
    std::string path = paths.size() == 3 && paths[0] == "color.r" ? std::string("color") : paths[0];
    for (auto& e : app.texMode.settings.exposed)
      if (e.node == target && e.path == path) {
        ImGui::GetWindowDrawList()->AddText({mx.x + 2, mn.y}, IM_COL32(120, 200, 255, 255), "P");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Texture parameter \"%s\"", e.name.c_str());
      }
  }
  if (ImGui::BeginPopupContextItem(("##key" + paths[0]).c_str())) {
    if (ImGui::MenuItem(keyed ? "Replace keyframe" : "Insert keyframe", app.keys.text(Action::InsertKey).c_str())) {
      Animation& W = app.anim;
      for (auto& p : paths) {
        if (p.rfind("surf.", 0) == 0) if (Node* sn = findNode(app.root, target)) app.ensureRest(*sn);  // surface motion starts here
        if (target == "scene") { if (p == "space.radius") setKey(W, target, p, fr, app.space.radius); }
        else if (target.rfind("light:", 0) == 0) {
          for (auto& l : app.lights) { double v; if ("light:" + l.id == target && getLightValue(l, p, v)) setKey(W, target, p, fr, v); }
        } else if (Node* n = findNode(app.root, target)) {
          if (p == "rot") setRotKey(W, target, fr, nodeRotation(*n));
          else { double v; if (getValue(*n, p, v)) setKey(W, target, p, fr, v); }
        }
      }
      app.commit();
    }
    // texture editing mode: values can become parameters of the texture
    if (app.texMode.active && target != "scene" && target.rfind("light:", 0) != 0) {
      std::string path = paths.size() == 3 && paths[0] == "color.r" ? std::string("color") : paths.size() == 1 ? paths[0] : std::string();
      Node* n = findNode(app.root, target);
      if (n && !path.empty() && exposablePath(*n, path)) {
        auto& ex = app.texMode.settings.exposed;
        auto it = std::find_if(ex.begin(), ex.end(), [&](const ExposedParam& e) { return e.node == target && e.path == path; });
        ImGui::Separator();
        if (it != ex.end()) { if (ImGui::MenuItem("Stop exposing")) ex.erase(it); }
        else if (ImGui::MenuItem("Expose as texture parameter")) {
          double v = 0;
          getValue(*n, path == "color" ? "color.r" : path, v);
          double span = std::max(1.0, std::abs(v)) * 2;
          ExposedParam e{target, path, n->name + " " + (path == "color" ? std::string("colour") : path), v - span, v + span};
          if (path == "scale" || path == "k") e.min = 0;
          ex.push_back(e);
        }
      }
    }
    if (keyed && ImGui::MenuItem("Delete keyframe")) { for (auto& p : paths) deleteKey(app.anim, target, p, fr); app.commit(); }
    if (animated && ImGui::MenuItem("Clear animation")) {
      auto& T = app.anim.tracks;
      T.erase(std::remove_if(T.begin(), T.end(), [&](const Track& t) { return t.target == target && std::find(paths.begin(), paths.end(), t.path) != paths.end(); }), T.end());
      app.commit();
    }
    ImGui::EndPopup();
  }
}

void UI::cameraProperties(Node& n, bool& live, bool& done) {
  auto track = [&] { if (ImGui::IsItemEdited()) live = true; if (ImGui::IsItemDeactivatedAfterEdit()) done = true; };
  std::string id = n.id;
  bool geoCam = n.anchor.has_value();
  ImGui::TextDisabled("%s camera", geoCam ? "Geodesic (on the world surface)" : "Full 4D");
  bool active = app.activeCamera == id;
  if (ImGui::Checkbox("Active camera", &active) && active) { app.activeCamera = id; done = true; }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Used for rendering images and animations");
  if (ImGui::Button("Look through")) app.lookThrough(id);
  ImGui::SameLine();
  if (ImGui::Button("Camera to view")) { app.cameraToView(id); return; }
  ImGui::SeparatorText("Lens");
  float fv = (float)n.camFov;
  if (ImGui::SliderFloat("Field of view", &fv, 10, 150, "%.0f deg")) n.camFov = fv;
  keyable(id, {"cam.fov"});
  track();
  if (!geoCam) {
    float ap = (float)n.camAperture, fo = (float)n.camFocus;
    if (ImGui::DragFloat("Aperture", &ap, 0.002f, 0, 2, "%.3f")) n.camAperture = std::max(0.f, ap);
    keyable(id, {"cam.aperture"});
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Depth of field: lens radius (0 = all sharp). Seen with stochastic sampling");
    if (ImGui::DragFloat("Focus distance", &fo, 0.02f, 0.05f, 500)) n.camFocus = std::max(0.05f, fo);
    keyable(id, {"cam.focus"});
    track();
    const char* ax[4] = {"X", "Y", "Z", "W"};
    if (!app.curved()) {
    if (ImGui::Combo("Slider axis", &n.camSlider, ax, 4)) done = true;
    keyable(id, {"cam.slider"});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shown axes when looking through it (as the viewport's slider axis)");
    }
  }
  ImGui::SeparatorText("Placement");
  float pos[4] = {(float)n.xf.pos[0], (float)n.xf.pos[1], (float)n.xf.pos[2], (float)n.xf.pos[3]};
  bool cv = app.curved();
  if (cv ? ImGui::DragFloat3("Position XYZ", pos, 0.02f) : ImGui::DragFloat4(geoCam ? "Surface point" : "Position XYZW", pos, 0.02f))
    for (int i = 0; i < (cv ? 3 : 4); i++) n.xf.pos[i] = pos[i];
  keyable(id, {"pos.x", "pos.y", "pos.z", "pos.w"});
  track();
  if (geoCam) ImGui::TextDisabled("Move it with the gizmo or G to slide along the surface");
  auto ang = anglesFromRotation(n.xf.rot);
  float a[6];
  for (int k = 0; k < 6; k++) a[k] = (float)ang[k];
  bool rc = false;
  ImGui::TextUnformatted("Rotation (degrees per plane)");
  for (int k = 0; k < 6; k++) {
    if ((geoCam || cv) && (k == 2 || k == 4 || k == 5)) continue;  // stays in the surface's tangent space / 3D space
    ImGui::PushID(k);
    ImGui::SetNextItemWidth(68);
    if (ImGui::DragFloat(PLANE_NAMES[k], &a[k], 0.5f, -180.f, 180.f, "%.1f")) rc = true;
    keyable(id, {"rot"});
    track();
    ImGui::PopID();
    if (k % 3 != 2 && !((geoCam || cv) && k == 3)) ImGui::SameLine();
  }
  if (rc) {
    double d[6];
    for (int k = 0; k < 6; k++) d[k] = a[k];
    n.xf.rot = rotationFromAngles(d);
  }
}

// a definition's parameters (modifier or texture), drawn from their descriptions: float drag,
// int drag, checkbox, drop-down list (enum / axis), colour picker; key(paths) adds the keyframe button
static void paramWidgets(const std::vector<ParamDef>& ps, const std::function<double(int, int)>& get,
                         const std::function<void(int, double, int)>& set, const std::string& keyPrefix,
                         const std::function<void(const std::vector<std::string>&)>& key, bool& live, bool& done) {
  for (int pi = 0; pi < (int)ps.size(); pi++) {
    const ParamDef& pd = ps[pi];
    std::string lab = pd.label.empty() ? pd.name : pd.label, k = keyPrefix + pd.name;
    ImGui::PushID(pi);
    ImGui::SetNextItemWidth(pd.type == P_COLOR ? 180.f : 120.f);
    switch (pd.type) {
      case P_FLOAT: {
        float v = (float)get(pi, 0);
        float speed = (float)std::max(1e-4, (pd.max - pd.min) / 300.0);
        if (ImGui::DragFloat(lab.c_str(), &v, speed, (float)pd.min, (float)pd.max, "%.3f")) set(pi, v, 0);
        break;
      }
      case P_INT: {
        int v = (int)std::lround(get(pi, 0));
        if (ImGui::DragInt(lab.c_str(), &v, 0.1f, (int)pd.min, (int)pd.max)) set(pi, v, 0);
        break;
      }
      case P_BOOL: {
        bool v = get(pi, 0) > 0.5;
        if (ImGui::Checkbox(lab.c_str(), &v)) { set(pi, v ? 1 : 0, 0); done = true; }
        break;
      }
      case P_ENUM: case P_AXIS: {
        int v = (int)std::lround(get(pi, 0));
        std::vector<const char*> opts;
        if (pd.type == P_AXIS) opts = {"X", "Y", "Z", "W"};
        else for (auto& o : pd.options) opts.push_back(o.c_str());
        if (ImGui::Combo(lab.c_str(), &v, opts.data(), (int)opts.size())) { set(pi, v, 0); done = true; }
        break;
      }
      case P_COLOR: {
        float c[3] = {(float)get(pi, 0), (float)get(pi, 1), (float)get(pi, 2)};
        if (ImGui::ColorEdit3(lab.c_str(), c)) for (int j = 0; j < 3; j++) set(pi, c[j], j);
        break;
      }
      default: break;
    }
    if (ImGui::IsItemEdited()) live = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
    if (!pd.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pd.tip.c_str());
    if (pd.type == P_COLOR) key({k + ".r", k + ".g", k + ".b"});
    else key({k});
    ImGui::PopID();
  }
}

// the remap curve: points (in, out) in 0..1; drag a point, double-click to add, right-click to remove
static bool curveEditor(std::vector<std::array<double, 2>>& pts, bool& done) {
  ImVec2 size(std::min(220.f, ImGui::GetContentRegionAvail().x), 90.f);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##curve", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(25, 27, 32, 255));
  dl->AddRect(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(80, 85, 95, 255));
  auto toScr = [&](double x, double y) { return ImVec2(p0.x + (float)x * size.x, p0.y + (1 - (float)y) * size.y); };
  auto fromScr = [&](ImVec2 m) { return std::array<double, 2>{std::clamp((m.x - p0.x) / size.x, 0.f, 1.f), std::clamp(1 - (m.y - p0.y) / size.y, 0.f, 1.f)}; };
  std::sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a[0] < b[0]; });
  // the mapping as the shader computes it
  TexSlot probe;
  probe.curve = pts;
  ImVec2 prev = toScr(0, texRemap(probe, 0));
  for (int k = 1; k <= 48; k++) {
    double x = k / 48.0;
    ImVec2 c = toScr(x, texRemap(probe, x));
    dl->AddLine(prev, c, IM_COL32(120, 190, 255, 255), 2.f);
    prev = c;
  }
  static int drag = -1;
  bool changed = false;
  ImVec2 mouse = ImGui::GetIO().MousePos;
  int hover = -1;
  for (int k = 0; k < (int)pts.size(); k++) {
    ImVec2 c = toScr(pts[k][0], pts[k][1]);
    if (std::abs(c.x - mouse.x) < 6 && std::abs(c.y - mouse.y) < 6) hover = k;
    dl->AddCircleFilled(c, 4.f, k == hover ? IM_COL32(255, 220, 120, 255) : IM_COL32(230, 230, 230, 255));
  }
  if (ImGui::IsItemHovered()) {
    if (ImGui::IsMouseClicked(0) && hover >= 0) drag = hover;
    if (ImGui::IsMouseDoubleClicked(0) && hover < 0 && (int)pts.size() < TEX_CURVE_MAX) {
      if (pts.empty()) { pts.push_back({0, 0}); pts.push_back({1, 1}); }
      pts.push_back(fromScr(mouse));
      changed = done = true;
    }
    if (ImGui::IsMouseClicked(1) && hover >= 0) { pts.erase(pts.begin() + hover); changed = done = true; }
    ImGui::SetTooltip("Drag points; double-click to add, right-click to remove");
  }
  if (drag >= 0 && drag < (int)pts.size()) {
    if (ImGui::IsMouseDown(0)) { pts[drag] = fromScr(mouse); changed = true; }
    else { drag = -1; done = true; }
  }
  return changed;
}

void UI::textureSlot(Node& n, int ch, bool& live, bool& done) {
  TexSlot& t = n.mat.tex[ch];
  auto d = defs().texture(t.def);
  ImGui::PushID(1000 + ch);
  std::string label = t.used() ? (d ? d->name : "Unknown (" + t.def + ")") : std::string("None");
  ImGui::SetNextItemWidth(150);
  std::string comboLab = std::string(ch == CH_BUMP ? "Bump" : app.texMode.active ? "Fill" : "Texture") + "##slot";
  if (ImGui::BeginCombo(comboLab.c_str(), label.c_str())) {
    if (ImGui::Selectable("None", !t.used())) { t.def.clear(); done = true; }
    std::string cat;
    for (auto& td : defs().textures()) {
      if (td->id.rfind("__", 0) == 0 || td->id == app.texMode.texId && app.texMode.active) continue;  // (internal / itself)
      std::string c = td->category.empty() ? "Custom" : td->category;
      if (c != cat) { ImGui::SeparatorText(c.c_str()); cat = c; }
      std::string nm = td->name + (textureKind(*td) != TEXK_MASK ? "  (colour)" : "");
      if (ImGui::Selectable(nm.c_str(), t.def == td->id)) {
        bool fresh = !t.used();
        t.def = td->id;
        t.params.clear();
        conformValues(t.params, td->params);
        if (fresh && ch == CH_BUMP) t.value2 = 0.02;  // a small relief
        if (fresh && ch != CH_COLOR && ch != CH_BUMP) t.value2 = ch == CH_ROUGHNESS ? 0.15 : 1.0;
        done = true;
      }
      if (!td->description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", td->description.c_str());
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(ch == CH_COLOR ? "A procedural 4D texture: masks blend to the second colour, colour textures give the colour"
                      : ch == CH_BUMP ? "Relief that changes the lighting only (the shape, clicks and collisions are unchanged)"
                                      : "Blends this value to the second value where the texture's mask is 1");
  d = defs().texture(t.def);
  if (!t.used() || !d) { ImGui::PopID(); return; }
  std::string nid = n.id, pre = std::string("tex.") + CHANNEL_IDS[ch] + ".";
  auto key = [&](const std::vector<std::string>& paths) { keyable(nid, paths); };
  auto track = [&] { if (ImGui::IsItemEdited()) live = true; if (ImGui::IsItemDeactivatedAfterEdit()) done = true; };
  ImGui::Indent(12);
  conformValues(t.params, d->params);
  paramWidgets(d->params, [&](int i, int c) { return paramValue(t.params, d->params, i, c); },
               [&](int i, double v, int c) { setParamValue(t.params, d->params, i, v, c); }, pre + "p.", key, live, done);
  TexKind kind = textureKind(*d);
  if (ch == CH_COLOR) {
    if (kind == TEXK_MASK) {
      float c2[3] = {(float)t.color2[0], (float)t.color2[1], (float)t.color2[2]};
      ImGui::SetNextItemWidth(180);
      if (ImGui::ColorEdit3("Second colour", c2)) for (int k = 0; k < 3; k++) t.color2[k] = c2[k];
      track();
      key({pre + "color2.r", pre + "color2.g", pre + "color2.b"});
    }
  } else {
    float v2 = (float)t.value2;
    ImGui::SetNextItemWidth(120);
    if (ch == CH_BUMP) { if (ImGui::DragFloat("Height", &v2, 0.001f, -1.f, 1.f, "%.4f")) t.value2 = v2; }
    else if (ImGui::SliderFloat("Second value", &v2, 0, ch == CH_SPECULAR ? 2.f : 1.f)) t.value2 = v2;
    track();
    key({pre + "value2"});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(ch == CH_BUMP ? "Relief height where the mask is 1 (negative: dents)" : "The value where the mask is 1");
  }
  float st = (float)t.strength;
  ImGui::SetNextItemWidth(120);
  if (ImGui::SliderFloat("Strength", &st, 0, 1)) t.strength = st;
  track();
  key({pre + "strength"});
  // remap (the mask: colour textures without coverage use their brightness)
  if (ImGui::TreeNode("Remap")) {
    float lo = (float)t.low, hi = (float)t.high, ct = (float)t.contrast;
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("Low", &lo, 0.005f, -1.f, 2.f, "%.3f")) t.low = lo;
    track(); key({pre + "low"});
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("High", &hi, 0.005f, -1.f, 2.f, "%.3f")) t.high = hi;
    track(); key({pre + "high"});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Masks from Low to High map to 0..1 (clamped)");
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("Contrast", &ct, -1, 1)) t.contrast = ct;
    track(); key({pre + "contrast"});
    if (ImGui::Checkbox("Invert", &t.invert)) done = true;
    if (curveEditor(t.curve, done)) live = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset curve")) { t.curve.clear(); done = true; }
    // the mapped mask, 0 -> 1
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = std::min(220.f, ImGui::GetContentRegionAvail().x);
    for (int k = 0; k < 64; k++) {
      int g = (int)std::lround(255 * std::clamp(texRemap(t, (k + 0.5) / 64.0), 0.0, 1.0));
      ImGui::GetWindowDrawList()->AddRectFilled({p0.x + w * k / 64, p0.y}, {p0.x + w * (k + 1) / 64 + 1, p0.y + 10}, IM_COL32(g, g, g, 255));
    }
    ImGui::Dummy({w, 12});
    ImGui::TreePop();
  }
  // mapping: the texture coordinates
  if (ImGui::TreeNode("Mapping")) {
    std::vector<std::string> fromNames{"Own"};
    std::vector<int> fromCh{-1};
    for (int c = 0; c < CH_COUNT; c++)
      if (c != ch && n.mat.tex[c].used() && n.mat.tex[c].mapFrom < 0) { fromNames.push_back(std::string("Same as ") + CHANNEL_NAMES[c]); fromCh.push_back(c); }
    int cur = 0;
    for (int k = 0; k < (int)fromCh.size(); k++) if (fromCh[k] == t.mapFrom) cur = k;
    std::vector<const char*> fn;
    for (auto& s2 : fromNames) fn.push_back(s2.c_str());
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("Use mapping", &cur, fn.data(), (int)fn.size())) { t.mapFrom = fromCh[cur]; done = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Share another channel's mapping so their patterns stay aligned");
    if (t.mapFrom < 0) {
      TexMapping& m = t.map;
      ImGui::SetNextItemWidth(120);
      if (ImGui::Combo("Space", &m.space, TEX_SPACE_NAMES, TS_COUNT)) done = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Object: moves and turns with the object. World: fixed in space. Parent: shared by a group's objects.\n"
                          "Curved spaces use object space.");
      float sc = (float)m.scale;
      ImGui::SetNextItemWidth(120);
      if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.001f, 1000.f, "%.3f")) m.scale = std::max(0.001f, sc);
      track(); key({pre + "scale"});
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pattern cells per unit");
      float as[4], of[4];
      for (int k = 0; k < 4; k++) { as[k] = (float)m.axisScale[k]; of[k] = (float)m.offset[k]; }
      ImGui::SetNextItemWidth(220);
      if (ImGui::DragFloat4("Axis scale", as, 0.01f, 0.001f, 1000.f, "%.2f")) for (int k = 0; k < 4; k++) m.axisScale[k] = std::max(0.001f, as[k]);
      track(); key({pre + "axisScale.x", pre + "axisScale.y", pre + "axisScale.z", pre + "axisScale.w"});
      ImGui::SetNextItemWidth(220);
      if (ImGui::DragFloat4("Offset", of, 0.01f, -1000.f, 1000.f, "%.2f")) for (int k = 0; k < 4; k++) m.offset[k] = of[k];
      track(); key({pre + "offset.x", pre + "offset.y", pre + "offset.z", pre + "offset.w"});
      // rotation: the 3D planes first, then the planes through W
      float r3[3] = {(float)m.rot[0], (float)m.rot[1], (float)m.rot[3]}, rw[3] = {(float)m.rot[2], (float)m.rot[4], (float)m.rot[5]};
      ImGui::SetNextItemWidth(220);
      if (ImGui::DragFloat3("Rotate XY XZ YZ", r3, 0.5f, -360.f, 360.f, "%.1f")) { m.rot[0] = r3[0]; m.rot[1] = r3[1]; m.rot[3] = r3[2]; }
      track(); key({pre + "rot.XY", pre + "rot.XZ", pre + "rot.YZ"});
      ImGui::SetNextItemWidth(220);
      if (ImGui::DragFloat3("Rotate XW YW ZW", rw, 0.5f, -360.f, 360.f, "%.1f")) { m.rot[2] = rw[0]; m.rot[4] = rw[1]; m.rot[5] = rw[2]; }
      track(); key({pre + "rot.XW", pre + "rot.YW", pre + "rot.ZW"});
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns the pattern through the fourth dimension");
      if (m.space == TS_OBJECT && ImGui::Button("Fit to object")) {
        // one texture cell spans the object's bounds (local units)
        Node c = n;
        c.xf = Transform{};
        c.anchor.reset();
        Node g = makeGroup(BoolOp::Union, {c});
        auto b = computeBounds(g, app.reg);
        auto it = b.find(c.id);
        if (it != b.end() && std::isfinite(it->second.r) && it->second.r > 1e-6) {
          double r = it->second.r;
          m.scale = 1 / (2 * r);
          m.axisScale = {1, 1, 1, 1};
          for (int k = 0; k < 4; k++) m.offset[k] = it->second.c[k] - r;
          done = true;
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale and offset so that one cell of the pattern covers the object");
    }
    ImGui::TreePop();
  }
  ImGui::Unindent(12);
  ImGui::PopID();
}

void UI::propertiesPanel() {
  if (!showProperties_) return;
  if (!ImGui::Begin("Properties", &showProperties_)) { ImGui::End(); return; }
  Node* n = app.selected();
  if (!n) {
    ImGui::TextWrapped("Nothing selected. Click an object in the viewport or the scene tree.");
    ImGui::End();
    return;
  }
  if (app.texMode.active && app.texMode.view == 1) {
    ImGui::TextWrapped("Preview of the texture: switch to Shapes (toolbar) to edit the composition.");
    ImGui::End();
    return;
  }
  bool live = false, done = false;
  auto track = [&] { if (ImGui::IsItemEdited()) live = true; if (ImGui::IsItemDeactivatedAfterEdit()) done = true; };
  bool isRoot = n->id == app.root.id;
  std::string nid = n->id;

  // ---- several objects selected: edits of the primary (last selected) one are applied to all ----
  std::vector<Node*> others;
  for (auto& id : app.selection)
    if (id != nid && id != app.root.id)
      if (Node* o = findNode(app.root, id)) others.push_back(o);
  Node before;
  { auto kids = std::move(n->children); before = *n; n->children = std::move(kids); }
  bool stacksMatch = true;
  for (Node* o : others) if (!sameModifierTypes(*o, *n)) stacksMatch = false;
  ModStackOp modOp;
  // marks a field whose value differs between the selected objects (of the same kind)
  auto mixed = [&](auto get) {
    bool diff = false;
    for (Node* o : others) if (o->isLeaf() == n->isLeaf() && !(get(*o) == get(*n))) diff = true;
    if (!diff) return;
    ImGui::SameLine();
    ImGui::TextDisabled("*");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Differs between the selected objects (showing \"%s\")", n->name.c_str());
  };
  if (!others.empty()) {
    ImGui::TextColored({0.55f, 0.8f, 1, 1}, "Editing %d objects: values from \"%s\"", (int)others.size() + 1, n->name.c_str());
  }

  char nameBuf[256];
  std::snprintf(nameBuf, sizeof nameBuf, "%s", n->name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof nameBuf)) n->name = nameBuf;
  track();
  if (n->isCamera()) {  // cameras: their own small panel
    cameraProperties(*n, live, done);
    ImGui::End();
    if (done) app.commit();
    else if (live) app.rebuild();
    return;
  }
  int dim = nodeDim(*n, app.reg);
  ImGui::TextDisabled("%dD %s", dim, n->isCustom() ? "custom SDF" : n->isPoly() ? "polyhedron" : n->isHoneycomb() ? "honeycomb" : n->isLeaf() ? "shape (point + modifiers)" : "group");
  ImGui::SameLine();
  if (ImGui::SmallButton("View SDF code")) { showSdf_ = true; sdfNodeId_ = n->id; }
  if (!app.texMode.active && !isRoot) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Save as texture")) { std::string id = n->id; ImGui::End(); app.saveAsTexture(id); return; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens the texture editing mode with a copy of this object: its shapes and colours become a texture");
  }
  if (!isRoot) {
    if (ImGui::Checkbox("Locked", &n->locked)) done = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cannot be clicked or moved in the viewport (still editable here)");
    if (!n->linkId.empty()) {
      ImGui::SameLine();
      ImGui::TextColored({0.55f, 0.8f, 1, 1}, "Linked (%d copies)", app.linkCount(n->linkId));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shape, material, modifiers and contents are shared: editing one edits all.\nEach copy keeps its own position, rotation and scale.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Unlink")) { app.unlink(nid); ImGui::End(); return; }
    }
  }

  if (n->isLeaf()) {
    if (n->isPolytope()) {
      PolyParams& pp = n->poly;
      if (n->isPoly()) {
        ImGui::SeparatorText(polyIs4D(pp.type) ? "Regular 4D polytope (flat cells)" : "Polyhedron (flat faces)");
        // (4D polytopes: flat 4D only; curved spaces are 3D)
        if (ImGui::Combo("Type", &pp.type, POLY_NAMES, app.curved() ? POLY_3D_COUNT : POLY_COUNT)) done = true;
        if (polyIs4D(pp.type) && ImGui::IsItemHovered())
          ImGui::SetTooltip("A 4D solid bounded by 3D cells: 5-cell (5 tetrahedra), 8-cell (8 cubes), 16-cell (16 tetrahedra),\n"
                            "24-cell (24 octahedra), 120-cell (120 dodecahedra), 600-cell (600 tetrahedra).\n"
                            "The view shows its 3D cross-section; move along W to see it change");
        float sz = (float)pp.size;
        if (ImGui::DragFloat("Inradius", &sz, 0.01f, 0.01f, 100.f)) { pp.size = std::max(0.01f, sz); live = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance from the centre to each face. In curved space the faces are geodesic\nplanes: the same inradius gives other angles than in flat space");
      } else {
        ImGui::SeparatorText("Honeycomb (fills the whole space)");
        const auto& all = honeycombs();
        SpaceKind sk = app.space.kind;
        std::string cur = pp.honey >= 0 && pp.honey < (int)all.size() ? std::string(all[pp.honey].schlafli) + " " + all[pp.honey].name : "?";
        if (ImGui::BeginCombo("Tiling", cur.c_str())) {
          for (int i = 0; i < (int)all.size(); i++) {
            if (all[i].space != sk) continue;
            std::string l = std::string(all[i].schlafli) + " " + all[i].name;
            if (ImGui::Selectable(l.c_str(), i == pp.honey)) { pp.honey = i; done = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", all[i].note);
          }
          ImGui::EndCombo();
        }
        if (pp.honey >= 0 && pp.honey < (int)all.size()) {
          const Honeycomb& h = all[pp.honey];
          if (h.space != sk) ImGui::TextColored({1, 0.6f, 0.3f, 1}, "This tiling belongs to %s space: choose another", SPACE_NAMES[(int)h.space]);
          else if (sk == SpaceKind::Flat) {
            float sz = (float)pp.size;
            if (ImGui::DragFloat("Cell inradius", &sz, 0.01f, 0.05f, 100.f)) { pp.size = std::max(0.05f, sz); live = true; }
            if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
          } else
          {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s; cell inradius %.3f (%.4f R: set by the curvature)", h.note, honeycombInradius(h) * app.space.R(), honeycombInradius(h));
            ImGui::PopStyleColor();
          }
        }
        const char* styles[2] = {"Edges", "Cells"};
        if (ImGui::Combo("Style", &pp.style, styles, 2)) done = true;
        float w = (float)pp.width;
        if (ImGui::DragFloat(pp.style ? "Gap" : "Edge radius", &w, 0.002f, 0.f, 2.f, "%.3f")) { pp.width = std::max(0.f, w); live = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
        if (ImGui::Checkbox("Alternate colours", &pp.alternate)) done = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Neighbouring cells use the colour and the material's second colour");
        if (pp.alternate) {
          float c2[3] = {(float)n->mat.color2[0], (float)n->mat.color2[1], (float)n->mat.color2[2]};
          if (ImGui::ColorEdit3("Second colour", c2)) { for (int i = 0; i < 3; i++) n->mat.color2[i] = c2[i]; live = true; }
          if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
        }
      }
    }
    ImGui::SeparatorText("Shape");
    float c[3] = {(float)n->color[0], (float)n->color[1], (float)n->color[2]};
    if (ImGui::ColorEdit3("Color", c)) for (int i = 0; i < 3; i++) n->color[i] = c[i];
    keyable(nid, {"color.r", "color.g", "color.b"});
    track();
    mixed([](const Node& x) { return x.color; });
    textureSlot(*n, CH_COLOR, live, done);
    mixed([](const Node& x) { return x.mat.tex[CH_COLOR]; });
    if (dim < 4) {
      float th = (float)n->thickness;
      if (ImGui::DragFloat("Thickness", &th, 0.005f, 0.001f, 10.f)) n->thickness = std::max(0.001f, th);
      keyable(nid, {"thickness"});
      track();
    mixed([](const Node& x) { return x.thickness; });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extent along the unused axes%s", n->anchor ? " (here: across the world surface)" : "");
    }
    if (!app.texMode.active) {  // (texture editing mode: objects have a colour and a fill only)
    ImGui::SeparatorText("Material");
    float em = (float)n->mat.emissive;
    if (ImGui::SliderFloat("Emissive", &em, 0, 1)) { bool was = n->mat.emissive > 0; n->mat.emissive = em; if (was != (em > 0)) done = true; }
    keyable(nid, {"mat.emissive"});
    track();
    mixed([](const Node& x) { return x.mat.emissive; });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Glows. Simple lighting: always lit, lights its surroundings as a point light.\n"
                        "Path-traced lighting: a real light source of its own shape");
    if (n->mat.emissive > 0) {
      float es = (float)n->mat.emissionStrength;
      if (ImGui::DragFloat("Emission strength", &es, 0.05f, 0.f, 200.f, "%.2f")) n->mat.emissionStrength = std::max(0.f, es);
      track();
      mixed([](const Node& x) { return x.mat.emissionStrength; });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path-traced lighting: how much light it gives (colour x emissive x strength)");
    }
    textureSlot(*n, CH_EMISSION, live, done);
    float ro = (float)n->mat.roughness, spc = (float)n->mat.specular, tr = (float)n->mat.transparency;
    if (ImGui::SliderFloat("Roughness", &ro, 0, 1)) n->mat.roughness = ro;
    keyable(nid, {"mat.roughness"});
    track();
    mixed([](const Node& x) { return x.mat.roughness; });
    textureSlot(*n, CH_ROUGHNESS, live, done);
    if (ImGui::SliderFloat("Specular", &spc, 0, 2)) n->mat.specular = spc;
    keyable(nid, {"mat.specular"});
    track();
    mixed([](const Node& x) { return x.mat.specular; });
    textureSlot(*n, CH_SPECULAR, live, done);
    if (ImGui::SliderFloat("Transparency", &tr, 0, 1)) n->mat.transparency = tr;
    keyable(nid, {"mat.transparency"});
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("See-through surfaces (bounces per pixel: Settings > Rendering)");
    mixed([](const Node& x) { return x.mat.transparency; });
    textureSlot(*n, CH_TRANSPARENCY, live, done);
    if (n->mat.transparency > 0) {
      float io = (float)n->mat.ior;
      if (ImGui::SliderFloat("Refraction index", &io, 1.0f, 2.5f, "%.2f")) n->mat.ior = io;
      keyable(nid, {"mat.ior"});
      track();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much light bends entering / leaving it: 1 = none, 1.33 water, 1.5 glass, 2.4 diamond");
      mixed([](const Node& x) { return x.mat.ior; });
    }
    float me = (float)n->mat.metallic;
    if (ImGui::SliderFloat("Metallic (mirror)", &me, 0, 1)) n->mat.metallic = me;
    keyable(nid, {"mat.metallic"});
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reflects the scene, tinted by the colour (1 = mirror)");
    mixed([](const Node& x) { return x.mat.metallic; });
    textureSlot(*n, CH_METALLIC, live, done);
    textureSlot(*n, CH_BUMP, live, done);
    mixed([](const Node& x) { return x.mat.tex[CH_BUMP]; });
    }
    if (n->isCustom()) {
      ImGui::SeparatorText("Custom SDF (GLSL)");
      if (codeEditId_ != n->id) { codeEditId_ = n->id; codeBuf_ = n->code; }
      codeBuf_.reserve(codeBuf_.size() + 4096);
      if (codeBuf_.capacity() < 16384) codeBuf_.reserve(16384);
      std::vector<char> buf(codeBuf_.begin(), codeBuf_.end());
      buf.resize(std::max<size_t>(16384, codeBuf_.size() + 4096), 0);
      if (ImGui::InputTextMultiline("##code", buf.data(), buf.size(), {-1, 200}, ImGuiInputTextFlags_AllowTabInput)) codeBuf_ = buf.data();
      if (ImGui::Button("Apply code")) {
        std::string err = app.renderer->checkCustomCode(codeBuf_, true);  // (a small shader: checked at once)
        codeErr_ = err;
        if (err.empty()) { n->code = codeBuf_; done = true; }
      }
      ImGui::SameLine();
      if (ImGui::Button("Revert")) { codeBuf_ = n->code; codeErr_.clear(); }
      if (!codeErr_.empty()) ImGui::TextColored({1, 0.55f, 0.4f, 1}, "%s", codeErr_.c_str());
      else if (!app.renderer->checkCustomCode(n->code).empty()) ImGui::TextColored({1, 0.55f, 0.4f, 1}, "Stored code does not compile: shown as its bounding box");
      float cb = (float)n->codeBound;
      if (ImGui::DragFloat("Bound (half size)", &cb, 0.02f, 0.01f, 100.f)) n->codeBound = std::max(0.01f, cb);
      track();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your shape must fit in this box: it is used to skip far-away work and,\non the CPU, for clicking / collisions / placement");
    }
  } else {
    ImGui::SeparatorText("Boolean");
    int op = (int)n->op;
    if (ImGui::Combo("Operator", &op, BOOL_OP_LABELS, 6)) { n->op = (BoolOp)op; done = true; }
    if (ImGui::IsItemHovered() && (n->op == BoolOp::Subtraction || n->op == BoolOp::SmoothSubtraction)) ImGui::SetTooltip("First child minus the following children");
    mixed([](const Node& x) { return x.op; });
    if (isSmooth(n->op)) { float k = (float)n->k; if (ImGui::DragFloat("Smoothness", &k, 0.01f, 0.001f, 10.f)) n->k = std::max(0.001f, k); keyable(nid, {"k"}); track(); }
    if (!isRoot && !n->children.empty()) {
      if (ImGui::Button("Recentre origin on first object")) { app.recentreGroup(nid); ImGui::End(); return; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the group's origin (centre of rotation and scaling) to its first object.\nNothing moves in the world.");
    }
  }

  if (!isRoot) {
    ImGui::SeparatorText(n->anchor ? "Transform (on the world surface)" : "Transform");
    float pos[4] = {(float)n->xf.pos[0], (float)n->xf.pos[1], (float)n->xf.pos[2], (float)n->xf.pos[3]};
    bool cv = app.curved();
    if (cv ? ImGui::DragFloat3("Position XYZ", pos, 0.02f) : ImGui::DragFloat4("Position XYZW", pos, 0.02f))
      for (int i = 0; i < (cv ? 3 : 4); i++) n->xf.pos[i] = pos[i];
    if (cv && ImGui::IsItemHovered())
      ImGui::SetTooltip("Normal coordinates in the parent's frame: walk %.2f along this direction from its origin", length(n->xf.pos));
    keyable(nid, {"pos.x", "pos.y", "pos.z", "pos.w"});
    track();
    mixed([](const Node& x) { return x.xf.pos; });
    // absolute rotation as 6 plane angles (decomposed from the stored rotation matrix)
    auto ang = anglesFromRotation(n->xf.rot);
    float a[6];
    for (int k = 0; k < 6; k++) a[k] = (float)ang[k];
    bool rotChanged = false;
    ImGui::TextUnformatted("Rotation (degrees per plane)");
    for (int k = 0; k < 6; k++) {
      bool hide = (n->anchor || cv) && (k == 2 || k == 4 || k == 5);  // stay in the surface's tangent space / 3D space
      if (hide) continue;
      ImGui::PushID(k);
      ImGui::SetNextItemWidth(68);
      if (ImGui::DragFloat(PLANE_NAMES[k], &a[k], 0.5f, -180.f, 180.f, "%.1f")) rotChanged = true;
      keyable(nid, {"rot"});
      track();
      ImGui::PopID();
      if (k % 3 != 2 && !((n->anchor || cv) && k == 3)) ImGui::SameLine();
    }
    if (rotChanged) {
      double d[6];
      for (int k = 0; k < 6; k++) d[k] = a[k];
      n->xf.rot = rotationFromAngles(d);
    }
    if (ImGui::Button("Reset rotation")) {
      n->xf.rot = identity();
      done = true;
      for (Node* o : others) o->xf.rot = identity();  // absolute for every selected object
      before.xf.rot = n->xf.rot;
    }
    mixed([](const Node& x) { return x.xf.rot; });
    float sc = (float)n->xf.scale;
    if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.01f, 100.f)) n->xf.scale = std::max(0.01f, sc);
    keyable(nid, {"scale"});
    track();
    mixed([](const Node& x) { return x.xf.scale; });
  }

  // ---- modifier stack ----
  ImGui::SeparatorText("Modifiers (applied top to bottom)");
  if (!others.empty() && !stacksMatch) ImGui::TextDisabled("Modifier stacks differ: editing \"%s\" only", n->name.c_str());
  auto problems = modifierProblems(*n, app.reg);
  auto axes = modifierAxes(*n, app.reg);
  int remove = -1, moveUp = -1;
  const char* axn[] = {"X", "Y", "Z", "W"};
  for (size_t i = 0; i < n->mods.size(); i++) {
    auto& m = n->mods[i];
    ImGui::PushID((int)i);
    auto mdef = modifierDefOf(m);
    std::string title = m.type == MOD_DEF ? (mdef ? mdef->name : "Unknown modifier (" + m.def + ")") : std::string(MOD_NAMES[m.type]);
    if (m.raises() && axes[i].valid) title += std::string(" -> ") + AXIS[axes[i].newAxis];
    ImGui::TextColored(m.raises() ? ImVec4(0.6f, 0.85f, 1, 1) : ImVec4(0.9f, 0.9f, 0.9f, 1), "%s", title.c_str());
    if (mdef && !mdef->description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mdef->description.c_str());
    float ww = ImGui::GetWindowWidth();
    ImGui::SameLine(ww - 75);
    if (i > 0 && ImGui::SmallButton("^")) moveUp = (int)i;
    ImGui::SameLine(ww - 50);
    if (ImGui::SmallButton("x")) remove = (int)i;
    ImGui::Indent(10);
    switch (m.type) {
      case MOD_DEF: {  // a data-driven modifier: its parameters, drawn from their descriptions
        if (!mdef) break;
        conformParams(m, *mdef);
        paramWidgets(mdef->params, [&](int pi, int c) { return modParam(m, *mdef, pi, c); },
                     [&](int pi, double v, int c) { setModParam(m, *mdef, pi, v, c); }, "mods." + std::to_string(i) + ".p.",
                     [&](const std::vector<std::string>& paths) { keyable(nid, paths); }, live, done);
        break;
      }
      case MOD_MIRROR: {
        ImGui::SetNextItemWidth(60);
        if (ImGui::Combo("Axis", &m.axis, axn, 4)) done = true;
        float off = (float)m.offset;
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Offset", &off, 0.02f, 0.f, 100.f)) m.offset = std::max(0.f, off);
        keyable(nid, {"mods." + std::to_string(i) + ".offset"});
        track();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance of the shape and of its mirror image from the mirror plane");
        break;
      }
      case MOD_REPEAT: {
        ImGui::SetNextItemWidth(60);
        if (ImGui::Combo("Axis", &m.axis, axn, 4)) done = true;
        float sp = (float)m.spacing;
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Spacing", &sp, 0.05f, 0.01f, 100.f)) m.spacing = std::max(0.01f, sp);
        keyable(nid, {"mods." + std::to_string(i) + ".spacing"});
        track();
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragInt("Copies +-", &m.count, 0.1f, 0, 100, m.count == 0 ? "infinite" : "%d")) m.count = std::max(0, m.count);
        keyable(nid, {"mods." + std::to_string(i) + ".count"});
        track();
        break;
      }
      case MOD_EXTRUDE: {
        int md = (int)m.mode;
        const char* modes[] = {"finite", "infinite", "half-space (<= 0)"};
        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("Mode", &md, modes, 3)) { m.mode = (ExtrudeMode)md; done = true; }
        if (m.mode == ExtrudeMode::Finite) {
          float L = (float)m.length;
          ImGui::SetNextItemWidth(120);
          if (ImGui::DragFloat("Length", &L, 0.02f, 0.001f, 100.f)) m.length = std::max(0.001f, L);
          keyable(nid, {"mods." + std::to_string(i) + ".length"});
          track();
        }
        break;
      }
      case MOD_REVOLVE: {
        int base = axes[i].valid ? axes[i].newAxis : 0;
        std::string cur = m.axis < 0 ? "newest axis" : std::string("spin ") + AXIS[m.axis];
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("Profile axis", cur.c_str())) {
          if (ImGui::Selectable("newest axis", m.axis < 0)) { m.axis = -1; done = true; }
          for (int k = 0; k < std::max(base, 1); k++)
            if (ImGui::Selectable((std::string("spin ") + AXIS[k]).c_str(), m.axis == k)) { m.axis = k; done = true; }
          ImGui::EndCombo();
        }
        float R = (float)m.radius;
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Radius", &R, 0.02f, 0.f, 100.f)) m.radius = std::max(0.f, R);
        keyable(nid, {"mods." + std::to_string(i) + ".radius"});
        track();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius of revolution: 0 = around the centre (disk/sphere), > 0 = torus-like");
        break;
      }
      case MOD_RADIAL: {
        // planes of rotation; the 3D ones are named by the axis they turn around
        static const char* PLANE_LABELS[6] = {"XY (around Z)", "XZ (around Y)", "XW", "YZ (around X)", "YW", "ZW"};
        int cur = -1;
        for (int p = 0; p < 6; p++) if ((m.i == PLANES[p][0] && m.j == PLANES[p][1]) || (m.i == PLANES[p][1] && m.j == PLANES[p][0])) cur = p;
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("Plane", cur >= 0 ? PLANE_LABELS[cur] : "?")) {
          for (int p = 0; p < 6; p++)
            if (ImGui::Selectable(PLANE_LABELS[p], p == cur)) { m.i = PLANES[p][0]; m.j = PLANES[p][1]; done = true; }
          ImGui::EndCombo();
        }
        float R = (float)m.radius, an = (float)m.angle, st = (float)m.startAngle;
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Radius", &R, 0.02f, 0.f, 100.f)) m.radius = std::max(0.f, R);
        keyable(nid, {"mods." + std::to_string(i) + ".radius"});
        track();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance of the copies from the axis");
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragInt("Copies", &m.count, 0.1f, 1, 360)) m.count = std::max(1, m.count);
        keyable(nid, {"mods." + std::to_string(i) + ".count"});
        track();
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Angle step", &an, 0.5f, 0.1f, 360.f, "%.1f deg")) m.angle = std::clamp(an, 0.1f, 360.f);
        keyable(nid, {"mods." + std::to_string(i) + ".angle"});
        track();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Angle between neighbouring copies (copies x step may be less than 360)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fill 360")) { m.angle = 360.0 / std::max(1, m.count); done = true; }
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Start angle", &st, 0.5f, -360.f, 360.f, "%.1f deg")) m.startAngle = st;
        keyable(nid, {"mods." + std::to_string(i) + ".startAngle"});
        track();
        break;
      }
    }
    for (auto& [k, msg] : problems) if (k == (int)i) ImGui::TextColored({1, 0.7f, 0.3f, 1}, "%s", msg.c_str());
    ImGui::Unindent(10);
    ImGui::PopID();
  }
  if (remove >= 0) { n->mods.erase(n->mods.begin() + remove); done = true; modOp = {ModStackOp::Remove, remove, {}}; }
  if (moveUp > 0) { std::swap(n->mods[moveUp], n->mods[moveUp - 1]); done = true; modOp = {ModStackOp::MoveUp, moveUp, {}}; }
  if (ImGui::Button("+ Add modifier")) ImGui::OpenPopup("addmod");
  if (ImGui::BeginPopup("addmod")) {
    // every modifier definition (built-in and custom), grouped by category
    auto all = defs().modifiers();
    std::vector<std::string> cats;
    for (auto& d : all) {
      std::string c = d->category.empty() ? "Custom" : d->category;
      if (std::find(cats.begin(), cats.end(), c) == cats.end()) cats.push_back(c);
    }
    for (auto& c : cats) {
      ImGui::SeparatorText(c.c_str());
      for (auto& d : all) {
        if ((d->category.empty() ? "Custom" : d->category) != c) continue;
        if (ImGui::MenuItem(d->name.c_str())) {
          Modifier m = makeModifier(*d);
          n->mods.push_back(m);
          done = true;
          modOp = {ModStackOp::Add, -1, m};
        }
        if (!d->description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d->description.c_str());
      }
    }
    ImGui::EndPopup();
  }

  if (app.curved()) ImGui::End();  // no world surfaces in curved spaces
  if (!app.curved()) {
  ImGui::SeparatorText("World surface");
  if (n->id == app.world) {
    ImGui::TextUnformatted("This object is the world.");
    if (ImGui::Button("Stop using as world")) { app.setWorld(""); ImGui::End(); return; }
  } else if (!isRoot && dim == 4 && !n->anchor) {
    if (ImGui::Button("Use as world")) { app.setWorld(nid); ImGui::End(); return; }
  }
  if (n->anchor) {
    Node* w = findNode(app.root, n->anchor->world);
    ImGui::TextWrapped("Attached to \"%s\".", w ? w->name.c_str() : "missing world");
    if (ImGui::Button("Detach from surface")) { app.detach(nid); ImGui::End(); return; }
    Anchor& an = *n->anchor;
    if (ImGui::Checkbox("Follow surface", &an.follow)) done = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Bend the object's X axis along the surface (the geodesic through its anchor):\n"
                        "a long rod lies along a curved world, and through a wormhole's throat");
    ImGui::SeparatorText("Surface motion");
    ImGui::TextDisabled("Turn by Heading, walk Travel along the surface, roll by Spin");
    float tv = (float)an.travel, hd = (float)an.heading, sp = (float)an.spin;
    bool sm = false;
    if (ImGui::DragFloat("Travel", &tv, 0.02f)) { app.ensureRest(*n); an.travel = tv; sm = true; }
    keyable(nid, {"surf.travel"});
    if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance walked along the surface (along the object's X axis), from its rest pose.\nRight-click: keyframes (smooth, undistorted motion, also through a wormhole)");
    if (ImGui::DragFloat("Heading", &hd, 0.5f, -3600, 3600, "%.1f deg")) { app.ensureRest(*n); an.heading = hd; sm = true; }
    keyable(nid, {"surf.heading"});
    if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn about the object's Y axis before walking (the direction it travels in)");
    if (ImGui::DragFloat("Spin", &sp, 0.5f, -36000, 36000, "%.1f deg")) { app.ensureRest(*n); an.spin = sp; sm = true; }
    keyable(nid, {"surf.spin"});
    if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Roll about the object's X axis (its length)");
    if (sm) { app.applySurfacePose(*n); live = true; }
    if (an.hasRest) ImGui::TextDisabled("Moving it by hand moves its rest pose (the whole motion)");
  } else if (!app.world.empty() && n->id != app.world && !isRoot && dim <= 3) {
    if (ImGui::Button("Attach to world surface")) { app.attachToWorld(nid); ImGui::End(); return; }
  }
  ImGui::End();
  }
  if ((done || live) && !others.empty())
    for (Node* o : others) propagateEdit(before, *n, *o, stacksMatch, modOp);
  if (done) app.commit();
  else if (live) app.rebuild();
}

}  // namespace fd
