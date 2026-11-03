// Viewport, outside thumbnail and overlays (gizmo, hints).
#include "ui_common.hpp"

namespace fd {

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
  if (app.mode == Mode::Edit && !app.geo && app.editSet.showWSlider) {
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
  if (app.geo && app.editSet.showThumbnail) thumbnail();
  bool thumbHovered = app.geo && app.editSet.showThumbnail && ImGui::IsItemHovered();
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
    const char* tool = app.tool == Tool::Move ? "Move: drag an arrow (local X/Y/Z) or the centre square; drag the object to move it in the screen plane"
                       : app.tool == Tool::Rotate ? "Rotate: drag a ring to rotate about a local axis" : "Select: click an object (Ctrl+click: its group)";
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
    const ImU32 cols[3] = {IM_COL32(230, 80, 80, 255), IM_COL32(100, 210, 100, 255), IM_COL32(80, 130, 240, 255)};
    ImVec2 c = {(float)g.cx, (float)g.cy};
    auto bright = [](ImU32 col) { return col | IM_COL32(60, 60, 60, 0); };
    if (app.tool == Tool::Move) {
      for (int a = 0; a < 3; a++) {
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
        // ring colour = the local axis it rotates about (YZ -> X red, XZ -> Y green, XY -> Z blue)
        int about = 3 - ring.a - ring.b;
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

}  // namespace fd
