#include "keybinds.hpp"

#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

namespace fd {

// groups: actions in the same group (or anything vs "General") must not share a key
static const ActionInfo INFO[(int)Action::Count] = {
    {"undo", "Undo", "General", false},
    {"redo", "Redo", "General", false},
    {"save", "Save", "General", false},
    {"saveAs", "Save as", "General", false},
    {"open", "Open", "General", false},
    {"new", "New scene", "General", false},
    {"copy", "Copy", "General", false},
    {"paste", "Paste", "General", false},
    {"toggleMode", "Edit / Explore", "General", false},
    {"toggleView", "Full 4D / Geodesic", "Edit", false},
    {"toolSelect", "Select tool", "Edit", false},
    {"toolMove", "Move tool", "Edit", false},
    {"toolRotate", "Rotate tool", "Edit", false},
    {"toggleGlobal", "Local / Global axes", "Edit", false},
    {"toggleSnap", "Snapping on / off", "Edit", false},
    {"grab", "Grab (G)", "Edit", false},
    {"rotate", "Rotate (R)", "Edit", false},
    {"scale", "Scale (S)", "Edit", false},
    {"delete", "Delete", "Edit", false},
    {"duplicate", "Duplicate", "Edit", false},
    {"duplicateLinked", "Duplicate linked", "Edit", false},
    {"group", "Group selection", "Edit", false},
    {"isolate", "Isolate selection / show all", "Edit", false},
    {"focus", "Focus selection", "Edit", false},
    {"deselect", "Deselect", "Edit", false},
    {"selectAll", "Select all", "Edit", false},
    {"viewFront", "Front view", "Edit", false},
    {"viewRight", "Right view", "Edit", false},
    {"viewTop", "Top view", "Edit", false},
    {"viewAlongW", "View along W", "Edit", false},
    {"reset4D", "Reset 4D orientation", "Edit", false},
    {"lookThrough", "Look through camera", "Edit", false},
    {"cameraToView", "Camera to view", "Edit", false},
    {"cameraView", "View through the active camera", "Edit", false},
    {"playPause", "Play / pause", "Edit", false},
    {"frameStart", "First frame", "Edit", false},
    {"frameEnd", "Last frame", "Edit", false},
    {"framePrev", "Previous frame", "Edit", false},
    {"frameNext", "Next frame", "Edit", false},
    {"insertKey", "Insert keyframe", "Edit", false},
    {"deleteKey", "Delete keyframe", "Edit", false},
    {"axisX", "Constrain X", "Transform", false},
    {"axisY", "Constrain Y", "Transform", false},
    {"axisZ", "Constrain Z", "Transform", false},
    {"axisW", "Constrain W", "Transform", false},
    {"confirm", "Confirm", "Transform", false},
    {"cancel", "Cancel", "Transform", false},
    {"releaseMouse", "Release mouse / back to Edit", "Explore", false},
    {"debugRay", "Debug ray on / off", "Explore", false},
    {"debugRayFreeze", "Debug ray: freeze / follow view", "Explore", false},
    {"explorePlay", "Play / pause the animation", "Explore", false},
    {"exploreRestart", "Play the animation from the start", "Explore", false},
    {"moveForward", "Forward", "Movement", true},
    {"moveBack", "Back", "Movement", true},
    {"moveLeft", "Left", "Movement", true},
    {"moveRight", "Right", "Movement", true},
    {"moveUp", "Up", "Movement", true},
    {"moveDown", "Down", "Movement", true},
    {"rollLeft", "Roll left", "Movement", true},
    {"rollRight", "Roll right", "Movement", true},
    {"moveAnaPlus", "Along slider axis +", "Movement", true},
    {"moveAnaMinus", "Along slider axis -", "Movement", true},
    {"turnWPlus", "Turn into W (forward)", "Movement", true},
    {"turnWMinus", "Turn into W (back)", "Movement", true},
    {"sideWPlus", "Turn into W (sideways +)", "Movement", true},
    {"sideWMinus", "Turn into W (sideways -)", "Movement", true},
};

const ActionInfo& actionInfo(Action a) { return INFO[(int)a]; }

void KeyBindings::resetDefaults() {
  for (auto& b : b_) b = {KeyCombo{}, KeyCombo{}};
  auto set = [&](Action a, KeyCombo c1, KeyCombo c2 = {}) { b_[(int)a] = {c1, c2}; };
  auto K = [](int k) { return KeyCombo{k}; };
  auto C = [](int k) { return KeyCombo{k, true}; };
  auto S = [](int k) { return KeyCombo{k, false, true}; };
  auto CS = [](int k) { return KeyCombo{k, true, true}; };
  set(Action::Undo, C(GLFW_KEY_Z));
  set(Action::Redo, CS(GLFW_KEY_Z), C(GLFW_KEY_Y));
  set(Action::Save, C(GLFW_KEY_S));
  set(Action::SaveAs, CS(GLFW_KEY_S));
  set(Action::Open, C(GLFW_KEY_O));
  set(Action::New, C(GLFW_KEY_N));
  set(Action::Copy, C(GLFW_KEY_C));
  set(Action::Paste, C(GLFW_KEY_V));
  set(Action::ToggleMode, K(GLFW_KEY_TAB));
  set(Action::ToggleView, K(GLFW_KEY_V));
  set(Action::ToolSelect, K(GLFW_KEY_Q));
  set(Action::ToolMove, K(GLFW_KEY_W));
  set(Action::ToolRotate, K(GLFW_KEY_E));
  set(Action::ToggleGlobal, K(GLFW_KEY_L));
  set(Action::ToggleSnap, S(GLFW_KEY_TAB));
  set(Action::Grab, K(GLFW_KEY_G));
  set(Action::Rotate, K(GLFW_KEY_R));
  set(Action::Scale, K(GLFW_KEY_S));
  set(Action::Delete, K(GLFW_KEY_X), K(GLFW_KEY_DELETE));
  set(Action::Duplicate, S(GLFW_KEY_D));
  set(Action::DuplicateLinked, KeyCombo{GLFW_KEY_D, false, false, true});
  set(Action::Isolate, K(GLFW_KEY_KP_DIVIDE), K(GLFW_KEY_SLASH));
  set(Action::Group, C(GLFW_KEY_G));
  set(Action::Focus, K(GLFW_KEY_F), K(GLFW_KEY_KP_DECIMAL));
  set(Action::Deselect, K(GLFW_KEY_ESCAPE));
  set(Action::SelectAll, K(GLFW_KEY_A));
  set(Action::ViewFront, K(GLFW_KEY_1), K(GLFW_KEY_KP_1));
  set(Action::ViewRight, K(GLFW_KEY_3), K(GLFW_KEY_KP_3));
  set(Action::ViewTop, K(GLFW_KEY_7), K(GLFW_KEY_KP_7));
  set(Action::ViewAlongW, K(GLFW_KEY_9), K(GLFW_KEY_KP_9));
  set(Action::Reset4D, S(GLFW_KEY_W), K(GLFW_KEY_BACKSPACE));
  set(Action::LookThrough, K(GLFW_KEY_KP_0), K(GLFW_KEY_0));
  set(Action::CameraToView, KeyCombo{GLFW_KEY_KP_0, true, false, true}, KeyCombo{GLFW_KEY_0, true, false, true});
  set(Action::CameraView, C(GLFW_KEY_KP_0), C(GLFW_KEY_0));
  set(Action::PlayPause, K(GLFW_KEY_SPACE));
  set(Action::FrameStart, S(GLFW_KEY_LEFT));
  set(Action::FrameEnd, S(GLFW_KEY_RIGHT));
  set(Action::FramePrev, K(GLFW_KEY_LEFT));
  set(Action::FrameNext, K(GLFW_KEY_RIGHT));
  set(Action::InsertKey, K(GLFW_KEY_I));
  set(Action::DeleteKey, KeyCombo{GLFW_KEY_I, false, false, true});
  set(Action::AxisX, K(GLFW_KEY_X));
  set(Action::AxisY, K(GLFW_KEY_Y));
  set(Action::AxisZ, K(GLFW_KEY_Z));
  set(Action::AxisW, K(GLFW_KEY_W));
  set(Action::Confirm, K(GLFW_KEY_ENTER), K(GLFW_KEY_KP_ENTER));
  set(Action::Cancel, K(GLFW_KEY_ESCAPE));
  set(Action::ReleaseMouse, K(GLFW_KEY_ESCAPE));
  set(Action::DebugRay, K(GLFW_KEY_T));
  set(Action::DebugRayFreeze, S(GLFW_KEY_T));
  set(Action::ExplorePlay, K(GLFW_KEY_P));
  set(Action::ExploreRestart, S(GLFW_KEY_P));
  set(Action::MoveForward, K(GLFW_KEY_W));
  set(Action::MoveBack, K(GLFW_KEY_S));
  set(Action::MoveLeft, K(GLFW_KEY_A));
  set(Action::MoveRight, K(GLFW_KEY_D));
  set(Action::MoveUp, K(GLFW_KEY_SPACE));
  set(Action::MoveDown, K(GLFW_KEY_LEFT_SHIFT), K(GLFW_KEY_RIGHT_SHIFT));
  set(Action::RollLeft, K(GLFW_KEY_Q));
  set(Action::RollRight, K(GLFW_KEY_E));
  set(Action::MoveAnaPlus, K(GLFW_KEY_R));
  set(Action::MoveAnaMinus, K(GLFW_KEY_F));
  set(Action::TurnWPlus, K(GLFW_KEY_Z));
  set(Action::TurnWMinus, K(GLFW_KEY_X));
  set(Action::SideWPlus, K(GLFW_KEY_C));
  set(Action::SideWMinus, K(GLFW_KEY_V));
}

bool KeyBindings::pressed(Action a, int key, bool ctrl, bool shift, bool alt) const {
  for (auto& c : b_[(int)a])
    if (c.bound() && c.key == key && c.ctrl == ctrl && c.shift == shift && c.alt == alt) return true;
  return false;
}

std::string keyName(int k) {
  if ((k >= GLFW_KEY_A && k <= GLFW_KEY_Z) || (k >= GLFW_KEY_0 && k <= GLFW_KEY_9)) return std::string(1, (char)k);
  if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F25) return "F" + std::to_string(k - GLFW_KEY_F1 + 1);
  if (k >= GLFW_KEY_KP_0 && k <= GLFW_KEY_KP_9) return "Num " + std::to_string(k - GLFW_KEY_KP_0);
  switch (k) {
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_APOSTROPHE: return "'";
    case GLFW_KEY_COMMA: return ",";
    case GLFW_KEY_MINUS: return "-";
    case GLFW_KEY_PERIOD: return ".";
    case GLFW_KEY_SLASH: return "/";
    case GLFW_KEY_SEMICOLON: return ";";
    case GLFW_KEY_EQUAL: return "=";
    case GLFW_KEY_LEFT_BRACKET: return "[";
    case GLFW_KEY_BACKSLASH: return "\\";
    case GLFW_KEY_RIGHT_BRACKET: return "]";
    case GLFW_KEY_GRAVE_ACCENT: return "`";
    case GLFW_KEY_ESCAPE: return "Esc";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_INSERT: return "Insert";
    case GLFW_KEY_DELETE: return "Del";
    case GLFW_KEY_RIGHT: return "Right";
    case GLFW_KEY_LEFT: return "Left";
    case GLFW_KEY_DOWN: return "Down";
    case GLFW_KEY_UP: return "Up";
    case GLFW_KEY_PAGE_UP: return "PgUp";
    case GLFW_KEY_PAGE_DOWN: return "PgDn";
    case GLFW_KEY_HOME: return "Home";
    case GLFW_KEY_END: return "End";
    case GLFW_KEY_KP_DECIMAL: return "Num .";
    case GLFW_KEY_KP_DIVIDE: return "Num /";
    case GLFW_KEY_KP_MULTIPLY: return "Num *";
    case GLFW_KEY_KP_SUBTRACT: return "Num -";
    case GLFW_KEY_KP_ADD: return "Num +";
    case GLFW_KEY_KP_ENTER: return "Num Enter";
    case GLFW_KEY_LEFT_SHIFT: return "Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    default: return "Key " + std::to_string(k);
  }
}

std::string KeyBindings::comboText(const KeyCombo& c) {
  if (!c.bound()) return "";
  std::string s;
  if (c.ctrl) s += "Ctrl+";
  if (c.shift) s += "Shift+";
  if (c.alt) s += "Alt+";
  return s + keyName(c.key);
}

std::string KeyBindings::text(Action a, bool both) const {
  auto& b = b_[(int)a];
  std::string t = comboText(b[0].bound() ? b[0] : b[1]);
  if (both && b[0].bound() && b[1].bound()) t += " / " + comboText(b[1]);
  return t.empty() ? "unbound" : t;
}

std::vector<Action> KeyBindings::conflicts(Action a) const {
  std::vector<Action> out;
  const ActionInfo& ia = INFO[(int)a];
  for (int o = 0; o < (int)Action::Count; o++) {
    if (o == (int)a) continue;
    const ActionInfo& io = INFO[o];
    bool sameContext = std::strcmp(ia.group, io.group) == 0 || std::strcmp(ia.group, "General") == 0 || std::strcmp(io.group, "General") == 0 ||
                       // Explore actions and movement keys are active at the same time
                       ((std::strcmp(ia.group, "Explore") == 0 || std::strcmp(ia.group, "Movement") == 0) &&
                        (std::strcmp(io.group, "Explore") == 0 || std::strcmp(io.group, "Movement") == 0));
    if (!sameContext) continue;
    bool hit = false;
    for (auto& x : b_[(int)a])
      for (auto& y : b_[o])
        if (x.bound() && y.bound()) {
          if (ia.held && io.held) hit |= x.key == y.key;
          else if (ia.held || io.held) {  // a held key vs a shortcut: clash unless the shortcut needs Ctrl / Alt
            const KeyCombo& press = ia.held ? y : x;
            hit |= x.key == y.key && !press.ctrl && !press.alt;
          } else hit |= x == y;
        }
    if (hit) out.push_back((Action)o);
  }
  return out;
}

void KeyBindings::toJson(nlohmann::json& j) const {
  j = nlohmann::json::object();
  for (int a = 0; a < (int)Action::Count; a++) {
    auto arr = nlohmann::json::array();
    for (auto& c : b_[a]) arr.push_back({{"key", c.key}, {"ctrl", c.ctrl}, {"shift", c.shift}, {"alt", c.alt}});
    j[INFO[a].id] = arr;
  }
}

void KeyBindings::fromJson(const nlohmann::json& j) {
  if (!j.is_object()) return;
  for (int a = 0; a < (int)Action::Count; a++) {
    auto it = j.find(INFO[a].id);
    if (it == j.end() || !it->is_array()) continue;  // new actions keep their defaults
    for (size_t k = 0; k < 2; k++) {
      KeyCombo c;
      if (k < it->size() && (*it)[k].is_object()) {
        auto& o = (*it)[k];
        c.key = o.value("key", -1);
        c.ctrl = o.value("ctrl", false);
        c.shift = o.value("shift", false);
        c.alt = o.value("alt", false);
      }
      b_[a][k] = c;
    }
  }
}

}  // namespace fd
