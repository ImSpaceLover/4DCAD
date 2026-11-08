// Modifier / texture definitions: the Library's Modifiers and Textures sections and the definition
// editor (name, parameters, code; Apply checks the code in the interpreter).
#include <algorithm>
#include <sstream>

#include "library.hpp"
#include "ui_common.hpp"

namespace fd {

static const char* MOD_TEMPLATE =
    "// Hooks (all optional; define warp and / or shape):\n"
    "//   vec4 warp(vec4 p)            moves the point before the inner shape is evaluated\n"
    "//   float shape(float d, vec4 p)  changes the inner distance d (p = the point before warp)\n"
    "//   float bound(float r)          the bounding radius for an inner radius r\n"
    "// Parameters are variables of their names. Helpers: rot2, axisGet, axisSet, opExtrude,\n"
    "// sU / sI / sS (smooth booleans), vnoise, fbm, PI, INF.\n"
    "float shape(float d, vec4 p) {\n"
    "  return d - amount * sin(4.0 * p.x);\n"
    "}\n";
static const char* TEX_TEMPLATE =
    "// float tex(vec4 p): a mask 0..1;  vec3 tex(vec4 p): a colour;  vec4 tex(vec4 p): colour + coverage.\n"
    "// p = the texture coordinate (the slot's mapping). Parameters are variables of their names.\n"
    "// Helpers: h4 (hash), vnoise, fbm, axisGet, rot2, PI.\n"
    "float tex(vec4 p) {\n"
    "  return 0.5 + 0.5 * sin(6.2832 * frequency * p.x);\n"
    "}\n";

// whether the current scene uses a definition (deleting it would leave "unknown" entries)
static bool sceneUses(const Node& root, const std::string& id, bool texture) {
  bool used = false;
  walkc(root, [&](const Node& n, const Node*, int) {
    if (texture) { for (auto& t : n.mat.tex) if (t.def == id) used = true; }
    else for (auto& m : n.mods) if (m.type == MOD_DEF && m.def == id) used = true;
  });
  return used;
}

void UI::openDefEditor(bool texture, const std::string& id, bool duplicate) {
  defIsTex_ = texture;
  defErr_.clear();
  defOk_.clear();
  defReadOnly_ = false;
  if (texture) {
    auto d = id.empty() ? nullptr : defs().texture(id);
    if (d) defTex_ = *d;
    else {
      defTex_ = TextureDef{};
      defTex_.name = "My texture";
      defTex_.category = "Custom";
      defTex_.code = TEX_TEMPLATE;
      defTex_.params = {ParamDef{"frequency", "Frequency", "", P_FLOAT, 0.1, 20, 2}};
    }
    if (d && d->builtin && !duplicate) defReadOnly_ = true;
    if (duplicate) { defTex_.name += " copy"; defTex_.category = "Custom"; defTex_.builtin = false; }
    defOrigId_ = (d && !duplicate) ? d->id : "";
    defCode_ = defTex_.code;
  } else {
    auto d = id.empty() ? nullptr : defs().modifier(id);
    if (d) defMod_ = *d;
    else {
      defMod_ = ModifierDef{};
      defMod_.name = "My modifier";
      defMod_.category = "Custom";
      defMod_.code = MOD_TEMPLATE;
      defMod_.params = {ParamDef{"amount", "Amount", "", P_FLOAT, 0, 1, 0.1}};
      defMod_.safety = 0.8;
    }
    if (d && d->builtin && !duplicate) defReadOnly_ = true;
    if (duplicate) { defMod_.name += " copy"; defMod_.category = "Custom"; defMod_.builtin = false; defMod_.role = 0; }
    defOrigId_ = (d && !duplicate) ? d->id : "";
    defCode_ = defMod_.code;
  }
  showDefEditor_ = true;
  ImGui::SetWindowFocus("Definition editor");
}

void UI::defsLibrary() {
  std::function<void()> deferred;
  for (int pass = 0; pass < 2; pass++) {
    bool tex = pass == 1;
    if (tex && app.texMode.active) continue;  // (texture editing mode: fills are chosen in Properties)
    ImGui::PushID(tex ? "texdefs" : "moddefs");
    if (!ImGui::CollapsingHeader(tex ? "Textures###texdefs" : "Modifiers###moddefs")) { ImGui::PopID(); continue; }
    if (ImGui::SmallButton(tex ? "+ New texture (code)" : "+ New modifier")) openDefEditor(tex, "", false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write your own: parameters + code (GLSL subset), checked on Apply");
    if (tex) {
      ImGui::SameLine();
      if (ImGui::SmallButton("+ New texture (shapes)")) {
        ShapeTexture st;
        st.root = makeGroup(BoolOp::Union, {}, "Texture");
        st.root.id = "root";
        deferred = [this, st] { app.enterTextureMode(st, "", "My texture"); };
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build a texture out of shapes in the texture editing mode");
    }
    // (id, name, category, description, builtin)
    struct Item { std::string id, name, cat, desc; bool builtin; };
    std::vector<Item> items;
    if (tex) for (auto& d : defs().textures()) if (d->id.rfind("__", 0) != 0) items.push_back({d->id, d->name, d->category, d->description, d->builtin});
    else for (auto& d : defs().modifiers()) items.push_back({d->id, d->name, d->category, d->description, d->builtin});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.builtin > b.builtin; });
    std::string cat;
    for (auto& it : items) {
      std::string c = (it.builtin ? "" : "Mine: ") + (it.cat.empty() ? std::string("Custom") : it.cat);
      if (c != cat) { ImGui::TextDisabled("%s", c.c_str()); cat = c; }
      ImGui::PushID(it.id.c_str());
      ImGui::Indent(8);
      ImGui::Selectable(it.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
      std::string id = it.id;
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s%sDouble-click: %s\nRight-click: edit, duplicate, export", it.desc.c_str(), it.desc.empty() ? "" : "\n",
                          tex ? "put it on the colour of the selected objects" : "add it to the selected objects");
        if (ImGui::IsMouseDoubleClicked(0)) {
          deferred = [this, id, tex] {
            int nApplied = 0;
            for (auto& sid : app.selection) {
              Node* n = findNode(app.root, sid);
              if (!n || n->isCamera() || sid == app.root.id) continue;
              if (tex) {
                if (!n->isLeaf()) continue;
                auto d = defs().texture(id);
                n->mat.tex[CH_COLOR].def = id;
                n->mat.tex[CH_COLOR].params.clear();
                conformValues(n->mat.tex[CH_COLOR].params, d->params);
              } else n->mods.push_back(makeModifier(*defs().modifier(id)));
              nApplied++;
            }
            if (nApplied) app.commit();
            else app.toast("Select objects first", true);
          };
        }
      }
      if (ImGui::BeginPopupContextItem("dctx")) {
        if (tex && ImGui::BeginMenu("Use on the selection")) {
          for (int ch = 0; ch < CH_COUNT; ch++)
            if (ImGui::MenuItem(CHANNEL_NAMES[ch]))
              deferred = [this, id, ch] {
                auto d = defs().texture(id);
                for (auto& sid : app.selection)
                  if (Node* n = findNode(app.root, sid); n && n->isLeaf()) {
                    TexSlot& t = n->mat.tex[ch];
                    bool fresh = !t.used();
                    t.def = id;
                    t.params.clear();
                    conformValues(t.params, d->params);
                    if (fresh && ch == CH_BUMP) t.value2 = 0.02;
                  }
                app.commit();
              };
          ImGui::EndMenu();
        }
        auto td = tex ? defs().texture(id) : nullptr;
        if (td && td->source == "shapes" && ImGui::MenuItem("Edit in texture mode"))
          deferred = [this, id] {
            auto d = defs().texture(id);
            try { app.enterTextureMode(shapeTextureFromJson(d->shapes), id, d->name); } catch (...) { app.toast("Cannot read the texture's shapes", true); }
          };
        if (ImGui::MenuItem(it.builtin ? "View code" : td && td->source == "shapes" ? "Edit code" : "Edit")) deferred = [this, id, tex] { openDefEditor(tex, id, false); };
        if (ImGui::MenuItem("Duplicate to edit")) deferred = [this, id, tex] { openDefEditor(tex, id, true); };
        if (ImGui::MenuItem("Export...")) deferred = [this, id, tex] { exportDefId_ = id; exportDefTex_ = tex; fileAction(9); };
        if (!it.builtin) {
          bool used = sceneUses(app.root, id, tex);
          if (ImGui::MenuItem("Delete", nullptr, false, !used))
            deferred = [id, tex] { tex ? defs().eraseTexture(id) : defs().eraseModifier(id); saveUserDefs(); };
          if (used && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Used in this scene");
        }
        ImGui::EndPopup();
      }
      ImGui::Unindent(8);
      ImGui::PopID();
    }
    ImGui::PopID();
  }
  if (deferred) deferred();
}

// the parameter table of the editor; returns true if something changed
static void paramTable(std::vector<ParamDef>& ps, bool ro) {
  int remove = -1, up = -1;
  if (ImGui::BeginTable("params", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Label");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Default");
    ImGui::TableSetupColumn("Range / options");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableHeadersRow();
    if (ro) ImGui::BeginDisabled();
    for (int i = 0; i < (int)ps.size(); i++) {
      ParamDef& p = ps[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      char buf[128];
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      std::snprintf(buf, sizeof buf, "%s", p.name.c_str());
      if (ImGui::InputText("##name", buf, sizeof buf)) p.name = buf;
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      std::snprintf(buf, sizeof buf, "%s", p.label.c_str());
      if (ImGui::InputText("##label", buf, sizeof buf)) p.label = buf;
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      int t = p.type;
      if (ImGui::Combo("##type", &t, PARAM_TYPE_NAMES, P_TYPE_COUNT)) {
        p.type = (ParamType)t;
        if (p.type == P_BOOL) { p.min = 0; p.max = 1; p.def = std::clamp(std::round(p.def), 0.0, 1.0); }
        if (p.type == P_AXIS) { p.min = 0; p.max = 3; p.def = std::clamp(std::round(p.def), 0.0, 3.0); }
        if (p.type == P_ENUM && p.options.empty()) p.options = {"A", "B"};
        if (p.type == P_ENUM) { p.min = 0; p.max = (double)p.options.size() - 1; }
      }
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      switch (p.type) {
        case P_COLOR: {
          float c[3] = {(float)p.defColor[0], (float)p.defColor[1], (float)p.defColor[2]};
          if (ImGui::ColorEdit3("##def", c, ImGuiColorEditFlags_NoInputs)) for (int k = 0; k < 3; k++) p.defColor[k] = c[k];
          break;
        }
        case P_BOOL: { bool b = p.def > 0.5; if (ImGui::Checkbox("##def", &b)) p.def = b; break; }
        case P_AXIS: { int a = (int)p.def; const char* ax[4] = {"X", "Y", "Z", "W"}; if (ImGui::Combo("##def", &a, ax, 4)) p.def = a; break; }
        case P_ENUM: {
          int a = (int)p.def;
          std::vector<const char*> o;
          for (auto& s : p.options) o.push_back(s.c_str());
          if (ImGui::Combo("##def", &a, o.data(), (int)o.size())) p.def = a;
          break;
        }
        default: {
          float v = (float)p.def;
          if (ImGui::DragFloat("##def", &v, 0.01f)) p.def = p.type == P_INT ? std::round(v) : v;
        }
      }
      ImGui::TableNextColumn();
      if (p.type == P_FLOAT || p.type == P_INT) {
        float r[2] = {(float)p.min, (float)p.max};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat2("##range", r, 0.01f)) { p.min = r[0]; p.max = std::max(r[0], r[1]); }
      } else if (p.type == P_ENUM) {
        std::string joined;
        for (size_t k = 0; k < p.options.size(); k++) joined += (k ? ", " : "") + p.options[k];
        char ob[256];
        std::snprintf(ob, sizeof ob, "%s", joined.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##opts", ob, sizeof ob)) {
          p.options.clear();
          std::stringstream ss(ob);
          std::string item;
          while (std::getline(ss, item, ',')) {
            size_t a = item.find_first_not_of(' '), b = item.find_last_not_of(' ');
            if (a != std::string::npos) p.options.push_back(item.substr(a, b - a + 1));
          }
          p.max = std::max(0.0, (double)p.options.size() - 1);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Options separated by commas (the variable is their index: 0, 1, ...)");
      } else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      if (i > 0 && ImGui::SmallButton("^")) up = i;
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::PopID();
    }
    if (ro) ImGui::EndDisabled();
    ImGui::EndTable();
  }
  if (up > 0) std::swap(ps[up], ps[up - 1]);
  if (remove >= 0) ps.erase(ps.begin() + remove);
  if (!ro && ImGui::SmallButton("+ Parameter")) {
    ParamDef p;
    p.name = "value" + std::to_string(ps.size() + 1);
    p.max = 1;
    p.def = 0.5;
    ps.push_back(p);
  }
}

void UI::defEditorWindow() {
  ImGui::SetNextWindowSize({720, 620}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Definition editor", &showDefEditor_)) { ImGui::End(); return; }
  bool tex = defIsTex_;
  std::string& name = tex ? defTex_.name : defMod_.name;
  std::string& cat = tex ? defTex_.category : defMod_.category;
  std::string& desc = tex ? defTex_.description : defMod_.description;
  std::vector<ParamDef>& ps = tex ? defTex_.params : defMod_.params;
  ImGui::TextColored({0.6f, 0.85f, 1, 1}, "%s %s", tex ? "Texture" : "Modifier", defOrigId_.empty() ? "(new)" : ("\"" + defOrigId_ + "\"").c_str());
  if (defReadOnly_) {
    ImGui::SameLine();
    ImGui::TextDisabled("built-in: read-only");
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate to edit")) { openDefEditor(tex, defOrigId_, true); ImGui::End(); return; }
  }
  if (defReadOnly_) ImGui::BeginDisabled();
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s", name.c_str());
  if (ImGui::InputText("Name", buf, sizeof buf)) name = buf;
  std::snprintf(buf, sizeof buf, "%s", cat.c_str());
  if (ImGui::InputText("Category", buf, sizeof buf)) cat = buf;
  std::snprintf(buf, sizeof buf, "%s", desc.c_str());
  if (ImGui::InputText("Description", buf, sizeof buf)) desc = buf;
  if (!tex) {
    float sf = (float)defMod_.safety;
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("Safety", &sf, 0.05f, 1.f, "%.2f")) defMod_.safety = sf;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Distance scale for warps that stretch space (twists, bends): lower it if the shape shows holes or noise.\n1 = exact");
    ImGui::SameLine();
    ImGui::Checkbox("Adds a dimension", &defMod_.raisesAxis);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Like Extrude / Revolve: the code gets the new axis as newAxis (and srcAxis)");
  }
  if (defReadOnly_) ImGui::EndDisabled();
  ImGui::SeparatorText("Parameters");
  paramTable(ps, defReadOnly_);
  ImGui::SeparatorText("Code");
  std::vector<char> cb(defCode_.begin(), defCode_.end());
  cb.resize(std::max<size_t>(32768, defCode_.size() + 8192), 0);
  ImGuiInputTextFlags fl = ImGuiInputTextFlags_AllowTabInput | (defReadOnly_ ? ImGuiInputTextFlags_ReadOnly : 0);
  if (ImGui::InputTextMultiline("##defcode", cb.data(), cb.size(), {-1, -70}, fl)) defCode_ = cb.data();
  if (!defReadOnly_) {
    if (ImGui::Button("Apply")) {
      // a working copy with the code; checked in the interpreter (the same subset is valid GLSL)
      std::string err;
      if (name.empty()) err = "give it a name";
      if (tex) {
        if (defTex_.source == "shapes" && defCode_ != defTex_.code) { defTex_.source = "code"; defTex_.shapes.clear(); }  // (hand-edited)
        defTex_.code = defCode_;
        if (err.empty()) err = checkTextureDef(defTex_);
      }
      else { defMod_.code = defCode_; if (err.empty()) err = checkModifierDef(defMod_); }
      defErr_ = err;
      defOk_.clear();
      if (err.empty()) {
        std::string id = defOrigId_.empty() ? newDefId(name, tex) : defOrigId_;
        if (tex) { defTex_.id = id; defs().setTexture(defTex_); }
        else { defMod_.id = id; defs().setModifier(defMod_); }
        defOrigId_ = id;
        // objects using it take the new parameter list (values kept by position)
        walk(app.root, [&](Node& n, Node*, int) {
          if (tex) { for (auto& t : n.mat.tex) if (t.def == id) conformValues(t.params, ps); }
          else for (auto& m : n.mods) if (m.type == MOD_DEF && m.def == id) conformParams(m, defMod_);
        });
        saveUserDefs();
        app.commit();
        defOk_ = "Applied: in the library (" + std::string(tex ? "Textures" : "Modifiers") + ") and in use everywhere";
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) openDefEditor(tex, defOrigId_, false);
    ImGui::SameLine();
    ImGui::TextDisabled(tex ? "Return float (mask), vec3 (colour) or vec4 (colour + coverage) from tex(vec4 p)"
                            : "Define warp and / or shape (see the comments in the template)");
  }
  if (tex && defTex_.source == "shapes")
    ImGui::TextColored({1, 0.8f, 0.4f, 1}, "Made of shapes: edit it in the texture mode (Library right-click). Applying code here makes it a code texture.");
  if (!defErr_.empty()) ImGui::TextColored({1, 0.55f, 0.4f, 1}, "Error: %s", defErr_.c_str());
  else if (!defOk_.empty()) ImGui::TextColored({0.5f, 0.9f, 0.5f, 1}, "%s", defOk_.c_str());
  ImGui::End();
}

// texture editing mode: the texture's settings (colours, coverage, tiling, exposed parameters)
void UI::textureSettingsWindow() {
  auto& T = app.texMode;
  ImGui::SetNextWindowSize({330, 430}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 700, 120}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Texture settings")) { ImGui::End(); return; }
  ShapeTexture& st = T.settings;
  ImGui::TextWrapped("Build the texture out of shapes (Full 4D). New shapes start white, so masks run 0..1. "
                     "Right-click a value in Properties: Expose as texture parameter.");
  ImGui::SeparatorText("Colours");
  int oc = st.objectColors ? 0 : 1;
  if (ImGui::RadioButton("Object colours", &oc, 0)) st.objectColors = true;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("A colour texture: each part's colour (smooth unions blend), plus coverage");
  ImGui::SameLine();
  if (ImGui::RadioButton("Single colour (mask)", &oc, 1)) st.objectColors = false;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("A mask: coverage x the colours' brightness; the material slot gives the colours");
  if (st.objectColors) ImGui::Checkbox("Merge equal colours", &st.mergeColors);
  if (st.objectColors && ImGui::IsItemHovered()) ImGui::SetTooltip("Parts of the same colour share one colour parameter");
  ImGui::SeparatorText("Coverage");
  ImGui::SetNextItemWidth(150);
  ImGui::Combo("Mode", &st.coverage, COVERAGE_NAMES, COV_COUNT);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inside: the shapes. Outline: their surfaces. Distance bands: rings around them. Falloff: a glow fading with distance");
  auto F = [](const char* l, double& v, float lo, float hi) { float f = (float)v; ImGui::SetNextItemWidth(150); if (ImGui::DragFloat(l, &f, (hi - lo) / 400, lo, hi, "%.3f")) v = f; };
  if (st.coverage == COV_INSIDE || st.coverage == COV_BANDS) F("Softness", st.softness, 0.0005f, 1);
  if (st.coverage == COV_OUTLINE) F("Width", st.width, 0.0005f, 2);
  if (st.coverage == COV_BANDS) F("Spacing", st.spacing, 0.01f, 5);
  if (st.coverage == COV_FALLOFF) F("Range", st.range, 0.01f, 20);
  ImGui::SeparatorText("Tiling");
  ImGui::Checkbox("Repeat in cells", &st.tile);
  if (st.tile) {
    float c[4] = {(float)st.cell[0], (float)st.cell[1], (float)st.cell[2], (float)st.cell[3]};
    ImGui::SetNextItemWidth(220);
    if (ImGui::DragFloat4("Cell size", c, 0.02f, 0.01f, 100.f, "%.2f")) for (int k = 0; k < 4; k++) st.cell[k] = std::max(0.01f, c[k]);
  }
  ImGui::TextDisabled("(these become the texture's parameters: changeable in material slots)");
  ImGui::SeparatorText("Exposed parameters");
  if (st.exposed.empty()) ImGui::TextDisabled("none (right-click a value in Properties)");
  int remove = -1;
  for (int i = 0; i < (int)st.exposed.size(); i++) {
    auto& e = st.exposed[i];
    ImGui::PushID(i);
    char nb[96];
    std::snprintf(nb, sizeof nb, "%s", e.name.c_str());
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##n", nb, sizeof nb)) e.name = nb;
    ImGui::SameLine();
    if (e.path != "color") {
      float r[2] = {(float)e.min, (float)e.max};
      ImGui::SetNextItemWidth(110);
      if (ImGui::DragFloat2("##r", r, 0.01f)) { e.min = r[0]; e.max = std::max(r[0], r[1]); }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slider range");
      ImGui::SameLine();
    }
    if (ImGui::SmallButton("x")) remove = i;
    Node* n = findNode(app.root, e.node);
    ImGui::TextDisabled("   %s: %s", n ? n->name.c_str() : "(deleted)", e.path.c_str());
    ImGui::PopID();
  }
  if (remove >= 0) st.exposed.erase(st.exposed.begin() + remove);
  if (app.sourceAvailable()) {
    ImGui::Separator();
    if (ImGui::Button("Update from source")) app.updateTextureFromSource();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the object again from the scene (replaces the composition)");
  }
  ImGui::End();
}

}  // namespace fd
