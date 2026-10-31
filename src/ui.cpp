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

struct LibPayload { char id[96]; };

static ImTextureID tex(GLuint t) { return (ImTextureID)(intptr_t)t; }

UI::UI(App& a, GLFWwindow* w) : app(a), win_(w) {
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
  const char* titles[] = {"", "Open scene", "Save scene", "Import library item", "Export library", "Export object", "Export library object"};
  std::string def;
  if (action == 2) def = app.currentFile.empty() ? "scene.4dscene.json" : app.currentFile;
  if (action == 4) def = "4dcad-library.json";
  if (action == 5) def = (app.selected() ? app.selected()->name : std::string("object")) + ".4dobj.json";
  if (action == 6) { auto* o = app.lib.find(exportPrimId_); def = (o ? o->name : std::string("object")) + ".4dobj.json"; }
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
      try { app.toast("Imported " + app.lib.importText(text) + " into the library"); } catch (const std::exception& e) { app.toast(e.what(), true); }
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
      if (auto* o = app.lib.find(exportPrimId_)) writeFile(path, libraryObjectToJson(*o)) ? app.toast("Exported " + o->name) : app.toast("Cannot write", true);
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
    if (ImGui::MenuItem("Import object / library...")) fileAction(3);
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
    ImGui::MenuItem("Show own body (Explore)", nullptr, &app.showOwnBody);
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
    if (toggleButton("Move", app.tool == Tool::Move, "W: gizmo arrows along the object's axes (projected into the visible slice)")) app.tool = Tool::Move;
    ImGui::SameLine(0, 2);
    if (toggleButton("Rotate", app.tool == Tool::Rotate, "E: gizmo rings for the planes of the object's axes")) app.tool = Tool::Rotate;
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
  if (ImGui::Button("+ Folder")) { std::string id = app.lib.addFolder("New folder"); renameId_ = "f:" + id; std::snprintf(renameBuf_, sizeof renameBuf_, "New folder"); }
  ImGui::SameLine();
  if (ImGui::Button("Import")) fileAction(3);
  ImGui::SameLine();
  if (ImGui::Button("Export")) fileAction(4);
  ImGui::TextDisabled(app.geo ? "Geodesic: drag 3D items onto the surface" : "Drag items into the viewport");

  std::function<void()> deferred;
  for (auto& f : app.lib.folders) {
    std::string fid = f.id;
    bool renamingFolder = renameId_ == "f:" + fid;
    ImGui::PushID(fid.c_str());
    bool open;
    if (renamingFolder) {
      open = true;
      ImGui::SetNextItemWidth(-1);
      ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##rn", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) || ImGui::IsItemDeactivated()) {
        deferred = [this, fid] { if (renameBuf_[0]) app.lib.renameFolder(fid, renameBuf_); renameId_.clear(); };
      }
    } else {
      std::string label = f.name + (f.builtin ? "  (built-in)" : "") + "###folder";
      open = ImGui::CollapsingHeader(label.c_str(), f.builtin ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_DefaultOpen);
    }
    // drop library items onto a (user) folder to move them there
    if (!f.builtin && ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FD_LIB")) {
        std::string oid = ((const LibPayload*)p->Data)->id;
        deferred = [this, oid, fid] { app.lib.moveObject(oid, fid); };
      }
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FD_NODE")) {  // scene object -> save into folder
        std::string nid((const char*)p->Data);
        deferred = [this, nid, fid] {
          if (Node* n = findNode(app.root, nid)) { app.lib.saveObject(makeLibraryObject(*n, app.reg, n->name), fid); app.toast("Saved \"" + n->name + "\""); }
        };
      }
      ImGui::EndDragDropTarget();
    }
    if (!renamingFolder && ImGui::BeginPopupContextItem("fctx")) {
      if (f.builtin) ImGui::TextDisabled("Built-in folders are read-only.\nDrag items into your folders to copy them.");
      else {
        if (ImGui::MenuItem("Rename")) { renameId_ = "f:" + fid; std::snprintf(renameBuf_, sizeof renameBuf_, "%s", f.name.c_str()); }
        if (ImGui::MenuItem("Delete folder", nullptr, false, fid != LibraryStore::CUSTOM)) deferred = [this, fid] { app.lib.deleteFolder(fid); };
      }
      ImGui::EndPopup();
    }
    if (open) {
      if (f.objects.empty()) ImGui::TextDisabled("  (empty: right-click an object in the scene tree > Save to library)");
      for (auto& o : f.objects) {
        std::string oid = o.id;
        int dim = nodeDim(o.node, app.reg);
        std::string why;
        bool ok = app.canPlace(oid, &why) || app.mode != Mode::Edit;
        ImGui::PushID(oid.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, DIM_COL[dim]);
        ImGui::SmallButton(DIM_LABEL[dim]);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (renameId_ == "o:" + oid) {
          ImGui::SetNextItemWidth(-1);
          ImGui::SetKeyboardFocusHere();
          if (ImGui::InputText("##rn", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) || ImGui::IsItemDeactivated())
            deferred = [this, oid] { if (renameBuf_[0]) app.lib.renameObject(oid, renameBuf_); renameId_.clear(); };
          ImGui::PopID();
          continue;
        }
        if (!ok) ImGui::BeginDisabled();
        ImGui::Selectable(o.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (ok && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) deferred = [this, oid] { app.placeItem(oid, std::nullopt); };
        if (ok && ImGui::BeginDragDropSource()) {
          LibPayload p{};
          std::snprintf(p.id, sizeof p.id, "%s", oid.c_str());
          ImGui::SetDragDropPayload("FD_LIB", &p, sizeof p);
          ImGui::Text("%s", o.name.c_str());
          ImGui::EndDragDropSource();
        }
        if (!ok) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          std::string mods;
          for (auto& m : o.node.mods) mods += std::string(mods.empty() ? "" : " > ") + MOD_NAMES[m.type];
          ImGui::SetTooltip("%dD %s%s%s%s", dim, o.node.isLeaf() ? "shape" : "group", mods.empty() ? "" : "\npoint > ", mods.c_str(), ok ? "" : ("\n" + why).c_str());
        }
        if (ImGui::BeginPopupContextItem("octx")) {
          if (!f.builtin) {
            if (ImGui::MenuItem("Rename")) { renameId_ = "o:" + oid; std::snprintf(renameBuf_, sizeof renameBuf_, "%s", o.name.c_str()); }
            if (ImGui::BeginMenu("Move to folder")) {
              for (auto& f2 : app.lib.folders)
                if (!f2.builtin && f2.id != fid && ImGui::MenuItem(f2.name.c_str())) { std::string tid = f2.id; deferred = [this, oid, tid] { app.lib.moveObject(oid, tid); }; }
              ImGui::EndMenu();
            }
          } else if (ImGui::BeginMenu("Copy to folder")) {
            for (auto& f2 : app.lib.folders)
              if (!f2.builtin && ImGui::MenuItem(f2.name.c_str())) { std::string tid = f2.id; deferred = [this, oid, tid] { app.lib.moveObject(oid, tid); }; }
            ImGui::EndMenu();
          }
          if (ImGui::MenuItem("Export...")) { exportPrimId_ = oid; fileAction(6); }
          if (!f.builtin && ImGui::MenuItem("Delete")) deferred = [this, oid] { app.lib.deleteObject(oid); };
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }
    }
    ImGui::PopID();
  }
  ImGui::End();
  if (deferred) deferred();
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
  if (ImGui::Button("Save to library") && app.selected()) {
    Node* n = app.selected();
    app.lib.saveObject(makeLibraryObject(*n, app.reg, n->name));
    app.toast("Saved \"" + n->name + "\" to Custom (rename / move it in the library)");
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
  if (n.isLeaf()) label = std::string("[") + DIM_LABEL[nodeDim(n, app.reg)] + "] ";
  else label = std::string("(") + OP_ICON[(int)n.op] + ") ";
  label += n.name;
  if (n.id == app.world) label += "  [world]";
  if (n.anchor) label += "  [on surface]";

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
        app.placeItem(lp.id, std::nullopt);
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
    if (ImGui::MenuItem("Save to library", nullptr, false, !isRoot)) deferred = [this, id] {
      if (Node* m = findNode(app.root, id)) { app.lib.saveObject(makeLibraryObject(*m, app.reg, m->name)); app.toast("Saved \"" + m->name + "\" to Custom"); }
    };
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
  int dim = nodeDim(*n, app.reg);
  ImGui::TextDisabled("%dD %s", dim, n->isLeaf() ? "shape (point + modifiers)" : "group");

  if (n->isLeaf()) {
    ImGui::SeparatorText("Shape");
    float c[3] = {(float)n->color[0], (float)n->color[1], (float)n->color[2]};
    if (ImGui::ColorEdit3("Color", c)) for (int i = 0; i < 3; i++) n->color[i] = c[i];
    track();
    if (dim < 4) {
      float th = (float)n->thickness;
      if (ImGui::DragFloat("Thickness", &th, 0.005f, 0.001f, 10.f)) n->thickness = std::max(0.001f, th);
      track();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extent along the unused axes%s", n->anchor ? " (here: across the world surface)" : "");
    }
    float rd = (float)n->round.value_or(0), sh = (float)n->shell.value_or(0);
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
    if (ImGui::Button("Reset rotation")) { n->xf.rot = identity(); done = true; }
    float sc = (float)n->xf.scale;
    if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.01f, 100.f)) n->xf.scale = std::max(0.01f, sc);
    track();
  }

  // ---- modifier stack ----
  ImGui::SeparatorText("Modifiers (applied top to bottom)");
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
      case MOD_MIRROR:
        ImGui::SetNextItemWidth(60);
        if (ImGui::Combo("Axis", &m.axis, axn, 4)) done = true;
        break;
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
      case MOD_ROTATE: {
        std::string pl = std::string(1, AXIS[m.i]) + AXIS[m.j];
        ImGui::SetNextItemWidth(70);
        if (ImGui::BeginCombo("Plane", pl.c_str())) {
          for (int p = 0; p < 6; p++)
            if (ImGui::Selectable(PLANE_NAMES[p], m.i == PLANES[p][0] && m.j == PLANES[p][1])) { m.i = PLANES[p][0]; m.j = PLANES[p][1]; done = true; }
          ImGui::EndCombo();
        }
        float an = (float)m.angle;
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Angle", &an, 0.5f)) m.angle = an;
        track();
        break;
      }
    }
    for (auto& [k, msg] : problems) if (k == (int)i) ImGui::TextColored({1, 0.7f, 0.3f, 1}, "%s", msg.c_str());
    ImGui::Unindent(10);
    ImGui::PopID();
  }
  if (remove >= 0) { n->mods.erase(n->mods.begin() + remove); done = true; }
  if (moveUp > 0) { std::swap(n->mods[moveUp], n->mods[moveUp - 1]); done = true; }
  if (ImGui::Button("+ Add modifier")) ImGui::OpenPopup("addmod");
  if (ImGui::BeginPopup("addmod")) {
    auto add = [&](int type, const char* label, const char* tip) {
      if (ImGui::MenuItem(label)) { Modifier m; m.type = type; if (type == MOD_REVOLVE) { m.axis = -1; m.radius = 0; } n->mods.push_back(m); done = true; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    add(MOD_EXTRUDE, "Extrude", "Adds the next axis: sweeps the shape along it (finite, infinite or half-space)");
    add(MOD_REVOLVE, "Revolve", "Adds the next axis: spins an existing axis into it at a radius");
    add(MOD_ROTATE, "Rotate", "Rotates the shape built so far within one plane (e.g. before a Revolve)");
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
      app.placeItem(lp.id, std::make_pair((double)io.MousePos.x, (double)io.MousePos.y));
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
  in.vpHovered = vpHovered_ && !blocked;
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
    const char* tool = app.tool == Tool::Move ? "Move: drag an arrow (object axis) or the centre square; drag the object to move it in the screen plane"
                       : app.tool == Tool::Rotate ? "Rotate: drag a ring (rotation plane of two object axes)" : "Select: click to select";
    lines.push_back(tool);
    if (app.geo) lines.push_back("Hold RMB: look + WASD fly, Space/Shift up/down, Q/E roll | wheel: fly speed | G/R/S | Tab explore | V view");
    else lines.push_back("RMB/MMB drag orbit | MMB click: set centre of rotation | Shift+drag pan | Alt+drag rotate into W | wheel zoom | G/R/S | Tab | V");
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

  // centre of rotation (set with a middle-click)
  if (!app.geo && app.editCam.pivotSet) {
    double px, py;
    if (app.project(app.editCam.pivot, v, px, py)) {
      ImVec2 c = {(float)px, (float)py};
      dl->AddCircle(c, 6, IM_COL32(255, 255, 255, 220), 16, 1.5f);
      dl->AddLine({c.x - 10, c.y}, {c.x + 10, c.y}, IM_COL32(255, 255, 255, 160));
      dl->AddLine({c.x, c.y - 10}, {c.x, c.y + 10}, IM_COL32(255, 255, 255, 160));
    }
  }

  // Blender-like gizmo: the object's own X/Y/Z/W axes projected into the visible slice
  Gizmo g = app.gizmo();
  if (g.visible) {
    const ImU32 cols[4] = {IM_COL32(230, 80, 80, 255), IM_COL32(100, 210, 100, 255), IM_COL32(80, 130, 240, 255), IM_COL32(200, 100, 235, 255)};
    ImVec2 c = {(float)g.cx, (float)g.cy};
    auto bright = [](ImU32 col) { return col | IM_COL32(60, 60, 60, 0); };
    if (app.tool == Tool::Move) {
      for (int a = 0; a < 4; a++) {
        auto& ax = g.axes[a];
        if (!ax.visible) continue;
        bool hot = g.hot == a;
        ImU32 col = hot ? IM_COL32(255, 230, 120, 255) : cols[a];
        ImVec2 e = {(float)ax.ex, (float)ax.ey};
        float dx = e.x - c.x, dy = e.y - c.y, l = std::sqrt(dx * dx + dy * dy);
        if (l < 1) continue;
        float ux = dx / l, uy = dy / l;
        dl->AddLine(c, {e.x - ux * 10, e.y - uy * 10}, col, hot ? 4.f : 3.f);
        dl->AddTriangleFilled(e, {e.x - ux * 14 - uy * 6, e.y - uy * 14 + ux * 6}, {e.x - ux * 14 + uy * 6, e.y - uy * 14 - ux * 6}, col);
        char l1[2] = {AXIS[a], 0};
        dl->AddText({e.x + ux * 6 - 4, e.y + uy * 6 - 7}, col, l1);
      }
      dl->AddRectFilled({c.x - 6, c.y - 6}, {c.x + 6, c.y + 6}, g.hot == 4 ? IM_COL32(255, 230, 120, 255) : IM_COL32(230, 230, 230, 200), 2);
    } else {
      for (size_t r = 0; r < g.rings.size(); r++) {
        auto& ring = g.rings[r];
        bool hot = g.hot == 10 + (int)r;
        // ring colour: the axis it rotates "about" in 3D terms (XY->Z blue, XZ->Y green, YZ->X red), W planes purple
        int about = (ring.a == 3 || ring.b == 3) ? 3 : 3 - ring.a - ring.b;
        ImU32 col = hot ? IM_COL32(255, 230, 120, 255) : bright(cols[about]);
        std::vector<ImVec2> pts;
        for (auto& p : ring.pts) pts.push_back({(float)p.first, (float)p.second});
        dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, hot ? 3.5f : 2.f);
        if (hot) {
          char lbl[8];
          std::snprintf(lbl, sizeof lbl, "%c%c", AXIS[ring.a], AXIS[ring.b]);
          dl->AddText({pts[8].x + 6, pts[8].y - 6}, col, lbl);
        }
      }
      dl->AddCircleFilled(c, 3, IM_COL32(230, 230, 230, 220));
    }
  }
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
  ImGui::Checkbox("Own body (Explore)", &app.showOwnBody);
  if (!app.renderer->lastError.empty()) ImGui::TextColored({1, 0.5f, 0.4f, 1}, "Last shader error:\n%.300s", app.renderer->lastError.c_str());
  ImGui::TextDisabled("Last shader compile: %.0f ms", app.renderer->lastCompileMs);
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
  ImGui::BulletText("Tools: Q select, W move, E rotate. The gizmo shows the object's own X/Y/Z/W axes projected into the visible slice");
  ImGui::BulletText("Move: drag an arrow to move along that axis, the centre square (or the object) to move in the screen plane");
  ImGui::BulletText("Rotate: drag a ring to rotate in the plane of two object axes");
  ImGui::BulletText("G grab, R rotate, S scale; then X/Y/Z/W = object axis (R: one axis = about it, two = that plane); Enter/click confirm, Esc cancel");
  ImGui::BulletText("Click: select (again: parent group) | Shift+click: multi | A all | Esc none | Shift+D duplicate | X/Del delete | Ctrl+G group | F focus");
  ImGui::BulletText("Shapes are a point plus modifiers: Extrude / Revolve add the next axis (X, Y, Z, W); Rotate, Mirror, Repeat reshape");
  ImGui::SeparatorText("Explore (game-like)");
  ImGui::BulletText("You start where the editor camera is. Click to capture the mouse, Esc releases (again: back to Edit)");
  ImGui::BulletText("WASD, Space/Shift up/down, wheel = speed. Full 4D: Q/E along W, Z/X turn into W, C/V sideways in W, R reset");
  ImGui::BulletText("Your body (yellow sphere) is part of the scene: it casts a shadow, and on closed worlds you can see yourself");
  ImGui::SeparatorText("Library");
  ImGui::BulletText("Built-in folders are read-only; saved objects go to Custom. '+ Folder' adds folders; right-click to rename / move / delete");
  ImGui::BulletText("Drag items into the viewport, onto tree rows, or onto a folder. Drag tree rows onto a folder to save them there");
  ImGui::End();
}

}  // namespace fd
