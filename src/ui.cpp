#include "ui.hpp"

#include <cstring>
#include <filesystem>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "examples.hpp"
#include "filedialog.hpp"
#include "imgui.h"

namespace fd {

static const char* DIM_LABEL[5] = {"0D", "1D", "2D", "3D", "4D"};
static const ImVec4 DIM_COL[5] = {{0.4f, 0.4f, 0.4f, 1}, {0.4f, 0.4f, 0.4f, 1}, {0.25f, 0.35f, 0.5f, 1}, {0.2f, 0.45f, 0.32f, 1}, {0.45f, 0.27f, 0.62f, 1}};
static const char* OP_ICON[6] = {"U", "I", "-", "~U", "~I", "~-"};
static const float LEFT_W = 250, RIGHT_W = 340, TOOLBAR_H = 38;

struct LibPayload { bool object; char id[96]; };

static ImTextureID tex(GLuint t) { return (ImTextureID)(intptr_t)t; }

UI::UI(App& a, GLFWwindow* w) : app(a), win_(w) {
  previewCam_.target = {0, 0, 0, 0};
  previewCam_.distance = 6;
}

// ============================================================ frame
void UI::frame(double dt) {
  ImGuiIO& io = ImGui::GetIO();
  menuBar();
  toolbar();
  libraryPanel();
  treePanel();
  propertiesPanel();
  viewport(dt);
  if (showBuilder_) builderWindow();
  if (showQuality_) qualityWindow();
  if (showHelp_) helpWindow();
  pathPrompt();

  // toast
  if (app.now < app.toastUntil && !app.toastMsg.empty()) {
    ImVec2 ds = io.DisplaySize;
    ImGui::SetNextWindowPos({ds.x * 0.5f, ds.y - 40}, ImGuiCond_Always, {0.5f, 1});
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, app.toastError ? ImVec4(0.45f, 0.12f, 0.12f, 1) : ImVec4(0.2f, 0.23f, 0.28f, 1));
    ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(app.toastMsg.c_str());
    ImGui::End();
    ImGui::PopStyleColor();
  }
}

void UI::onKey(int key, int action, int mods) {
  if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput && !app.mouseCaptured) return;
  if (showBuilder_ && !app.mouseCaptured && key != GLFW_KEY_ESCAPE) return;
  bool ctrl = (mods & GLFW_MOD_CONTROL) || (mods & GLFW_MOD_SUPER);
  bool shift = mods & GLFW_MOD_SHIFT;
  if (action == GLFW_REPEAT && !(ctrl && (key == GLFW_KEY_Z || key == GLFW_KEY_Y))) return;
  if (ctrl && key == GLFW_KEY_S) { fileAction(2); return; }
  if (ctrl && key == GLFW_KEY_O) { fileAction(1); return; }
  if (ctrl && key == GLFW_KEY_N) { app.newScene(); return; }
  app.handleKey(key, shift, ctrl);
}

// ============================================================ file actions
void UI::fileAction(int action) {
  const char* titles[] = {"", "Open scene", "Save scene", "Import library item", "Export library", "Export object", "Export primitive"};
  std::string def;
  if (action == 2) def = app.currentFile.empty() ? "scene.4dscene.json" : app.currentFile;
  if (action == 4) def = "4dcad-library.json";
  if (action == 5) def = (app.selected() ? app.selected()->name : std::string("object")) + ".4dobj.json";
  if (action == 6) def = "primitive.4dprim.json";
  if (!nativeDialogsAvailable()) {
    pendingAction_ = action;
    std::snprintf(pathBuf_, sizeof pathBuf_, "%s", def.c_str());
    ImGui::OpenPopup("File path");
    return;
  }
  std::string path = (action == 1 || action == 3) ? openFileDialog(titles[action]) : saveFileDialog(titles[action], def);
  if (!path.empty()) doFileAction(action, path);
}

void UI::doFileAction(int action, const std::string& path) {
  std::string err;
  switch (action) {
    case 1: if (app.loadSceneFile(path, err)) app.toast("Opened " + path); else app.toast(err, true); break;
    case 2: if (app.saveSceneFile(path, err)) app.toast("Saved " + path); else app.toast(err, true); break;
    case 3: {
      std::string text;
      if (!readFile(path, text)) { app.toast("Cannot read " + path, true); break; }
      try { app.toast("Imported " + app.lib.importText(text)); app.rebuild(); } catch (const std::exception& e) { app.toast(e.what(), true); }
      break;
    }
    case 4: writeFile(path, app.lib.exportText()) ? app.toast("Exported library") : app.toast("Cannot write " + path, true); break;
    case 5: {
      Node* n = app.selected();
      if (!n) break;
      writeFile(path, libraryObjectToJson(makeLibraryObject(*n, app.reg, n->name))) ? app.toast("Exported object") : app.toast("Cannot write", true);
      break;
    }
    case 6: {
      if (auto* p = app.reg.get(exportPrimId_)) writeFile(path, primitiveToJson(*p, true)) ? app.toast("Exported primitive") : app.toast("Cannot write", true);
      break;
    }
  }
}

void UI::pathPrompt() {
  if (ImGui::BeginPopupModal("File path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Enter a file path:");
    ImGui::SetNextItemWidth(500);
    bool enter = ImGui::InputText("##path", pathBuf_, sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("OK") || enter) { doFileAction(pendingAction_, pathBuf_); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

// ============================================================ menu & toolbar
void UI::menuBar() {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New", "Ctrl+N")) app.newScene();
    if (ImGui::MenuItem("Open...", "Ctrl+O")) fileAction(1);
    if (ImGui::MenuItem("Save as...", "Ctrl+S")) fileAction(2);
    if (ImGui::BeginMenu("Examples")) {
      for (auto& ex : EXAMPLES) if (ImGui::MenuItem(ex.name)) { app.loadScene(ex.make()); app.currentFile.clear(); }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Import primitive / object / library...")) fileAction(3);
    if (ImGui::MenuItem("Export library...")) fileAction(4);
    if (ImGui::MenuItem("Export selected object...", nullptr, false, app.selected() != nullptr)) fileAction(5);
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) quitRequested = true;
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
    if (ImGui::MenuItem("New primitive...")) openBuilder(nullptr, false);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Edit mode", "Tab", app.mode == Mode::Edit)) app.setMode(Mode::Edit);
    if (ImGui::MenuItem("Explore mode", "Tab", app.mode == Mode::Explore)) app.setMode(Mode::Explore);
    ImGui::Separator();
    if (ImGui::MenuItem("Full 4D", "V", !app.geo)) app.setGeo(false);
    if (ImGui::MenuItem("Geodesic 3D", "V", app.geo)) app.setGeo(true);
    ImGui::Separator();
    ImGui::MenuItem("Outside thumbnail (Geodesic)", nullptr, &app.showThumbnail);
    ImGui::MenuItem("Show player sphere (Edit)", nullptr, &app.showPlayerMarker);
    if (ImGui::MenuItem("Reset 4D orientation", "Shift+W")) app.editCam.reset4();
    if (ImGui::MenuItem("Front / Right / Top / Along W", "1 3 7 9")) app.standardView(0);
    ImGui::Separator();
    ImGui::MenuItem("Rendering quality...", nullptr, &showQuality_);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Help")) showHelp_ = !showHelp_;
  ImGui::EndMainMenuBar();
}

static bool toggleButton(const char* label, bool active, const char* tip = nullptr) {
  if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.5f, 0.15f, 1));
  bool r = ImGui::Button(label);
  if (active) ImGui::PopStyleColor();
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  return r;
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
    if (toggleButton("Move", app.tool == Tool::Move, "W: drag an object to move it in the visible slice (Shift: along depth)")) app.tool = Tool::Move;
    ImGui::SameLine(0, 2);
    if (toggleButton("Rotate", app.tool == Tool::Rotate, "E: drag an object to rotate it within the visible slice (Shift: roll)")) app.tool = Tool::Rotate;
  }
  ImGui::SameLine(0, 16);
  if (ImGui::Button("Undo")) app.undo();
  ImGui::SameLine(0, 2);
  if (ImGui::Button("Redo")) app.redo();
  ImGui::SameLine(0, 16);
  if (ImGui::Button("Quality")) showQuality_ = !showQuality_;
  ImGui::SameLine();
  ImGui::TextDisabled("%.0f fps", app.fps);
  ImGui::End();
}

// ============================================================ library
void UI::libraryPanel() {
  ImGuiIO& io = ImGui::GetIO();
  float top = ImGui::GetFrameHeight() + TOOLBAR_H;
  ImGui::SetNextWindowPos({0, top});
  ImGui::SetNextWindowSize({LEFT_W, io.DisplaySize.y - top});
  ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  if (ImGui::Button("+ Primitive")) openBuilder(nullptr, false);
  ImGui::SameLine();
  if (ImGui::Button("Import")) fileAction(3);
  ImGui::SameLine();
  if (ImGui::Button("Export")) fileAction(4);
  ImGui::TextDisabled(app.geo ? "Geodesic: drag 3D items onto the surface" : "Drag items into the viewport");

  auto item = [&](const LibraryItem& it, const std::string& name, const std::string& desc, int dim, const std::function<void()>& ctx) {
    std::string why;
    bool ok = app.canPlace(it, &why) || app.mode != Mode::Edit;
    ImGui::PushID(((it.object ? "o" : "p") + it.id).c_str());
    ImGui::PushStyleColor(ImGuiCol_Button, DIM_COL[dim]);
    ImGui::SmallButton(DIM_LABEL[dim]);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (!ok) ImGui::BeginDisabled();
    ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
    if (ok && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) app.placeItem(it, std::nullopt);
    if (ok && ImGui::BeginDragDropSource()) {
      LibPayload p{it.object, {}};
      std::snprintf(p.id, sizeof p.id, "%s", it.id.c_str());
      ImGui::SetDragDropPayload("FD_LIB", &p, sizeof p);
      ImGui::Text("%s", name.c_str());
      ImGui::EndDragDropSource();
    }
    if (!ok) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s%s%s", desc.c_str(), ok ? "" : "\n", ok ? "" : why.c_str());
    if (ImGui::BeginPopupContextItem("ctx")) { ctx(); ImGui::EndPopup(); }
    ImGui::PopID();
  };

  for (int d = 0; d <= 4; d++) {
    std::vector<const PrimitiveDef*> list;
    for (auto* p : app.reg.all()) if (primitiveDim(*p) == d) list.push_back(p);
    if (list.empty()) continue;
    if (!ImGui::CollapsingHeader((std::string(DIM_LABEL[d]) + " primitives").c_str(), d >= 3 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
    for (auto* p : list) {
      std::string id = p->id;
      bool builtin = p->builtin;
      item({false, id}, p->name, p->description, d, [&, id, builtin] {
        const PrimitiveDef* q = app.reg.get(id);
        if (!q) return;
        if (ImGui::MenuItem(builtin ? "Copy into builder" : "Edit")) openBuilder(q, !builtin);
        if (!builtin && ImGui::MenuItem("Export...")) { exportPrimId_ = id; fileAction(6); }
        if (!builtin && ImGui::MenuItem("Delete")) { app.lib.deletePrimitive(id); app.reg.erase(id); app.rebuild(); }
      });
    }
  }
  if (!app.lib.objects.empty() && ImGui::CollapsingHeader("Saved objects", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (size_t i = 0; i < app.lib.objects.size(); i++) {
      auto& o = app.lib.objects[i];
      std::string id = o.id;
      item({true, id}, o.name, "Saved object", nodeDim(o.node, app.reg), [&, id] {
        if (ImGui::MenuItem("Delete")) app.lib.deleteObject(id);
      });
    }
  }
  ImGui::End();
}

// ============================================================ scene tree
void UI::treePanel() {
  ImGuiIO& io = ImGui::GetIO();
  float top = ImGui::GetFrameHeight() + TOOLBAR_H;
  float h = (io.DisplaySize.y - top) * 0.45f;
  ImGui::SetNextWindowPos({io.DisplaySize.x - RIGHT_W, top});
  ImGui::SetNextWindowSize({RIGHT_W, h});
  ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  static int groupOp = 0;
  ImGui::SetNextItemWidth(130);
  ImGui::Combo("##op", &groupOp, BOOL_OP_LABELS, 6);
  ImGui::SameLine();
  if (ImGui::Button("Group")) app.groupSelection((BoolOp)groupOp);
  ImGui::SameLine();
  if (ImGui::Button("Dup")) app.duplicateSelection();
  ImGui::SameLine();
  if (ImGui::Button("Del")) app.deleteSelection();
  if (ImGui::Button("Save as library object") && app.selected()) {
    Node* n = app.selected();
    app.lib.saveObject(makeLibraryObject(*n, app.reg, n->name));
    app.toast("Saved \"" + n->name + "\" to the library");
  }
  ImGui::SameLine();
  if (ImGui::Button("Export object") && app.selected()) fileAction(5);
  ImGui::Separator();
  ImGui::BeginChild("tree");
  // copy needed: tree modifications are deferred until after drawing
  treeNode(app.root, 0);
  ImGui::EndChild();
  ImGui::End();
}

void UI::treeNode(Node& n, int depth) {
  bool isRoot = n.id == app.root.id;
  bool sel = std::find(app.selection.begin(), app.selection.end(), n.id) != app.selection.end();
  ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
  if (n.isLeaf() || n.children.empty()) fl |= ImGuiTreeNodeFlags_Leaf;
  if (sel) fl |= ImGuiTreeNodeFlags_Selected;
  std::string label;
  if (n.isLeaf()) { auto* p = app.reg.get(n.prim); label = std::string("[") + DIM_LABEL[p ? primitiveDim(*p) : 0] + "] "; }
  else label = std::string("(") + OP_ICON[(int)n.op] + ") ";
  label += n.name;
  if (n.id == app.world) label += "  [world]";
  if (n.anchor) label += "  [on surface]";
  if (!n.mods.empty()) label += "  *";
  if (n.hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1));
  bool open = ImGui::TreeNodeEx(n.id.c_str(), fl, "%s", label.c_str());
  if (n.hidden) ImGui::PopStyleColor();
  std::string id = n.id;
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    if (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl) {
      auto it = std::find(app.selection.begin(), app.selection.end(), id);
      if (it != app.selection.end()) app.selection.erase(it); else app.selection.push_back(id);
    } else app.selection = {id};
  }
  // deferred actions (the tree must not change while iterating it)
  static std::function<void()> deferred;
  if (!isRoot && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("FD_NODE", id.c_str(), id.size() + 1);
    ImGui::Text("%s", n.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    float t = (ImGui::GetMousePos().y - mn.y) / std::max(1.f, mx.y - mn.y);
    App::Drop where = isRoot ? App::Drop::Into : t < 0.25f ? App::Drop::Before : t > 0.75f ? App::Drop::After : n.isLeaf() ? App::Drop::Wrap : App::Drop::Into;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c = IM_COL32(80, 150, 240, 255);
    if (where == App::Drop::Before) dl->AddLine(mn, {mx.x, mn.y}, c, 2);
    else if (where == App::Drop::After) dl->AddLine({mn.x, mx.y}, mx, c, 2);
    else dl->AddRect(mn, mx, c);
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FD_NODE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      std::string src((const char*)p->Data);
      deferred = [this, src, id, where] { app.reparent(src, id, where); };
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FD_LIB", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      LibPayload lp = *(const LibPayload*)p->Data;
      deferred = [this, lp, id, where] {
        size_t before = app.root.children.size();
        app.placeItem({lp.object, lp.id}, std::nullopt);
        if (app.root.children.size() > before) app.reparent(app.root.children.back().id, id, where);
      };
    }
    ImGui::EndDragDropTarget();
  }
  if (ImGui::BeginPopupContextItem()) {
    if (!sel) app.selection = {id};
    if (ImGui::MenuItem(n.hidden ? "Show" : "Hide", nullptr, false, !isRoot)) deferred = [this, id] { if (Node* m = findNode(app.root, id)) { m->hidden = !m->hidden; app.commit(); } };
    if (ImGui::MenuItem("Duplicate", nullptr, false, !isRoot)) deferred = [this] { app.duplicateSelection(); };
    if (ImGui::MenuItem("Delete", nullptr, false, !isRoot)) deferred = [this] { app.deleteSelection(); };
    if (!n.isLeaf() && ImGui::MenuItem("Ungroup", nullptr, false, !isRoot)) deferred = [this, id] { app.ungroup(id); };
    if (ImGui::BeginMenu("Group as", !isRoot)) {
      for (int i = 0; i < 6; i++) if (ImGui::MenuItem(BOOL_OP_LABELS[i])) deferred = [this, i] { app.groupSelection((BoolOp)i); };
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  if (open) {
    if (!n.isLeaf()) for (size_t i = 0; i < n.children.size(); i++) treeNode(n.children[i], depth + 1);
    ImGui::TreePop();
  }
  if (isRoot && deferred) { auto f = deferred; deferred = nullptr; f(); }
}

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

  char nameBuf[256];
  std::snprintf(nameBuf, sizeof nameBuf, "%s", n->name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof nameBuf)) n->name = nameBuf;
  track();

  if (n->isLeaf()) {
    ImGui::SeparatorText("Primitive");
    auto* def = app.reg.get(n->prim);
    if (ImGui::BeginCombo("Type", def ? def->name.c_str() : n->prim.c_str())) {
      for (auto* p : app.reg.all())
        if (ImGui::Selectable((p->name + " (" + DIM_LABEL[primitiveDim(*p)] + ")").c_str(), p->id == n->prim)) { n->prim = p->id; done = true; }
      ImGui::EndCombo();
    }
    float c[3] = {(float)n->color[0], (float)n->color[1], (float)n->color[2]};
    if (ImGui::ColorEdit3("Color", c)) for (int i = 0; i < 3; i++) n->color[i] = c[i];
    track();
    int dim = def ? primitiveDim(*def) : 4;
    if (dim < 4) {
      float th = (float)n->thickness;
      if (ImGui::DragFloat("Thickness", &th, 0.005f, 0.001f, 10.f)) n->thickness = std::max(0.001f, th);
      track();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extent along the unused axes%s", n->anchor ? " (here: across the world surface)" : "");
    }
    float rd = (float)n->round.value_or(def ? def->round : 0), sh = (float)n->shell.value_or(def ? def->shell : 0);
    if (ImGui::DragFloat("Rounding", &rd, 0.005f, -2.f, 5.f)) n->round = rd;
    track();
    if (ImGui::DragFloat("Shell", &sh, 0.005f, 0.f, 5.f)) n->shell = std::max(0.f, sh);
    track();
  } else {
    ImGui::SeparatorText("Boolean");
    int op = (int)n->op;
    if (ImGui::Combo("Operator", &op, BOOL_OP_LABELS, 6)) { n->op = (BoolOp)op; done = true; }
    if (isSmooth(n->op)) { float k = (float)n->k; if (ImGui::DragFloat("Smoothness", &k, 0.01f, 0.001f, 10.f)) n->k = std::max(0.001f, k); track(); }
    if (n->op == BoolOp::Subtraction || n->op == BoolOp::SmoothSubtraction) ImGui::TextDisabled("First child minus the following children.");
  }

  if (!isRoot) {
    ImGui::SeparatorText(n->anchor ? "Transform (on the world surface)" : "Transform");
    float pos[4] = {(float)n->xf.pos[0], (float)n->xf.pos[1], (float)n->xf.pos[2], (float)n->xf.pos[3]};
    if (ImGui::DragFloat4("Position XYZW", pos, 0.02f)) for (int i = 0; i < 4; i++) n->xf.pos[i] = pos[i];
    track();
    if (n->anchor) ImGui::TextDisabled("Anchor point; use Move (W) or G to slide along the surface.");
    float sc = (float)n->xf.scale;
    if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.01f, 100.f)) n->xf.scale = std::max(0.01f, sc);
    track();
    ImGui::TextUnformatted("Rotate (drag) in plane:");
    for (int k = 0; k < 6; k++) {
      if (n->anchor && (k == 2 || k == 4 || k == 5)) continue;  // stay within the surface's 3D tangent space
      float delta = 0;
      ImGui::PushID(k);
      ImGui::SetNextItemWidth(70);
      if (ImGui::DragFloat(PLANE_NAMES[k], &delta, 0.5f, 0, 0, "drag")) {
        n->xf.rot = orthonormalize(mul(n->xf.rot, planeRotation(PLANES[k][0], PLANES[k][1], delta * PI / 180)));
        live = true;
      }
      if (ImGui::IsItemDeactivated()) done = true;
      ImGui::PopID();
      if (k % 3 != 2) ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::Button("Reset rotation")) { n->xf.rot = identity(); done = true; }
  }

  ImGui::SeparatorText("Modifiers");
  int removeMod = -1;
  for (size_t i = 0; i < n->mods.size(); i++) {
    auto& m = n->mods[i];
    ImGui::PushID((int)i);
    ImGui::TextUnformatted(m.type == 1 ? "Mirror" : "Repeat");
    ImGui::SameLine(70);
    ImGui::SetNextItemWidth(50);
    const char* axes[] = {"X", "Y", "Z", "W"};
    if (ImGui::Combo("##ax", &m.axis, axes, 4)) done = true;
    if (m.type == 2) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70);
      float sp = (float)m.spacing;
      if (ImGui::DragFloat("##sp", &sp, 0.05f, 0.01f, 100.f, "d %.2f")) m.spacing = std::max(0.01f, sp);
      if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(60);
      if (ImGui::DragInt("##n", &m.count, 0.1f, 0, 100, m.count == 0 ? "inf" : "+-%d")) m.count = std::max(0, m.count);
      if (ImGui::IsItemDeactivatedAfterEdit()) done = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) removeMod = (int)i;
    ImGui::PopID();
  }
  if (removeMod >= 0) { n->mods.erase(n->mods.begin() + removeMod); done = true; }
  if (ImGui::Button("+ Mirror")) { n->mods.push_back({1, 0, 0, 0}); done = true; }
  ImGui::SameLine();
  if (ImGui::Button("+ Repeat")) { n->mods.push_back({2, 0, 3, 2}); done = true; }

  ImGui::SeparatorText("World surface");
  int dim = nodeDim(*n, app.reg);
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
  if (done) app.commit();
  else if (live) app.rebuild();
}

// ============================================================ viewport
void UI::viewport(double dt) {
  ImGuiIO& io = ImGui::GetIO();
  float top = ImGui::GetFrameHeight() + TOOLBAR_H;
  ImGui::SetNextWindowPos({LEFT_W, top});
  ImGui::SetNextWindowSize({io.DisplaySize.x - LEFT_W - RIGHT_W, io.DisplaySize.y - top});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::Begin("##viewport", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 sz = ImGui::GetContentRegionAvail();
  app.vpX = p0.x; app.vpY = p0.y; app.vpW = std::max(1.f, sz.x); app.vpH = std::max(1.f, sz.y);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##vp", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  vpHovered_ = ImGui::IsItemHovered() || app.mouseCaptured;
  // drop target for library items
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FD_LIB")) {
      LibPayload lp = *(const LibPayload*)p->Data;
      app.placeItem({lp.object, lp.id}, std::make_pair((double)io.MousePos.x, (double)io.MousePos.y));
    }
    ImGui::EndDragDropTarget();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (app.mainRT.tex) dl->AddImage(tex(app.mainRT.tex), p0, {p0.x + sz.x, p0.y + sz.y}, {0, 1}, {1, 0});

  // W slider (Edit, Full 4D): moves the camera's visible slice along its 4th axis
  bool sliderHovered = false;
  if (app.mode == Mode::Edit && !app.geo) {
    ImGui::SetCursorScreenPos({p0.x + 12, p0.y + 90});
    float w = (float)app.sliceW();
    float range = std::max(5.f, std::ceil(std::abs(w) / 5.f) * 5.f + 5.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.13f, 0.75f));
    if (ImGui::VSliderFloat("##W", {26, 220}, &w, -range, range, "")) app.setSliceW(w);
    ImGui::PopStyleColor();
    sliderHovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slice W = %.2f\nMoves the visible 3D slice along the camera's 4th axis.\nDouble-click the button below to reset.", w);
    ImGui::SetCursorScreenPos({p0.x + 8, p0.y + 314});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3, 1});
    if (ImGui::SmallButton("W=0")) app.setSliceW(0);
    sliderHovered |= ImGui::IsItemHovered();
    ImGui::PopStyleVar();
    dl->AddText({p0.x + 12, p0.y + 72}, IM_COL32(200, 120, 230, 255), "W");
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", w);
    dl->AddText({p0.x + 44, p0.y + 90 + 220 * (1 - (w + range) / (2 * range)) - 7}, IM_COL32(220, 220, 220, 255), buf);
  }
  if (app.geo && app.showThumbnail) thumbnail();
  bool thumbHovered = app.geo && app.showThumbnail && ImGui::IsItemHovered();
  if (sliderHovered || thumbHovered) vpHovered_ = false;
  overlays(dl);
  ImGui::End();
  ImGui::PopStyleVar();

  // ---- input forwarding (GLFW based so it also works with the cursor captured) ----
  InputState in;
  double mx, my;
  glfwGetCursorPos(win_, &mx, &my);
  in.mx = mx; in.my = my;
  in.dx = mx - lastMx_; in.dy = my - lastMy_;
  lastMx_ = mx; lastMy_ = my;
  for (int b = 0; b < 3; b++) {
    bool d = glfwGetMouseButton(win_, b == 0 ? GLFW_MOUSE_BUTTON_LEFT : b == 1 ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    in.down[b] = d;
    in.pressed[b] = d && !prevDown_[b];
    in.released[b] = !d && prevDown_[b];
    prevDown_[b] = d;
  }
  // don't start viewport interactions while ImGui is dragging a payload or a popup is open
  bool blocked = ImGui::GetDragDropPayload() != nullptr || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
  in.vpHovered = vpHovered_ && !blocked && !(showBuilder_ && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !vpHovered_);
  if (!in.vpHovered) for (int b = 0; b < 3; b++) in.pressed[b] = false;
  in.wheel = (vpHovered_ && !blocked) ? wheel_ : 0;
  wheel_ = 0;
  in.shift = glfwGetKey(win_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
  in.ctrl = glfwGetKey(win_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
  in.alt = glfwGetKey(win_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(win_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
  in.keysEnabled = !io.WantTextInput || app.mouseCaptured;
  in.keyDown = [this](int k) { return glfwGetKey(win_, k) == GLFW_PRESS; };
  app.handleInput(in, dt);
}

void UI::thumbnail() {
  ImVec2 p0 = {app.vpX + app.vpW - app.thumbW - 12, app.vpY + app.vpH - app.thumbH - 12};
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##thumb", {(float)app.thumbW, (float)app.thumbH});
  bool hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemActive()) {
    ImVec2 d = ImGui::GetIO().MouseDelta;
    app.thumbYaw += d.x * 0.01;
    app.thumbPitch = clampd(app.thumbPitch - d.y * 0.01, -1.55, 1.55);
  }
  if (hovered && wheel_ != 0) { app.thumbZoom = clampd(app.thumbZoom * std::exp(-wheel_ * 0.1), 0.3, 4); wheel_ = 0; }
  if (hovered) ImGui::SetTooltip("Outside view (Full 4D slice through the world centre and the camera).\nYellow sphere = camera. Drag to orbit, wheel to zoom.");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1 = {p0.x + app.thumbW, p0.y + app.thumbH};
  if (app.thumbRT.tex) dl->AddImage(tex(app.thumbRT.tex), p0, p1, {0, 1}, {1, 0});
  dl->AddRect(p0, p1, IM_COL32(230, 180, 60, 255), 0, 0, 2);
  dl->AddText({p0.x + 6, p0.y + 4}, IM_COL32(240, 240, 240, 255), "Outside view");
}

void UI::overlays(ImDrawList* dl) {
  View v = app.currentView();
  V4 right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2), ana = col(v.frame, 3);
  float x0 = app.vpX, y0 = app.vpY;
  // status text
  std::vector<std::string> lines;
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s | %s", app.mode == Mode::Edit ? "EDIT" : "EXPLORE", app.geo ? "Geodesic 3D" : "Full 4D");
  lines.push_back(buf);
  std::snprintf(buf, sizeof buf, "eye (%.2f, %.2f, %.2f, %.2f)", v.eye[0], v.eye[1], v.eye[2], v.eye[3]);
  lines.push_back(buf);
  if (app.mode == Mode::Explore) {
    if (!app.mouseCaptured) lines.push_back("Click the viewport to capture the mouse (Esc releases)");
    else if (!app.geo) lines.push_back("WASD move | Space/Shift up/down | Q/E along W | Z/X turn into W | C/V turn sideways in W | R reset | wheel speed");
    else lines.push_back("WASD move | Space/Shift up/down (tangent) | Q/E roll | wheel speed | Esc release");
  } else if (app.modal != Modal::None) lines.push_back(app.transformHint());
  else {
    const char* tool = app.tool == Tool::Move ? "Move tool: drag objects in the visible slice (Shift = depth)"
                       : app.tool == Tool::Rotate ? "Rotate tool: drag objects (Shift = roll)" : "Select tool: click to select";
    lines.push_back(tool);
    lines.push_back("RMB/MMB orbit | Shift+drag pan | Alt+drag rotate into W | wheel zoom | G/R/S | Tab explore | V view");
  }
  float ty = y0 + 8;
  float tx = x0 + (app.mode == Mode::Edit && !app.geo ? 60 : 8);
  for (size_t i = 0; i < lines.size(); i++) {
    ImVec2 ts = ImGui::CalcTextSize(lines[i].c_str());
    dl->AddRectFilled({tx - 4, ty - 1}, {tx + ts.x + 4, ty + ts.y + 1}, IM_COL32(0, 0, 0, 130));
    dl->AddText({tx, ty}, (i == 2 && app.modal != Modal::None) ? IM_COL32(255, 180, 70, 255) : IM_COL32(232, 232, 232, 255), lines[i].c_str());
    ty += ts.y + 4;
  }

  // 4D axis gizmo
  float cx = x0 + 60, cy = y0 + app.vpH - 60, R = 40;
  dl->AddCircleFilled({cx, cy}, R + 14, IM_COL32(20, 22, 28, 150));
  const ImU32 ac[4] = {IM_COL32(224, 85, 85, 255), IM_COL32(95, 207, 95, 255), IM_COL32(85, 136, 238, 255), IM_COL32(192, 96, 224, 255)};
  int order[4] = {0, 1, 2, 3};
  std::sort(order, order + 4, [&](int a, int b) { return dot(unit(a), fwd) > dot(unit(b), fwd); });
  for (int i : order) {
    V4 e = unit(i);
    float ex = cx + (float)dot(e, right) * R, ey = cy - (float)dot(e, up) * R;
    float out = (float)std::abs(dot(e, ana));
    ImU32 c = (ac[i] & 0x00FFFFFF) | ((ImU32)(255 * (0.35f + 0.65f * (1 - out))) << 24);
    dl->AddLine({cx, cy}, {ex, ey}, c, 2);
    dl->AddCircleFilled({ex, ey}, 8, c);
    if (out > 0.02f) dl->PathArcTo({ex, ey}, 8 + 4 * out, 0, 2 * (float)PI * out), dl->PathStroke(IM_COL32(255, 255, 255, 200), 0, 1);
    char l[2] = {AXIS[i], 0};
    dl->AddText({ex - 3.5f, ey - 7}, IM_COL32(15, 15, 15, 255), l);
  }

  if (app.mode == Mode::Explore) {
    float mx = x0 + app.vpW / 2, my = y0 + app.vpH / 2;
    dl->AddLine({mx - 8, my}, {mx + 8, my}, IM_COL32(255, 255, 255, 200), 1.5f);
    dl->AddLine({mx, my - 8}, {mx, my + 8}, IM_COL32(255, 255, 255, 200), 1.5f);
    return;
  }

  // player sphere: label with its distance from the visible slice + facing arrow
  if (app.showPlayerMarker && (app.geo ? app.geoPlayer.valid : app.playerPlaced)) {
    View pv = app.playerView();
    double dw = dot(pv.eye - v.eye, ana);
    V4 m = app.geo ? pv.eye : pv.eye - ana * dw;
    double sx, sy, fx, fy;
    if (length(m - v.eye) > 0.8 && app.project(m, v, sx, sy)) {
      V4 f = col(pv.frame, 2);
      if (!app.geo) f = reject(f, ana);
      if (app.project(m + normalize(f) * 0.6, v, fx, fy)) dl->AddLine({(float)sx, (float)sy}, {(float)fx, (float)fy}, IM_COL32(255, 220, 60, 255), 2.5f);
      std::snprintf(buf, sizeof buf, app.geo ? "player" : "player  dW=%+.2f", dw);
      dl->AddText({(float)sx + 10, (float)sy - 18}, IM_COL32(255, 230, 120, 255), buf);
    }
  }

  // tool gizmo at the selection
  if (Node* n = app.selected(); n && n->id != app.root.id && app.tool != Tool::Select && app.modal == Modal::None) {
    V4 p{0, 0, 0, 0};
    for (Node* a : pathTo(app.root, n->id)) p = p + a->xf.pos;
    double sx, sy;
    if (app.project(p, v, sx, sy)) {
      ImVec2 c = {(float)sx, (float)sy};
      if (app.tool == Tool::Move) {
        dl->AddLine(c, {c.x + 45, c.y}, IM_COL32(230, 90, 90, 255), 3);
        dl->AddTriangleFilled({c.x + 52, c.y}, {c.x + 42, c.y - 5}, {c.x + 42, c.y + 5}, IM_COL32(230, 90, 90, 255));
        dl->AddLine(c, {c.x, c.y - 45}, IM_COL32(100, 210, 100, 255), 3);
        dl->AddTriangleFilled({c.x, c.y - 52}, {c.x - 5, c.y - 42}, {c.x + 5, c.y - 42}, IM_COL32(100, 210, 100, 255));
        dl->AddCircleFilled(c, 5, IM_COL32(90, 140, 240, 255));
      } else {
        dl->AddCircle(c, 40, IM_COL32(240, 200, 80, 220), 48, 2.5f);
        dl->AddCircle(c, 30, IM_COL32(90, 140, 240, 160), 48, 1.5f);
      }
    }
  }
}

// ============================================================ builder
void UI::openBuilder(const PrimitiveDef* base, bool editing) {
  if (base) {
    draft_ = *base;
    if (!editing) { draft_.id.clear(); draft_.name += " copy"; draft_.builtin = false; }
  } else {
    draft_ = PrimitiveDef{};
    draft_.name = "My primitive";
    draft_.steps.push_back(Step{});
  }
  draftEditing_ = editing;
  showBuilder_ = true;
  previewCam_ = FullCamera();
  previewCam_.target = {0, 0, 0, 0};
  previewCam_.distance = 6;
  previewW_ = previewZW_ = 0;
}

void UI::builderWindow() {
  ImGui::SetNextWindowSize({980, 560}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
  if (!ImGui::Begin(draftEditing_ ? "Edit primitive" : "New primitive", &showBuilder_)) { ImGui::End(); return; }
  ImGui::BeginChild("left", {520, -40});
  char nameBuf[128];
  std::snprintf(nameBuf, sizeof nameBuf, "%s", draft_.name.c_str());
  if (ImGui::InputText("Name", nameBuf, sizeof nameBuf)) draft_.name = nameBuf;
  ImGui::TextDisabled("Each Extrude / Revolve adds the next axis: X -> Y -> Z -> W.");
  ImGui::TextUnformatted("Point (0D start)");
  int dim = 0, remove = -1, moveUp = -1;
  for (size_t k = 0; k < draft_.steps.size(); k++) {
    auto& s = draft_.steps[k];
    ImGui::PushID((int)k);
    ImGui::Separator();
    if (s.op == StepOp::Extrude) {
      ImGui::Text("Extrude -> %c", dim < 4 ? AXIS[dim] : '?');
      ImGui::SameLine(130);
      int mode = (int)s.mode;
      ImGui::SetNextItemWidth(130);
      const char* modes[] = {"finite", "infinite", "half-space (<= 0)"};
      if (ImGui::Combo("##mode", &mode, modes, 3)) s.mode = (ExtrudeMode)mode;
      if (s.mode == ExtrudeMode::Finite) {
        ImGui::SameLine();
        float L = (float)s.length;
        ImGui::SetNextItemWidth(110);
        if (ImGui::DragFloat("Length", &L, 0.02f, 0.001f, 100.f)) s.length = std::max(0.001f, L);
      }
      dim++;
    } else if (s.op == StepOp::Revolve) {
      ImGui::Text("Revolve -> %c", dim < 4 ? AXIS[dim] : '?');
      ImGui::SameLine(130);
      if (dim > 0) {
        int a = s.axis < 0 ? dim - 1 : s.axis;
        std::string preview = std::string("spin ") + AXIS[a] + " into " + AXIS[std::min(dim, 3)];
        ImGui::SetNextItemWidth(130);
        if (ImGui::BeginCombo("##axis", preview.c_str())) {
          for (int i = 0; i < dim; i++)
            if (ImGui::Selectable((std::string("spin ") + AXIS[i] + " into " + AXIS[std::min(dim, 3)]).c_str(), i == a)) s.axis = i;
          ImGui::EndCombo();
        }
      }
      ImGui::SameLine();
      float R = (float)s.radius;
      ImGui::SetNextItemWidth(110);
      if (ImGui::DragFloat("Radius", &R, 0.02f, 0.f, 100.f)) s.radius = std::max(0.f, R);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radius of revolution (0 = around the centre)");
      dim++;
    } else {
      ImGui::TextUnformatted("Rotate");
      ImGui::SameLine(130);
      std::string pl = std::string(1, AXIS[s.i]) + AXIS[s.j];
      ImGui::SetNextItemWidth(80);
      if (ImGui::BeginCombo("##plane", pl.c_str())) {
        for (int a = 0; a < dim; a++)
          for (int b = a + 1; b < dim; b++)
            if (ImGui::Selectable((std::string(1, AXIS[a]) + AXIS[b]).c_str(), s.i == a && s.j == b)) { s.i = a; s.j = b; }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      float ang = (float)s.angle;
      ImGui::SetNextItemWidth(110);
      if (ImGui::DragFloat("Angle", &ang, 0.5f)) s.angle = ang;
    }
    ImGui::SameLine(460);
    if (k > 0 && ImGui::SmallButton("^")) moveUp = (int)k;
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) remove = (int)k;
    ImGui::PopID();
  }
  if (remove >= 0) draft_.steps.erase(draft_.steps.begin() + remove);
  if (moveUp > 0) std::swap(draft_.steps[moveUp], draft_.steps[moveUp - 1]);
  ImGui::Separator();
  int cd = primitiveDim(draft_);
  if (ImGui::Button("+ Extrude") && cd < 4) draft_.steps.push_back(Step{});
  ImGui::SameLine();
  if (ImGui::Button("+ Revolve") && cd >= 1 && cd < 4) { Step s; s.op = StepOp::Revolve; s.radius = 0; draft_.steps.push_back(s); }
  ImGui::SameLine();
  if (ImGui::Button("+ Rotate") && cd >= 2) { Step s; s.op = StepOp::Rotate; s.i = 0; s.j = 1; s.angle = 45; draft_.steps.push_back(s); }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate the profile within its own space (matters before a Revolve)");
  float rd = (float)draft_.round, sh = (float)draft_.shell;
  if (ImGui::DragFloat("Rounding", &rd, 0.005f, -2, 5)) draft_.round = rd;
  if (ImGui::DragFloat("Shell", &sh, 0.005f, 0, 5)) draft_.shell = std::max(0.f, sh);
  auto errs = validatePrimitive(draft_);
  if (!errs.empty()) ImGui::TextColored({1, 0.7f, 0.3f, 1}, "%s", errs[0].c_str());
  else ImGui::TextDisabled("Result: %dD primitive%s", cd, cd < 4 ? " (thin along the remaining axes when placed in 4D)" : "");
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("right", {0, -40});
  ImVec2 psz = {std::max(200.f, ImGui::GetContentRegionAvail().x), 340};
  ImVec2 pp = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##preview", psz);
  if (ImGui::IsItemActive()) previewCam_.orbit(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y);
  if (ImGui::IsItemHovered() && wheel_ != 0) { previewCam_.zoom(-wheel_); wheel_ = 0; }
  if (errs.empty()) {
    static PrimitiveRegistry previewReg;
    PrimitiveDef d = draft_;
    d.id = "__draft";
    d.builtin = false;
    previewReg.set(d);
    static Node previewRoot = makeGroup(BoolOp::Union, {makeLeaf("__draft", "draft", {0, 0, 0, 0}, {0.9, 0.6, 0.3})});
    previewCam_.reset4();
    previewCam_.rotate4(2, 3, previewZW_ * PI / 180);
    FrameParams fp;
    fp.view = previewCam_.view();
    fp.view.eye[3] += previewW_;
    fp.geo = false;
    fp.grid = true;
    fp.fovDeg = 50;
    fp.quality = &app.quality;
    fp.scale = 1;
    app.renderer->render(previewRT_, (int)psz.x, (int)psz.y, previewRoot, previewReg, "", {}, fp);
    ImGui::GetWindowDrawList()->AddImage(tex(previewRT_.tex), pp, {pp.x + psz.x, pp.y + psz.y}, {0, 1}, {1, 0});
  }
  ImGui::SliderFloat("W slice", &previewW_, -2, 2);
  ImGui::SliderFloat("ZW turn", &previewZW_, -180, 180, "%.0f deg");
  ImGui::TextDisabled("Drag to orbit, wheel to zoom. 4D shapes show as the 3D slice at W.");
  ImGui::EndChild();

  bool ok = errs.empty();
  if (!ok) ImGui::BeginDisabled();
  auto save = [&](bool place) {
    PrimitiveDef out = draft_;
    if (!draftEditing_ || out.id.empty()) out.id = newId("p");
    out.builtin = false;
    if (out.description.empty()) out.description = std::to_string(primitiveDim(out)) + "D custom primitive";
    app.lib.savePrimitive(out);
    app.rebuild();
    app.toast("Saved primitive \"" + out.name + "\"");
    showBuilder_ = false;
    if (place) app.placeItem({false, out.id}, std::nullopt);
  };
  if (ImGui::Button("Save to library")) save(false);
  ImGui::SameLine();
  if (ImGui::Button("Save & place")) save(true);
  if (!ok) ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) showBuilder_ = false;
  ImGui::End();
}

// ============================================================ quality / help
void UI::qualityWindow() {
  ImGui::SetNextWindowSize({360, 0}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Rendering quality", &showQuality_)) { ImGui::End(); return; }
  auto& q = app.quality;
  ImGui::SliderFloat("Resolution", &q.resolutionScale, 0.25f, 2.f, "%.2fx");
  ImGui::SliderFloat("While moving", &q.motionScale, 0.2f, 1.f, "%.2fx");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resolution factor while the camera or objects move; full resolution when idle.");
  ImGui::SliderInt("Max steps", &q.maxSteps, 32, 512);
  ImGui::Checkbox("Soft shadows", &q.shadows);
  ImGui::Checkbox("Ambient occlusion", &q.ao);
  ImGui::SeparatorText("Geodesic rays");
  ImGui::SliderFloat("Max step", &q.geoMaxStep, 0.02f, 1.f);
  ImGui::SliderInt("Projections", &q.geoProjectIters, 1, 4);
  ImGui::SeparatorText("Camera");
  float fov = (float)app.fov;
  if (ImGui::SliderFloat("FOV", &fov, 30, 110)) app.fov = fov;
  ImGui::Checkbox("Collisions (Explore)", &app.collide);
  ImGui::Checkbox("Outside thumbnail (Geodesic)", &app.showThumbnail);
  ImGui::Checkbox("Player sphere (Edit)", &app.showPlayerMarker);
  if (!app.renderer->lastError.empty()) ImGui::TextColored({1, 0.5f, 0.4f, 1}, "Last shader error:\n%.300s", app.renderer->lastError.c_str());
  ImGui::TextDisabled("Last shader compile: %.0f ms", app.renderer->lastCompileMs);
  ImGui::End();
}

void UI::helpWindow() {
  ImGui::SetNextWindowSize({620, 520}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Help", &showHelp_)) { ImGui::End(); return; }
  ImGui::SeparatorText("Modes");
  ImGui::TextWrapped("Tab: Edit / Explore.  V: Full 4D / Geodesic 3D (needs a world object: World menu in the toolbar, or Properties > Use as world).");
  ImGui::SeparatorText("Edit (Blender-like)");
  ImGui::BulletText("RMB/MMB drag: orbit | Shift+drag: pan | Ctrl+drag or wheel: zoom | Alt+drag: rotate the view into W (Geodesic: roll)");
  ImGui::BulletText("W slider (left edge of the viewport): moves the visible 3D slice along the camera's 4th axis");
  ImGui::BulletText("Tools: Q select, W move, E rotate. Move/rotate are relative to the camera and stay inside the visible slice (Shift: depth / roll)");
  ImGui::BulletText("G grab, R rotate, S scale; then X / Y / Z = screen-right / screen-up / depth; click or Enter confirms, Esc or RMB cancels");
  ImGui::BulletText("Click: select (again: parent group) | Shift+click: multi-select | A: all | Esc: none | Shift+D duplicate | X/Del delete | Ctrl+G group | F focus");
  ImGui::BulletText("1 / 3 / 7 / 9: front / right / top / along W | Shift+W: reset 4D orientation");
  ImGui::BulletText("The yellow sphere is the Explore player (projected into the visible slice; dW = its distance from the slice)");
  ImGui::SeparatorText("Explore (game-like)");
  ImGui::BulletText("Click to capture the mouse, Esc releases (Esc again returns to Edit). WASD, Space/Shift up/down, wheel = speed");
  ImGui::BulletText("Full 4D: Q/E move along W, Z/X turn into W, C/V turn sideways in W, R reset");
  ImGui::BulletText("Geodesic: you move on the world's 3D surface; Q/E roll. The thumbnail shows you from outside");
  ImGui::SeparatorText("Library & tree");
  ImGui::BulletText("Drag library items into the viewport or onto tree rows. Right-click items for edit/export/delete");
  ImGui::BulletText("Drag tree rows onto groups (into), onto objects (union with it) or onto row edges (reorder). Right-click for more");
  ImGui::End();
}

}  // namespace fd
