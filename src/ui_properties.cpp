// Properties panel (single and multi-object editing, modifiers).
#include "ui_common.hpp"

namespace fd {

// ============================================================ properties
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
    ImGui::TextDisabled("Changes apply to all (position / rotation / scale relatively)");
  }

  char nameBuf[256];
  std::snprintf(nameBuf, sizeof nameBuf, "%s", n->name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof nameBuf)) n->name = nameBuf;
  track();
  int dim = nodeDim(*n, app.reg);
  ImGui::TextDisabled("%dD %s", dim, n->isCustom() ? "custom SDF" : n->isLeaf() ? "shape (point + modifiers)" : "group");
  ImGui::SameLine();
  if (ImGui::SmallButton("View SDF code")) { showSdf_ = true; sdfNodeId_ = n->id; }

  if (n->isLeaf()) {
    ImGui::SeparatorText("Shape");
    float c[3] = {(float)n->color[0], (float)n->color[1], (float)n->color[2]};
    if (ImGui::ColorEdit3("Color", c)) for (int i = 0; i < 3; i++) n->color[i] = c[i];
    track();
    mixed([](const Node& x) { return x.color; });
    if (dim < 4) {
      float th = (float)n->thickness;
      if (ImGui::DragFloat("Thickness", &th, 0.005f, 0.001f, 10.f)) n->thickness = std::max(0.001f, th);
      track();
    mixed([](const Node& x) { return x.thickness; });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extent along the unused axes%s", n->anchor ? " (here: across the world surface)" : "");
    }
    float rd = (float)n->round.value_or(0), sh = (float)n->shell.value_or(0);
    if (ImGui::DragFloat("Rounding", &rd, 0.005f, -2.f, 5.f)) n->round = rd;
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inflates the solid by this distance: rounds edges and corners (negative shrinks it)");
    mixed([](const Node& x) { return x.round.value_or(0); });
    if (ImGui::DragFloat("Shell", &sh, 0.005f, 0.f, 5.f)) n->shell = std::max(0.f, sh);
    track();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hollows the shape: keeps only a skin 2 x this thick around its surface\n(visible when cut open, e.g. by a subtraction)");
    mixed([](const Node& x) { return x.shell.value_or(0); });
    ImGui::SeparatorText("Material");
    float em = (float)n->mat.emissive;
    if (ImGui::SliderFloat("Emissive", &em, 0, 1)) { bool was = n->mat.emissive > 0; n->mat.emissive = em; if (was != (em > 0)) done = true; }
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
      track();
    mixed([](const Node& x) { return x.mat.color2; });
      float ts = (float)n->mat.texScale, tst = (float)n->mat.texStrength;
      if (ImGui::DragFloat("Texture scale", &ts, 0.02f, 0.01f, 100.f)) n->mat.texScale = std::max(0.01f, ts);
      track();
    mixed([](const Node& x) { return x.mat.texScale; });
      if (ImGui::SliderFloat("Texture strength", &tst, 0, 1)) n->mat.texStrength = tst;
      track();
    mixed([](const Node& x) { return x.mat.texStrength; });
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
    mixed([](const Node& x) { return x.op; });
    if (isSmooth(n->op)) { float k = (float)n->k; if (ImGui::DragFloat("Smoothness", &k, 0.01f, 0.001f, 10.f)) n->k = std::max(0.001f, k); track(); }
    if (n->op == BoolOp::Subtraction || n->op == BoolOp::SmoothSubtraction) ImGui::TextDisabled("First child minus the following children.");
    if (!isRoot && !n->children.empty()) {
      if (ImGui::Button("Recentre origin on first object")) { app.recentreGroup(nid); ImGui::End(); return; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the group's origin (centre of rotation and scaling) to its first object.\nNothing moves in the world.");
    }
  }

  if (!isRoot) {
    ImGui::SeparatorText(n->anchor ? "Transform (on the world surface)" : "Transform");
    float pos[4] = {(float)n->xf.pos[0], (float)n->xf.pos[1], (float)n->xf.pos[2], (float)n->xf.pos[3]};
    if (ImGui::DragFloat4("Position XYZW", pos, 0.02f)) for (int i = 0; i < 4; i++) n->xf.pos[i] = pos[i];
    track();
    mixed([](const Node& x) { return x.xf.pos; });
    if (n->anchor) ImGui::TextDisabled("Anchor point; drag the gizmo or press G to slide along the surface.");
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
        track();
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragInt("Copies +-", &m.count, 0.1f, 0, 100, m.count == 0 ? "infinite" : "%d")) m.count = std::max(0, m.count);
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
        track();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance of the copies from the axis");
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragInt("Copies", &m.count, 0.1f, 1, 360)) m.count = std::max(1, m.count);
        track();
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Angle step", &an, 0.5f, 0.1f, 360.f, "%.1f deg")) m.angle = std::clamp(an, 0.1f, 360.f);
        track();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Angle between neighbouring copies (copies x step may be less than 360)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fill 360")) { m.angle = 360.0 / std::max(1, m.count); done = true; }
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Start angle", &st, 0.5f, -360.f, 360.f, "%.1f deg")) m.startAngle = st;
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
    ImGui::TextWrapped("This object is the world: its 3D boundary is the space of the Geodesic view.");
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
