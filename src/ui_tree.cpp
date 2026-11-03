// Scene tree panel.
#include "ui_common.hpp"

namespace fd {

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

}  // namespace fd
