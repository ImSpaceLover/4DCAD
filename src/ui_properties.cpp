// Properties panel (single and multi-object editing, modifiers).
#include "ui_common.hpp"

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
  if (ImGui::BeginPopupContextItem(("##key" + paths[0]).c_str())) {
    if (ImGui::MenuItem(keyed ? "Replace keyframe" : "Insert keyframe", app.keys.text(Action::InsertKey).c_str())) {
      Animation& W = app.anim;
      for (auto& p : paths) {
        if (target.rfind("light:", 0) == 0) {
          for (auto& l : app.lights) { double v; if ("light:" + l.id == target && getLightValue(l, p, v)) setKey(W, target, p, fr, v); }
        } else if (Node* n = findNode(app.root, target)) {
          if (p == "rot") setRotKey(W, target, fr, nodeRotation(*n));
          else { double v; if (getValue(*n, p, v)) setKey(W, target, p, fr, v); }
        }
      }
      app.commit();
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
    if (ImGui::Combo("Slider axis", &n.camSlider, ax, 4)) done = true;
    keyable(id, {"cam.slider"});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shown axes when looking through it (as the viewport's slider axis)");
  }
  ImGui::SeparatorText("Placement");
  float pos[4] = {(float)n.xf.pos[0], (float)n.xf.pos[1], (float)n.xf.pos[2], (float)n.xf.pos[3]};
  if (ImGui::DragFloat4(geoCam ? "Surface point" : "Position XYZW", pos, 0.02f)) for (int i = 0; i < 4; i++) n.xf.pos[i] = pos[i];
  keyable(id, {"pos.x", "pos.y", "pos.z", "pos.w"});
  track();
  if (geoCam) ImGui::TextDisabled("Move it with the gizmo or G to slide along the surface");
  auto ang = anglesFromRotation(n.xf.rot);
  float a[6];
  for (int k = 0; k < 6; k++) a[k] = (float)ang[k];
  bool rc = false;
  ImGui::TextUnformatted("Rotation (degrees per plane)");
  for (int k = 0; k < 6; k++) {
    if (geoCam && (k == 2 || k == 4 || k == 5)) continue;  // stays in the surface's tangent space
    ImGui::PushID(k);
    ImGui::SetNextItemWidth(68);
    if (ImGui::DragFloat(PLANE_NAMES[k], &a[k], 0.5f, -180.f, 180.f, "%.1f")) rc = true;
    keyable(id, {"rot"});
    track();
    ImGui::PopID();
    if (k % 3 != 2 && !(geoCam && k == 3)) ImGui::SameLine();
  }
  if (rc) {
    double d[6];
    for (int k = 0; k < 6; k++) d[k] = a[k];
    n.xf.rot = rotationFromAngles(d);
  }
}

void UI::propertiesPanel() {
  ImGuiIO& io = ImGui::GetIO();
  float top = ImGui::GetFrameHeight() + TOOLBAR_H;
  float h = (io.DisplaySize.y - top) * 0.45f;
  ImGui::SetNextWindowPos({io.DisplaySize.x - RIGHT_W, top + h});
  ImGui::SetNextWindowSize({RIGHT_W, io.DisplaySize.y - top - h});
  ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  Node* n = app.selected();
  if (!n) {
    ImGui::TextWrapped("Nothing selected. Click an object in the viewport or the scene tree.");
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
  ImGui::TextDisabled("%dD %s", dim, n->isCustom() ? "custom SDF" : n->isLeaf() ? "shape (point + modifiers)" : "group");
  ImGui::SameLine();
  if (ImGui::SmallButton("View SDF code")) { showSdf_ = true; sdfNodeId_ = n->id; }
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
    ImGui::SeparatorText("Shape");
    float c[3] = {(float)n->color[0], (float)n->color[1], (float)n->color[2]};
    if (ImGui::ColorEdit3("Color", c)) for (int i = 0; i < 3; i++) n->color[i] = c[i];
    keyable(nid, {"color.r", "color.g", "color.b"});
    track();
    mixed([](const Node& x) { return x.color; });
    if (dim < 4) {
      float th = (float)n->thickness;
      if (ImGui::DragFloat("Thickness", &th, 0.005f, 0.001f, 10.f)) n->thickness = std::max(0.001f, th);
      keyable(nid, {"thickness"});
      track();
    mixed([](const Node& x) { return x.thickness; });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extent along the unused axes%s", n->anchor ? " (here: across the world surface)" : "");
    }
    float rd = (float)n->round.value_or(0), sh = (float)n->shell.value_or(0);
    if (ImGui::DragFloat("Rounding", &rd, 0.005f, -2.f, 5.f)) n->round = rd;
    keyable(nid, {"round"});
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inflates the solid by this distance: rounds edges and corners (negative shrinks it)");
    mixed([](const Node& x) { return x.round.value_or(0); });
    if (ImGui::DragFloat("Shell", &sh, 0.005f, 0.f, 5.f)) n->shell = std::max(0.f, sh);
    keyable(nid, {"shell"});
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hollows the shape: keeps only a skin 2 x this thick around its surface\n(visible when cut open, e.g. by a subtraction)");
    mixed([](const Node& x) { return x.shell.value_or(0); });
    ImGui::SeparatorText("Material");
    float em = (float)n->mat.emissive;
    if (ImGui::SliderFloat("Emissive", &em, 0, 1)) { bool was = n->mat.emissive > 0; n->mat.emissive = em; if (was != (em > 0)) done = true; }
    keyable(nid, {"mat.emissive"});
    track();
    mixed([](const Node& x) { return x.mat.emissive; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Always lit, casts no shadow, does not light other objects");
    int tex = n->mat.texture;
    if (ImGui::Combo("Texture", &tex, TEXTURE_NAMES, TEX_COUNT)) { n->mat.texture = tex; done = true; }
    mixed([](const Node& x) { return x.mat.texture; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Procedural 4D solid texture in the object's local space (moves with it)");
    if (n->mat.texture != TEX_NONE) {
      float c2[3] = {(float)n->mat.color2[0], (float)n->mat.color2[1], (float)n->mat.color2[2]};
      if (ImGui::ColorEdit3("Texture colour", c2)) for (int i = 0; i < 3; i++) n->mat.color2[i] = c2[i];
      keyable(nid, {"mat.color2.r", "mat.color2.g", "mat.color2.b"});
      track();
    mixed([](const Node& x) { return x.mat.color2; });
      float ts = (float)n->mat.texScale, tst = (float)n->mat.texStrength;
      if (ImGui::DragFloat("Texture scale", &ts, 0.02f, 0.01f, 100.f)) n->mat.texScale = std::max(0.01f, ts);
      keyable(nid, {"mat.texScale"});
      track();
    mixed([](const Node& x) { return x.mat.texScale; });
      if (ImGui::SliderFloat("Texture strength", &tst, 0, 1)) n->mat.texStrength = tst;
      keyable(nid, {"mat.texStrength"});
      track();
    mixed([](const Node& x) { return x.mat.texStrength; });
    }
    float ro = (float)n->mat.roughness, spc = (float)n->mat.specular, tr = (float)n->mat.transparency;
    if (ImGui::SliderFloat("Roughness", &ro, 0, 1)) n->mat.roughness = ro;
    keyable(nid, {"mat.roughness"});
    track();
    mixed([](const Node& x) { return x.mat.roughness; });
    if (ImGui::SliderFloat("Specular", &spc, 0, 2)) n->mat.specular = spc;
    keyable(nid, {"mat.specular"});
    track();
    mixed([](const Node& x) { return x.mat.specular; });
    if (ImGui::SliderFloat("Transparency", &tr, 0, 1)) n->mat.transparency = tr;
    keyable(nid, {"mat.transparency"});
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("See-through surfaces (bounces per pixel: Settings > Rendering)");
    mixed([](const Node& x) { return x.mat.transparency; });
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
    if (n->isCustom()) {
      ImGui::SeparatorText("Custom SDF (GLSL)");
      if (codeEditId_ != n->id) { codeEditId_ = n->id; codeBuf_ = n->code; }
      codeBuf_.reserve(codeBuf_.size() + 4096);
      if (codeBuf_.capacity() < 16384) codeBuf_.reserve(16384);
      std::vector<char> buf(codeBuf_.begin(), codeBuf_.end());
      buf.resize(std::max<size_t>(16384, codeBuf_.size() + 4096), 0);
      if (ImGui::InputTextMultiline("##code", buf.data(), buf.size(), {-1, 200}, ImGuiInputTextFlags_AllowTabInput)) codeBuf_ = buf.data();
      if (ImGui::Button("Apply code")) {
        std::string err = app.renderer->checkCustomCode(codeBuf_);
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
    if (ImGui::DragFloat4("Position XYZW", pos, 0.02f)) for (int i = 0; i < 4; i++) n->xf.pos[i] = pos[i];
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
      bool hide = n->anchor && (k == 2 || k == 4 || k == 5);  // stay in the surface's tangent space
      if (hide) continue;
      ImGui::PushID(k);
      ImGui::SetNextItemWidth(68);
      if (ImGui::DragFloat(PLANE_NAMES[k], &a[k], 0.5f, -180.f, 180.f, "%.1f")) rotChanged = true;
      keyable(nid, {"rot"});
      track();
      ImGui::PopID();
      if (k % 3 != 2 && !(n->anchor && k == 3)) ImGui::SameLine();
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
    std::string title = MOD_NAMES[m.type];
    if (m.raises() && axes[i].valid) title += std::string(" -> ") + AXIS[axes[i].newAxis];
    ImGui::TextColored(m.raises() ? ImVec4(0.6f, 0.85f, 1, 1) : ImVec4(0.9f, 0.9f, 0.9f, 1), "%s", title.c_str());
    ImGui::SameLine(RIGHT_W - 75);
    if (i > 0 && ImGui::SmallButton("^")) moveUp = (int)i;
    ImGui::SameLine(RIGHT_W - 50);
    if (ImGui::SmallButton("x")) remove = (int)i;
    ImGui::Indent(10);
    switch (m.type) {
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
    auto add = [&](int type, const char* label, const char* tip) {
      if (ImGui::MenuItem(label)) { Modifier m; m.type = type; if (type == MOD_REVOLVE) { m.axis = -1; m.radius = 0; }
        if (type == MOD_RADIAL) { m.i = 0; m.j = 2; m.radius = 2; m.count = 6; m.angle = 60; m.startAngle = 0; } n->mods.push_back(m); done = true; modOp = {ModStackOp::Add, -1, m}; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    add(MOD_EXTRUDE, "Extrude", "Adds the next axis: sweeps the shape along it (finite, infinite or half-space)");
    add(MOD_REVOLVE, "Revolve", "Adds the next axis: spins an existing axis into it at a radius");
    add(MOD_RADIAL, "Radial repeat", "Copies the shape built so far around an axis: radius, number of copies, angle between them");
    add(MOD_MIRROR, "Mirror", "Mirrors the shape built so far across an axis");
    add(MOD_REPEAT, "Repeat", "Repeats the shape built so far along an axis");
    ImGui::EndPopup();
  }

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
  } else if (!app.world.empty() && n->id != app.world && !isRoot && dim <= 3) {
    if (ImGui::Button("Attach to world surface")) { app.attachToWorld(nid); ImGui::End(); return; }
  }
  ImGui::End();
  if ((done || live) && !others.empty())
    for (Node* o : others) propagateEdit(before, *n, *o, stacksMatch, modOp);
  if (done) app.commit();
  else if (live) app.rebuild();
}

}  // namespace fd
