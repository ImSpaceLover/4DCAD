// Texture slots of materials: the mapping (texture coordinates), the remap of masks, the node data
// the shader reads and the CPU evaluation of the same (see Material in scene.hpp, TextureDef in defs.hpp).
#pragma once
#include <array>
#include <vector>

#include "defs.hpp"
#include "scene.hpp"

namespace fd {

/** Channel ch's texture-coordinate transform: tex = A * x + b, x = the point in the mapping's space.
 *  Objects with dim < 4 sit mid-cell along their unused axes (object space; x has 0 there). */
void texMapMatrix(const Material& m, int ch, int dim, M4& A, V4& b);
/** The remapped mask (low..high, contrast, invert, curve), 0..1. */
double texRemap(const TexSlot& s, double m);
/** The mask a texture value gives a channel: masks as they are, colours their brightness, colours with
 *  coverage their coverage. */
double texMaskOf(TexKind kind, const double v[4]);

// ---- node data (packNodes; the shader reads the same layout, see texturesGLSL in codegen.cpp) ----
// per used channel: 0-3 A (columns), 4 b, 5 (strength, low, 1 / (high - low), contrast exponent),
// 6 (invert, curve points, value 2, 0), 7 (colour 2, 0), 8-11 curve points (in, out) x 8, 12.. parameters
constexpr int TEX_SLOT_HEADER = 12;
// the node's texture rows start with its texture frame: a 4x4 matrix (4 vec4) taking the shader's
// node-frame point back to the object's own frame (curved spaces re-anchor planes, rods, repeats and
// honeycombs near the camera; identity otherwise); the slots follow
constexpr int TEX_FRAME_VEC4S = 4;
int texSlotVec4s(const TexSlot& s);
int texOffset(const Material& m, int ch);  // vec4 offset within the node's texture rows, -1 if unused
int texTotalVec4s(const Material& m);
std::vector<float> packTexSlot(const Material& m, int ch, int dim);

// ---- CPU ----
/** The raw texture value of channel ch at point x of its mapping's space; false if none / failed. */
bool sampleTexSlot(const Material& m, int ch, int dim, const V4& x, double out[4], TexKind* kind = nullptr);
/** A leaf's surface colour at a point given in object-local, world and parent coordinates. */
std::array<double, 3> texturedColor(const Node& leaf, int dim, const V4& local, const V4& world, const V4& parent);
/** A non-colour channel at a point: roughness, metallic, specular, emission (emissive), transparency
 *  (base values blended by their textures); bump: the relief height. */
double texturedChannel(const Node& leaf, int ch, int dim, const V4& local, const V4& world, const V4& parent);

// ---- textures made of shapes (Save as texture / the texture editing mode) ----
enum Coverage { COV_INSIDE = 0, COV_OUTLINE, COV_BANDS, COV_FALLOFF, COV_COUNT };
inline const char* COVERAGE_NAMES[COV_COUNT] = {"Inside", "Outline", "Distance bands", "Falloff"};

/** A value of an object of the composition made a parameter of the texture. */
struct ExposedParam {
  std::string node, path;  // the object and its value (animation path: pos.x, scale, k, color, mods.<i>.p.<name>, tex.color.p.<name>)
  std::string name;        // the parameter's label in material slots
  double min = 0, max = 1;
  bool operator==(const ExposedParam& o) const { return node == o.node && path == o.path && name == o.name && min == o.min && max == o.max; }
};

/** The composition of a texture made of shapes and its settings. Objects keep one colour each
 *  (their colour slot is their fill: a code texture inside the shape). */
struct ShapeTexture {
  Node root;                 // a group, flat 4D
  bool objectColors = true;  // each part's colour (a colour texture) or one mask (the colours' brightness)
  bool mergeColors = true;   // parts of the same colour share one colour parameter
  int coverage = COV_INSIDE;
  double softness = 0.02, width = 0.05, spacing = 0.25, range = 1;
  bool tile = false;
  std::array<double, 4> cell{2, 2, 2, 2};
  std::vector<ExposedParam> exposed;
  std::string sourceId;  // the scene object it was captured from (Update from source)
};
std::string shapeTextureToJson(const ShapeTexture& st);
ShapeTexture shapeTextureFromJson(const std::string& text);  // throws on bad data
/** Whether a value can be exposed (and the parameter type it becomes). */
bool exposablePath(const Node& n, const std::string& path);
/** The texture definition the composition compiles to (code + parameters); its source / shapes are set. */
TextureDef compileShapeTexture(const ShapeTexture& st, const PrimitiveRegistry& reg, const std::string& id, const std::string& name);
/** True if the composition's fills use the texture `id` (directly or through other shape textures). */
bool textureCycle(const std::string& id, const ShapeTexture& st);

}  // namespace fd
