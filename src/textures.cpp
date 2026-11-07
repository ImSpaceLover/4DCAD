#include "textures.hpp"

#include <algorithm>
#include <cmath>

namespace fd {

void texMapMatrix(const Material& m, int ch, int dim, M4& A, V4& b) {
  const TexMapping& mp = m.tex[texMappingChannel(m, ch)].map;
  // tex = S * R^T * (x - offset): the pattern moves by the offset, turns by the rotation, cells / unit = scale
  M4 Rt = transpose(rotationFromAngles(mp.rot.data()));
  A = Rt;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) A[c * 4 + r] *= mp.scale * mp.axisScale[r];
  V4 o{mp.offset[0], mp.offset[1], mp.offset[2], mp.offset[3]};
  b = -mul(A, o);
  // thin objects: along the axes they do not use they sit on a cell boundary (coordinate 0), which
  // would make grids solid and checkers flicker, so they are put in the middle of a cell
  if (mp.space == TS_OBJECT)
    for (int a = std::max(dim, 0); a < 4; a++) b[a] += 0.5;
}

static double smoothstep(double e0, double e1, double x) {
  double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}
// the curve's points as the shader gets them: sorted, inside 0..1, strictly increasing inputs
static std::vector<std::array<double, 2>> curvePoints(const TexSlot& s) {
  auto c = s.curve;
  if ((int)c.size() > TEX_CURVE_MAX) c.resize(TEX_CURVE_MAX);
  std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a[0] < b[0]; });
  for (size_t k = 0; k < c.size(); k++) {
    c[k][0] = std::clamp(c[k][0], 0.0, 1.0);
    c[k][1] = std::clamp(c[k][1], 0.0, 1.0);
    if (k > 0 && c[k][0] <= c[k - 1][0]) c[k][0] = c[k - 1][0] + 1e-4;
  }
  return c;
}
static double contrastExp(const TexSlot& s) { return std::pow(2.0, 2.0 * std::clamp(s.contrast, -1.0, 1.0)); }
static double invRange(const TexSlot& s) {
  double r = s.high - s.low;
  if (std::abs(r) < 1e-6) r = r < 0 ? -1e-6 : 1e-6;
  return 1.0 / r;
}

double texRemap(const TexSlot& s, double m) {
  // (float steps as the shader's texRemap)
  m = std::clamp((m - s.low) * invRange(s), 0.0, 1.0);
  double g = contrastExp(s);
  if (g != 1) m = m < 0.5 ? 0.5 * std::pow(2 * m, g) : 1 - 0.5 * std::pow(2 - 2 * m, g);
  if (s.invert) m = 1 - m;
  auto c = curvePoints(s);
  if (c.size() >= 2) {
    double res = c[0][1];
    for (size_t i = 1; i < c.size(); i++) {
      if (m <= c[i][0]) { res = m <= c[i - 1][0] ? c[i - 1][1] : c[i - 1][1] + (c[i][1] - c[i - 1][1]) * smoothstep(c[i - 1][0], c[i][0], m); break; }
      res = c[i][1];
    }
    m = res;
  }
  return m;
}

double texMaskOf(TexKind kind, const double v[4]) {
  if (kind == TEXK_MASK) return v[0];
  if (kind == TEXK_RGBA) return v[3];
  return 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2];
}

int texSlotVec4s(const TexSlot& s) {
  auto d = defs().texture(s.def);
  return TEX_SLOT_HEADER + (d ? paramsVec4s(d->params) : 1);
}
int texOffset(const Material& m, int ch) {
  if (ch < 0 || ch >= CH_COUNT || !m.tex[ch].used()) return -1;
  int o = 0;
  for (int c = 0; c < ch; c++) if (m.tex[c].used()) o += texSlotVec4s(m.tex[c]);
  return o;
}
int texTotalVec4s(const Material& m) {
  int o = 0;
  for (auto& t : m.tex) if (t.used()) o += texSlotVec4s(t);
  return o;
}

std::vector<float> packTexSlot(const Material& m, int ch, int dim) {
  const TexSlot& s = m.tex[ch];
  std::vector<float> v((size_t)texSlotVec4s(s) * 4, 0.f);
  M4 A;
  V4 b;
  texMapMatrix(m, ch, dim, A, b);
  for (int k = 0; k < 16; k++) v[k] = (float)A[k];
  for (int k = 0; k < 4; k++) v[16 + k] = (float)b[k];
  v[20] = (float)s.strength;
  v[21] = (float)s.low;
  v[22] = (float)invRange(s);
  v[23] = (float)contrastExp(s);
  auto c = curvePoints(s);
  v[24] = s.invert ? 1.f : 0.f;
  v[25] = (float)c.size();
  v[26] = (float)s.value2;
  for (int k = 0; k < 3; k++) v[28 + k] = (float)s.color2[k];
  for (size_t k = 0; k < c.size(); k++) { v[32 + k * 2] = (float)c[k][0]; v[33 + k * 2] = (float)c[k][1]; }
  if (auto d = defs().texture(s.def)) {
    auto P = packValues(s.params, d->params);
    for (size_t k = 0; k < P.size() && 48 + k < v.size(); k++) v[48 + k] = (float)P[k];
  }
  return v;
}

bool sampleTexSlot(const Material& m, int ch, int dim, const V4& x, double out[4], TexKind* kind) {
  const TexSlot& s = m.tex[ch];
  auto d = defs().texture(s.def);
  if (!d) return false;
  M4 A;
  V4 b;
  texMapMatrix(m, ch, dim, A, b);
  V4 xx = x;
  if (m.tex[texMappingChannel(m, ch)].map.space == TS_OBJECT)
    for (int a = std::max(dim, 0); a < 4; a++) xx[a] = 0;
  V4 tq = mul(A, xx) + b;
  if (kind) *kind = textureKind(*d);
  return runTexture(*d, packValues(s.params, d->params), tq.data(), out);
}

static const V4& spacePoint(const Material& m, int ch, const V4& local, const V4& world, const V4& parent) {
  int sp = m.tex[texMappingChannel(m, ch)].map.space;
  return sp == TS_WORLD ? world : sp == TS_PARENT ? parent : local;
}

std::array<double, 3> texturedColor(const Node& leaf, int dim, const V4& local, const V4& world, const V4& parent) {
  std::array<double, 3> col = leaf.color;
  const TexSlot& s = leaf.mat.tex[CH_COLOR];
  double v[4];
  TexKind kind;
  if (!s.used() || !sampleTexSlot(leaf.mat, CH_COLOR, dim, spacePoint(leaf.mat, CH_COLOR, local, world, parent), v, &kind)) return col;
  std::array<double, 3> target = s.color2;
  double w;
  if (kind == TEXK_MASK) w = texRemap(s, v[0]);
  else { target = {v[0], v[1], v[2]}; w = kind == TEXK_RGBA ? texRemap(s, v[3]) : 1.0; }
  w *= s.strength;
  for (int k = 0; k < 3; k++) col[k] += (target[k] - col[k]) * w;
  return col;
}

double texturedChannel(const Node& leaf, int ch, int dim, const V4& local, const V4& world, const V4& parent) {
  const Material& m = leaf.mat;
  double base = ch == CH_ROUGHNESS ? m.roughness : ch == CH_METALLIC ? m.metallic : ch == CH_SPECULAR ? m.specular
              : ch == CH_EMISSION ? m.emissive : ch == CH_TRANSPARENCY ? m.transparency : 0.0;
  if (ch <= CH_COLOR || ch >= CH_COUNT) return base;
  const TexSlot& s = m.tex[ch];
  double v[4];
  TexKind kind;
  if (!s.used() || !sampleTexSlot(m, ch, dim, spacePoint(m, ch, local, world, parent), v, &kind)) return base;
  double w = texRemap(s, texMaskOf(kind, v)) * s.strength;
  if (ch == CH_BUMP) return s.value2 * w;
  return base + (s.value2 - base) * w;
}

}  // namespace fd
