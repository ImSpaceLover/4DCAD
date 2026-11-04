// Rebindable keyboard shortcuts. Every action has up to two key combos (key + modifiers).
// "Press" actions fire once on a key press with exactly matching modifiers; "held" actions
// (movement) are polled every frame and ignore modifiers (Shift itself can be a held key).
#pragma once
#include <array>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace fd {

enum class Action {
  // file / edit (Edit and Explore)
  Undo, Redo, Save, SaveAs, Open, New, Copy, Paste, RenderImage, RenderAnimation,
  ToggleMode, ToggleView,
  // Edit mode
  ToolSelect, ToolMove, ToolRotate, ToggleGlobal, ToggleSnap,
  Grab, Rotate, Scale, Delete, Duplicate, DuplicateLinked, Group, Isolate, Focus, Deselect, SelectAll,
  ViewFront, ViewRight, ViewTop, ViewAlongW, Reset4D, LookThrough, CameraToView, CameraView,
  // animation (Edit mode)
  PlayPause, FrameStart, FrameEnd, FramePrev, FrameNext, InsertKey, DeleteKey,
  // during G / R / S
  AxisX, AxisY, AxisZ, AxisW, Confirm, Cancel,
  // Explore
  ReleaseMouse, DebugRay, DebugRayFreeze, ExplorePlay, ExploreRestart,
  // held (Explore and Geodesic fly)
  MoveForward, MoveBack, MoveLeft, MoveRight, MoveUp, MoveDown, RollLeft, RollRight,
  MoveAnaPlus, MoveAnaMinus, TurnWPlus, TurnWMinus, SideWPlus, SideWMinus,
  Count
};

struct KeyCombo {
  int key = -1;  // GLFW key code, -1 = unbound
  bool ctrl = false, shift = false, alt = false;
  bool bound() const { return key >= 0; }
  bool operator==(const KeyCombo& o) const { return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt; }
};

struct ActionInfo {
  const char* id;        // stable name in settings.json
  const char* label;     // shown in Settings > Keys
  const char* group;     // section
  bool held;             // polled every frame (movement), modifiers ignored
};
const ActionInfo& actionInfo(Action a);

/** Name of a GLFW key code ("A", "Space", "Num 1", ...). */
std::string keyName(int key);

class KeyBindings {
 public:
  KeyBindings() { resetDefaults(); }
  void resetDefaults();
  std::array<KeyCombo, 2>& at(Action a) { return b_[(int)a]; }
  const std::array<KeyCombo, 2>& at(Action a) const { return b_[(int)a]; }
  /** A key press with these modifiers triggers action a. */
  bool pressed(Action a, int key, bool ctrl, bool shift, bool alt) const;
  /** Held action: any of its keys is down. */
  template <class F> bool held(Action a, const F& keyDown) const {
    for (auto& c : b_[(int)a]) if (c.bound() && keyDown(c.key)) return true;
    return false;
  }
  /** "Shift+D" (first binding), or "Shift+D / Del" with both. */
  std::string text(Action a, bool both = false) const;
  static std::string comboText(const KeyCombo& c);
  /** Actions that share a combo with a in the same context (to warn about conflicts). */
  std::vector<Action> conflicts(Action a) const;
  void toJson(nlohmann::json& j) const;
  void fromJson(const nlohmann::json& j);

 private:
  std::array<std::array<KeyCombo, 2>, (int)Action::Count> b_;
};

}  // namespace fd
