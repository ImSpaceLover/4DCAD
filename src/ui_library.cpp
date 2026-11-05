// Library panel (folders of objects).
#include "ui_common.hpp"

namespace fd {

// ============================================================ library
void UI::libraryPanel() {
  ImGuiIO& io = ImGui::GetIO();
  float top = ImGui::GetFrameHeight() + TOOLBAR_H;
  (void)top;
  if (!showLibrary_) return;
  if (!ImGui::Begin("Library", &showLibrary_)) { ImGui::End(); return; }
  if (ImGui::Button("+ Folder")) { std::string id = app.lib.addFolder("New folder"); renameId_ = "f:" + id; std::snprintf(renameBuf_, sizeof renameBuf_, "New folder"); }
  ImGui::SameLine();
  if (ImGui::Button("Import")) fileAction(3);
  ImGui::SameLine();
  if (ImGui::Button("Export")) fileAction(4);

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
      if (f.objects.empty()) ImGui::TextDisabled("  (empty)");
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

}  // namespace fd
