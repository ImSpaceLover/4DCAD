// Texture editing mode: textures made of shapes (Save as texture, Library > Textures).
#include "app.hpp"

#include <algorithm>
#include <cmath>

#include "defs.hpp"
#include "examples.hpp"
#include "primitives.hpp"

namespace fd {

static const char* PREVIEW_TEX_ID = "__texmode_preview";

// the composition a node becomes: a copy at the origin, colours and fills kept, other material
// channels, world attachments and cameras dropped
static Node compositionOf(const Node& src) {
  Node c = src;
  c.xf.pos = {0, 0, 0, 0};
  std::function<void(Node&)> clean = [&](Node& n) {
    n.anchor.reset();
    n.linkId.clear();
    for (int ch = 1; ch < CH_COUNT; ch++) n.mat.tex[ch] = TexSlot{};
    n.children.erase(std::remove_if(n.children.begin(), n.children.end(), [](const Node& k) { return k.isCamera(); }), n.children.end());
    for (auto& k : n.children) clean(k);
  };
  clean(c);
  if (c.isGroup()) {
    c.xf = Transform{};
    c.id = "root";
    c.hidden = false;
    return c;
  }
  Node g = makeGroup(BoolOp::Union, {c}, "Texture");
  g.id = "root";
  return g;
}

void App::enterTextureMode(const ShapeTexture& st, const std::string& texId, const std::string& name) {
  if (texMode.active) return;
  bool sourceExists = !st.sourceId.empty() && findNode(root, st.sourceId) != nullptr;
  // keep the main scene (and its undo history) aside
  texParked_ = serialize();
  texParkedFile_ = currentFile;
  texParkedCommitted_ = committed_;
  texParkedSaved_ = saved_;
  texParkedHistory_ = std::move(history_);
  texParkedFuture_ = std::move(future_);
  texParkedUndoBytes_ = undoBytes_;
  history_.clear();
  future_.clear();
  undoBytes_ = 0;
  texMode = TexMode{};
  texMode.active = true;
  texMode.texId = texId;
  texMode.name = name;
  texMode.settings = st;
  texMode.settings.root = Node{};
  texMode.sourceExists = sourceExists;
  SceneFile f;
  f.root = st.root;
  f.root.id = "root";
  f.space = Space{};
  mode = Mode::Edit;
  geo = false;
  loadScene(f);
  currentFile.clear();
  editCam = FullCamera{};
  rebuild();
}

ShapeTexture App::currentShapeTexture() const {
  ShapeTexture st = texMode.settings;
  st.root = texMode.view == 1 ? texMode.composition : root;
  return st;
}

void App::textureModeView(int view) {
  if (!texMode.active || view == texMode.view) return;
  if (view == 1) {
    // the composition compiled into a temporary texture on a preview object
    texMode.composition = root;
    ShapeTexture st = currentShapeTexture();
    st.root = root;
    TextureDef d = compileShapeTexture(st, reg, PREVIEW_TEX_ID, texMode.name);
    defs().setTexture(d);
    const char* shapes[3] = {"hypersphere", "tesseract", "slab"};
    Node obj = makeShape(shapes[std::clamp(texMode.previewShape, 0, 2)], "Preview", {0, 0, 0, 0}, {0.75, 0.75, 0.78});
    // sized to the composition, so its surface passes through the pattern
    double rad = 1.5;
    auto b = computeBounds(root, reg);
    if (auto it = b.find(root.id); it != b.end() && std::isfinite(it->second.r) && it->second.r > 1e-3) rad = it->second.r * 0.75;
    obj.xf.scale = std::clamp(rad, 0.05, 50.0);
    TexSlot& t = obj.mat.tex[std::clamp(texMode.previewChannel, 0, CH_COUNT - 1)];
    t.def = PREVIEW_TEX_ID;
    conformValues(t.params, d.params);
    t.color2 = {0.1, 0.1, 0.12};
    t.value2 = texMode.previewChannel == CH_BUMP ? 0.03 : texMode.previewChannel == CH_ROUGHNESS ? 0.05 : 1.0;
    obj.mat.roughness = 0.45;
    Node r = makeGroup(BoolOp::Union, {obj}, "Preview");
    r.id = "root";
    root = r;
    selection.clear();
    texMode.view = 1;
  } else {
    root = texMode.composition;
    texMode.composition = Node{};
    texMode.view = 0;
  }
  rebuild();
}

const QualitySettings& App::viewQuality() {
  if (!texMode.active || texMode.view != 0) return quality;
  // the Shapes view: each part in its flat colour on black (the texture's value field)
  shapesQuality_ = quality;
  QualitySettings& q = shapesQuality_;
  q.shadows = false;
  q.ao = false;
  q.sunIntensity = 0;
  q.ambient = 1;
  q.fog = 0;
  q.skyTop = {0, 0, 0};
  q.skyBottom = {0, 0, 0};
  q.lighting = 0;
  q.pointShadows = false;
  q.emissiveLights = false;
  return q;
}

bool App::exitTextureMode(bool apply) {
  if (!texMode.active) return true;
  texMode.error.clear();
  if (texMode.view == 1) textureModeView(0);
  std::string id;
  if (apply) {
    ShapeTexture st = currentShapeTexture();
    if (texMode.name.empty()) { texMode.error = "Give the texture a name"; return false; }
    id = texMode.texId.empty() ? newDefId(texMode.name, true) : texMode.texId;
    if (textureCycle(id, st)) { texMode.error = "A fill uses this texture itself (directly or through another texture)"; return false; }
    TextureDef d = compileShapeTexture(st, reg, id, texMode.name);
    std::string err = checkTextureDef(d);
    if (!err.empty()) { texMode.error = "The generated code does not compile: " + err; return false; }
    defs().setTexture(d);
  }
  defs().eraseTexture(PREVIEW_TEX_ID);
  if (apply) saveUserDefs();
  // the main scene back, as it was (its undo history too)
  try { loadScene(sceneFromJson(texParked_)); } catch (...) {}
  currentFile = texParkedFile_;
  history_ = std::move(texParkedHistory_);
  future_ = std::move(texParkedFuture_);
  undoBytes_ = texParkedUndoBytes_;
  committed_ = texParkedCommitted_;
  saved_ = texParkedSaved_;
  texParked_.clear();
  std::string name = texMode.name;
  texMode = TexMode{};
  if (apply) {
    // objects already using the texture take its new parameter list
    auto d = defs().texture(id);
    walk(root, [&](Node& n, Node*, int) { for (auto& t : n.mat.tex) if (t.def == id && d) conformValues(t.params, d->params); });
    toast("Texture \"" + name + "\" saved (Library > Textures)");
  }
  rebuild();
  return true;
}

void App::saveAsTexture(const std::string& nodeId) {
  Node* n = findNode(root, nodeId);
  if (!n || n->isCamera() || texMode.active) return;
  ShapeTexture st;
  st.root = compositionOf(*n);
  st.sourceId = nodeId;
  st.objectColors = true;
  enterTextureMode(st, "", n->name);
}

bool App::sourceAvailable() const { return texMode.active && texMode.sourceExists; }

bool App::updateTextureFromSource() {
  if (!sourceAvailable()) return false;
  SceneFile f = sceneFromJson(texParked_);
  Node* src = findNode(f.root, texMode.settings.sourceId);
  if (!src) return false;
  if (texMode.view == 1) textureModeView(0);
  root = compositionOf(*src);
  selection.clear();
  commit();
  return true;
}

}  // namespace fd
