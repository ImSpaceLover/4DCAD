#include "codegen.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <regex>
#include "math4.hpp"
#include "shaders.hpp"
#include <sstream>
#include <unordered_map>

namespace fd {

static const char* C = "xyzw";

// SDF helpers available to generated code and to custom SDFs.
static const char* SDF_HELPERS = R"(
#define INF 1e9
float opExtrude(float d, float wy) {
  vec2 w = vec2(d, wy);
  return min(max(w.x, w.y), 0.0) + length(max(w, 0.0));
}
// smooth booleans; an operand >= INF/2 means "empty" (avoids float cancellation in mix(1e9, a, 1))
float sU(float a, float b, float k) { if (b >= INF * 0.5) return a; if (a >= INF * 0.5) return b; float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); return mix(b, a, h) - k * h * (1.0 - h); }
float sI(float a, float b, float k) { if (a >= INF * 0.5 || b >= INF * 0.5) return INF; float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0); return mix(b, a, h) + k * h * (1.0 - h); }
// radial repeat in a plane: v = the plane's coordinates. Copies at angles s + k*st (k = 0..n-1),
// at distance r from the axis: maps v into the frame of the nearest copy.
vec2 opRadial(vec2 v, float r, float n, float s, float st) {
  float a = (v.x == 0.0 && v.y == 0.0) ? 0.0 : atan(v.y, v.x);
  float rel = mod(a - s, 6.28318530718);
  float k0 = abs(st) > 1e-9 ? clamp(floor(rel / st), 0.0, n - 1.0) : 0.0;
  float best = 1e9, ang = s;
  for (int c = 0; c < 4; c++) {
    float k = c == 0 ? k0 : c == 1 ? min(k0 + 1.0, n - 1.0) : c == 2 ? 0.0 : n - 1.0;
    float t = s + k * st;
    float dd = abs(mod(a - t + 3.14159265359, 6.28318530718) - 3.14159265359);
    if (dd < best) { best = dd; ang = t; }
  }
  float cs = cos(ang), sn = sin(ang);
  return vec2(cs * v.x + sn * v.y - r, -sn * v.x + cs * v.y);
}
float sS(float a, float b, float k) { if (b >= INF * 0.5 || a >= INF * 0.5) return a; float h = clamp(0.5 - 0.5 * (a + b) / k, 0.0, 1.0); return mix(a, -b, h) + k * h * (1.0 - h); }
)";

// Follow surface: the bent object's local coordinates from its path (extra node-data rows from
// row `base`: (half length, sample spacing), then per sample: position, Y, Z, normal); the CPU
// evaluator's followLocal is the same
static_assert(NPATH == 32, "FOLLOW_GLSL assumes 32 path segments");
static_assert(NODE_VEC4S == 16, "ndatx assumes 16 vec4 per row");
static const char* FOLLOW_GLSL = R"(
vec4 followLocal(vec4 p, int base) {
  vec4 hdr = ndatx(base, 0);
  float bestD = 1e30, bh = 0.0;
  int bk = 0;
  for (int k = 0; k < 32; k++) {
    vec4 a = ndatx(base, 1 + 4 * k), ab = ndatx(base, 5 + 4 * k) - a;
    float h = dot(p - a, ab) / max(dot(ab, ab), 1e-12);
    if (k > 0) h = max(h, 0.0);
    if (k < 31) h = min(h, 1.0);
    vec4 c = a + ab * h;
    float d2 = dot(p - c, p - c);
    if (d2 < bestD) { bestD = d2; bh = h; bk = k; }
  }
  int ia = 1 + 4 * bk, ib = ia + 4;
  float t = clamp(bh, 0.0, 1.0);
  vec4 a = ndatx(base, ia), c = a + (ndatx(base, ib) - a) * bh, o = p - c;
  vec4 Y = normalize(mix(ndatx(base, ia + 1), ndatx(base, ib + 1), t));
  vec4 Z = normalize(mix(ndatx(base, ia + 2), ndatx(base, ib + 2), t));
  vec4 N = normalize(mix(ndatx(base, ia + 3), ndatx(base, ib + 3), t));
  return vec4(-hdr.x + (float(bk) + bh) * hdr.y, dot(o, Y), dot(o, Z), dot(o, N));
}
)";

// Material = mat3x4: column 0 (rgb, selection highlight), column 1 (emissive, roughness, specular,
// transparency), column 2 (metallic, index of refraction, 0, 0).
static const char* SCENE_HELPERS = R"(
// extra node-data rows (modifier parameters, follow paths): vec4 k counted from row `base`
vec4 ndatx(int base, int k) { return ndat(base + k / 16, k - (k / 16) * 16); }
bool g_exact = false;      // disable bounding-sphere shortcuts (shadow / AO samples need true distances)
bool g_noEmissive = false; // shadow / AO rays ignore emissive objects
#define MAT mat3x4
float opU(float a, inout MAT ma, float b, MAT mb) { if (b < a) { ma = mb; return b; } return a; }
float opI(float a, inout MAT ma, float b, MAT mb) { if (b > a) { ma = mb; return b; } return a; }
float opS(float a, inout MAT ma, float b, MAT mb) { if (-b > a) { ma = mb; return -b; } return a; }
float opSU(float a, inout MAT ma, float b, MAT mb, float k) {
  if (b >= INF * 0.5) return a;
  if (a >= INF * 0.5) { ma = mb; return b; }
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); ma = mb * (1.0 - h) + ma * h; return mix(b, a, h) - k * h * (1.0 - h);
}
float opSI(float a, inout MAT ma, float b, MAT mb, float k) {
  if (a >= INF * 0.5 || b >= INF * 0.5) return INF;
  float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0); ma = mb * (1.0 - h) + ma * h; return mix(b, a, h) + k * h * (1.0 - h);
}
float opSS(float a, inout MAT ma, float b, MAT mb, float k) {
  if (b >= INF * 0.5 || a >= INF * 0.5) return a;
  float h = clamp(0.5 - 0.5 * (a + b) / k, 0.0, 1.0); ma = ma * (1.0 - h) + mb * h; return mix(a, -b, h) + k * h * (1.0 - h);
}
// procedural 4D solid textures (0..1), evaluated in object-local coordinates
float texChecker(vec4 p) { vec4 f = floor(p); return mod(f.x + f.y + f.z + f.w, 2.0); }
float texStripes(vec4 p) { return step(0.5, fract(p.x + 0.5 * p.w)); }
float texGrid(vec4 p) { vec4 e = 0.5 - abs(fract(p) - 0.5); return 1.0 - step(0.05, min(min(e.x, e.y), min(e.z, e.w))); }
float texNoise(vec4 p) {
  float n = sin(p.x * 1.7 + sin(p.y * 2.3 + p.w)) + sin(p.z * 1.9 + sin(p.w * 2.9 + p.x)) + 0.5 * sin(3.1 * p.y + 2.0 * sin(1.3 * p.z));
  return clamp(0.5 + 0.2 * n, 0.0, 1.0);
}
float texRings(vec4 p) { return step(0.5, fract(length(p))); }
// 4D value noise: hashed lattice values, smoothly interpolated over the 16 corners of a cell
float h4(vec4 c) { return fract(sin(dot(c, vec4(127.1, 311.7, 74.7, 269.5))) * 43758.5453); }
vec4 corner(int k) { return vec4(float(k & 1), float((k >> 1) & 1), float((k >> 2) & 1), float((k >> 3) & 1)); }
float vnoise(vec4 p) {
  vec4 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float r = 0.0;
  for (int k = 0; k < 16; k++) {
    vec4 o = corner(k), w = mix(1.0 - f, f, o);
    r += h4(i + o) * w.x * w.y * w.z * w.w;
  }
  return r;
}
float fbm(vec4 p) { return 0.5 * vnoise(p) + 0.25 * vnoise(p * 2.03 + 1.7) + 0.125 * vnoise(p * 4.01 + 3.1); }
float texDots(vec4 p) { return step(length(fract(p) - 0.5), 0.32); }
float texWaves(vec4 p) { return 0.5 + 0.5 * sin(6.2832 * (p.x + 0.35 * sin(2.5 * p.y + p.w) + 0.25 * p.z)); }
float texMarble(vec4 p) { return 0.5 + 0.5 * sin(3.0 * p.x + 6.0 * fbm(p)); }
float texWood(vec4 p) { return smoothstep(0.35, 0.65, fract(length(p.xz) * 3.0 + 1.5 * fbm(p * 0.8))); }
float texCells(vec4 p) {  // edges of 4D Voronoi cells (feature points of the 16 nearest lattice cells)
  vec4 base = floor(p) + step(0.5, fract(p)) - 1.0;
  float d1 = 9.0, d2 = 9.0;
  for (int k = 0; k < 16; k++) {
    vec4 c = base + corner(k);
    float d = length(p - c - vec4(h4(c), h4(c + 11.3), h4(c + 23.7), h4(c + 37.1)));
    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
  }
  return 1.0 - smoothstep(0.03, 0.08, d2 - d1);
}
float texBricks(vec4 p) {
  vec4 q = p * vec4(1.0, 2.0, 1.0, 1.0);
  float shift = 0.5 * mod(floor(q.y), 2.0);  // every other row offset by half a brick
  q.x += shift; q.z += shift;
  vec4 e = 0.5 - abs(fract(q) - 0.5);
  return 1.0 - step(0.06, min(min(e.x, e.y), e.z));  // mortar
}
float texGradient(vec4 p) { return clamp(0.5 + 0.5 * p.y, 0.0, 1.0); }
// the texture type is data (node slot 12.y): switching it never recompiles
float texAny(int t, vec4 p) {
  if (t == 1) return texChecker(p);
  if (t == 2) return texStripes(p);
  if (t == 3) return texGrid(p);
  if (t == 4) return texNoise(p);
  if (t == 5) return texRings(p);
  if (t == 6) return texDots(p);
  if (t == 7) return texWaves(p);
  if (t == 8) return texMarble(p);
  if (t == 9) return texWood(p);
  if (t == 10) return texCells(p);
  if (t == 11) return texBricks(p);
  if (t == 12) return texGradient(p);
  return 0.0;
}
)";

const char* sdfHelpersGLSL() { return SDF_HELPERS; }

std::string renameCustomCode(const std::string& code, const std::string& suffix) {
  // rename every top-level function defined in the code (e.g. sdf -> sdf_c3) so that
  // several custom SDFs (and their helpers) can live in one shader
  static const std::regex def(R"(\b(?:float|vec[234]|mat[234]|int|bool|void)\s+([A-Za-z_]\w*)\s*\()");
  std::vector<std::string> names;
  for (auto it = std::sregex_iterator(code.begin(), code.end(), def); it != std::sregex_iterator(); ++it) {
    std::string n = (*it)[1];
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
  }
  std::string out = code;
  for (auto& n : names) out = std::regex_replace(out, std::regex("\\b" + n + "\\b"), n + suffix);
  return out;
}


// Coordinate pass of a modifier stack (outermost first). Extrusions capture their
// coordinate in t<k>; the distance pass (extrudeGLSL) applies them afterwards.
// mb: GLSL int expression of the node's first modifier-parameter row (one vec4 per modifier, see
// packNodes): the numbers are data, so editing them never recompiles. nullptr: constants (the
// standalone code viewer).
static std::string modP(const char* mb, int k) { return std::string("ndatx(") + mb + ", " + std::to_string(k) + ")"; }

static std::string modifierCoordsGLSL(const Node& n, const std::vector<ModAxis>& ax, const char* v, const std::string& tag, const char* mb = nullptr) {
  std::ostringstream o;
  auto c = [&](int a) { return std::string(v) + "." + C[a]; };
  for (int k = (int)n.mods.size() - 1; k >= 0; k--) {
    const Modifier& m = n.mods[k];
    if (!ax[k].valid) continue;
    if (mb) {
      std::string P = modP(mb, k);
      switch (m.type) {
        case MOD_MIRROR: o << "  " << c(m.axis) << " = abs(" << c(m.axis) << ") - " << P << ".x;\n"; break;
        case MOD_REPEAT:  // (count 0 = infinite: packed as a huge count)
          o << "  { vec4 rp = " << P << "; " << c(m.axis) << " -= rp.x * clamp(floor(" << c(m.axis) << " / rp.x + 0.5), -rp.y, rp.y); }\n";
          break;
        case MOD_RADIAL: {
          std::string sw = std::string(v) + "." + C[m.i] + C[m.j];
          o << "  { vec4 rp = " << P << "; " << sw << " = opRadial(" << sw << ", rp.x, rp.y, rp.z, rp.w); }\n";
          break;
        }
        case MOD_EXTRUDE:
          if (m.mode != ExtrudeMode::Infinite) o << "  float t" << tag << k << " = " << c(ax[k].newAxis) << ";\n";
          o << "  " << c(ax[k].newAxis) << " = 0.0;\n";
          break;
        case MOD_REVOLVE:
          o << "  " << c(ax[k].srcAxis) << " = length(vec2(" << c(ax[k].srcAxis) << ", " << c(ax[k].newAxis) << ")) - " << P << ".x;\n";
          o << "  " << c(ax[k].newAxis) << " = 0.0;\n";
          break;
      }
      continue;
    }
    switch (m.type) {
      case MOD_MIRROR:
        o << "  " << c(m.axis) << " = abs(" << c(m.axis) << ")";
        if (m.offset != 0) o << " - " << glslFloat(m.offset);
        o << ";\n";
        break;
      case MOD_REPEAT: {
        if (m.spacing <= 0) break;
        std::string a = c(m.axis), s = glslFloat(m.spacing);
        std::string r = m.count > 0 ? "clamp(floor(" + a + " / " + s + " + 0.5), " + glslFloat(-m.count) + ", " + glslFloat(m.count) + ")"
                                    : "floor(" + a + " / " + s + " + 0.5)";
        o << "  " << a << " -= " << s << " * " << r << ";\n";
        break;
      }
      case MOD_RADIAL: {
        std::string sw = std::string(v) + "." + C[m.i] + C[m.j];
        o << "  " << sw << " = opRadial(" << sw << ", " << glslFloat(m.radius) << ", " << glslFloat(std::max(1, m.count)) << ", "
          << glslFloat(m.startAngle * PI / 180) << ", " << glslFloat(m.angle * PI / 180) << ");\n";
        break;
      }
      case MOD_EXTRUDE:
        if (m.mode != ExtrudeMode::Infinite) o << "  float t" << tag << k << " = " << c(ax[k].newAxis) << ";\n";
        o << "  " << c(ax[k].newAxis) << " = 0.0;\n";
        break;
      case MOD_REVOLVE:
        o << "  " << c(ax[k].srcAxis) << " = length(vec2(" << c(ax[k].srcAxis) << ", " << c(ax[k].newAxis) << ")) - " << glslFloat(m.radius) << ";\n";
        o << "  " << c(ax[k].newAxis) << " = 0.0;\n";
        break;
    }
  }
  return o.str();
}

static std::string extrudeGLSL(const Node& n, const std::vector<ModAxis>& ax, const std::string& tag, const char* mb = nullptr) {
  std::ostringstream o;
  for (size_t k = 0; k < n.mods.size(); k++) {
    const Modifier& m = n.mods[k];
    if (!ax[k].valid || m.type != MOD_EXTRUDE || m.mode == ExtrudeMode::Infinite) continue;
    if (m.mode == ExtrudeMode::Half) o << "  d = opExtrude(d, t" << tag << k << ");\n";
    else if (mb) o << "  d = opExtrude(d, abs(t" << tag << k << ") - " << modP(mb, (int)k) << ".x);\n";  // .x: half length
    else o << "  d = opExtrude(d, abs(t" << tag << k << ") - " << glslFloat(m.length / 2) << ");\n";
  }
  return o.str();
}

// ---------------------------------------------------------------- polyhedra and honeycombs
static std::string v3(const V4& f) { return "vec3(" + glslFloat(f[0]) + ", " + glslFloat(f[1]) + ", " + glslFloat(f[2]) + ")"; }

// GLSL computing `float d` for a polyhedron. Flat: q = local point, h = inradius expression.
// Curved: q = model point in the node's frame, h = inradius in world units.
static std::string polyGLSL(int type, bool curved, const std::string& q, const std::string& h) {
  std::ostringstream o;
  o << "  float d = -1e30;\n";
  if (curved) o << "  { float th = (" << h << ") / u_curvR, Ct = cC(th), St = cS(th);\n";
  for (auto& f : polyFaces(type)) {
    if (curved) o << "    d = max(d, dot(" << q << ".xyz, " << v3(f) << ") * Ct - " << q << ".w * St);\n";
    else o << "  d = max(d, dot(" << q << ".xyz, " << v3(f) << ") - (" << h << "));\n";
  }
  if (curved) o << "    d = u_curvR * asinK(d); }\n";
  return o.str();
}

// A function folding a point into the central cell of a honeycomb and returning the distance to
// its edges / its shrunk cell. par = 1 for every other cell (alternate colours).
static std::string honeycombFn(const std::string& name, const PolyParams& pp, bool curved) {
  std::ostringstream o;
  const auto& all = honeycombs();
  int hi = std::clamp(pp.honey, 0, (int)all.size() - 1);
  const Honeycomb& hc = all[hi];
  const auto& F = polyFaces(hc.cell);
  int n = (int)F.size();
  double th = honeycombInradius(hc);
  o << "float " << name << "(vec4 q, float h, float w, float style, out float par) {\n";
  o << "  const vec3 F[" << n << "] = vec3[" << n << "](";
  for (int i = 0; i < n; i++) o << (i ? ", " : "") << v3(F[i]);
  o << ");\n";
  if (curved) {
    double k = hc.space == SpaceKind::Spherical ? 1 : -1;
    o << "  const float Ct = " << glslFloat(cs::C(k, th)) << ", St = " << glslFloat(cs::S(k, th)) << ";\n";
  }
  std::string fv = curved ? "dot(q.xyz, F[i]) * Ct - q.w * St" : "dot(q.xyz, F[i]) - h";
  o << "  par = 0.0;\n  for (int it = 0; it < 96; it++) {\n    float best = 1e-7; int bi = -1;\n"
    << "    for (int i = 0; i < " << n << "; i++) { float x = " << fv << "; if (x > best) { best = x; bi = i; } }\n"
    << "    if (bi < 0) break;\n    vec3 f = F[bi];\n";
  if (curved) o << "    q -= vec4(f * Ct, -KAPPA * St) * (2.0 * best);\n";
  else o << "    q.xyz -= f * (2.0 * best);\n";
  o << "    par = 1.0 - par;\n  }\n";
  if (curved) o << "  if (abs(q.w) < 100.0) q = crenormC(q);\n";
  o << "  float a[" << n << "];\n  for (int i = 0; i < " << n << "; i++) { float x = " << fv << "; a[i] = -(" << (curved ? "u_curvR * asinK(x)" : "x") << "); }\n";
  // style (data): 1 = cells shrunk by the gap, 0 = edges
  o << "  if (style > 0.5) { float m = 1e30;\n  for (int i = 0; i < " << n << "; i++) m = min(m, a[i]);\n  return -m + w; }\n";
  double t = hc.dihedralDeg * PI / 180, ct = std::cos(t), st2 = std::max(std::sin(t) * std::sin(t), 1e-6);
  o << "  float e = 1e30, x, y;\n";
  for (auto& [i, j] : polyEdges(hc.cell))
    o << "  x = max(a[" << i << "], 0.0); y = max(a[" << j << "], 0.0); e = min(e, sqrt(max((x * x + y * y + " << glslFloat(2 * ct) << " * x * y) / "
      << glslFloat(st2) << ", 0.0)));\n";
  o << "  return e - w;\n}\n";
  return o.str();
}

// ---------------------------------------------------------------- curved spaces
// Every node function gets a model point in its parent's frame, applies the node's inverse
// isometry (relative to the parent, slots 0-3; the root's includes the camera) and either passes
// the point on (groups) or evaluates its shape in normal coordinates around its origin (leaves).
static const char* CURVED_HELPERS = R"(
float cC(float t) { return KAPPA > 0.0 ? cos(t) : 0.5 * (exp(t) + exp(-t)); }
float cS(float t) { return KAPPA > 0.0 ? sin(t) : 0.5 * (exp(t) - exp(-t)); }
// angle (model distance) of q from the origin
float cAngO(vec4 q) { float l = length(q.xyz); return KAPPA > 0.0 ? atan(l, q.w) : log(l + sqrt(l * l + 1.0)); }
float cdistO(vec4 q) { return u_curvR * cAngO(q); }
float asinK(float x) { return KAPPA > 0.0 ? asin(clamp(x, -1.0, 1.0)) : sign(x) * log(abs(x) + sqrt(x * x + 1.0)); }
float cmdot(vec4 a, vec4 b) { return dot(a.xyz, b.xyz) + KAPPA * a.w * b.w; }
// q moved so its foot on the geodesic subspace through the origin spanned by the axes in mask
// (1 = keep that coordinate) becomes the origin: a translation along the subspace
vec4 toSlice(vec4 q, vec3 mask) {
  // closed form (no cancellation of large coordinates far away): the translation along the
  // subspace keeps the components off it and zeroes those along it; w follows from the model
  vec3 off = q.xyz * (1.0 - mask);
  return vec4(off, sqrt(max(1.0 - KAPPA * dot(off, off), 0.0)));
}
// exact repeat along a local axis: the nearest copy (within +-count, 0 = infinite) moved to the origin.
// In the (axis, w) plane the point is rho (sinK t, cosK t) with rho^2 = 1 - KAPPA |other components|^2.
vec4 repeatExact(vec4 q, int a, float spacing, float count) {
  vec3 msk = vec3(1.0);
  msk[a] = 0.0;
  vec3 oth = q.xyz * msk;  // the other two components (summed directly: no cancellation)
  float o2 = dot(oth, oth);
  float rho = sqrt(max(1.0 - KAPPA * o2, 1e-12));
  float t = KAPPA < 0.0 ? asinK(q[a] / rho) : atan(q[a], q.w);
  float kc = floor(t * u_curvR / spacing + 0.5);
  if (count > 0.0) kc = clamp(kc, -count, count);
  float t2 = t - kc * spacing / u_curvR;
  q[a] = rho * cS(t2);
  q.w = rho * cC(t2);
  return q;
}
// distance from q to the geodesic line along local axis a (well conditioned far away)
float lineDist(vec4 q, int a) { vec3 msk = vec3(1.0); msk[a] = 0.0; return u_curvR * asinK(length(q.xyz * msk)); }
vec4 crenormC(vec4 p) { float l = dot(p.xyz, p.xyz) + KAPPA * p.w * p.w; return p * inversesqrt(max(l * KAPPA, 1e-30)); }
vec4 clogO(vec4 q) {  // normal coordinates (world units) of q
  float l = length(q.xyz);
  vec3 d = l > 1e-12 ? q.xyz / l : vec3(1.0, 0.0, 0.0);
  return vec4(d * (u_curvR * cAngO(q)), 0.0);
}
vec4 cexpO(vec4 u) {
  float l = length(u.xyz);
  if (l < 1e-12) return vec4(0.0, 0.0, 0.0, 1.0);
  float t = l / u_curvR;
  return vec4(u.xyz * (cS(t) / l), cC(t));
}
)";

static CompiledScene compileCurved(Node& root, const PrimitiveRegistry& reg, const std::unordered_set<std::string>* badCode, SpaceKind spaceKind) {
  CompiledScene cs;
  std::unordered_map<std::string, int> index;
  walk(root, [&](Node& n, Node*, int) { index[n.id] = (int)cs.nodeIds.size(); cs.nodeIds.push_back(n.id); });
  std::ostringstream o;
  o << SDF_HELPERS << SCENE_HELPERS << CURVED_HELPERS;
  std::map<std::string, std::string> primFn;
  walk(root, [&](Node& n, Node*, int) {
    if (n.isCustom()) {
      std::string suffix = "_c" + std::to_string(index[n.id]);
      if (badCode && badCode->count(n.code))
        o << "float sdf" << suffix << "(vec4 p) { vec4 e = abs(p) - " << glslFloat(n.codeBound) << "; return length(max(e, 0.0)) + min(max(max(e.x, e.y), max(e.z, e.w)), 0.0); }\n";
      else o << "// custom SDF: " << n.name << "\n" << renameCustomCode(n.code, suffix) << "\n";
      return;
    }
    if (!n.isLeaf() || primFn.count(n.prim)) return;
    auto* def = reg.get(n.prim);
    if (!def) return;
    std::string name = "prim" + std::to_string(primFn.size());
    primFn[n.prim] = name;
    o << "// " << def->name << "\n" << primitiveGLSL(*def, name);
  });
  auto D = [](int i, int k) { return "ndat(" + std::to_string(i) + ", " + std::to_string(k) + ")"; };
  auto ROT = [&](int i) { return "mat4(" + D(i, 0) + ", " + D(i, 1) + ", " + D(i, 2) + ", " + D(i, 3) + ")"; };
  auto fn = [&](const Node& n) { return "n" + std::to_string(index[n.id]) + "_c"; };
  // "absolute" nodes (no ancestor group with modifiers) take their transform straight from the
  // camera-relative point pc (one matrix, made in double precision); others chain from the parent
  std::function<void(Node&, bool)> emit = [&](Node& n, bool absolute) {
    if (!n.hidden && !n.isCamera() && !n.isLeaf()) for (auto& c : n.children) emit(c, absolute && n.mods.empty());
    int i = index[n.id];
    std::ostringstream L;
    L << "float " << fn(n) << "(vec4 p, vec4 pc, out MAT m) {\n";
    const PrimitiveDef* def = n.isLeaf() ? reg.get(n.prim) : nullptr;
    if (n.isHoneycomb()) {
      int hi = std::clamp(n.poly.honey, 0, (int)honeycombs().size() - 1);
      if (honeycombs()[hi].space != spaceKind) def = nullptr;  // another space's tiling: nothing
      else { o << honeycombFn("honey_c" + std::to_string(i), n.poly, true); }
    }
    bool shapeOk = n.isPolytope() ? !(n.isHoneycomb() && honeycombs()[std::clamp(n.poly.honey, 0, (int)honeycombs().size() - 1)].space != spaceKind) : def != nullptr;
    if (n.hidden || n.isCamera() || (n.isLeaf() && !shapeOk) || (n.isGroup() && n.children.empty())) {
      L << "  m = MAT(0.0); return INF;\n}\n";
      o << L.str();
      return;
    }
    if (n.isLeaf()) L << "  if (g_noEmissive && " << D(i, 9) << ".x > 0.0) { m = MAT(0.0); return INF; }\n";  // (emissive: data)
    L << "  vec4 par = " << D(i, 5) << ";\n";
    const Modifier* rep = n.isLeaf() ? exactRepeat(n) : nullptr;
    if (n.id != root.id) L << "  float br = " << D(i, 8) << ".x;\n  if (br < 0.0) { m = MAT(0.0); return INF; }\n";  // out of reach (packNodes)
    L << "  vec4 qm = " << ROT(i) << " * " << (absolute ? "pc" : "p") << ";\n";
    if (n.id != root.id) {
      // (a repeat: br = one copy's radius, bounded by the distance to the line of copies)
      if (rep) L << "  float bd = lineDist(qm, " << rep->axis << ") - br;\n";
      else L << "  float bd = cdistO(qm) - br;\n";
      L << "  if (bd > par.w && !g_exact) { m = MAT(0.0); return bd; }\n";
    }
    L << "  float s = par.x;\n";
    auto ax = modifierAxes(n, reg);
    if (n.isPolytope()) {
      // exact flat faces of the curved space, on the model point itself
      L << "  vec4 q0 = clogO(qm) / s;\n  vec4 c6 = " << D(i, 6) << ", m9 = " << D(i, 9) << ";\n  vec3 col = c6.rgb;\n";
      if (n.isPoly()) L << polyGLSL(n.poly.type, true, "qm", "m9.w * s");
      else {
        L << "  float hpar;\n  float d = honey_c" << i << "(qm, 0.0, " << D(i, 10) << ".w, " << D(i, 13) << ".z, hpar);\n";
        L << "  col = mix(col, " << D(i, 10) << ".rgb, hpar * " << D(i, 13) << ".w);\n";  // .w: alternate colours
      }
      L << "  vec4 x12 = " << D(i, 12) << ";\n  d -= x12.w * s;\n";  // rounding
      L << "  if (u_textures == 1 && x12.y > 0.5) col = mix(col, " << D(i, 10) << ".rgb, texAny(int(x12.y), q0 * m9.y) * m9.z);\n";
      L << "  vec4 m11 = " << D(i, 11) << ";\n  m = MAT(vec4(col, c6.w), vec4(m9.x, m11.xyz), vec4(m11.w, " << D(i, 8) << ".y, 0.0, 0.0));\n";
      L << "  return d;\n}\n";
    } else if (n.isLeaf()) {
      // exact in curved space (as the CPU evaluator): infinite extrusions measured from the point's
      // foot on their plane / line, the outermost repeat by exact copies along its line
      auto sym = symmetryAxes(n, reg);
      if (!sym.empty()) {
        double mk[3] = {0, 0, 0};
        for (int a : sym) mk[a] = 1;
        L << "  qm = toSlice(qm, vec3(" << glslFloat(mk[0]) << ", " << glslFloat(mk[1]) << ", " << glslFloat(mk[2]) << "));\n";
      }
      Node shape = n;
      if (const Modifier* rm = exactRepeat(n)) {
        L << "  int mrow = int(" << D(i, 12) << ".x);\n";
        L << "  { vec4 rp = ndatx(mrow, " << (int)n.mods.size() - 1 << "); qm = repeatExact(qm, " << rm->axis << ", rp.x * s, rp.y); }\n";
        shape.mods.pop_back();
        ax = modifierAxes(shape, reg);
      } else if (!n.mods.empty()) L << "  int mrow = int(" << D(i, 12) << ".x);\n";
      L << "  vec4 q = clogO(qm) / s;\n  vec4 q0 = q;\n";
      L << modifierCoordsGLSL(shape, ax, "q", "", "mrow");
      if (n.isCustom()) L << "  float d = sdf_c" << i << "(q);\n";
      else L << "  float d = " << primFn[n.prim] << "(q);\n";
      L << extrudeGLSL(shape, ax, "", "mrow");
      L << "  vec4 x12 = " << D(i, 12) << ";\n  if (x12.z > 0.0) d = abs(d) - x12.z;\n  d -= x12.w;\n";  // shell, rounding (data)
      for (int a = nodeDim(n, reg); a < 4; a++) L << "  d = opExtrude(d, abs(q." << C[a] << ") - 0.5 * par.z);\n";
      L << "  vec4 c6 = " << D(i, 6) << ", m9 = " << D(i, 9) << ";\n  vec3 col = c6.rgb;\n";
      L << "  if (u_textures == 1 && x12.y > 0.5) {\n    vec4 tq = q0 * m9.y;\n";
      for (int a = nodeDim(n, reg); a < 4; a++) L << "    tq." << C[a] << " = 0.5;\n";
      L << "    col = mix(col, " << D(i, 10) << ".rgb, texAny(int(x12.y), tq) * m9.z);\n  }\n";
      L << "  vec4 m11 = " << D(i, 11) << ";\n  m = MAT(vec4(col, c6.w), vec4(m9.x, m11.xyz), vec4(m11.w, " << D(i, 8) << ".y, 0.0, 0.0));\n";
      L << "  return d * s * " << D(i, 8) << ".w;\n}\n";  // .w: spherical safety factor
    } else {
      if (!n.mods.empty()) L << "  int mrow = int(" << D(i, 12) << ".x);\n";
      std::string coords = modifierCoordsGLSL(n, ax, "q", "", "mrow");
      if (!coords.empty()) L << "  vec4 q = clogO(qm) / s;\n" << coords << "  q.w = 0.0;\n  qm = cexpO(q * s);\n";
      static const char* opf[6] = {"opU", "opI", "opS", "opSU", "opSI", "opSS"};
      L << "  float k = max(par.y, 1e-4);\n  MAT mb;\n";
      L << "  float d = " << fn(n.children[0]) << "(qm, pc, m);\n";
      for (size_t c = 1; c < n.children.size(); c++)
        L << "  { float b = " << fn(n.children[c]) << "(qm, pc, mb); d = " << opf[(int)n.op] << "(d, m, b, mb" << (isSmooth(n.op) ? ", k" : "") << "); }\n";
      std::string ex = extrudeGLSL(n, ax, "", "mrow");
      if (!ex.empty()) L << "  if (d < INF * 0.5) {\n  d /= s;\n" << ex << "  d *= s;\n  }\n";
      L << "  return d;\n}\n";
    }
    o << L.str();
  };
  emit(root, true);
  o << "float mapScene(vec4 p, out MAT m) { return " << fn(root) << "(p, p, m); }\n";
  o << "float worldSDF(vec4 p) { return INF; }\nfloat worldSDFm(vec4 p, out MAT m) { m = MAT(0.0); return INF; }\n";
  cs.glsl = o.str();
  return cs;
}

CompiledScene compileScene(Node& root, const PrimitiveRegistry& reg, const std::string& activeWorldIn, bool geo,
                           const std::unordered_set<std::string>* badCode, SpaceKind space) {
  if (space != SpaceKind::Flat) return compileCurved(root, reg, badCode, space);
  CompiledScene cs;
  std::unordered_map<std::string, int> index;
  walk(root, [&](Node& n, Node*, int) { index[n.id] = (int)cs.nodeIds.size(); cs.nodeIds.push_back(n.id); });
  std::string world = (!activeWorldIn.empty() && findNode(root, activeWorldIn)) ? activeWorldIn : "";
  cs.hasWorld = !world.empty();
  auto exists = [&](const std::string& id) { return index.count(id) > 0; };
  int N = (int)cs.nodeIds.size();
  EvalCtx main = geo ? EvalCtx::Geo : EvalCtx::Full;

  std::ostringstream o;
  // per-node data comes from the node buffer (see packNodes): ndat(node, slot)
  (void)N;
  o << SDF_HELPERS << SCENE_HELPERS << FOLLOW_GLSL;

  // primitives
  std::map<std::string, std::string> primFn;
  walk(root, [&](Node& n, Node*, int) {
    if (n.isCustom()) {  // user GLSL (functions renamed per node); broken code falls back to its bounding box
      std::string suffix = "_c" + std::to_string(index[n.id]);
      if (badCode && badCode->count(n.code))
        o << "float sdf" << suffix << "(vec4 p) { vec4 e = abs(p) - " << glslFloat(n.codeBound) << "; return length(max(e, 0.0)) + min(max(max(e.x, e.y), max(e.z, e.w)), 0.0); }\n";
      else o << "// custom SDF: " << n.name << "\n" << renameCustomCode(n.code, suffix) << "\n";
      return;
    }
    if (!n.isLeaf() || primFn.count(n.prim)) return;
    auto* def = reg.get(n.prim);
    if (!def) return;
    std::string name = "prim" + std::to_string(primFn.size());
    primFn[n.prim] = name;
    o << "// " << def->name << "\n" << primitiveGLSL(*def, name);
  });

  // world slots: active world + worlds used by 'hug' nodes in this context
  std::vector<std::string> worlds;
  if (!world.empty()) worlds.push_back(world);
  walk(root, [&](Node& n, Node*, int) {
    if (nodeRole(n, main, world, exists) == Role::Hug && std::find(worlds.begin(), worlds.end(), n.anchor->world) == worlds.end())
      worlds.push_back(n.anchor->world);
  });
  auto wslot = [&](const std::string& id) { return (int)(std::find(worlds.begin(), worlds.end(), id) - worlds.begin()); };
  for (size_t k = 0; k < worlds.size(); k++) o << "float worldSDF" << k << "(vec4 g);\nfloat g_world" << k << " = INF;\n";

  std::unordered_set<std::string> emitted;
  auto fnName = [&](const Node& n, EvalCtx ctx) { return "n" + std::to_string(index[n.id]) + (ctx == EvalCtx::World ? "_w" : "_m"); };

  // node data slots: 0-3 inverse rotation, 4 position, 5 (scale, k | reach, thickness, skip),
  // 6 colour (+ highlight | group reach), 7 bounding centre, 8 bounding radius, 9 (emissive, tex scale,
  // tex strength), 10 texture colour, 11 (roughness, specular, transparency)
  auto D = [](int i, int k) { return "ndat(" + std::to_string(i) + ", " + std::to_string(k) + ")"; };
  auto ROT = [&](int i) { return "mat4(" + D(i, 0) + ", " + D(i, 1) + ", " + D(i, 2) + ", " + D(i, 3) + ")"; };
  std::function<void(Node&, EvalCtx)> emitNode = [&](Node& n, EvalCtx ctx) {
    std::string name = fnName(n, ctx);
    if (emitted.count(name)) return;
    emitted.insert(name);
    Role role = nodeRole(n, ctx, world, exists);
    if (role != Role::Empty && !n.isLeaf()) for (auto& c : n.children) emitNode(c, ctx);
    int i = index[n.id];
    std::ostringstream L;
    L << "float " << name << "(vec4 p, vec4 g, float sAcc, out MAT m) {\n";
    const PrimitiveDef* def = n.isLeaf() ? reg.get(n.prim) : nullptr;
    bool flatHoney = n.isHoneycomb() && honeycombs()[std::clamp(n.poly.honey, 0, (int)honeycombs().size() - 1)].space == SpaceKind::Flat;
    if (flatHoney && role != Role::Empty) o << honeycombFn("honey_f" + std::to_string(i), n.poly, false);
    bool shapeOk = n.isPoly() || flatHoney || (!n.isPolytope() && def);
    if (role == Role::Empty || (n.isLeaf() && !shapeOk) || (!n.isLeaf() && n.children.empty())) {
      L << "  m = MAT(0.0); return INF;\n}\n";
      o << L.str();
      return;
    }
    if (n.isLeaf()) L << "  if (g_noEmissive && " << D(i, 9) << ".x > 0.0) { m = MAT(0.0); return INF; }\n";  // (emissive: data)
    // bounding-sphere early out (threshold covers the parent's smooth blend radius)
    if (n.id != root.id) L << "  vec4 par = " << D(i, 5) << ";\n  float bd = length(p - " << D(i, 7) << ") - " << D(i, 8) << ".x;\n  if (bd > par.w && !g_exact) { m = MAT(0.0); return bd; }\n";
    else L << "  vec4 par = " << D(i, 5) << ";\n";
      bool follow = n.anchor && n.anchor->follow && (role == Role::Tangent || role == Role::Hug);
    if (follow)  // bent along the surface (packNodes stores the path; 13.x = its first row)
      L << "  float s = par.x;\n  vec4 q = followLocal(p, int(" << D(i, 13) << ".x)) / s;\n  vec4 q0 = q;\n";
    else L << "  float s = par.x;\n  vec4 q = (" << ROT(i) << " * (p - " << D(i, 4) << ")) / s;\n  vec4 q0 = q;\n";
    bool anchored = role == Role::Tangent || role == Role::Hug;
    if (anchored) L << "  float nd = q.w;\n";
    if (role == Role::Tangent) L << "  q.w = 0.0;\n";
    if (role == Role::Hug) L << "  q.w = g_world" << wslot(n.anchor->world) << " / (sAcc * s);\n";
    auto ax = modifierAxes(n, reg);
    if (!n.mods.empty()) L << "  int mrow = int(" << D(i, 12) << ".x);\n";  // modifier parameters (data)
    L << modifierCoordsGLSL(n, ax, "q", "", "mrow");
    if (n.isLeaf()) {
      L << "  float hpar = 0.0;\n";
      if (n.isPoly()) L << polyGLSL(n.poly.type, false, "q", D(i, 9) + ".w");
      else if (n.isHoneycomb()) L << "  float d = honey_f" << i << "(q, " << D(i, 9) << ".w, " << D(i, 10) << ".w, " << D(i, 13) << ".z, hpar);\n";
      else if (n.isCustom()) L << "  float d = sdf_c" << i << "(q);\n";
      else L << "  float d = " << primFn[n.prim] << "(q);\n";
      L << extrudeGLSL(n, ax, "", "mrow");
      // shell / rounding act on the shape itself, before its thickness along the unused axes (data: slot 12)
      L << "  vec4 x12 = " << D(i, 12) << ";\n  if (x12.z > 0.0) d = abs(d) - x12.z;\n  d -= x12.w;\n";
      for (int a = nodeDim(n, reg); a < 4; a++) L << "  d = opExtrude(d, abs(q." << C[a] << ") - 0.5 * par.z);\n";
      L << "  vec4 c6 = " << D(i, 6) << ", m9 = " << D(i, 9) << ";\n  vec3 col = c6.rgb;\n";
      if (n.isHoneycomb()) L << "  col = mix(col, " << D(i, 10) << ".rgb, hpar * " << D(i, 13) << ".w);\n";
      // textures are 4D patterns: along the axes a thin (< 4D) object does not use it sits on a
      // cell boundary (coordinate 0), which would make grids solid and checkers flicker, so those
      // coordinates are put in the middle of a cell. The type is data (12.y); textures can be off (fast mode)
      L << "  if (u_textures == 1 && x12.y > 0.5) {\n    vec4 tq = q0 * m9.y;\n";
      for (int a = nodeDim(n, reg); a < 4; a++) L << "    tq." << C[a] << " = 0.5;\n";
      L << "    col = mix(col, " << D(i, 10) << ".rgb, texAny(int(x12.y), tq) * m9.z);\n  }\n";
      L << "  vec4 m11 = " << D(i, 11) << ";\n  m = MAT(vec4(col, c6.w), vec4(m9.x, m11.xyz), vec4(m11.w, " << D(i, 8) << ".y, 0.0, 0.0));\n";
      // attached to a surface: the shape is extruded along the surface normal, so keep it to its
      // own sheet (two-sided, within its size): other sheets of the world (e.g. the other room of a
      // double wormhole) must not show it
      if (anchored) L << "  return max(d * s, abs(nd) * s - " << D(i, 8) << ".z);\n}\n";  // .z: attachCut
      else L << "  return d * s;\n}\n";
    } else {
      static const char* opf[6] = {"opU", "opI", "opS", "opSU", "opSI", "opSS"};
      L << "  float k = max(par.y, 1e-4);\n  MAT mb;\n";
      L << "  float d = " << fnName(n.children[0], ctx) << "(q, g, sAcc * s, m);\n";
      for (size_t c = 1; c < n.children.size(); c++)
        L << "  { float b = " << fnName(n.children[c], ctx) << "(q, g, sAcc * s, mb); d = " << opf[(int)n.op] << "(d, m, b, mb"
          << (isSmooth(n.op) ? ", k" : "") << "); }\n";
      std::string ex = extrudeGLSL(n, ax, "", "mrow");
      if (!ex.empty()) L << "  if (d < INF * 0.5) {\n" << ex << "  }\n";
      if (anchored) L << "  return d >= INF * 0.5 ? d : max(d * s, abs(nd) * s - " << D(i, 8) << ".z);\n}\n";
      else L << "  return d >= INF * 0.5 ? d : d * s;\n}\n";
    }
    o << L.str();
  };

  for (size_t k = 0; k < worlds.size(); k++) {
    auto path = pathTo(root, worlds[k]);
    Node& target = *path.back();
    emitNode(target, EvalCtx::World);
    o << "float worldSDF" << k << "m(vec4 g, out MAT m) {\n  vec4 p = g; float s = 1.0;\n";
    for (size_t a = 0; a + 1 < path.size(); a++) {
      int i = index[path[a]->id];
      o << "  p = (" << ROT(i) << " * (p - " << D(i, 4) << ")) / " << D(i, 5) << ".x; s *= " << D(i, 5) << ".x;\n";
      std::string mr = "int(" + D(i, 12) + ".x)";
      o << "  {\n" << modifierCoordsGLSL(*path[a], modifierAxes(*path[a], reg), "p", "w", mr.c_str()) << "  }\n";
    }
    o << "  return " << fnName(target, EvalCtx::World) << "(p, g, s, m) * s;\n}\n";
    o << "float worldSDF" << k << "(vec4 g) { MAT m; return worldSDF" << k << "m(g, m); }\n";
  }

  emitNode(root, main);
  std::ostringstream hug;
  walk(root, [&](Node& n, Node*, int) {
    if (nodeRole(n, main, world, exists) == Role::Hug) {
      std::string line = "g_world" + std::to_string(wslot(n.anchor->world)) + " = worldSDF" + std::to_string(wslot(n.anchor->world)) + "(p); ";
      if (hug.str().find(line) == std::string::npos) hug << line;
    }
  });
  o << "float mapScene(vec4 p, out MAT m) { " << hug.str() << "return " << fnName(root, main) << "(p, p, 1.0, m); }\n";
  // active world: distance, and distance + surface material (used for the Geodesic sky colour)
  if (!world.empty()) o << "float worldSDF(vec4 p) { return worldSDF0(p); }\nfloat worldSDFm(vec4 p, out MAT m) { return worldSDF0m(p, m); }\n";
  else o << "float worldSDF(vec4 p) { return INF; }\nfloat worldSDFm(vec4 p, out MAT m) { m = MAT(0.0); return INF; }\n";
  cs.glsl = o.str();
  return cs;
}

std::vector<float> packNodes(Node& root, const PrimitiveRegistry& reg, const CompiledScene& cs, const std::unordered_set<std::string>& highlight,
                             const Space* space, const M4* camIso) {
  const int S = NODE_VEC4S * 4;  // floats per node (NODE_VEC4S vec4 slots, see emitNode)
  const int S_PER = S;
  size_t N = cs.nodeIds.size();
  std::vector<float> d(std::max<size_t>(N, 1) * S, 0.f);
  auto bounds = computeBounds(root, reg);
  std::unordered_map<std::string, Node*> byId;
  std::unordered_map<std::string, Node*> parentOf;
  walk(root, [&](Node& n, Node* p, int) { byId[n.id] = &n; parentOf[n.id] = p; });
  for (size_t i = 0; i < N; i++) {
    auto it = byId.find(cs.nodeIds[i]);
    if (it == byId.end()) continue;
    Node& n = *it->second;
    float* v = &d[i * S];
    auto slot = [&](int k) { return v + k * 4; };
    M4 inv = transpose(nodeRotation(n));
    for (int k = 0; k < 16; k++) v[k] = (float)inv[k];  // slots 0-3: columns
    for (int k = 0; k < 4; k++) slot(4)[k] = (float)n.xf.pos[k];
    Node* parent = parentOf[n.id];
    double skip = 0.5 + (parent && isSmooth(parent->op) ? std::max(parent->k, 0.0) : 0.0);
    double reach = n.anchor ? n.anchor->reach : 1e4;
    // slot 5 = (scale, k | leaf: anchor reach, thickness, skip distance)
    slot(5)[0] = (float)n.xf.scale;
    slot(5)[1] = (float)(n.isLeaf() ? reach : n.k);
    slot(5)[2] = (float)(n.isLeaf() ? n.thickness : 0);
    slot(5)[3] = (float)skip;
    if (n.isLeaf()) {
      for (int k = 0; k < 3; k++) slot(6)[k] = (float)n.color[k];
      slot(6)[3] = highlight.count(n.id) ? 1.f : 0.f;
      slot(9)[0] = (float)n.mat.emissive;
      slot(9)[1] = (float)n.mat.texScale;
      slot(9)[2] = (float)n.mat.texStrength;
      for (int k = 0; k < 3; k++) slot(10)[k] = (float)n.mat.color2[k];
      slot(11)[0] = (float)n.mat.roughness;
      slot(11)[1] = (float)n.mat.specular;
      slot(11)[2] = (float)n.mat.transparency;
      slot(11)[3] = (float)n.mat.metallic;
      slot(8)[1] = (float)n.mat.ior;
      slot(9)[3] = (float)n.poly.size;   // polyhedron inradius / flat honeycomb cell size
      slot(10)[3] = (float)n.poly.width;  // honeycomb edge radius / gap
      // slot 12 = (modifier rows, texture type, shell, round), 13 = (follow rows, emissive, honeycomb style, alternate)
      slot(12)[1] = (float)n.mat.texture;
      slot(12)[2] = (float)std::max(n.shell.value_or(0.0), 0.0);
      slot(12)[3] = (float)n.round.value_or(0.0);
      slot(13)[1] = n.mat.emissive > 0 ? 1.f : 0.f;
      slot(13)[2] = (float)n.poly.style;
      slot(13)[3] = n.poly.alternate ? 1.f : 0.f;
    } else slot(6)[3] = (float)reach;  // groups: colour unused, store reach
    auto b = bounds.find(n.id);
    slot(8)[0] = 1e9f;
    if (b != bounds.end()) {
      for (int k = 0; k < 4; k++) slot(7)[k] = (float)b->second.c[k];
      slot(8)[0] = std::isfinite(b->second.r) ? (float)b->second.r : 1e9f;
    }
    // attached objects: how far from the tangent space they may reach (see attachCut)
    slot(8)[2] = (float)attachCut(reach, b != bounds.end() ? b->second.r : INFINITY);
  }
  // modifier parameters: extra rows after the nodes, one vec4 per modifier (see modifierCoordsGLSL)
  for (size_t i = 0; i < N; i++) {
    auto it = byId.find(cs.nodeIds[i]);
    if (it == byId.end() || it->second->mods.empty()) continue;
    const Node& n = *it->second;
    size_t base = d.size() / S;
    size_t rows = (n.mods.size() + NODE_VEC4S - 1) / NODE_VEC4S;
    d.resize(d.size() + rows * S, 0.f);
    for (size_t k = 0; k < n.mods.size(); k++) {
      const Modifier& m = n.mods[k];
      float* x = &d[base * S + k * 4];
      switch (m.type) {
        case MOD_MIRROR: x[0] = (float)m.offset; break;
        case MOD_REPEAT:
          x[0] = m.spacing > 0 ? (float)m.spacing : 1e9f;
          x[1] = m.count > 0 ? (float)m.count : 1e9f;  // 0 = infinite
          break;
        case MOD_RADIAL:
          x[0] = (float)m.radius;
          x[1] = (float)std::max(1, m.count);
          x[2] = (float)(m.startAngle * PI / 180);
          x[3] = (float)(m.angle * PI / 180);
          break;
        case MOD_EXTRUDE: x[0] = (float)(m.length / 2); x[1] = (float)(int)m.mode; break;
        case MOD_REVOLVE: x[0] = (float)m.radius; break;
      }
    }
    d[i * S + 12 * 4 + 0] = (float)base;  // slot 12.x: first modifier row
  }
  // follow-surface paths: extra rows after the nodes (header + 33 samples x 4 vec4 = 133 vec4)
  for (size_t i = 0; i < N; i++) {
    auto it = byId.find(cs.nodeIds[i]);
    if (it == byId.end() || !it->second->anchor) continue;
    const Anchor& a = *it->second->anchor;
    if (!a.follow || a.path.size() != (size_t)NPATH + 1) continue;
    size_t base = d.size() / S;
    d.resize(d.size() + ((1 + 4 * (NPATH + 1) + NODE_VEC4S - 1) / NODE_VEC4S) * S, 0.f);
    float* x = &d[base * S];
    x[0] = (float)a.pathLen;
    x[1] = (float)(2 * a.pathLen / NPATH);
    for (int k = 0; k <= NPATH; k++) {
      const V4* v[4] = {&a.path[k].pos, &a.path[k].y, &a.path[k].z, &a.path[k].n};
      for (int c = 0; c < 4; c++)
        for (int e = 0; e < 4; e++) x[(1 + 4 * k + c) * 4 + e] = (float)(*v[c])[e];
    }
    d[i * S + 13 * 4 + 0] = (float)base;  // slot 13.x: first row of the path
  }
  if (space && space->curved()) {
    // curved: inverse isometries relative to the parent (the root's also undoes the camera: the
    // shader works in camera-relative coordinates, which keeps float precision near the eye),
    // total scales, world-unit bounds and blend radii
    std::unordered_map<std::string, int> idx;
    for (size_t i = 0; i < N; i++) idx[cs.nodeIds[i]] = (int)i;
    double k = space->k();
    M4 camInv = camIso ? cs::inverse(k, *camIso) : identity();
    std::function<void(Node&, double, const M4&, bool)> visit = [&](Node& n, double sAnc, const M4& parentWorld, bool absolute) {
      auto it = idx.find(n.id);
      double S = sAnc * n.xf.scale;
      M4 world = mul(parentWorld, cs::nodeIso(*space, n.xf, sAnc));
      if (it != idx.end()) {
        float* v = &d[it->second * S_PER];
        M4 iso = cs::nodeIso(*space, n.xf, sAnc);
        double anchoredR = -1;  // infinite objects re-anchored near the camera: their near part's size
        if (n.isHoneycomb() && camIso) {
          // anchor the tiling at the cell the camera is in (one of its own symmetries, found in
          // double precision): the shader then folds small coordinates near the eye
          int par = 0;
          M4 g = honeycombCellOf(*space, n.poly, mul(cs::inverse(k, world), col(*camIso, 3)), &par);
          iso = mul(iso, cs::inverse(k, g));
          if (par && n.poly.alternate)
            for (int q = 0; q < 3; q++) std::swap(v[6 * 4 + q], v[10 * 4 + q]);  // that cell's colour
        }
        else if (camIso && n.isLeaf()) {
          // planes, rods and infinite repeats: re-anchored next to the camera as well, so their
          // matrix stays small however far from the scene's origin you are
          M4 A = anchorNear(*space, n, reg, mul(cs::inverse(k, world), col(*camIso, 3)), S);
          iso = mul(iso, A);
          anchoredR = anchoredRadius(n, reg);  // one copy / the extent across the symmetric axes
        }
        world = mul(parentWorld, iso);  // (re-anchored)
        // absolute: camera-relative point -> node frame, in one matrix (double precision here)
        M4 inv = absolute ? cs::inverse(k, mul(camInv, world)) : cs::inverse(k, iso);
        for (int q = 0; q < 16; q++) v[q] = (float)inv[q];
        v[5 * 4 + 0] = (float)S;
        v[5 * 4 + 3] *= (float)sAnc;  // skip distance in world units
        if (!n.isLeaf()) v[5 * 4 + 1] = (float)(n.k * S);
        auto b = bounds.find(n.id);
        double r = b != bounds.end() && std::isfinite(b->second.r) ? b->second.r * sAnc : INFINITY;
        v[8 * 4 + 0] = std::isfinite(r) ? (float)r : 1e9f;
        v[8 * 4 + 3] = (float)curvedFactor(*space, r);
        // hyperbolic: objects beyond the rays' reach (12 R) are left out (decided here in double
        // precision; on the GPU their huge coordinates would only produce noise). Marked by r < 0.
        if (const Modifier* rp = exactRepeat(n)) {  // the shader bounds it by its line of copies
          double rc = anchoredRadiusAny(n, reg);
          if (std::isfinite(rc)) v[8 * 4 + 0] = (float)(rc * sAnc);
        }
        if (anchoredR >= 0 && std::isfinite(anchoredR)) r = anchoredR * sAnc;
        if (space->kind == SpaceKind::Hyperbolic && std::isfinite(r) && &n != &root) {
          V4 eye = camIso ? col(*camIso, 3) : cs::origin();
          if (cs::distance(*space, eye, col(world, 3)) - r > 13 * space->R()) v[8 * 4 + 0] = -1;
        }
      }
      for (auto& c : n.children) visit(c, S, world, absolute && n.mods.empty());
    };
    visit(root, 1.0, identity(), true);
  }
  return d;
}

std::string structureKey(const Node& root, const PrimitiveRegistry& reg, const std::string& world, bool geo, SpaceKind space) {
  std::ostringstream o;
  o << world << "|" << geo << "|" << (int)space << "|";
  walkc(root, [&](const Node& n, const Node*, int depth) {
    o << depth << n.id << (n.isLeaf() ? "L" + n.prim : n.isCamera() ? std::string("C") : "G" + std::to_string((int)n.op)) << (n.hidden ? "h" : "") << (n.anchor ? "a" + n.anchor->world : "");
    if (n.anchor && n.anchor->follow) o << "F";
    // only what decides the code: every number is node data (packNodes), so editing values never recompiles
    for (auto& m : n.mods) o << "m" << m.type << m.axis << (m.type == MOD_EXTRUDE ? (int)m.mode : 0) << m.i << m.j << (m.spacing > 0 ? "+" : "-");
    if (n.isCustom()) o << "c" << std::hash<std::string>()(n.code) << ":" << n.codeBound;
    if (n.isPolytope()) o << "p" << n.poly.type << ":" << n.poly.honey;
    o << ";";
  });
  for (auto& p : reg.custom()) o << p.id << primitiveGLSL(p, "f");
  return o.str();
}

// ---------------------------------------------------------------- standalone SDF (code viewer)
static std::string mat4Const(const M4& m) {
  std::ostringstream o;
  o << "mat4(";
  for (int k = 0; k < 16; k++) o << (k ? ", " : "") << glslFloat(m[k]);
  o << ")";
  return o.str();
}
static std::string vec4Const(const V4& v) {
  return "vec4(" + glslFloat(v[0]) + ", " + glslFloat(v[1]) + ", " + glslFloat(v[2]) + ", " + glslFloat(v[3]) + ")";
}

std::string standaloneGLSL(const Node& top, const PrimitiveRegistry& reg) {
  std::ostringstream pre, body;
  int counter = 0;
  std::function<std::string(const Node&, const std::string&, bool)> emit = [&](const Node& n, const std::string& pin, bool isTop) -> std::string {
    int k = counter++;
    std::string q = "q" + std::to_string(k), d = "d" + std::to_string(k);
    if (n.hidden && !isTop) { body << "  float " << d << " = INF;  // hidden: " << n.name << "\n"; return d; }
    body << "  // " << n.name << "\n";
    if (isTop) body << "  vec4 " << q << " = " << pin << ";\n";
    else body << "  vec4 " << q << " = (" << mat4Const(transpose(nodeRotation(n))) << " * (" << pin << " - " << vec4Const(n.xf.pos) << ")) / "
              << glslFloat(n.xf.scale) << ";\n";
    auto ax = modifierAxes(n, reg);
    std::string tag = std::to_string(k) + "_";
    std::string coords = modifierCoordsGLSL(n, ax, q.c_str(), tag);
    body << coords;
    if (n.isLeaf()) {
      if (n.isCustom()) {
        std::string suffix = "_n" + std::to_string(k);
        pre << "// custom SDF: " << n.name << "\n" << renameCustomCode(n.code, suffix) << "\n";
        body << "  float " << d << " = sdf" << suffix << "(" << q << ");\n";
      } else {
        auto* def = reg.get(n.prim);
        std::string fn = "prim_" + std::to_string(k);
        if (def && !def->steps.empty()) { pre << primitiveGLSL(*def, fn); body << "  float " << d << " = " << fn << "(" << q << ");\n"; }
        else body << "  float " << d << " = -1e9;  // point\n";
      }
    } else {
      static const char* opf[6] = {"min(%a, %b)", "max(%a, %b)", "max(%a, -%b)", "sU(%a, %b, %k)", "sI(%a, %b, %k)", "sS(%a, %b, %k)"};
      std::vector<std::string> kids;
      for (auto& c : n.children) kids.push_back(emit(c, q, false));
      body << "  float " << d << " = " << (kids.empty() ? std::string("INF") : kids[0]) << ";\n";
      for (size_t c = 1; c < kids.size(); c++) {
        std::string e = opf[(int)n.op];
        e = std::regex_replace(e, std::regex("%a"), d);
        e = std::regex_replace(e, std::regex("%b"), kids[c]);
        e = std::regex_replace(e, std::regex("%k"), glslFloat(std::max(n.k, 1e-4)));
        body << "  " << d << " = " << e << ";  // " << BOOL_OP_LABELS[(int)n.op] << "\n";
      }
    }
    std::string ex = extrudeGLSL(n, ax, tag);
    if (!ex.empty()) body << std::regex_replace(ex, std::regex("\\bd\\b"), d);
    if (n.isLeaf()) {
      if (n.shell.value_or(0) > 0) body << "  " << d << " = abs(" << d << ") - " << glslFloat(*n.shell) << ";\n";
      if (n.round.value_or(0) != 0) body << "  " << d << " -= " << glslFloat(*n.round) << ";\n";
      if (!n.isCustom())
        for (int a = nodeDim(n, reg); a < 4; a++)
          body << "  " << d << " = opExtrude(" << d << ", abs(" << q << "." << C[a] << ") - " << glslFloat(n.thickness / 2) << ");  // thin along " << AXIS[a] << "\n";
    }
    if (!isTop && n.xf.scale != 1) body << "  " << d << " *= " << glslFloat(n.xf.scale) << ";\n";
    return d;
  };
  std::string res = emit(top, "p", true);
  std::ostringstream o;
  o << "// SDF of \"" << top.name << "\" in its own local space (generated, self-contained).\n"
    << "// Helpers available: opExtrude, opRadial, sU, sI, sS, INF.\n" << pre.str() << "float sdf(vec4 p) {\n" << body.str() << "  return " << res << ";\n}\n";
  return o.str();
}

}  // namespace fd
