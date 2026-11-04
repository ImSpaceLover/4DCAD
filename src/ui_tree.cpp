// Scene tree panel.
#include "ui_common.hpp"

namespace fd {

// ============================================================ scene tree
void UI::treePanel() {
  ImGuiIO& io = ImGui::GetIO();
  (void)io;
  if (!showScene_) return;
  if (!ImGui::Begin("Scene", &showScene_)) { ImGui::End(); return; }
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
  // search, collapse / expand all
  ImGui::SetNextItemWidth(std::max(60.f, ImGui::GetContentRegionAvail().x - 60));
  ImGui::InputTextWithHint("##filter", "Search...", treeFilter_, sizeof treeFilter_);
  ImGui::SameLine();
  if (ImGui::SmallButton("-##col")) treeOpenAll_ = 0;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse all");
  ImGui::SameLine();
  if (ImGui::SmallButton("+##exp")) treeOpenAll_ = 1;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand all");
  if (!app.isolate.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Show all")) app.toggleIsolate();
  }
  ImGui::Separator();
  ImGui::BeginChild("tree");
  // copy needed: tree modifications are deferred until after drawing
  treeNode(app.root, 0);
  treeOpenAll_ = -1;
  ImGui::EndChild();
  ImGui::End();
}

bool UI::treeMatches(const Node& n) {
  if (!treeFilter_[0]) return true;
  auto lower = [](std::string x) { for (auto& c : x) c = (char)std::tolower((unsigned char)c); return x; };
  std::string f = lower(treeFilter_);
  bool hit = false;
  walkc(n, [&](const Node& m, const Node*, int) { if (!hit && lower(m.name).find(f) != std::string::npos) hit = true; });
  return hit;
}

// eye (visibility) and padlock icons at the right end of a tree row; returns which was clicked
static int rowIcons(bool hidden, bool locked, bool isolatedOut) {
  float x = ImGui::GetWindowContentRegionMax().x - 40;
  ImGui::SameLine(x);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetFrameHeight();
  int clicked = 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::InvisibleButton("eye", {18, h});
  if (ImGui::IsItemClicked()) clicked = 1;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(hidden ? "Hidden: click to show" : "Visible: click to hide");
  ImU32 ce = hidden || isolatedOut ? IM_COL32(110, 110, 120, 255) : IM_COL32(220, 220, 230, 255);
  ImVec2 c = {p.x + 9, p.y + h * 0.5f};
  dl->PathEllipticalArcTo(c, {7, 4}, 0, 0, 2 * (float)PI);
  dl->PathStroke(ce, ImDrawFlags_Closed, 1.3f);
  if (!hidden) dl->AddCircleFilled(c, 2.2f, ce);
  else dl->AddLine({c.x - 7, c.y + 4}, {c.x + 7, c.y - 4}, ce, 1.3f);
  ImGui::SameLine(0, 2);
  p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("lock", {16, h});
  if (ImGui::IsItemClicked()) clicked = 2;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(locked ? "Locked: cannot be clicked or moved in the viewport" : "Click to lock");
  ImU32 cl = locked ? IM_COL32(240, 190, 80, 255) : IM_COL32(95, 95, 105, 255);
  float bx = p.x + 3, by = p.y + h * 0.5f - 1;
  dl->AddRectFilled({bx, by}, {bx + 10, by + 7}, cl, 1);
  dl->PathArcTo({bx + 5, by}, 3.5f, (float)PI, 2 * (float)PI);
  dl->PathStroke(cl, 0, 1.5f);
  return clicked;
}

void UI::treeNode(Node& n, int depth) {
  bool isRoot = n.id == app.root.id;
  if (!isRoot && !treeMatches(n)) return;
  if (treeFilter_[0] && n.isGroup()) ImGui::SetNextItemOpen(true);
  else if (treeOpenAll_ >= 0 && !isRoot) ImGui::SetNextItemOpen(treeOpenAll_ == 1);
  bool sel = std::find(app.selection.begin(), app.selection.end(), n.id) != app.selection.end();
  ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
  if (!n.isGroup() || n.children.empty()) fl |= ImGuiTreeNodeFlags_Leaf;
  if (sel) fl |= ImGuiTreeNodeFlags_Selected;
  std::string label;
  if (n.isCamera()) label = std::string("[cam] ");
  else if (n.isLeaf()) label = std::string("[") + DIM_LABEL[nodeDim(n, app.reg)] + "] ";
  else label = std::string("(") + OP_ICON[(int)n.op] + ") ";
  label += n.name;
  if (n.id == app.world) label += "  [world]";
  if (n.anchor) label += "  [on surface]";
  if (n.isCamera() && n.id == app.activeCamera) label += "  [active]";
  if (!n.linkId.empty()) label += "  [linked]";

  bool dim = n.hidden || (!app.isolate.empty() && !isRoot && !app.isolate.count(n.id));
  if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1));
  bool open = ImGui::TreeNodeEx(n.id.c_str(), fl, "%s", label.c_str());
  if (dim) ImGui::PopStyleColor();
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
    App::Drop where = isRoot ? App::Drop::Into : t < 0.25f ? App::Drop::Before : t > 0.75f ? App::Drop::After : n.isGroup() ? App::Drop::Into : n.isCamera() ? App::Drop::After : App::Drop::Wrap;
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
    if (n.isGroup() && ImGui::MenuItem("Ungroup", nullptr, false, !isRoot)) deferred = [this, id] { app.ungroup(id); };
    if (n.isCamera()) {
      if (ImGui::MenuItem("Look through")) deferred = [this, id] { app.lookThrough(id); };
      if (ImGui::MenuItem("Set as active camera", nullptr, n.id == app.activeCamera)) deferred = [this, id] { app.activeCamera = id; app.commit(); };
    }
    if (ImGui::MenuItem("Duplicate linked", nullptr, false, !isRoot)) deferred = [this] { app.duplicateLinked(); };
    if (!n.linkId.empty() && ImGui::MenuItem("Unlink")) deferred = [this, id] { app.unlink(id); };
    if (ImGui::MenuItem(n.locked ? "Unlock" : "Lock", nullptr, false, !isRoot))
      deferred = [this, id] { if (Node* m = findNode(app.root, id)) { m->locked = !m->locked; app.commit(); } };
    if (ImGui::MenuItem(app.isolate.empty() ? "Isolate" : "Show all")) deferred = [this] { app.toggleIsolate(); };
    if (ImGui::MenuItem("Copy", nullptr, false, !isRoot)) deferred = [this] { copy(); };
    if (ImGui::BeginMenu("Group as", !isRoot)) {
      for (int i = 0; i < 6; i++) if (ImGui::MenuItem(BOOL_OP_LABELS[i])) deferred = [this, i] { app.groupSelection((BoolOp)i); };
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  if (!isRoot) {  // visibility / lock icons (after the row's click, drag and menu handling, which use the row item)
    ImGui::PushID(id.c_str());
    int ic = rowIcons(n.hidden, n.locked, false);
    ImGui::PopID();
    if (ic == 1) deferred = [this, id] { if (Node* m = findNode(app.root, id)) { m->hidden = !m->hidden; app.commit(); } };
    if (ic == 2) deferred = [this, id] { if (Node* m = findNode(app.root, id)) { m->locked = !m->locked; app.commit(); } };
  }
  if (open) {
    if (n.isGroup()) for (size_t i = 0; i < n.children.size(); i++) treeNode(n.children[i], depth + 1);
    ImGui::TreePop();
  }
  if (isRoot && deferred) { auto f = deferred; deferred = nullptr; f(); }
}

}  // namespace fd
