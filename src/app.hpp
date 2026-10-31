// Application state and logic (no ImGui here): scene + undo, cameras, picking,
// placement, transform tools, explore movement, frame rendering.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "camera.hpp"
#include "evaluator.hpp"
#include "library.hpp"
#include "renderer.hpp"
#include "surface.hpp"

namespace fd {

enum class Mode { Edit, Explore };
enum class Tool { Select, Move, Rotate };
enum class Modal { None, Grab, Rotate, Scale };

struct LibraryItem { bool object = false; std::string id; };

/** Abstract per-frame input, filled by the UI layer from GLFW / ImGui. */
struct InputState {
  bool vpHovered = false;         // mouse over the 3D viewport (and not over an overlay widget)
  double mx = 0, my = 0;          // mouse position (window pixels)
  double dx = 0, dy = 0;          // mouse delta since last frame
  bool down[3] = {false, false, false}, pressed[3] = {false, false, false}, released[3] = {false, false, false};
  double wheel = 0;
  bool shift = false, ctrl = false, alt = false;
  bool keysEnabled = true;        // false while a text field has focus
  std::function<bool(int)> keyDown;  // GLFW key code -> held?
};

class App {
 public:
  App();
  // --- scene ---
  Node root;
  PrimitiveRegistry reg;
  LibraryStore lib{reg};
  std::string world;  // active world id ("" = none)
  std::vector<std::string> selection;
  std::unique_ptr<SceneEvaluator> ev;
  std::string currentFile;

  // --- modes / cameras ---
  Mode mode = Mode::Edit;
  bool geo = false;
  Tool tool = Tool::Select;
  FullCamera editCam, playerCam;
  GeoCamera geoEdit, geoPlayer;
  bool playerPlaced = false;
  QualitySettings quality;
  double fov = 60;
  V4 light{0.5, 0.8, -0.35, 0.25};
  bool collide = true;
  double moveSpeed = 3;
  bool showThumbnail = true, showPlayerMarker = true;
  double thumbYaw = 0.4, thumbPitch = -0.9, thumbZoom = 1;
  bool mouseCaptured = false;  // explore mouse look
  bool wantCapture = false, wantRelease = false;

  // --- viewport (window pixels), set by the UI each frame ---
  float vpX = 0, vpY = 0, vpW = 1, vpH = 1;
  RenderTarget mainRT, thumbRT;
  int thumbW = 300, thumbH = 225;
  std::unique_ptr<Renderer> renderer;
  double fps = 0;
  std::string status;

  // --- lifecycle ---
  void init();  // after the GL context exists
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
  void autosave();

  // --- selection & tree ---
  void select(std::vector<std::string> ids) { selection = std::move(ids); }
  Node* selected();
  std::unordered_set<std::string> highlightSet();
  void deleteSelection();
  void duplicateSelection();
  void groupSelection(BoolOp op);
  void ungroup(const std::string& id);
  enum class Drop { Into, Before, After, Wrap };
  void reparent(const std::string& id, const std::string& target, Drop where);
  void setWorld(const std::string& id);
  void attachToWorld(const std::string& id);
  void detach(const std::string& id);

  // --- modes & views ---
  void setMode(Mode m);
  void setGeo(bool g);
  View currentView();
  View playerView();
  double tanHalf() const { return std::tan(fov * PI / 360); }
  /** Screen ray through window pixel (x, y) in the current view. */
  V4 screenRay(double x, double y, const View& v) const;
  /** Window pixel of a 4D point projected into the current view (false if behind). */
  bool project(const V4& p, const View& v, double& sx, double& sy) const;
  std::optional<RayHit> pick(double x, double y);
  void focusSelection();
  void standardView(int which);  // 0 front, 1 right, 2 top, 3 along W

  // --- placement ---
  bool canPlace(const LibraryItem& it, std::string* reason);
  int itemDim(const LibraryItem& it);
  void placeItem(const LibraryItem& it, std::optional<std::pair<double, double>> screen);

  // --- transforms ---
  Modal modal = Modal::None;
  std::vector<int> modalAxes;  // camera axes: 0 right, 1 up, 2 forward (depth)
  std::string transformHint() const;
  void startModal(Modal m);
  void confirmModal();
  void cancelModal();
  /** Move by a world-space delta (restricted by callers to the visible slice). */
  void translateNodes(const std::vector<std::string>& ids, const V4& delta);
  /** Rotate nodes about their own origin by a world-space rotation. */
  void rotateNodes(const std::vector<std::string>& ids, const M4& q);

  // --- per frame ---
  void handleInput(const InputState& in, double dt);
  void handleKey(int key, bool shift, bool ctrl);
  void renderFrame();
  /** Outside (Full 4D) view used by the thumbnail while in the Geodesic view. */
  View thumbnailView();
  V4 thumbnailMarker();
  double sliceW() { return editCam.sliceW(); }
  void setSliceW(double w) { editCam.setSliceW(w); }
  void toast(const std::string& msg, bool error = false);
  std::string toastMsg;
  bool toastError = false;
  double toastUntil = 0;
  double now = 0;

 private:
  std::vector<std::string> history_, future_;
  std::string committed_;
  int version_ = 0;
  std::string lastKey_, lastThumbKey_;
  double lastChange_ = 0;
  // drag / modal state
  struct Orig { Transform xf; std::optional<Anchor> anchor; };
  std::vector<std::pair<std::string, Orig>> orig_;
  double modalX0_ = 0, modalY0_ = 0, modalX_ = 0, modalY_ = 0;
  View modalView_;
  enum class Drag { None, Orbit, Pan, Zoom, Rot4, ToolMove, ToolRotate };
  Drag drag_ = Drag::None;
  int dragButton_ = -1;
  double pressX_ = 0, pressY_ = 0, lastMx_ = 0, lastMy_ = 0;
  bool dragMoved_ = false;
  std::string snapshot();
  void restore(const std::string& s);
  void captureOrig();
  void restoreOrig();
  void applyModal();
  M4 parentWorldRotation(const std::string& id, double* scaleAcc);
  SurfaceField field() { return worldField(*ev); }
  void syncPlayerFromEdit();
  void updateExplore(const InputState& in, double dt);
};

}  // namespace fd
