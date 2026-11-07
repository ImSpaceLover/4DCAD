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

}  // namespace fd
