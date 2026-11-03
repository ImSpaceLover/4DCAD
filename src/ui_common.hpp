// Shared helpers of the UI implementation files (ui*.cpp). Not part of the public UI interface.
#pragma once
#include <cstring>
#include <filesystem>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "examples.hpp"
#include "filedialog.hpp"
#include "imgui.h"
#include "ui.hpp"

namespace fd {

inline const char* DIM_LABEL[5] = {"0D", "1D", "2D", "3D", "4D"};
inline const ImVec4 DIM_COL[5] = {{0.4f, 0.4f, 0.4f, 1}, {0.4f, 0.4f, 0.4f, 1}, {0.25f, 0.35f, 0.5f, 1}, {0.2f, 0.45f, 0.32f, 1}, {0.45f, 0.27f, 0.62f, 1}};
inline const char* OP_ICON[6] = {"U", "I", "-", "~U", "~I", "~-"};
inline const float LEFT_W = 250, RIGHT_W = 340, TOOLBAR_H = 38;

struct LibPayload { char id[96]; };

inline ImTextureID tex(GLuint t) { return (ImTextureID)(intptr_t)t; }

inline bool toggleButton(const char* label, bool active, const char* tip = nullptr) {
  if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.5f, 0.15f, 1));
  bool r = ImGui::Button(label);
  if (active) ImGui::PopStyleColor();
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  return r;
}

}  // namespace fd
