// Application state and logic (no ImGui here): scene + undo, cameras, picking,
// placement, gizmo / transform tools, explore movement, frame rendering.
#pragma once
#include <deque>
#include <functional>
#include <sstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "camera.hpp"
#include "animation.hpp"
#include "evaluator.hpp"
#include "keybinds.hpp"
#include "library.hpp"
#include "render_thread.hpp"
#include "renderer.hpp"
#include "surface.hpp"
#include "textures.hpp"

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

  // --- space: Flat 4D, or a curved 3D space (Hyperbolic / Spherical), chosen for a new scene ---
  Space space;
  bool curved() const { return space.curved(); }
  CurvedCamera curvCam;  // editor / Explore camera of curved scenes
  /** World isometry of a node in a curved scene (columns: its axes at its position, the position). */
  M4 nodeWorldIso(const std::string& id, double* sAncOut = nullptr);
  /** Where a light is in the world (curved scenes: its position is normal coordinates at the origin). */
  V4 lightWorldPos(const PointLight& l) const;
  V4 lightFromWorld(const V4& p) const { return space.curved() ? cs::logO(space, p) : p; }
  /** Changes the curvature radius (curved scenes; objects keep their coordinates). */
  void setCurvatureRadius(double r);
  /** The point reached from p along tangent vector v (its length = distance): p + v in flat space, a geodesic in curved. */
  V4 along(const V4& p, const V4& v) const;
  // --- objects attached to a world surface: surface motion (Travel / Heading / Spin from a rest
  // pose) and Follow surface (bent along the surface geodesic through the anchor) ---
  /** Makes the current pose the rest pose (travel / heading / spin 0) if there is none yet. */
  void ensureRest(Node& n);
  /** Places the object from its rest pose and its travel / heading / spin. */
  void applySurfacePose(Node& n);
  /** After a manual move: the rest pose that gives the current pose with the current values. */
  void setRestFromPose(Node& n);
  /** Recomputes the follow-surface paths (called by rebuild). */
  void updateFollowPaths();
  /** Curved overview: the outside camera looking at the ball, and a point's place in the ball
   *  (Poincare ball / stereographic projection: p.xyz / (1 + p.w)). */
  View ballView() const;
  static V4 toBall(const V4& p);
  // transforms that keep objects in place (flat: plain composition; curved: isometries)
  std::optional<Transform> worldTransform(const std::string& id);
  Transform relativeTransform(const Transform& parentWorld, const Transform& world);
  /** child (in parent's frame) expressed in the frame the parent lives in; sAnc = scale above the parent. */
  Transform composeLocal(const Transform& parent, const Transform& child, double sAnc);
  double scaleAbove(const std::string& id);  // product of the scales of the node's ancestors
  bool centreGroup(Node& g, double sAnc);
  /** Depth of p in front of the view (along its forward axis). */
  double viewDepth(const V4& p, const View& v) const;
  /** Moves / rotates nodes; directions are tangent vectors at dirBase_ (curved scenes). */
  void rotateNodesPlane(const std::vector<std::string>& ids, const V4& u, const V4& v, double ang);
  V4 dirBase_{0, 0, 0, 1};

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
  double fov() const;
  /** Viewing through the active camera right now (Edit mode, camera view on and a valid camera). */
  bool inCameraView() const;
  bool keyMenuRequest = false;  // I pressed: the UI shows the insert-keyframe menu
  int accumSamples = 0;         // stochastic samples averaged in the viewport image so far
  bool viewportCompiling = false;  // the viewport waits for a shader being compiled in the background
  V4 light() const;
  double thumbYaw = 0.4, thumbPitch = -0.9, thumbZoom = 1;
  bool mouseCaptured = false, wantCapture = false, wantRelease = false;

  // --- viewport (window pixels) ---
  float vpX = 0, vpY = 0, vpW = 1, vpH = 1;
  RenderTarget mainRT, thumbRT;
  RenderTarget mainAccRT;  // path tracing (single-threaded viewport): the linear running average
  RenderTarget mainGuideRT, mainTmpRT[2];  // (its denoiser: guide image, scratch)
  int thumbW = 300, thumbH = 225;
  std::unique_ptr<Renderer> renderer;  // the UI thread's renderer (offline renders; headless runs: everything)
  // the viewport and the thumbnail render on their own threads (interactive use; see render_thread.hpp)
  std::unique_ptr<RenderThread> viewThread, thumbThread;
  void startRenderThreads(const std::function<std::unique_ptr<Renderer>()>& make, std::function<bool(bool)> viewCtx,
                          std::function<bool(bool)> thumbCtx);
  void stopRenderThreads();
  /** The viewport / thumbnail image to draw (0: none yet); flip: stored bottom-up (OpenGL). */
  uint64_t viewportTexture(bool& flip);
  uint64_t thumbnailTexture(bool& flip);
  Backend backendPref = Backend::Auto;  // graphics API for the next start (settings)
  bool showLights = false;              // Lights window
  double fps = 0, now = 0;

  // --- offline renders (Render window; render_job.cpp) ---
  struct RenderSettings {
    int width = 1280, height = 720;
    int aa = 2;        // supersampling factor (1..3)
    int samples = 32;  // stochastic samples per pixel (1 = plain render)
    int stillFormat = 0;  // STILL_FORMATS
    int animFormat = 0;   // ANIM_FORMATS
    std::string stillPath, animPath;  // "" = next to the scene file
    bool sceneRange = true;           // frame range and fps from the timeline
    int start = 1, end = 120;
    double fps = 24;
    int jpegQuality = 92;
    std::string ffmpeg = "ffmpeg";
    int priority = 0;  // 0 low, 1 normal, 2 high: render work per frame (RENDER_BUDGET_MS)
  };
  static constexpr const char* PRIORITY_NAMES[3] = {"Low (smooth PC)", "Normal", "High (fastest)"};
  static constexpr double RENDER_BUDGET_MS[3] = {15, 40, 100};
  double renderBudget() const { return RENDER_BUDGET_MS[std::clamp(renderSet.priority, 0, 2)] / 1000.0; }
  static constexpr const char* STILL_FORMATS[2] = {"PNG", "JPEG"};
  static constexpr const char* ANIM_FORMATS[5] = {"GIF", "AVI (Motion JPEG)", "PNG sequence", "MP4 (H.264, ffmpeg)", "WebM (VP9, ffmpeg)"};
  RenderSettings renderSet;
  struct RenderJob {
    bool active = false, animation = false, finished = false;
    int first = 1, last = 1, frame = 1, sample = 0, samples = 1, aa = 1, w = 0, h = 0;
    int tile = 0;  // next tile of the current sample (tiles of TILE pixels, row by row)
    bool prepared = false;
    RenderTarget rt;                    // the supersampled image being refined (also the preview)
    RenderTarget acc;                   // path tracing: the linear running average (rt = its display version)
    RenderTarget guide, tmp[2];         // (its denoiser)
    std::unique_ptr<FrameWriter> writer;
    std::string path, error;
    double started = 0, fps = 24, seconds = 0;
    bool usedCamera = false;
    // what is rendered (snapshot at the start: editing meanwhile does not change the render)
    Node sceneRoot, frameRoot;
    std::vector<PointLight> sceneLights;
    Animation sceneAnim;
    std::string camera, world;
    Space space;
    QualitySettings quality;
    FrameParams fp;
  };
  RenderJob job;
  /** Starts rendering a still image or the animation (from the active camera). */
  bool startRender(bool animation, std::string& err);
  /** Renders for about `budget` seconds (called every frame while a job runs). */
  void renderStep(double budget);
  void cancelRender();
  /** 0..1 */
  double renderProgress() const;
  /** The output path a render would use (the chosen one, or next to the scene file; extension = format). */
  std::string renderPath(bool animation) const;
  // --- render presets (all rendering settings; JSON files in <data>/presets) ---
  struct RenderPreset {
    std::string name;
    bool builtin = false;  // Fast / Balanced / Full: read-only
    std::string json;
  };
  static std::vector<RenderPreset> builtinPresets();
  std::vector<RenderPreset> renderPresets() const;  // built-in first, then the user's (by name)
  std::string currentPresetJson(const std::string& name) const;
  bool applyPreset(const std::string& json, std::string& err);
  bool savePreset(const std::string& name, std::string& err);  // the current settings (overwrites)
  bool deletePreset(const std::string& name);
  bool renamePreset(const std::string& from, const std::string& to, std::string& err);
  bool importPreset(const std::string& path, std::string& err, std::string* name = nullptr);
  bool exportPreset(const std::string& name, const std::string& path, std::string& err);
  std::string activePreset;  // last loaded / saved (shown in the preset list)
  /** The main settings as a render profile (what the viewport draws when idle). */
  RenderProfile mainProfile() const;

  // --- lifecycle ---
  void init(std::unique_ptr<Renderer> r);
  void loadScene(SceneFile f);
  bool loadSceneFile(const std::string& path, std::string& err);
  bool saveSceneFile(const std::string& path, std::string& err);
  std::string serialize();
  void newScene(SpaceKind kind = SpaceKind::Flat, double radius = 8);
  void rebuild();
  void commit();
  void undo();
  void redo();
  bool canUndo() const { return !history_.empty(); }
  bool canRedo() const { return !future_.empty(); }
  size_t undoSteps() const { return history_.size(); }
  std::string snapshotForTest() { return snapshot(); }
  /** Memory the undo / redo steps take (bytes). */
  size_t undoBytes() const { return undoBytes_; }
  static constexpr size_t UNDO_MAX_BYTES = 64u << 20, UNDO_MAX_STEPS = 1000;
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
  std::vector<PointLight> lights;  // scene point lights (saved in the scene)
  // --- animation ---
  Animation anim;
  bool playing = false;
  bool autoKey = false;            // every change is keyed at the current frame
  bool viewThroughCamera = false;  // the viewport shows the active camera (preview the animation)
  /** Moves to a frame: every animated value takes its value there (no undo step). */
  void setFrame(double frame);
  /** Advances playback (called every frame by the main loop). */
  void tickAnimation(double dt);
  /** Where playback loops back to the first frame (Explore: the last keyframe; Edit: the end of the range). */
  double playbackEnd() const;
  /** Keys the selected objects at the current frame (transform only, or every value). */
  void insertKeys(bool transformOnly);
  /** Removes the selected objects' keys at the current frame. */
  void deleteKeys();
  /** Per-value keying from Properties / the Lights window: key, or remove the key at this frame. */
  void toggleValueKey(const std::string& target, const std::string& path);
  /** The point lights for a frame: enabled scene lights, then emissive objects (if enabled). */
  /** Lights of a frame. pathTrace: emissive objects become area lights (bounding spheres) instead of point lights. */
  std::vector<LightParam> frameLights(bool pathTrace = false);
  // cameras are scene objects (NodeKind::Camera); the active one is used for rendering
  std::string activeCamera;
  /** View, Geodesic flag and field of view of a camera object. */
  View cameraView(const std::string& id, bool* geoCamera = nullptr, double* fov = nullptr);
  /** A new camera at the current view (attached to the world surface in the Geodesic view); returns its id. */
  std::string addCamera();
  /** Moves the view to the camera (Numpad 0). */
  void lookThrough(const std::string& id);
  /** Moves the camera to the current view (Ctrl+Alt+Numpad 0). */
  void cameraToView(const std::string& id);
  /** Camera whose icon is under the screen point ("" if none). */
  std::string pickCamera(double x, double y);
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
  void curvedParams(FrameParams& fp, const View& v, std::ostringstream& key);
  void prepareJobFrame();
  void finishJobFrame();
  View thumbnailView();
  double sliceW() { return editCam.sliceW(); }
  void setSliceW(double w) { editCam.setSliceW(w); }
  std::array<float, 3> worldColor();
  void toast(const std::string& msg, bool error = false);

  // --- texture editing mode: a texture made of shapes is edited as a small flat 4D scene (the main
  // scene is kept aside meanwhile; Done compiles it into a texture definition, Cancel drops it) ---
  struct TexMode {
    bool active = false;
    std::string texId;       // the texture being edited ("" = a new one)
    std::string name = "My texture";
    ShapeTexture settings;   // all but the composition (the scene root while editing)
    int view = 0;            // 0 = Shapes (flat colours on black), 1 = Preview
    int previewShape = 0;    // 0 hypersphere, 1 tesseract, 2 slab
    int previewChannel = CH_COLOR;
    Node composition;        // kept while the preview is shown
    std::string error;       // why Done failed
    bool sourceExists = false;  // the captured object is in the kept scene (Update from source)
  } texMode;
  /** Opens the mode on a composition (texId "" = a new texture). */
  void enterTextureMode(const ShapeTexture& st, const std::string& texId, const std::string& name);
  /** Done (apply = compile and keep the texture) or Cancel; returns false if Done failed (texMode.error). */
  bool exitTextureMode(bool apply);
  /** Shapes / Preview. */
  void textureModeView(int view);
  /** Save as texture: the mode with a copy of the object or group (its colours kept). */
  void saveAsTexture(const std::string& nodeId);
  /** Re-captures the texture's source object (while it exists in the kept scene). */
  bool updateTextureFromSource();
  bool sourceAvailable() const;
  ShapeTexture currentShapeTexture() const;
  /** The settings the viewport renders with (the Shapes view: flat colours on black). */
  const QualitySettings& viewQuality();
  std::string toastMsg;
  bool toastError = false;
  double toastUntil = 0;

 private:
  // undo / redo: each step is the difference to the neighbouring state (the scene snapshots share
  // everything outside the edited part): rebuild it with prefix + mid + suffix of the current one
  struct UndoStep {
    size_t pre = 0, suf = 0;
    std::string mid;
    size_t bytes() const { return mid.size() + sizeof(UndoStep); }
  };
  static UndoStep diffTo(const std::string& target, const std::string& from);
  static std::string applyStep(const UndoStep& d, const std::string& from);
  std::deque<UndoStep> history_;
  std::vector<UndoStep> future_;
  size_t undoBytes_ = 0;
  // the main scene while the texture editing mode is open
  std::string texParked_, texParkedFile_, texParkedCommitted_, texParkedSaved_;
  std::deque<UndoStep> texParkedHistory_;
  std::vector<UndoStep> texParkedFuture_;
  size_t texParkedUndoBytes_ = 0;
  QualitySettings shapesQuality_;
  void clearFuture();
  std::string committed_, saved_, autosaved_;
  std::map<std::string, std::string> linkDefs_;  // last known definition of each linked group
  Node evalRoot_;                                 // the scene as the animation left it (auto-key compares to it)
  std::vector<PointLight> evalLights_;
  /** Applies the animation at anim.frame and re-attaches surface objects to their world. */
  void applyAnim();
  void autoKeyChanges();
  double evalRadius_ = 8;  // curvature radius as the animation / last edit left it
  Node viewRoot_;                                 // the tree as drawn while isolating
  double lastAutosave_ = 0;
  std::vector<std::string> selectionAtPress_;
  int version_ = 0;
  std::string lastKey_, lastThumbKey_;
  // scene snapshots for the render threads (rebuilt when the scene changes)
  std::shared_ptr<const Node> snap_;
  std::shared_ptr<const PrimitiveRegistry> snapReg_;
  std::string snapKey_;
  // path tracing: the measured radius of each glowing object around its origin (per scene version)
  std::unordered_map<std::string, double> emitRadius_;
  int emitRadiusVersion_ = -1;
  double emitterRadius(const Node& n, const V4& centre, double bound);
  std::shared_ptr<const Node> sceneSnapshot();
  void postView(RenderThread& t, int w, int h, const std::unordered_set<std::string>& hl, const FrameParams& fp, int samples, bool stochastic);
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
