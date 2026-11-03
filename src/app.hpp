// Application state and logic (no ImGui here): scene + undo, cameras, picking,
// placement, gizmo / transform tools, explore movement, frame rendering.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "camera.hpp"
#include "evaluator.hpp"
#include "keybinds.hpp"
#include "library.hpp"
#include "renderer.hpp"
#include "surface.hpp"

namespace fd {

enum class Mode { Edit, Explore };
enum class Tool { Select, Move, Rotate };
enum class Modal { None, Grab, Rotate, Scale };

/** Abstract per-frame input, filled by the UI layer from GLFW / ImGui. */
struct InputState {
  bool vpHovered = false;
  double mx = 0, my = 0, dx = 0, dy = 0;
  bool down[3] = {false, false, false}, pressed[3] = {false, false, false}, released[3] = {false, false, false};  // left, right, middle
  double wheel = 0;
  bool shift = false, ctrl = false, alt = false;
  bool keysEnabled = true;
  std::function<bool(int)> keyDown;  // GLFW key code -> held?
};

/** Edit mode settings (Full 4D + Geodesic sub-settings). */
struct EditSettings {
  double fov = 60;
  // Full 4D
  double orbitSpeed = 1, panSpeed = 1, zoomSpeed = 1, rot4Speed = 1;
  bool showWSlider = true;
  // Geodesic
  double flySpeed = 2, lookSpeed = 1;
  bool showThumbnail = true;
  int thumbSize = 300;
  double autosaveSeconds = 60;  // recovery copy of unsaved changes (0 = off)
  // transform tools
  bool globalAxes = false;  // gizmo / G / R axes: false = the object's local axes, true = world axes
  bool snap = false;        // snapping (Ctrl while transforming inverts it)
  double snapMove = 0.25;   // grid step (world units)
  double snapAngle = 15;    // degrees
  double snapScale = 0.1;
};

/** Explore mode settings (Full 4D + Geodesic sub-settings). */
struct ExploreSettings {
  double fov = 70, lookSpeed = 1;
  bool showOwnBody = true;
  double bodyRadius = 0.22;
  // Full 4D
  double moveSpeed = 3, turn4Speed = 1;
  bool collide = true;
  // Geodesic
  double geoMoveSpeed = 2;
  bool geoCollide = true;
};

/** Screen-space gizmo geometry for drawing and hit testing. */
struct Gizmo {
  bool visible = false;
  double cx = 0, cy = 0;           // centre (window pixels)
  struct Axis { bool visible = false; int axis = 0; V4 dir{}; double ex = 0, ey = 0; bool faded = false; };  // axis: label (X..W); faded: leaves the slice
  Axis axes[3];                    // the object's local X, Y, Z, straightened into the visible slice
  struct Ring { int a = 0, b = 0; V4 u{}, v{}; std::vector<std::pair<double, double>> pts; double orient = 1; int la = 0, lb = 0; };  // la/lb: axis labels
  std::vector<Ring> rings;         // rotation planes between visible axes
  double worldLen = 1;             // world length of an arrow
  int hot = -1;                    // hovered handle: 0..2 arrows, 4 centre, 10+ rings
};

class App {
 public:
  App();
  // --- scene ---
  Node root;
  PrimitiveRegistry reg;
  LibraryStore lib;
  std::string world;
  std::vector<std::string> selection;
  std::unique_ptr<SceneEvaluator> ev;
  std::string currentFile;

  // --- modes / cameras ---
  Mode mode = Mode::Edit;
  bool geo = false;
  Tool tool = Tool::Select;
  // one camera shared by Edit and Explore (switching modes never moves you)
  FullCamera editCam;
  GeoCamera geoEdit;  // geodesic camera (first-person; Edit: hold RMB to fly)
  QualitySettings quality;  // rendering
  EditSettings editSet;
  ExploreSettings exploreSet;
  double fov() const { return mode == Mode::Explore ? exploreSet.fov : editSet.fov; }
  V4 light() const;
  double thumbYaw = 0.4, thumbPitch = -0.9, thumbZoom = 1;
  bool mouseCaptured = false, wantCapture = false, wantRelease = false;

  // --- viewport (window pixels) ---
  float vpX = 0, vpY = 0, vpW = 1, vpH = 1;
  RenderTarget mainRT, thumbRT;
  int thumbW = 300, thumbH = 225;
  std::unique_ptr<Renderer> renderer;
  Backend backendPref = Backend::Auto;  // graphics API for the next start (settings)
  bool showLights = false;              // Lights window
  double fps = 0, now = 0;

  // --- lifecycle ---
  void init(std::unique_ptr<Renderer> r);
  void loadScene(SceneFile f);
  bool loadSceneFile(const std::string& path, std::string& err);
  bool saveSceneFile(const std::string& path, std::string& err);
  std::string serialize();
  void newScene();
  void rebuild();
  void commit();
  void undo();
  void redo();
  bool canUndo() const { return !history_.empty(); }
  bool canRedo() const { return !future_.empty(); }
  /** Unsaved changes since the scene was loaded / saved. */
  bool dirty() const { return committed_ != saved_; }
  std::string lastFile;  // reopened on startup
  void loadSettings();
  void saveSettings();
  /** The graphics API chosen in the settings (read before the window is created). */
  static Backend savedBackend();
  // --- autosave / crash recovery ---
  /** Writes the recovery copy if there are unsaved changes and the interval has passed (force: now). */
  void autosaveTick(bool force = false);
  /** Deletes the recovery copy (after saving, or when quitting normally). */
  void clearRecovery();
  /** A recovery copy left by a crash: the file it belonged to and when it was written. */
  struct RecoveryInfo { std::string file, time; };
  std::optional<RecoveryInfo> findRecovery();
  bool recover(std::string& err);
  std::optional<RecoveryInfo> pendingRecovery;  // found at startup: the UI asks what to do

  // --- selection & tree ---
  Node* selected();
  std::unordered_set<std::string> highlightSet();
  void deleteSelection();
  void duplicateSelection();
  /** Linked duplicates: the copies share their definition (editing one edits all). */
  void duplicateLinked();
  void unlink(const std::string& id);
  int linkCount(const std::string& linkId);
  /** Clipboard text (JSON) of the selected objects, with their world transforms. */
  std::string copySelection();
  /** Adds objects from clipboard text to the scene root (new ids); false if it is not 4DCAD data. */
  bool pasteNodes(const std::string& text);
  bool isLocked(const std::string& id);  // the node or an ancestor is locked
  // Isolate (view only, not saved): only these objects (and the world) are drawn and pickable
  std::unordered_set<std::string> isolate;
  void toggleIsolate();
  // camera bookmarks (saved in the scene)
  std::vector<CameraBookmark> bookmarks;
  std::vector<PointLight> lights;  // scene point lights (saved in the scene)
  /** The point lights for a frame: enabled scene lights, then emissive objects (if enabled). */
  std::vector<LightParam> frameLights();
  void addBookmark(const std::string& name = "");
  void gotoBookmark(size_t i);
  void updateBookmark(size_t i);
  void groupSelection(BoolOp op);
  void ungroup(const std::string& id);
  /** Moves a group's origin to its first object, without moving anything. */
  void recentreGroup(const std::string& id);
  enum class Drop { Into, Before, After, Wrap };
  void reparent(const std::string& id, const std::string& target, Drop where);
  void setWorld(const std::string& id);
  void attachToWorld(const std::string& id);
  void detach(const std::string& id);
  V4 nodeWorldPos(const std::string& id);
  M4 nodeWorldFrame(const std::string& id);  // columns = the object's X, Y, Z, W axes in world space

  // --- modes & views ---
  void setMode(Mode m);
  void setGeo(bool g);
  View currentView();
  View playerView();
  double tanHalf() const { return std::tan(fov() * PI / 360); }
  V4 screenRay(double x, double y, const View& v) const;
  bool project(const V4& p, const View& v, double& sx, double& sy) const;
  std::optional<RayHit> pick(double x, double y);
  void focusSelection();
  void standardView(int which);
  /** Full 4D: put world axis s (0..3 = X..W) on the slice slider; the other three are shown. */
  void setSliderAxis(int s);
  /** World axis (0..3) shown as gizmo slot k (0..2) in Global mode. */
  int globalAxisLabel(int k) const;

  // --- placement (library objects) ---
  bool canPlace(const std::string& objectId, std::string* reason);
  int itemDim(const std::string& objectId);
  void placeItem(const std::string& objectId, std::optional<std::pair<double, double>> screen);

  // --- transforms ---
  Modal modal = Modal::None;
  std::vector<int> modalAxes;  // object axes (0..3) for G / R constraints
  std::string transformHint() const;
  void startModal(Modal m);
  void confirmModal();
  void cancelModal();
  void translateNodes(const std::vector<std::string>& ids, const V4& delta);
  void rotateNodes(const std::vector<std::string>& ids, const M4& q);
  /** Gizmo for the current selection (Move / Rotate tool), in screen space. */
  Gizmo gizmo();
  bool gizmoDragging() const { return gizmoHandle_ >= 0; }
  /** Snapping is in effect for the current transform (setting, inverted while Ctrl is held). */
  bool snapping() const { return snapNow_; }
  bool flying() const { return drag_ == Drag::Fly; }
  int gizmoHandle() const { return gizmoHandle_; }

  // --- per frame ---
  void handleInput(const InputState& in, double dt);
  void handleKey(int key, bool shift, bool ctrl, bool alt = false);
  KeyBindings keys;      // rebindable shortcuts (saved with the settings)
  bool debugRay = false; // Explore: show the debug ray
  bool debugFrozen = false;  // keep the current debug ray instead of re-tracing it from the view
  RayTrace debugTrace;       // the debug ray (traced from the eye along the view direction)
  void renderFrame();
  View thumbnailView();
  double sliceW() { return editCam.sliceW(); }
  void setSliceW(double w) { editCam.setSliceW(w); }
  std::array<float, 3> worldColor();
  void toast(const std::string& msg, bool error = false);
  std::string toastMsg;
  bool toastError = false;
  double toastUntil = 0;

 private:
  std::vector<std::string> history_, future_;
  std::string committed_, saved_, autosaved_;
  std::map<std::string, std::string> linkDefs_;  // last known definition of each linked group
  Node viewRoot_;                                 // the tree as drawn while isolating
  double lastAutosave_ = 0;
  std::vector<std::string> selectionAtPress_;
  int version_ = 0;
  std::string lastKey_, lastThumbKey_;
  double lastChange_ = 0;
  struct Orig { Transform xf; std::optional<Anchor> anchor; };
  std::vector<std::pair<std::string, Orig>> orig_;
  double modalX0_ = 0, modalY0_ = 0, modalX_ = 0, modalY_ = 0;
  View modalView_;
  enum class Drag { None, Orbit, Pan, Zoom, Rot4, Fly, FreeMove, Gizmo, Click };
  Drag drag_ = Drag::None;
  int dragButton_ = -1;
  double pressX_ = 0, pressY_ = 0, lastMx_ = 0, lastMy_ = 0;
  bool dragMoved_ = false;
  int gizmoHandle_ = -1;
  Gizmo gizmoAtPress_;
  // drag totals since the press (transforms are re-applied from the original state, so snapping never drifts)
  double dragDx_ = 0, dragDy_ = 0, dragDepth_ = 0, dragAlong_ = 0, dragAngle_ = 0;
  bool snapNow_ = false;
  void resetDragTotals() { dragDx_ = dragDy_ = dragDepth_ = dragAlong_ = dragAngle_ = 0; }
  std::string primaryOf(const std::vector<std::string>& ids) const;
  /** Moves `ids` so the primary object's world position lands on the snapping grid, within the span of `dirs`. */
  void snapToGrid(const std::vector<std::string>& ids, const std::vector<V4>& dirs);
  /** Anchored primary: rounds the travelled distance instead (no grid on a curved surface). */
  V4 snapDistance(const std::vector<std::string>& ids, const V4& delta);
  double snapAng(double a) const;
  std::string snapshot();
  void restore(const std::string& s);
  void captureOrig();
  void restoreOrig();
  void applyModal();
  M4 parentWorldRotation(const std::string& id, double* scaleAcc);
  /** The object's local X, Y, Z straightened into the visible slice (orthonormal, world space). */
  std::array<V4, 3> gizmoAxes(const std::string& id, const View& v);
  /** Direction for the G / R axis key k (0..3 = X..W): Global = that world axis exactly, Local = gizmo axis. */
  V4 keyAxis(const std::string& id, const View& v, int k);
  bool globalExact(const std::string& id);
  SurfaceField field() { return worldField(*ev); }
  void updateExplore(const InputState& in, double dt);
  void flyGeoEdit(const InputState& in, double dt);
};

}  // namespace fd
