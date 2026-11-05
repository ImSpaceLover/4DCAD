// Viewport, outside thumbnail and overlays (gizmo, hints).
#include "ui_common.hpp"

namespace fd {

// ============================================================ viewport
void UI::viewport(double dt) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  // docked: its tab bar hides while it is alone in its dock node (the viewport cannot be closed)
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
  ImGui::SetNextWindowClass(&wc);
  ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
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
  if (uint64_t t = app.renderer ? app.renderer->texture(app.mainRT) : 0) {
    bool fl = app.renderer->flipY();  // OpenGL images are stored bottom-up
    dl->AddImage((ImTextureID)t, p0, {p0.x + sz.x, p0.y + sz.y}, {0, fl ? 1.f : 0.f}, {1, fl ? 0.f : 1.f});
  }

  // W slider (Edit, Full 4D): moves the camera's visible slice along its 4th axis
  bool sliderHovered = false;
  if (app.mode == Mode::Edit && !app.geo && !app.curved() && app.editSet.showWSlider) {
    ImGui::SetCursorScreenPos({p0.x + 12, p0.y + 90});
    float w = (float)app.sliceW();
    float range = std::max(5.f, std::ceil(std::abs(w) / 5.f) * 5.f + 5.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.13f, 0.75f));
    if (ImGui::VSliderFloat("##W", {26, 220}, &w, -range, range, "")) app.setSliceW(w);
    ImGui::PopStyleColor();
    sliderHovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
    int sa = app.editCam.slider;
    ImGui::SetCursorScreenPos({p0.x + 8, p0.y + 314});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3, 1});
    char zero[8];
    std::snprintf(zero, sizeof zero, "%c=0", AXIS[sa]);
    if (ImGui::SmallButton(zero)) app.setSliceW(0);
    sliderHovered |= ImGui::IsItemHovered();
    // which world axis is on the slider (the other three are shown)
    for (int a = 0; a < 4; a++) {
      ImGui::SetCursorScreenPos({p0.x + 8, p0.y + 338 + 21.f * a});
      char lb[4] = {AXIS[a], 0};
      ImGui::PushID(a);
      if (toggleButton(lb, a == sa)) app.setSliderAxis(a);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put %c on the slider (show the other three axes)", AXIS[a]);
      sliderHovered |= ImGui::IsItemHovered();
      ImGui::PopID();
    }
    ImGui::PopStyleVar();
    char sl[2] = {AXIS[sa], 0};
    const ImU32 slc[4] = {IM_COL32(224, 85, 85, 255), IM_COL32(95, 207, 95, 255), IM_COL32(85, 136, 238, 255), IM_COL32(200, 120, 230, 255)};
    dl->AddText({p0.x + 12, p0.y + 72}, slc[sa], sl);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", w);
    dl->AddText({p0.x + 44, p0.y + 90 + 220 * (1 - (w + range) / (2 * range)) - 7}, IM_COL32(220, 220, 220, 255), buf);
  }
  if ((app.geo || app.curved()) && app.editSet.showThumbnail) thumbnail();
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
  if (hovered) ImGui::SetTooltip("Drag to orbit, wheel to zoom");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1 = {p0.x + app.thumbW, p0.y + app.thumbH};
  if (uint64_t t = app.renderer ? app.renderer->texture(app.thumbRT) : 0) {
    bool fl = app.renderer->flipY();
    dl->AddImage((ImTextureID)t, p0, p1, {0, fl ? 1.f : 0.f}, {1, fl ? 0.f : 1.f});
  }
  // debug ray seen from outside (projected onto the thumbnail's view)
  if (app.debugRay && app.debugTrace.pts.size() > 1) {
    View tv = app.curved() ? app.ballView() : app.thumbnailView();
    double th = std::tan(40 * PI / 360), aspect = (double)app.thumbW / app.thumbH;
    std::vector<ImVec2> pts;
    for (auto& p0w : app.debugTrace.pts) {
      // curved: where the point sits in the ball (spherical: skip the far side, it runs off to infinity)
      if (app.curved() && 1 + p0w[3] < 0.05) continue;
      V4 p = app.curved() ? App::toBall(p0w) : p0w;
      V4 r = p - tv.eye;
      double z = dot(r, col(tv.frame, 2));
      if (z < 1e-3) continue;
      double x = dot(r, col(tv.frame, 0)) / (z * th), y = dot(r, col(tv.frame, 1)) / (z * th);
      pts.push_back({(float)(p0.x + (x / aspect + 1) * 0.5 * app.thumbW), (float)(p0.y + (1 - y) * 0.5 * app.thumbH)});
    }
    dl->PushClipRect(p0, p1, true);
    if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 140, 25, 255), 0, 2);
    dl->PopClipRect();
  }
  dl->AddRect(p0, p1, IM_COL32(230, 180, 60, 255), 0, 0, 2);
  dl->AddText({p0.x + 6, p0.y + 4}, IM_COL32(240, 240, 240, 255),
              !app.curved() ? "Outside view" : app.space.kind == SpaceKind::Hyperbolic ? "Poincare ball (all of space)" : "Stereographic view");
}

void UI::overlays(ImDrawList* dl) {
  if (app.job.active) {  // rendering: the viewport is paused
    char b[160];
    std::snprintf(b, sizeof b, "Rendering %s: %.0f%%  (viewport paused, Render window to cancel)", app.job.animation ? "animation" : "image",
                  app.renderProgress() * 100);
    ImVec2 ts = ImGui::CalcTextSize(b);
    float cx = app.vpX + app.vpW / 2, cy = app.vpY + app.vpH / 2;
    dl->AddRectFilled({cx - ts.x / 2 - 12, cy - ts.y / 2 - 10}, {cx + ts.x / 2 + 12, cy + ts.y / 2 + 10}, IM_COL32(0, 0, 0, 170), 6);
    dl->AddText({cx - ts.x / 2, cy - ts.y / 2}, IM_COL32(255, 220, 140, 255), b);
    return;
  }
  View v = app.currentView();
  V4 right = col(v.frame, 0), up = col(v.frame, 1), fwd = col(v.frame, 2), ana = col(v.frame, 3);
  float x0 = app.vpX, y0 = app.vpY;
  // status text
  std::vector<std::string> lines;
  char buf[256];
  // controls are listed in Help (and Settings > Keys); only the state and what needs an answer now is shown here
  if (app.curved()) {
    // position as normal coordinates (distance and direction from the origin), and how far that is
    V4 u = cs::logO(app.space, v.eye);
    std::snprintf(buf, sizeof buf, "%s | %s, R = %.2f | eye (%.2f, %.2f, %.2f), %.2f from the origin", app.mode == Mode::Edit ? "EDIT" : "EXPLORE",
                  SPACE_NAMES[(int)app.space.kind], app.space.radius, u[0], u[1], u[2], length(u));
  } else
    std::snprintf(buf, sizeof buf, "%s | %s | eye (%.2f, %.2f, %.2f, %.2f)", app.mode == Mode::Edit ? "EDIT" : "EXPLORE", app.geo ? "Geodesic 3D" : "Full 4D",
                  v.eye[0], v.eye[1], v.eye[2], v.eye[3]);
  lines.push_back(buf);
  int highlight = -1;
  if (app.accumSamples > 0 && app.accumSamples < app.quality.viewportSamples && app.mode == Mode::Edit) {
    std::snprintf(buf, sizeof buf, "Refining %d / %d", app.accumSamples, app.quality.viewportSamples);
    lines.push_back(buf);
  }
  if (!app.isolate.empty()) {
    std::snprintf(buf, sizeof buf, "Isolated: %d object(s) (%s: show all)", (int)app.isolate.size(), app.keys.text(Action::Isolate).c_str());
    lines.push_back(buf);
  }
  if (app.mode == Mode::Explore) {
    if (!app.mouseCaptured) lines.push_back("Click the viewport to capture the mouse");
    if (!app.anim.empty()) {
      std::snprintf(buf, sizeof buf, "Animation: frame %d / %d %s (%s: play / pause, %s: from the start)", (int)std::round(app.anim.frame),
                    (int)std::round(app.playbackEnd()),
                    app.playing ? "playing" : "paused", app.keys.text(Action::ExplorePlay).c_str(), app.keys.text(Action::ExploreRestart).c_str());
      lines.push_back(buf);
    }
    if (app.debugRay) {
      const auto& d = app.debugTrace;
      std::snprintf(buf, sizeof buf, "Debug ray%s: length %.2f, %d steps, stopped: %s", app.debugFrozen ? " (frozen)" : "", d.t, d.steps,
                    rayReasonText(d.reason));
      lines.push_back(buf);
    }
  } else if (app.modal != Modal::None) {
    highlight = (int)lines.size();
    lines.push_back(app.transformHint());
  }
  float ty = y0 + 8;
  float tx = x0 + (app.mode == Mode::Edit && !app.geo ? 60 : 8);
  for (size_t i = 0; i < lines.size(); i++) {
    ImVec2 ts = ImGui::CalcTextSize(lines[i].c_str());
    dl->AddRectFilled({tx - 4, ty - 1}, {tx + ts.x + 4, ty + ts.y + 1}, IM_COL32(0, 0, 0, 130));
    dl->AddText({tx, ty}, (int)i == highlight ? IM_COL32(255, 180, 70, 255) : IM_COL32(232, 232, 232, 255), lines[i].c_str());
    ty += ts.y + 4;
  }

  // 4D axis gizmo
  float cx = x0 + 60, cy = y0 + app.vpH - 60, R = 40;
  dl->AddCircleFilled({cx, cy}, R + 14, IM_COL32(20, 22, 28, 150));
  const ImU32 ac[4] = {IM_COL32(224, 85, 85, 255), IM_COL32(95, 207, 95, 255), IM_COL32(85, 136, 238, 255), IM_COL32(192, 96, 224, 255)};
  // curved: the world axes carried from the origin to the eye, in the camera's frame (3 axes)
  V4 axv[4] = {unit(0), unit(1), unit(2), unit(3)};
  int nAx = 4;
  if (app.curved()) {
    double k = app.space.k();
    M4 T = cs::transvection(k, cs::origin(), v.eye), inv = cs::inverse(k, v.frame);
    for (int i = 0; i < 3; i++) {
      V4 l = mul(inv, mul(T, unit(i)));  // components along right, up, forward
      axv[i] = {l[0], l[1], l[2], 0};
    }
    right = unit(0); up = unit(1); fwd = unit(2); ana = unit(3);
    nAx = 3;
  }
  int order[4] = {0, 1, 2, 3};
  std::sort(order, order + nAx, [&](int a, int b) { return dot(axv[a], fwd) > dot(axv[b], fwd); });
  for (int oi = 0; oi < nAx; oi++) {
    int i = order[oi];
    V4 e = axv[i];
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

  // camera objects: small frustums (orange = active camera; a triangle marks "up")
  walk(app.root, [&](Node& n, Node*, int) {
    if (!n.isCamera() || n.hidden) return;
    if (!app.isolate.empty() && !app.isolate.count(n.id)) return;
    double cf = 60;
    View cv = app.cameraView(n.id, nullptr, &cf);
    double s = 0.55, b = s * std::tan(cf * PI / 360), a2 = b * 1.5;
    V4 R = col(cv.frame, 0), U = col(cv.frame, 1), F = col(cv.frame, 2);
    V4 pts[7] = {cv.eye, app.along(cv.eye, F * s + R * a2 + U * b), app.along(cv.eye, F * s - R * a2 + U * b), app.along(cv.eye, F * s - R * a2 - U * b),
                 app.along(cv.eye, F * s + R * a2 - U * b), app.along(cv.eye, F * s + U * (b * 1.15) + R * (a2 * 0.4)), app.along(cv.eye, F * s + U * (b * 1.6))};
    ImVec2 sp[7];
    for (int i = 0; i < 7; i++) {
      double x, y;
      if (!app.project(pts[i], v, x, y)) return;
      sp[i] = {(float)x, (float)y};
    }
    bool sel = std::find(app.selection.begin(), app.selection.end(), n.id) != app.selection.end();
    ImU32 c = n.id == app.activeCamera ? IM_COL32(255, 170, 60, 230) : IM_COL32(200, 200, 210, 200);
    if (sel) c = IM_COL32(255, 235, 140, 255);
    float th = sel ? 2.f : 1.3f;
    for (int i = 1; i <= 4; i++) {
      dl->AddLine(sp[0], sp[i], c, th);
      dl->AddLine(sp[i], sp[i % 4 + 1], c, th);
    }
    double lx, ly, rx, ry;
    if (app.project(app.along(cv.eye, F * s + U * (b * 1.15) - R * (a2 * 0.4)), v, lx, ly) && app.project(app.along(cv.eye, F * s + U * (b * 1.15) + R * (a2 * 0.4)), v, rx, ry))
      dl->AddTriangle({(float)lx, (float)ly}, {(float)rx, (float)ry}, sp[6], c, th);
    dl->AddCircleFilled(sp[0], 3, c);
  });

  // centre of rotation (set with a middle-click)
  if (!app.geo && !app.curved() && app.editCam.pivotSet) {
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
    // colour / letter of the axis each handle moves along (local X/Y/Z, or the world axis in Global)
    const ImU32 cols[4] = {IM_COL32(230, 80, 80, 255), IM_COL32(100, 210, 100, 255), IM_COL32(80, 130, 240, 255), IM_COL32(200, 110, 230, 255)};
    ImVec2 c = {(float)g.cx, (float)g.cy};
    auto bright = [](ImU32 col) { return col | IM_COL32(60, 60, 60, 0); };
    if (app.tool == Tool::Move) {
      for (int a = 0; a < 3; a++) {
        auto& ax = g.axes[a];
        if (!ax.visible) continue;
        bool hot = g.hot == a;
        ImU32 col = hot ? IM_COL32(255, 230, 120, 255) : cols[ax.axis];
        if (ax.faded && !hot) col = (col & 0x00FFFFFF) | IM_COL32(0, 0, 0, 110);  // points out of the visible slice
        ImVec2 e = {(float)ax.ex, (float)ax.ey};
        float dx = e.x - c.x, dy = e.y - c.y, l = std::sqrt(dx * dx + dy * dy);
        if (l < 1) continue;
        float ux = dx / l, uy = dy / l;
        dl->AddLine(c, {e.x - ux * 10, e.y - uy * 10}, col, hot ? 4.f : 3.f);
        dl->AddTriangleFilled(e, {e.x - ux * 14 - uy * 6, e.y - uy * 14 + ux * 6}, {e.x - ux * 14 + uy * 6, e.y - uy * 14 - ux * 6}, col);
        char l1[2] = {AXIS[ax.axis], 0};
        dl->AddText({e.x + ux * 6 - 4, e.y + uy * 6 - 7}, col, l1);
      }
      dl->AddRectFilled({c.x - 6, c.y - 6}, {c.x + 6, c.y + 6}, g.hot == 4 ? IM_COL32(255, 230, 120, 255) : IM_COL32(230, 230, 230, 200), 2);
    } else {
      for (size_t r = 0; r < g.rings.size(); r++) {
        auto& ring = g.rings[r];
        bool hot = g.hot == 10 + (int)r;
        // ring colour = the local axis it rotates about (YZ -> X red, XZ -> Y green, XY -> Z blue)
        int about = g.axes[3 - ring.a - ring.b].axis;
        ImU32 col = hot ? IM_COL32(255, 230, 120, 255) : bright(cols[about]);
        std::vector<ImVec2> pts;
        for (auto& p : ring.pts) pts.push_back({(float)p.first, (float)p.second});
        dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, hot ? 3.5f : 2.f);
        if (hot) {
          char lbl[8];
          std::snprintf(lbl, sizeof lbl, "%c%c", AXIS[ring.la], AXIS[ring.lb]);
          dl->AddText({pts[8].x + 6, pts[8].y - 6}, col, lbl);
        }
      }
      dl->AddCircleFilled(c, 3, IM_COL32(230, 230, 230, 220));
    }
  }
}

}  // namespace fd
