// Ray-marching shader, shared by the OpenGL (GLSL 3.30) and Vulkan (GLSL 4.50 -> SPIR-V)
// backends. A small backend-specific prelude declares the frame uniform block, the node
// data accessor nd(node, slot) and the in/out variables; the scene functions from
// codegen.cpp follow, then the common body below.
//
// main() is a single-loop state machine (march -> normal -> AO -> one shadow ray per light
// -> shade -> [continue through transparent surfaces]) with exactly one call to map() and
// one to the world projection, so compiled code stays proportional to the scene size.
#pragma once
#include <string>

namespace fd {

enum class ShaderTarget { OpenGL, Vulkan };

/** Number of vec4 per node in the node data buffer (see packNodes). */
constexpr int NODE_VEC4S = 16;
constexpr int MAX_LIGHTS = 8;
constexpr int MAX_DEBUG_PTS = 48;

// Frame uniforms: one std140 block (all members vec4 / mat4 / ivec4, so no padding rules).
inline const char* FRAME_BLOCK = R"(
  mat4 u_frame;      // columns: right, up, forward, ana/normal
  mat4 u_axes;       // world axes acting as the display X, Y (up), Z and slider axis
  vec4 u_eye;
  vec4 u_light;      // sun direction
  vec4 u_marker;     // player body centre (4D)
  vec4 u_markerCol;  // rgb, radius (0 = hidden)
  vec4 u_markerFwd;
  vec4 f_skyTop, f_skyBottom, f_skyCol, f_dbgEndCol;
  vec4 f_a;          // res.x, res.y, tanHalf, maxDist
  vec4 f_b;          // eps, stepScale, shadowK, shadowMax
  vec4 f_c;          // aoStrength, fog, ambient, geoMaxStep
  vec4 f_d;          // markerSkip, sun intensity, 0, 0
  ivec4 f_i;         // maxSteps, shadows, ao, grid
  ivec4 f_j;         // geoIters, debug points, point lights, textures on
  ivec4 f_k;         // point light shadow mask, bounces, over-relaxation, AO samples
  ivec4 f_l;         // shadow ray steps, specular on, stochastic on, sample index
  vec4 f_e;          // lens aperture, focus distance, (accumulation weight: used by the renderer), curvature radius
  vec4 f_f;          // sky intensity (path tracing), 4D light falloff, debug view, 0
  vec4 u_lpos[8];    // point lights: 4D position
  vec4 u_lcol[8];    // rgb * intensity, range
  vec4 u_lext[8];    // radius, area light (emissive object, path tracing) / point light
  vec4 u_dbg[48];    // debug ray polyline (Explore)
)";

inline const char* FRAME_DEFINES = R"(
#define u_res f_a.xy
#define u_tanHalf f_a.z
#define u_maxDist f_a.w
#define u_eps f_b.x
#define u_stepScale f_b.y
#define u_shadowK f_b.z
#define u_shadowMax f_b.w
#define u_aoStrength f_c.x
#define u_fog f_c.y
#define u_ambient f_c.z
#define u_geoMaxStep f_c.w
#define u_markerSkip f_d.x
#define u_sunI f_d.y
#define u_maxSteps f_i.x
#define u_shadows f_i.y
#define u_ao f_i.z
#define u_grid f_i.w
#define u_geoIters f_j.x
#define u_dbgN f_j.y
#define u_nLights f_j.z
#define u_textures f_j.w
#define u_aoSamples f_k.w
#define u_shadowSteps f_l.x
#define u_specular f_l.y
#define u_stochastic f_l.z
#define u_sample f_l.w
#define u_sunSize f_d.z
#define u_lightRadius f_d.w
#define u_aperture f_e.x
#define u_focus f_e.y
#define u_curvR f_e.w
#define u_skyI f_f.x
#define u_view int(f_f.z + 0.5)
#define u_lightShadowMask f_k.x
#define u_maxLayers f_k.y
#define u_relax f_k.z
#define u_skyTop f_skyTop.rgb
#define u_skyBottom f_skyBottom.rgb
#define u_skyCol f_skyCol.rgb
#define u_dbgEndCol f_dbgEndCol.rgb
)";

/** curved: 0 = flat, 1 = hyperbolic, 2 = spherical (SpaceKind). */
inline std::string curvedDefines(int curved) {
  if (curved == 0) return "";
  return std::string("#define VIEW_CURVED\n#define KAPPA ") + (curved == 2 ? "1.0" : "-1.0") + "\n";
}

inline std::string shaderPrelude(ShaderTarget t, bool geo, int curved = 0) {
  std::string s;
  if (t == ShaderTarget::OpenGL) {
    s = "#version 330 core\n";
    if (geo) s += "#define VIEW_GEO\n";
    s += curvedDefines(curved);
    s += std::string("layout(std140) uniform Frame {") + FRAME_BLOCK + "};\n";
    s += "uniform sampler2D u_nodes;  // node data: row = node, 12 RGBA32F texels\n"
         "vec4 ndat(int i, int k) { return texelFetch(u_nodes, ivec2(k, i), 0); }\n"
         "in vec2 v_uv;\nout vec4 fragColor;\n";
  } else {
    s = "#version 450\n";
    if (geo) s += "#define VIEW_GEO\n";
    s += curvedDefines(curved);
    s += std::string("layout(std140, set = 0, binding = 0) uniform Frame {") + FRAME_BLOCK + "};\n";
    s += "layout(std430, set = 0, binding = 1) readonly buffer Nodes { vec4 nodeData[]; };\n"
         "vec4 ndat(int i, int k) { return nodeData[i * " + std::to_string(NODE_VEC4S) + " + k]; }\n"
         "layout(location = 0) in vec2 v_uv;\nlayout(location = 0) out vec4 fragColor;\n";
  }
  return s + FRAME_DEFINES;
}

/** Full-screen triangle (no vertex buffer). Vulkan's clip space has Y down: flip it there. */
inline std::string vertexShader(ShaderTarget t) {
  if (t == ShaderTarget::OpenGL)
    return "#version 330 core\nout vec2 v_uv;\n"
           "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0; v_uv = p; gl_Position = vec4(p, 0.0, 1.0); }\n";
  return "#version 450\nlayout(location = 0) out vec2 v_uv;\n"
         "void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0; v_uv = vec2(p.x, -p.y); gl_Position = vec4(p, 0.0, 1.0); }\n";
}

inline const char* RAYMARCH_COMMON = R"(
// ---- metric helpers: Euclidean, or the curved model's form (see space.hpp) ----
#ifdef VIEW_CURVED
float mdot(vec4 a, vec4 b) { return dot(a.xyz, b.xyz) + KAPPA * a.w * b.w; }
vec4 crenorm(vec4 p) { return p * inversesqrt(max(mdot(p, p) * KAPPA, 1e-30)); }
// world distance between two model points
float cdist(vec4 a, vec4 b) {
  vec4 d = a - b;
  if (KAPPA > 0.0) return u_curvR * 2.0 * atan(length(d), length(a + b));
  float l = sqrt(max(mdot(d, d), 0.0)) * 0.5;
  return u_curvR * 2.0 * log(l + sqrt(l * l + 1.0));
}
#else
float mdot(vec4 a, vec4 b) { return dot(a, b); }
#endif
vec4 mnorm(vec4 v) { return v * inversesqrt(max(mdot(v, v), 1e-30)); }
vec4 mreflect(vec4 i, vec4 n) { return i - 2.0 * mdot(n, i) * n; }
vec4 mrefract(vec4 I, vec4 N, float eta) {
  float c = mdot(N, I), k = 1.0 - eta * eta * (1.0 - c * c);
  return k < 0.0 ? vec4(0.0) : eta * I - (eta * c + sqrt(k)) * N;
}
// part of v tangent at a point (curved: w = the point) / along a surface (Geodesic: w = its normal)
vec4 tproj(vec4 v, vec4 w) { return v - w * (mdot(v, w) / mdot(w, w)); }
// the point reached from p going distance h along unit tangent d (and the direction there)
void geoMove(inout vec4 p, inout vec4 d, float h) {
#ifdef VIEW_CURVED
  float t = h / u_curvR, c = cC(t), s = cS(t);
  vec4 np = p * c + d * s;
  d = d * c - p * (KAPPA * s);
  p = np;
#else
  p += d * h;
#endif
}
vec4 geoPoint(vec4 p, vec4 d, float h) { geoMove(p, d, h); return p; }

// bump: the normal phase also samples the relief (g_bumpH, summed by the leaves' bump textures) at
// the hit and BUMP_W further along each axis, wider than the geometry's samples, so hard-edged
// patterns (checkers, stripes) get bevels instead of a slope only along their edges
#ifdef SCENE_BUMP
#define NORMAL_END 10
#else
#define NORMAL_END 5
#endif
#define BUMP_W (0.012 + 0.002 * t)
// debug views (Settings > Rendering > View): one material value at the hit
vec3 debugView(int v, MAT m, vec4 n, float bh) {
  float g = 0.0;
  if (v == 1) return m[0].rgb;                 // colour
  if (v == 2) g = m[1].y;                      // roughness
  if (v == 3) g = m[2].x;                      // metallic
  if (v == 4) g = m[1].z * 0.5;                // specular (0..2)
  if (v == 5) g = m[1].x;                      // emission
  if (v == 6) g = m[1].w;                      // transparency
  if (v == 7) g = 0.5 + bh * 10.0;             // bump relief (world units: +-0.05 = white / black)
  if (v == 8) return n.xyz * 0.5 + 0.5;        // shading normal (after bump)
  if (v == 9) g = m[2].w;                      // the colour texture's mask
  return vec3(clamp(g, 0.0, 1.0));
}
bool g_noBody = false;     // primary ray still inside the player's own body
bool g_noDbg = false;      // primary ray still next to the eye (the debug ray may pass through it)

// scene + the player's body (a sphere with a small "nose" showing where it faces) + the debug ray
float map(vec4 p, out MAT m) {
  float d = mapScene(p, m);
  if (u_markerCol.w > 0.0 && !g_noBody) {
    float r = u_markerCol.w;
#ifdef VIEW_CURVED
    float body = cdist(p, u_marker) - r;
    float nose = cdist(p, geoPoint(u_marker, u_markerFwd, r * 0.95)) - r * 0.4;
#else
    float body = length(p - u_marker) - r;
    float nose = length(p - (u_marker + u_markerFwd * r * 0.95)) - r * 0.4;
#endif
    float dm = min(body, nose);
    if (dm < d) { d = dm; m = MAT(vec4(nose < body ? u_markerCol.rgb * 0.55 : u_markerCol.rgb, 0.0), vec4(0.0, 0.6, 0.3, 0.0), vec4(0.0)); }
  }
  // debug ray: a thin glowing tube along its path (emissive: no shadows), end marker coloured by the stop reason
#ifndef VIEW_CURVED
  if (u_dbgN > 1 && !g_noEmissive && !g_noDbg) {
    float dt = INF;
    for (int i = 0; i < 47; i++) {
      if (i + 1 >= u_dbgN) break;
      vec4 a = u_dbg[i], ba = u_dbg[i + 1] - a, pa = p - a;
      float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
      dt = min(dt, length(pa - ba * h) - 0.03);
    }
    float de = length(p - u_dbg[u_dbgN - 1]) - 0.09;
    // a minimum step outside the thin tube (smaller than its diameter, so it cannot be skipped):
    // rays grazing it along its length would otherwise run out of steps
    dt = dt < u_eps * 2.0 ? dt : max(dt, 0.02);
    if (dt < d) { d = dt; m = MAT(vec4(1.0, 0.55, 0.1, 0.0), vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0)); }
    if (de < d) { d = de; m = MAT(vec4(u_dbgEndCol, 0.0), vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0)); }
  }
#else
  // curved: the polyline's pieces are short (a quarter of R at most), so the nearest point is found
  // on the chord and put back on the space; distances are true world distances
  if (u_dbgN > 1 && !g_noEmissive && !g_noDbg) {
    float dt = INF;
    for (int i = 0; i < 47; i++) {
      if (i + 1 >= u_dbgN) break;
      vec4 a = u_dbg[i], ba = u_dbg[i + 1] - a;
      if (abs(a.w) > 60.0) { dt = min(dt, cdist(p, a) - 0.03); continue; }  // far: floats are too coarse for the chord
      float h = clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
      dt = min(dt, cdist(p, crenorm(a + ba * h)) - 0.03);
    }
    float de = cdist(p, u_dbg[u_dbgN - 1]) - 0.09;
    dt = dt < u_eps * 2.0 ? dt : max(dt, 0.02);
    if (dt < d) { d = dt; m = MAT(vec4(1.0, 0.55, 0.1, 0.0), vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0)); }
    if (de < d) { d = de; m = MAT(vec4(u_dbgEndCol, 0.0), vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0)); }
  }
#endif
  return d;
}

#ifdef VIEW_GEO
MAT g_projMat = MAT(0.0);  // world surface material at the last projected point

vec4 projectToWorld(inout vec4 p) {
  const float e = 1e-3;
  vec4 n = vec4(0.0, 1.0, 0.0, 0.0);
  for (int k = 0; k < 4; k++) {
    if (k >= u_geoIters) break;
    float v[5];
    for (int i = 0; i < 5; i++) {
      vec4 o = vec4(0.0);
      if (i > 0) o[i - 1] = e;
      MAT wm;
      v[i] = worldSDFm(p + o, wm);
      if (i == 0) g_projMat = wm;
    }
    vec4 g = (vec4(v[1], v[2], v[3], v[4]) - v[0]) / e;
    float gg = max(dot(g, g), 1e-8);
    p -= v[0] * g / gg;
    n = g * inversesqrt(gg);
  }
  return n;
}
#endif

// surf: Geodesic only, the colour of the world surface where the ray ended
// p: where the direction is (curved spaces: the sky is fixed in the world, u_axes = camera isometry)
vec3 skyBase(vec4 p, vec4 dir, vec3 surf);
// the sky's colour x the sky intensity setting (0 = black: in the Geodesic view this is the world
// surface's colour that rays reaching far show, so it also darkens the fog there)
vec3 sky(vec4 p, vec4 dir, vec3 surf) { return skyBase(p, dir, surf) * u_skyI; }
vec3 skyBase(vec4 p, vec4 dir, vec3 surf) {
#ifdef VIEW_GEO
  // the sky of a world surface has the colour of the surface the ray travelled to
  return surf * (0.9 + 0.1 * dot(dir, normalize(u_light)));
#elif defined(VIEW_CURVED)
  float y;
  if (KAPPA < 0.0) {
    // hyperbolic: the point at infinity the ray heads for (p + dir is light-like), in world coordinates
    vec4 v = u_axes * (p + dir);
    y = normalize(v.xyz).y;
  } else {
    // spherical: the direction compared with "up" there (away from the ground sphere y = 0)
    vec4 pw = u_axes * p, vw = u_axes * dir;
    vec4 up = tproj(vec4(0.0, 1.0, 0.0, 0.0), pw);
    y = mdot(vw, up) * inversesqrt(max(mdot(up, up), 1e-12));
  }
  return mix(u_skyBottom, u_skyTop, clamp(y * 0.5 + 0.5, 0.0, 1.0));
#else
  return mix(u_skyBottom, u_skyTop, clamp(dot(dir, u_axes[1]) * 0.5 + 0.5, 0.0, 1.0)) + 0.05 * dot(dir, u_axes[3]);
#endif
}

// colour of a world axis direction (X red, Y green, Z blue, W purple)
vec3 axisCol(vec4 v) { v = abs(v); return v.x * vec3(0.9, 0.25, 0.25) + v.y * vec3(0.3, 0.8, 0.3) + v.z * vec3(0.25, 0.45, 0.95) + v.w * vec3(0.8, 0.3, 0.9); }

float gridLine(float x) {
  float f = abs(fract(x - 0.5) - 0.5);
  return 1.0 - smoothstep(0.0, fwidth(x), f);
}

// ---- stochastic sampling: one random path per pixel and sample, averaged over many samples ----
uint g_seed = 1u;
uint pcg(uint v) { uint s = v * 747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }
float rnd() { g_seed = pcg(g_seed); return float(g_seed) * (1.0 / 4294967296.0); }
// random direction in 4D (approximately uniform: normalised near-gaussian components)
vec4 rndDir4() {
  vec4 v = vec4(rnd() + rnd() + rnd(), rnd() + rnd() + rnd(), rnd() + rnd() + rnd(), rnd() + rnd() + rnd()) - 1.5;
  return v * inversesqrt(max(dot(v, v), 1e-8));
}

// light li (0 = sun, 1.. = point lights) seen from hp: direction, colour, distance to it
void lightAt(int li, vec4 hp, vec4 wn, vec4 dir, out vec4 L, out vec3 lc, out float lmax) {
  bool stoch = u_stochastic == 1;
#ifdef VIEW_CURVED
  // curved: the sun is a direction at infinity (hyperbolic) / a far point (spherical) given as a
  // 4-vector; light travels along geodesics, so directions are tangent projections
  if (li == 0) {
    L = mnorm(tproj(u_light, hp));
    if (stoch) L = mnorm(tproj(L + rndDir4() * u_sunSize, hp));
    lc = vec3(u_sunI);
    lmax = u_shadowMax;
  } else {
    vec4 lp = u_lpos[li - 1];
    if (stoch) lp = crenorm(lp + rndDir4() * (u_lightRadius * rnd() / u_curvR));
    float dist = cdist(hp, lp);
    vec4 tl = tproj(lp, hp);
    L = mdot(tl, tl) > 1e-14 ? mnorm(tl) : -dir;
    float a = clamp(1.0 - dist / max(u_lcol[li - 1].w, 1e-3), 0.0, 1.0);
    lc = u_lcol[li - 1].rgb * a * a;
    lmax = dist;
  }
  return;
#endif
  if (li == 0) {
    L = normalize(u_light);
    if (stoch) L = normalize(L + rndDir4() * u_sunSize);  // a sun of some size: soft shadows
    lc = vec3(u_sunI);
    lmax = u_shadowMax;
  } else {
    vec4 lp = u_lpos[li - 1];
    if (stoch) lp += rndDir4() * (u_lightRadius * rnd());  // a light ball, not a point
    vec4 d = lp - hp;
    float dist = length(d);
    L = d / max(dist, 1e-6);
    float a = clamp(1.0 - dist / max(u_lcol[li - 1].w, 1e-3), 0.0, 1.0);
    lc = u_lcol[li - 1].rgb * a * a;
    lmax = dist;
  }
#ifdef VIEW_GEO
  L = L - dot(L, wn) * wn;  // in the Geodesic view light travels along the surface
  L = dot(L, L) < 1e-4 ? -dir : normalize(L);
#endif
}

)";

inline const char* RAYMARCH_GRID = R"(
// ground grid over the image (Edit mode): gro / grd = the primary ray, tGrid = where it hit something
vec3 groundGrid(vec3 col, vec4 gro, vec4 grd, float tGrid) {

#ifdef VIEW_CURVED
  // ground grid on the geodesic plane y = 0 of the world (normal coordinates x, z)
  if (u_grid == 1) {
    mat4 G = mat4(1.0); G[3][3] = KAPPA;
    vec4 nu = G * transpose(u_axes) * G * vec4(0.0, 1.0, 0.0, 0.0);  // the plane's normal, camera-relative
    float a = mdot(gro, nu), b = mdot(grd, nu), th = -1.0;
    if (abs(b) > 1e-6) {
      float r = -a / b;
      if (KAPPA > 0.0) { th = atan(r); if (th < 0.0) th += 3.14159265; }
      else if (abs(r) < 1.0) th = 0.5 * log((1.0 + r) / (1.0 - r));
    }
    float tg = th * u_curvR;
    if (th > 0.0 && tg < tGrid && tg < u_maxDist) {
      vec4 gp = gro, gd = grd;
      geoMove(gp, gd, tg);
      vec4 g = clogO(u_axes * gp);
      float fade = exp(-tg * 0.05);
      float gl = max(gridLine(g.x), gridLine(g.z)) * 0.25;
      float ax = 1.0 - smoothstep(0.0, fwidth(g.z) * 1.5, abs(g.z));
      float az = 1.0 - smoothstep(0.0, fwidth(g.x) * 1.5, abs(g.x));
      col = mix(col, vec3(0.8) * gl + col * 0.5, gl * fade);
      col = mix(col, axisCol(vec4(1.0, 0.0, 0.0, 0.0)), ax * fade * 0.8);
      col = mix(col, axisCol(vec4(0.0, 0.0, 1.0, 0.0)), az * fade * 0.8);
    }
  }
#elif !defined(VIEW_GEO)
  // ground grid in display coordinates (the shown axes: X, up, Z; lines along the slider axis)
  vec4 rod = gro * u_axes, rdd = grd * u_axes;
  if (u_grid == 1 && abs(rdd.y) > 1e-4) {
    float tg = -rod.y / rdd.y;
    if (tg > 0.0 && tg < tGrid) {
      vec4 g = rod + rdd * tg;
      float fade = exp(-tg * 0.05);
      float gl = max(gridLine(g.x), gridLine(g.z)) * 0.25;
      float gw = gridLine(g.w) * 0.25;
      vec3 gc = vec3(0.8) * gl + axisCol(u_axes[3]) * gw;
      float ax = 1.0 - smoothstep(0.0, fwidth(g.z) * 1.5, abs(g.z));
      float az = 1.0 - smoothstep(0.0, fwidth(g.x) * 1.5, abs(g.x));
      col = mix(col, gc + col * 0.5, max(gl, gw) * fade);
      col = mix(col, axisCol(u_axes[0]), ax * fade * 0.8);
      col = mix(col, axisCol(u_axes[2]), az * fade * 0.8);
    }
  }
#endif

  return col;
}
)";

inline const char* SIMPLE_MAIN = R"(
// ---- simple lighting: sun, point lights, ambient light, AO ----
const int MARCH = 0, NORMAL = 1, AO = 2, SHADOW = 3, DONE = 4;

void main() {
  bool stoch = u_stochastic == 1;
  g_seed = pcg(uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + uint(u_sample) * 26699u) | 1u;
  // stochastic: a random point inside the pixel (anti-aliasing as the samples add up)
  vec2 jit = stoch ? (vec2(rnd(), rnd()) - 0.5) * 2.0 / u_res.y : vec2(0.0);
  vec2 uv = v_uv * vec2(u_res.x / u_res.y, 1.0) + jit;
  vec4 ro = u_eye;
  vec4 rd = normalize(u_frame * vec4(uv * u_tanHalf, 1.0, 0.0));
#if defined(VIEW_CURVED)
  if (stoch && u_aperture > 0.0) {  // depth of field (camera-relative: the eye is the origin)
    vec4 fp = geoPoint(ro, rd, u_focus / max(rd.z, 1e-3));
    float a = 6.2831853 * rnd(), r = u_aperture * sqrt(rnd());
    ro = geoPoint(ro, vec4(cos(a), sin(a), 0.0, 0.0), r);
    rd = mnorm(tproj(fp, ro));
  }
#elif !defined(VIEW_GEO)
  if (stoch && u_aperture > 0.0) {  // depth of field: rays from a lens disc, sharp at the focus distance
    vec4 fp = ro + rd * (u_focus / max(dot(rd, u_frame[2]), 1e-3));
    float a = 6.2831853 * rnd(), r = u_aperture * sqrt(rnd());
    ro += (u_frame[0] * cos(a) + u_frame[1] * sin(a)) * r;
    rd = normalize(fp - ro);
  }
#endif

  vec4 p = ro, dir = rd;
  float t = 0.0;
  int steps = 0;
  vec4 wn = u_frame[3];
  int phase = MARCH, k = 0;
  bool hit = false, inside = false, exiting = false, toSky = true;
  MAT hm = MAT(0.0);
  vec4 hp = p, n = vec4(0.0);
  float nv[5], bv[5];
  float bumpHit = 0.0;
  float occ = 0.0, aow = 1.0;
  vec4 sp = vec4(0.0), sdir = vec4(0.0), slp = vec4(0.0);  // shadow ray: point, direction, point light it homes in on
  float st = 0.0, sh = 1.0, shMax = 0.0;
  int sSteps = 0;
  vec3 rayWorldCol = u_skyCol;  // Geodesic: world surface colour where the primary ray currently is
  // lighting of the current hit: light being processed, its direction / colour, sums
  int li = 0;
  vec4 Ll = vec4(0.0);
  vec3 lc = vec3(0.0), diffAcc = vec3(0.0), specAcc = vec3(0.0);
  float dif = 0.0, firstT = -1.0;
  // reflections / transparency: colour gathered so far and the light still travelling
  vec3 accum = vec3(0.0), thr = vec3(1.0);
  int layers = 0;
  vec4 gro = ro, grd = rd;  // primary ray (ground grid)
  float t0 = 0.0;           // Full 4D: t at the start of the current straight segment (ro, rd)
  // over-relaxed sphere tracing (Full 4D primary rays)
  float omega = u_relax == 1 ? 1.4 : 1.0, prevR = 0.0, stepLen = 0.0;

  for (int it = 0; it < 3000; it++) {
    if (phase == DONE) break;
    vec4 q = p;
#ifdef VIEW_CURVED
    if (phase == NORMAL) { q = hp; if (k > 0 && k != 5) { if (k >= 6) q[k - 6] += BUMP_W / u_curvR; else q[k - 1] += 1e-3 / u_curvR; if (abs(q.w) < 100.0) q = crenorm(q); } }
#else
    if (phase == NORMAL) { q = hp; if (k >= 6) q[k - 6] += BUMP_W; else if (k > 0 && k < 5) q[k - 1] += 1e-3; }
#endif
    else if (phase == AO) {
      vec4 ad = n;
      if (stoch) {  // sampled ambient occlusion: random directions around the normal
        ad = n + rndDir4() * 0.8;
#ifdef VIEW_GEO
        ad -= dot(ad, wn) * wn;
#endif
#ifdef VIEW_CURVED
        ad = tproj(ad, hp);
#endif
        ad = mnorm(ad);
        if (mdot(ad, n) < 0.0) ad = mreflect(ad, n);
      }
      q = geoPoint(hp, ad, 0.03 + 0.12 * float(k + 1));
    }
    else if (phase == SHADOW) q = sp;

    MAT mm;
    g_exact = phase == AO || phase == SHADOW;
    g_noEmissive = g_exact;
    g_noBody = phase == MARCH && t < u_markerSkip;
    g_noDbg = phase == MARCH && t < 0.12;
    // textures only near the surface: material ones for the hit, bump ones for the normal samples
    g_texMat = phase == MARCH && !inside;
    g_texBump = phase == NORMAL && k >= 5;
    g_texNear = phase == MARCH || phase == NORMAL ? u_eps * (1.0 + t * 0.02) * 3.0 + 3e-3 + (g_texBump ? BUMP_W : 0.0) : -1.0;
    g_bumpH = 0.0;
    float d = map(q, mm);   // the only scene evaluation site

    bool advance = false, startLights = false;
    vec4 ap = vec4(0.0), adir = vec4(0.0);
    float h = 0.0;

    if (phase == MARCH) {
      if (inside) {  // travelling through a transparent object until its far side
        if (d > -u_eps * 0.5) {
          // reached the far side: find its normal, then bend the ray out of the object
          exiting = true; hp = p; nv[0] = d; phase = NORMAL; k = 1;
        } else {
          h = max(-d, u_eps * 2.0);
#ifdef VIEW_GEO
          h = min(h, u_geoMaxStep);
#endif
          ap = p; adir = dir; advance = true; t += h; steps++;
          if (steps >= u_maxSteps || t > u_maxDist) phase = DONE;
        }
      } else {
#ifdef VIEW_GEO
        bool sorFail = false;
#else
        bool sorFail = omega > 1.0 && (abs(d) + prevR) < stepLen;
#endif
        if (!sorFail && d < u_eps * (1.0 + t * 0.02)) {
          hit = true; hm = mm; hp = p; nv[0] = d;
          if (firstT < 0.0) firstT = t;
          phase = NORMAL; k = 1;
        } else if (t > u_maxDist || steps >= u_maxSteps) {
          phase = DONE;
        } else {
#ifdef VIEW_GEO
          h = min(d * u_stepScale, u_geoMaxStep);
#else
          if (sorFail) { stepLen -= omega * stepLen; omega = 1.0; }  // overshot: step back, then plain steps
          else stepLen = d * u_stepScale * omega;
          prevR = abs(d);
          h = stepLen;
#endif
          ap = p; adir = dir; advance = true;
          t += h; steps++;
        }
      }
    } else if (phase == NORMAL) {
      if (k < 5) nv[k] = d; else bv[k - 5] = g_bumpH;
      k++;
      if (k == NORMAL_END) {
        vec4 gd = (vec4(nv[1], nv[2], nv[3], nv[4]) - nv[0]) / 1e-3;
#ifdef SCENE_BUMP
        gd -= (vec4(bv[1], bv[2], bv[3], bv[4]) - bv[0]) / BUMP_W;  // the relief raises the surface: d - h
        bumpHit = bv[0];
#endif
        n = normalize(gd);
#ifdef VIEW_GEO
        n = normalize(n - dot(n, wn) * wn + 1e-6);
#endif
#ifdef VIEW_CURVED
        n = mnorm(tproj(vec4(n.xyz, KAPPA * n.w), hp));  // ambient gradient -> tangent gradient
#endif
        k = 0;
        if (exiting) {
          // leaving a transparent object: refract outward (total internal reflection keeps it inside)
          exiting = false;
          float ior = max(hm[2].y, 1.0);
          vec4 nd2 = mrefract(dir, -n, ior);
          phase = MARCH;
          if (mdot(nd2, nd2) < 1e-6) { dir = mreflect(dir, -n); p = geoPoint(hp, n, -u_eps * 3.0); }
          else { dir = mnorm(nd2); inside = false; p = geoPoint(hp, n, u_eps * 3.0); }
#ifdef VIEW_CURVED
          dir = mnorm(tproj(dir, p));
#endif
          ro = p; rd = dir; t0 = t;
          omega = 1.0; prevR = 0.0; stepLen = 0.0;
          continue;
        }
        if (u_view > 0) {  // debug views: one material value at the hit, nothing else
          accum = pow(debugView(u_view, hm, n, bumpHit), vec3(2.2));
          phase = DONE; toSky = false;
          continue;
        }
        occ = 0.0; aow = 1.0;
        diffAcc = vec3(0.0); specAcc = vec3(0.0);
        li = -1;
        bool emissive = hm[1].x > 0.999;  // fully emissive: no lighting needed
        if (emissive) { li = u_nLights; startLights = true; }
        else if (u_ao == 1) phase = AO;
        else startLights = true;
      }
    } else if (phase == AO) {
      float hh = 0.03 + 0.12 * float(k + 1);
      occ += (hh - d) * aow; aow *= 0.7; k++;
      if (k >= u_aoSamples) startLights = true;
    } else {  // SHADOW toward light li
      sh = stoch ? (d < 0.002 ? 0.0 : sh) : min(sh, u_shadowK * d / st);  // stochastic: hard per sample, soft on average
      sSteps++;
#ifdef VIEW_GEO
      // point lights in the Geodesic view: the ray homes in on the light along the surface; it
      // has to *arrive* (within the light's ball) to be lit, and the light fades with the distance
      // travelled along the surface (another sheet of the world, or a light far round a fold, is dark)
      bool arrived = li > 0 && length(slp - sp) < max(u_lightRadius, 0.15);
      if (arrived) { float a = clamp(1.0 - st / max(u_lcol[li - 1].w, 1e-3), 0.0, 1.0); lc = u_lcol[li - 1].rgb * a * a; }
      bool outOfSteps = sSteps > u_shadowSteps * (li > 0 ? 4 : 1);
#else
      bool arrived = false, outOfSteps = sSteps > u_shadowSteps;
#endif
      if (sh < 0.02 || st > shMax || outOfSteps || arrived) {
        // this light is done: add its contribution
#ifdef VIEW_GEO
        // a ray that ran out of steps (or path) before reaching its light cannot confirm it: dark
        if (!arrived && (li > 0 || outOfSteps)) sh = 0.0;
#endif
        float shv = clamp(sh, 0.0, 1.0);
        diffAcc += lc * dif * shv;
        vec4 H = mnorm(Ll - dir);
        float rough = clamp(hm[1].y, 0.02, 1.0);
        specAcc += lc * pow(max(mdot(n, H), 0.0), mix(160.0, 6.0, rough)) * hm[1].z * shv * float(u_specular);
        startLights = true;
      } else {
#ifdef VIEW_GEO
        h = clamp(d, 0.03, u_geoMaxStep);  // (larger steps would leave the curved surface)
        if (li > 0) h = min(h, max(length(slp - sp) * 0.5, 0.03));  // do not overshoot the light
#else
        // adaptive: the step bound grows so the ray can always cover what is left to its light within
        // its step budget (a fixed bound let distant walls leak); steps never exceed the free distance d
        float remain = max(shMax - st, 0.0);
        h = clamp(d, 0.02, max(0.4, remain / float(max(u_shadowSteps - sSteps, 1))));
#endif
        ap = sp; adir = sdir; advance = true; st += h;
      }
    }

    // next light (or shading when all are done): no scene evaluation needed here
    if (startLights) {
      for (int guard = 0; guard < 9; guard++) {
        li++;
        if (li > u_nLights) break;
        float lmax;
        lightAt(li, hp, wn, dir, Ll, lc, lmax);
        dif = max(mdot(n, Ll), 0.0);
        if (dif <= 0.0 || max(lc.r, max(lc.g, lc.b)) < 1e-4) continue;
        bool shadowed = li == 0 ? u_shadows == 1 : ((u_lightShadowMask >> (li - 1)) & 1) == 1;
        if (shadowed) {
          sp = geoPoint(hp, n, 0.01); sdir = Ll;
#ifdef VIEW_CURVED
          sdir = mnorm(tproj(Ll, sp));
#endif
          st = 0.02; sh = 1.0; sSteps = 0; shMax = min(lmax, li == 0 ? u_shadowMax : 1e9);
#ifdef VIEW_GEO
          if (li > 0) { slp = u_lpos[li - 1]; shMax = u_lcol[li - 1].w; }  // home in on it, up to its range
#endif
          phase = SHADOW;
          break;
        }
        diffAcc += lc * dif;  // unshadowed light
        vec4 H = mnorm(Ll - dir);
        float rough = clamp(hm[1].y, 0.02, 1.0);
        specAcc += lc * pow(max(mdot(n, H), 0.0), mix(160.0, 6.0, rough)) * hm[1].z * float(u_specular);
      }
      if (li > u_nLights) {
        // all lights done: shade this surface
        vec3 base = hm[0].rgb;
        float emissive = clamp(hm[1].x, 0.0, 1.0);
        float ao = u_ao == 1 ? clamp(1.0 - u_aoStrength * occ, 0.0, 1.0) : 1.0;
        vec4 V = -dir;
        float fre = pow(1.0 - max(mdot(n, V), 0.0), 3.0);
#ifdef VIEW_GEO
        vec3 rim = rayWorldCol * u_skyI;
#else
        vec3 rim = sky(hp, n, rayWorldCol);
#endif
        // the rim light (the sky / world colour on grazing surfaces) is ambient light: it follows the
        // ambient setting (0.18 = default), so ambient 0 leaves only what the lights reach
        // (the sky's reflection on grazing surfaces grows with the specular value: 0.35 = as before)
        vec3 lit = base * (u_ambient * ao + 0.9 * diffAcc) + specAcc + 0.12 * fre * ao * rim * (u_ambient / 0.18) * (clamp(hm[1].z, 0.0, 2.0) / 0.35);
        vec3 c = mix(lit, base, emissive);  // emissive: always fully lit, unshadowed
        c = mix(c, vec3(1.0, 0.6, 0.15), hm[0].a * (0.25 + 0.5 * fre));  // selection highlight
        float f = t / u_maxDist;
        c = mix(c, sky(hp, dir, rayWorldCol), clamp(u_fog * f * f * 1.5, 0.0, 1.0));
        // no bounces left (or bounces off): mirrors and glass are drawn as plain surfaces, not dark
        bool canBounce = layers < u_maxLayers;
        float transp = canBounce ? clamp(hm[1].w, 0.0, 1.0) : 0.0;
        // rough metal: part of its reflection is spread out (satin): drawn as its own lit colour
        float metal = canBounce ? clamp(hm[2].x, 0.0, 1.0) * (1.0 - 0.75 * hm[1].y * hm[1].y) : 0.0;
        float wSurf = (1.0 - transp) * (1.0 - metal);  // the surface's own colour
        int path = 0;                                   // 0: stop here, 1: reflect, 2: refract
        vec3 tint = vec3(1.0);
        if (stoch) {
          // one path per sample, chosen by its share: the surface, a mirror reflection, or (glass)
          // a Fresnel reflection / refraction; the average over samples mixes them correctly
          float ior = max(hm[2].y, 1.0), f0 = (ior - 1.0) / (ior + 1.0);
          float F = f0 * f0 + (1.0 - f0 * f0) * pow(1.0 - abs(mdot(dir, n)), 5.0);
          float wM = metal * (1.0 - transp), wR = transp * F, r = rnd();
          if (r < wSurf) accum += thr * c;
          else if (r < wSurf + wM) { path = 1; tint = mix(vec3(1.0), base, 0.6); }
          else if (r < wSurf + wM + wR) path = 1;
          else { path = 2; tint = mix(vec3(1.0), base, 0.25); }
        } else {
          accum += thr * wSurf * c;
          float cont = 1.0 - wSurf;  // what continues as a reflected / refracted ray
          if (cont > 0.01 && max(thr.r, max(thr.g, thr.b)) * cont > 0.02) {
            path = metal >= transp ? 1 : 2;
            tint = cont * mix(vec3(1.0), base, path == 1 ? 0.6 : 0.25);
          }
        }
        if (path != 0 && layers < u_maxLayers) {
          layers++;
          phase = MARCH; hit = false;
          thr *= tint;
          if (path == 1) {
            // mirror: reflect (stochastic: rough surfaces scatter the reflection)
            dir = mreflect(dir, n);
            if (stoch && hm[1].y > 0.02) {
              dir = mnorm(dir + rndDir4() * hm[1].y * hm[1].y);
#ifdef VIEW_GEO
              dir = normalize(dir - dot(dir, wn) * wn);
#endif
#ifdef VIEW_CURVED
              dir = mnorm(tproj(dir, hp));
#endif
              if (mdot(dir, n) < 0.0) dir = mreflect(dir, n);
            }
            p = geoPoint(hp, n, u_eps * 4.0);
          } else {
            // glass: bend into the object by its index of refraction
            vec4 nd2 = mrefract(dir, n, 1.0 / max(hm[2].y, 1.0));
            dir = mdot(nd2, nd2) < 1e-6 ? mreflect(dir, n) : mnorm(nd2);
            inside = mdot(nd2, nd2) >= 1e-6;
            p = geoPoint(hp, n, inside ? -u_eps * 4.0 : u_eps * 4.0);
          }
#ifdef VIEW_CURVED
          dir = mnorm(tproj(dir, p));  // the small step off the surface moved the tangent space
#endif
          ro = p; rd = dir; t0 = t;
          omega = 1.0; prevR = 0.0; stepLen = 0.0;
        } else { phase = DONE; toSky = false; }
      }
    }

    if (advance) {
#ifdef VIEW_GEO
      ap += adir * h;
      vec4 nn = projectToWorld(ap);   // the only world-projection site
      if (phase == SHADOW && li > 0) adir = slp - ap;  // homing toward the point light along the surface
      adir = adir - dot(adir, nn) * nn;
      adir = dot(adir, adir) > 1e-12 ? normalize(adir) : sdir;
#elif defined(VIEW_CURVED)
      // exact geodesics: the primary segment from its start (over-relaxation may step back)
      // (no re-normalisation of far points: there the form cancels catastrophically in floats)
      if (phase == MARCH) { ap = ro; adir = rd; geoMove(ap, adir, t - t0); }
      else { geoMove(ap, adir, h); if (abs(ap.w) < 100.0) { ap = crenorm(ap); adir = mnorm(tproj(adir, ap)); } }
#else
      if (phase == MARCH) ap = ro + rd * (t - t0);  // straight segment: exact (and over-relaxation may step back)
      else ap += adir * h;
#endif
      if (phase == MARCH) {
        p = ap; dir = adir;
#ifdef VIEW_GEO
        wn = nn;
        if (u_geoIters > 0) rayWorldCol = g_projMat[0].rgb;
#endif
      } else { sp = ap; sdir = adir; }
    }
  }

  vec3 col = accum;
  if (toSky) col += u_view > 0 ? vec3(0.002) : thr * sky(p, dir, rayWorldCol);  // the ray (or what continued of it) ended in the sky
  float tGrid = firstT >= 0.0 ? firstT : 1e9;
  col = groundGrid(col, gro, grd, tGrid);
  col = pow(col, vec3(0.4545));
  fragColor = vec4(col, 1.0);
}
)";

inline const char* PATHTRACE_MAIN = R"(
// ---- path tracing: physically based lighting ----
// Each sample follows one random light path. At every surface, the light arriving straight from one
// randomly chosen light (the sun, a point light or a glowing object) is added (next-event
// estimation); then the path continues in a direction drawn from the surface's BSDF (Lambert
// diffuse + GGX microfacet specular, or rough glass). Glowing objects are area lights: directions
// toward one are drawn in the cone of its bounding sphere and count only where the shadow ray
// reaches a glowing surface, so they shine with their real shape. A path that hits a glowing
// surface by itself is weighted against that (multiple importance sampling, balance heuristic), so
// the two ways add up without counting light twice. No ambient light: with the sky off and nothing
// glowing, everything is black. Directions live in 4D (Full 4D) or 3D (curved spaces, the Geodesic
// view's surface), which changes the normalisations below.
#if defined(VIEW_GEO) || defined(VIEW_CURVED)
#define PT_DIM3
const float COS_NORM = 3.14159265;  // integral of cos over a hemisphere of directions: 3D pi
#else
const float COS_NORM = 4.18879020;  // 4D: 4 pi / 3
#endif
const float PT_PI = 3.14159265;

// material of the current surface
vec3 g_base = vec3(0.0), g_F0 = vec3(0.04);
float g_alpha = 0.5, g_kd = 1.0, g_ps = 0.5;

float rndGauss() { return sqrt(-2.0 * log(max(rnd(), 1e-9))) * cos(6.2831853 * rnd()); }
// a random unit direction (uniform) perpendicular to the unit tangent a at x; Geodesic view: also
// along the surface (perpendicular to its normal wn)
vec4 perpDir(vec4 x, vec4 a, vec4 wn) {
  vec4 g = vec4(0.0);
  for (int i = 0; i < 3; i++) {
    g = vec4(rndGauss(), rndGauss(), rndGauss(), rndGauss());
#ifdef VIEW_CURVED
    g = tproj(g, x);
#endif
#ifdef VIEW_GEO
    g -= dot(g, wn) * wn;
#endif
    g -= mdot(g, a) * a;
    if (mdot(g, g) > 1e-8) break;
  }
  return mnorm(g);
}
// cosine-weighted direction around n: a uniform point of the (D-1)-ball lifted onto the hemisphere
vec4 cosineDir(vec4 x, vec4 n, vec4 wn) {
#ifdef PT_DIM3
  float r = sqrt(rnd());
#else
  float r = pow(rnd(), 1.0 / 3.0);
#endif
  return mnorm(perpDir(x, n, wn) * r + n * sqrt(max(1.0 - r * r, 0.0)));
}
// solid angle of a cone of half-angle a (4D: 2 pi (a - sin a cos a); series for small cones)
float cap4(float a) { return a < 0.1 ? a * a * a * (2.0 / 3.0 - a * a * (2.0 / 15.0)) : a - sin(a) * cos(a); }
float capPdf(float a) {
#ifdef PT_DIM3
  float s = sin(a * 0.5);
  return 1.0 / (4.0 * PT_PI * max(s * s, 1e-9));
#else
  return 1.0 / (2.0 * PT_PI * max(cap4(a), 1e-12));
#endif
}
// uniform direction in the cone of half-angle a around the unit tangent c
vec4 coneDir(vec4 x, vec4 c, float a, vec4 wn) {
#ifdef PT_DIM3
  float th = 2.0 * asin(clamp(sqrt(rnd()) * sin(a * 0.5), 0.0, 1.0));
#else
  float xi = rnd(), target = xi * cap4(a), th = a * pow(xi, 1.0 / 3.0);
  for (int i = 0; i < 4; i++) th = clamp(th - (cap4(th) - target) / max(2.0 * sin(th) * sin(th), 1e-7), 0.0, a);
#endif
  return mnorm(c * cos(th) + perpDir(x, c, wn) * sin(th));
}
// the cone of area light j's bounding sphere seen from x (false: x is inside it / it is behind the world)
bool lightCone(int j, vec4 x, out vec4 c, out float a, out float dist) {
  vec4 lp = u_lpos[j];
  float r = u_lext[j].x, s;
  c = vec4(0.0); a = 0.0;
#ifdef VIEW_CURVED
  dist = cdist(x, lp);
  vec4 tl = tproj(lp, x);
  if (mdot(tl, tl) < 1e-14 || dist <= r) return false;
  c = mnorm(tl);
  s = KAPPA < 0.0 ? sinh(r / u_curvR) / sinh(dist / u_curvR) : sin(r / u_curvR) / sin(dist / u_curvR);
#else
  vec4 dv = lp - x;
  dist = length(dv);
  if (dist <= r) return false;
  c = dv / dist;
  s = r / dist;
#endif
  if (s <= 0.0 || s >= 0.9999) return false;
  a = asin(s);
  return true;
}
// the area light (glowing object in the light list) whose sphere contains x, -1 if none
int areaLightAt(vec4 x) {
  for (int j = 0; j < 8; j++) {
    if (j >= u_nLights) break;
    if (u_lext[j].y < 0.5) continue;
#ifdef VIEW_CURVED
    if (cdist(x, u_lpos[j]) <= u_lext[j].x * 1.05) return j;
#else
    if (length(x - u_lpos[j]) <= u_lext[j].x * 1.05) return j;
#endif
  }
  return -1;
}
// Full 4D, "like 3D" falloff (setting): light from a glowing object dist away is scaled by
// dist / its radius, so it fades with distance^2 instead of ^3 (the same factor whichever way
// the light is found, so the two sampling strategies still agree)
float lightComp(vec4 x, float dist) {
#ifdef PT_DIM3
  return 1.0;
#else
  if (f_f.y < 0.5) return 1.0;
  int j = areaLightAt(x);
  return j < 0 ? 1.0 : max(dist / max(u_lext[j].x, 1e-3), 1.0);
#endif
}
// probability density of light sampling choosing direction w at x (all cones containing it)
float lightPdf(vec4 x, vec4 w, int nl, float ncand) {
#ifdef VIEW_GEO
  return 0.0;  // (the Geodesic view finds its lights by homing along the surface: no cones)
#endif
  float p = 0.0;
  for (int j = 0; j < 8; j++) {
    if (j >= nl) break;
    if (u_lext[j].y < 0.5) continue;
    vec4 c; float a, dist;
    if (lightCone(j, x, c, a, dist) && mdot(w, c) >= cos(a)) p += capPdf(a);
  }
  return p / max(ncand, 1.0);
}
// light reaching a path after a diffuse bounce is capped: rare very bright paths (a lamp seen
// through glass from a floor: caustics) would show as fireflies for a long time (a common, slight
// bias; mirrors and glass seen directly keep their full brightness)
bool g_hadDiffuse = false;
vec3 capIndirect(vec3 c, int depth) {
  float m = max(c.r, max(c.g, c.b));
  return g_hadDiffuse && m > 4.0 ? c * (4.0 / m) : c;
}
float G1(float x, float a2) { return 2.0 * x / (x + sqrt(a2 + (1.0 - a2) * x * x)); }
vec3 fresnel(vec3 F0, float c) { return F0 + (1.0 - F0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0); }
// BSDF x cos for light arriving from L, seen from V (both lobes; delta lights)
vec3 evalBsdfCos(vec4 n, vec4 V, vec4 L) {
  float nl = mdot(n, L), nv = mdot(n, V);
  if (nl <= 0.0 || nv <= 0.0) return vec3(0.0);
  vec4 H = mnorm(L + V);
  float nh = max(mdot(n, H), 0.0), vh = max(mdot(V, H), 0.0), a2 = g_alpha * g_alpha;
  float dd = nh * nh * (a2 - 1.0) + 1.0;
  vec3 spec = fresnel(g_F0, vh) * (a2 / (PT_PI * dd * dd)) * G1(nv, a2) * G1(nl, a2) / (4.0 * nv * nl);
  return (g_kd * g_base / COS_NORM + spec * float(u_specular)) * nl;
}
// a GGX microfacet normal around n (drawn from the distribution D(m) cos)
vec4 ggxNormal(vec4 x, vec4 n, vec4 wn, out float cm) {
  float xi = rnd(), t2 = g_alpha * g_alpha * xi / max(1.0 - xi, 1e-6);
  cm = inversesqrt(1.0 + t2);
  return mnorm(n * cm + perpDir(x, n, wn) * sqrt(max(1.0 - cm * cm, 0.0)));
}

const int MARCH = 0, NORMAL = 1, SHADOW = 3, DONE = 4;

void main() {
  g_seed = pcg(uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + uint(u_sample) * 26699u) | 1u;
#ifdef PT_GUIDE
  vec2 jit = vec2(0.0);  // the denoiser's guide: pixel centres
#else
  vec2 jit = (vec2(rnd(), rnd()) - 0.5) * 2.0 / u_res.y;  // a random point inside the pixel
#endif
  vec2 uv = v_uv * vec2(u_res.x / u_res.y, 1.0) + jit;
  vec4 ro = u_eye;
  vec4 rd = normalize(u_frame * vec4(uv * u_tanHalf, 1.0, 0.0));
#if defined(VIEW_CURVED)
  if (u_aperture > 0.0) {
    vec4 fp = geoPoint(ro, rd, u_focus / max(rd.z, 1e-3));
    float a = 6.2831853 * rnd(), r = u_aperture * sqrt(rnd());
    ro = geoPoint(ro, vec4(cos(a), sin(a), 0.0, 0.0), r);
    rd = mnorm(tproj(fp, ro));
  }
#elif !defined(VIEW_GEO)
  if (u_aperture > 0.0) {
    vec4 fp = ro + rd * (u_focus / max(dot(rd, u_frame[2]), 1e-3));
    float a = 6.2831853 * rnd(), r = u_aperture * sqrt(rnd());
    ro += (u_frame[0] * cos(a) + u_frame[1] * sin(a)) * r;
    rd = normalize(fp - ro);
  }
#endif

  vec4 p = ro, dir = rd;
  float t = 0.0;
  int steps = 0;
  vec4 wn = u_frame[3];
  int phase = MARCH, k = 0;
  bool inside = false, exiting = false, toSky = true, glass = false;
  MAT hm = MAT(0.0);
  vec4 hp = p, n = vec4(0.0);
  float nv[5], bv[5];
  float bumpHit = 0.0;
  vec4 sp = vec4(0.0), sdir = vec4(0.0);
  float st = 0.0, shMax = 0.0;
  int sSteps = 0;
  vec3 rayWorldCol = u_skyCol;
  vec3 accum = vec3(0.0), thr = vec3(1.0);
  int bounces = 0, depth = 0;
  float firstT = -1.0;
  vec4 gro = ro, grd = rd;
  float t0 = 0.0;
  float omega = u_relax == 1 ? 1.4 : 1.0, prevR = 0.0, stepLen = 0.0;
  // next-event estimation in flight: toward an area light?, its contribution before visibility / emission
  bool neeArea = false;
  vec3 neeF = vec3(0.0);
  // after a diffuse bounce: the light-sampling and the BSDF densities of the direction taken (MIS)
  bool lastDiffuse = false;
  float misPL = 0.0, misPB = 1.0;
  int nLights = u_nLights;
  // Geodesic view: light travels along the surface, so a shadow ray homes in on its light (slp)
  // the way the simple model's point lights do; neeR = the glowing object's radius
  bool homing = false;
  vec4 slp = vec4(0.0);
  float neeR = 0.0;
  int neeLi = 0;
  float ncand = float((u_sunI > 0.0 ? 1 : 0) + nLights);

  for (int it = 0; it < 3000; it++) {
    if (phase == DONE) break;
    vec4 q = p;
#ifdef VIEW_CURVED
    if (phase == NORMAL) { q = hp; if (k > 0 && k != 5) { if (k >= 6) q[k - 6] += BUMP_W / u_curvR; else q[k - 1] += 1e-3 / u_curvR; if (abs(q.w) < 100.0) q = crenorm(q); } }
#else
    if (phase == NORMAL) { q = hp; if (k >= 6) q[k - 6] += BUMP_W; else if (k > 0 && k < 5) q[k - 1] += 1e-3; }
#endif
    else if (phase == SHADOW) q = sp;

    MAT mm;
    g_exact = phase == SHADOW;
    g_noEmissive = false;  // glowing objects are physical here: they block light and are found by shadow rays
    g_noBody = phase == MARCH && t < u_markerSkip;
    g_noDbg = phase == MARCH && t < 0.12;
    // textures only near the surface: material ones for the hit, bump ones for the normal samples
    g_texMat = phase == MARCH && !inside;
    g_texBump = phase == NORMAL && k >= 5;
    g_texNear = phase == MARCH || phase == NORMAL ? u_eps * (1.0 + t * 0.02) * 3.0 + 3e-3 + (g_texBump ? BUMP_W : 0.0) : -1.0;
    g_bumpH = 0.0;
    float d = map(q, mm);   // the only scene evaluation site

    bool advance = false, bounce = false;
    vec4 ap = vec4(0.0), adir = vec4(0.0);
    float h = 0.0;

    if (phase == MARCH) {
      if (inside) {  // through a transparent object until its far side
        if (d > -u_eps * 0.5) { exiting = true; hp = p; nv[0] = d; phase = NORMAL; k = 1; }
        else {
          h = max(-d, u_eps * 2.0);
#ifdef VIEW_GEO
          h = min(h, u_geoMaxStep);
#endif
          ap = p; adir = dir; advance = true; t += h; steps++;
          if (steps >= u_maxSteps || t > u_maxDist) { phase = DONE; toSky = false; }
        }
      } else {
#ifdef VIEW_GEO
        bool sorFail = false;
#else
        bool sorFail = omega > 1.0 && (abs(d) + prevR) < stepLen;
#endif
        if (!sorFail && d < u_eps * (1.0 + t * 0.02)) {
          hm = mm; hp = p; nv[0] = d;
          if (firstT < 0.0) firstT = t;
          phase = NORMAL; k = 1;
        } else if (t > u_maxDist || steps >= u_maxSteps) {
          phase = DONE;
        } else {
#ifdef VIEW_GEO
          h = min(d * u_stepScale, u_geoMaxStep);
#else
          if (sorFail) { stepLen -= omega * stepLen; omega = 1.0; }
          else stepLen = d * u_stepScale * omega;
          prevR = abs(d);
          h = stepLen;
#endif
          ap = p; adir = dir; advance = true;
          t += h; steps++;
        }
      }
    } else if (phase == NORMAL) {
      if (k < 5) nv[k] = d; else bv[k - 5] = g_bumpH;
      k++;
      if (k == NORMAL_END) {
        vec4 gd = (vec4(nv[1], nv[2], nv[3], nv[4]) - nv[0]) / 1e-3;
#ifdef SCENE_BUMP
        gd -= (vec4(bv[1], bv[2], bv[3], bv[4]) - bv[0]) / BUMP_W;  // the relief raises the surface: d - h
        bumpHit = bv[0];
#endif
        n = normalize(gd);
#ifdef VIEW_GEO
        n = normalize(n - dot(n, wn) * wn + 1e-6);
#endif
#ifdef VIEW_CURVED
        n = mnorm(tproj(vec4(n.xyz, KAPPA * n.w), hp));
#endif
        k = 0;
        if (exiting) {  // leaving a transparent object: refract outward (total internal reflection stays inside)
          exiting = false;
          float ior = max(hm[2].y, 1.0);
          vec4 nd2 = mrefract(dir, -n, ior);
          phase = MARCH;
          if (mdot(nd2, nd2) < 1e-6) { dir = mreflect(dir, -n); p = geoPoint(hp, n, -u_eps * 3.0); }
          else { dir = mnorm(nd2); inside = false; p = geoPoint(hp, n, u_eps * 3.0); }
#ifdef VIEW_CURVED
          dir = mnorm(tproj(dir, p));
#endif
          ro = p; rd = dir; t0 = t;
          omega = 1.0; prevR = 0.0; stepLen = 0.0;
          continue;
        }
        // ---- a surface: its own light, then one light's direct light, then the path goes on ----
        g_base = hm[0].rgb;
#ifdef PT_GUIDE
        accum = g_base;  // the guide: the first surface's colour (and depth, below)
        phase = DONE; toSky = false;
        continue;
#endif
        float emis = clamp(hm[1].x, 0.0, 1.0);
        if (emis > 0.0) {
#ifdef VIEW_GEO
          // glowing objects in the light list are found by the homing shadow rays after diffuse bounces
          float w = lastDiffuse && areaLightAt(hp) >= 0 ? 0.0 : 1.0;
#else
          float w = lastDiffuse ? misPB / (misPB + misPL) : 1.0;
#endif
          accum += capIndirect(thr * g_base * emis * max(hm[2].z, 0.0) * w * (depth > 0 ? lightComp(hp, t - t0) : 1.0), depth);
        }
        if (depth == 0 && hm[0].a > 0.0) accum += vec3(1.0, 0.6, 0.15) * hm[0].a * 0.3;  // selection highlight
        float metal = clamp(hm[2].x, 0.0, 1.0);
        g_alpha = max(clamp(hm[1].y, 0.02, 1.0) * clamp(hm[1].y, 0.02, 1.0), 0.002);
        vec3 F0d = vec3(0.08 * clamp(hm[1].z, 0.0, 2.0));  // (specular 0.5: F0 = 0.04)
        g_F0 = mix(F0d, g_base, metal);
        float nvv = max(mdot(n, -dir), 1e-4);
        g_kd = (1.0 - metal) * (1.0 - fresnel(F0d, nvv).x) * (1.0 - emis);
        float ls = dot(fresnel(g_F0, nvv), vec3(1.0 / 3.0)) * float(u_specular), ld = g_kd * dot(g_base, vec3(1.0 / 3.0));
        g_ps = ld < 1e-5 ? 1.0 : ls < 1e-6 ? 0.0 : clamp(ls / (ls + ld), 0.05, 0.95);
        glass = rnd() < clamp(hm[1].w, 0.0, 1.0);
        bounce = true;
        if (!glass && ncand > 0.5) {
          int pick = min(int(rnd() * ncand), int(ncand) - 1);
          int li = u_sunI > 0.0 ? pick : pick + 1;  // 0 = the sun, 1.. = lights
          neeArea = li > 0 && u_lext[li - 1].y > 0.5;
          neeLi = li;
          homing = false;
          vec4 L = n;
          float cosL = 0.0;
#ifdef VIEW_GEO
          if (li > 0) {
            // home in on the light along the surface (a glowing object: a random point inside it, so
            // its shadows come out soft); its solid angle is known when the ray arrives
            homing = true;
            neeR = u_lext[li - 1].x;
            slp = u_lpos[li - 1];
            if (neeArea) { vec4 g = vec4(rndGauss(), rndGauss(), rndGauss(), rndGauss()); slp += normalize(g) * neeR * 0.7 * pow(rnd(), 0.25); }
            L = slp - hp;
            L -= dot(L, wn) * wn;
            L = dot(L, L) > 1e-12 ? normalize(L) : n;
            cosL = mdot(n, L);
            neeF = neeArea ? thr * ncand * (g_kd * g_base / COS_NORM) * cosL : thr * ncand * evalBsdfCos(n, -dir, L) * COS_NORM;
            shMax = neeArea ? u_maxDist : u_lcol[li - 1].w;
          } else
#endif
          if (neeArea) {
            vec4 c; float a, dist;
            if (lightCone(li - 1, hp, c, a, dist)) {
              L = coneDir(hp, c, a, wn);
              cosL = mdot(n, L);
              float pB = (1.0 - g_ps) * max(cosL, 0.0) / COS_NORM;
              neeF = thr * (g_kd * g_base / COS_NORM) * cosL / max(lightPdf(hp, L, nLights, ncand) + pB, 1e-12);
              shMax = dist + u_lext[li - 1].x;
            }
          } else {
            vec3 lc; float lmax;
            lightAt(li, hp, wn, dir, L, lc, lmax);
            cosL = mdot(n, L);
            // (delta lights: their brightness as in the simple model, where a white surface facing
            // a light of intensity 1 is lit to 1)
            neeF = thr * ncand * evalBsdfCos(n, -dir, L) * lc * COS_NORM;
            shMax = min(lmax, li == 0 ? u_shadowMax : 1e9);
          }
          if (cosL > 0.0 && max(neeF.r, max(neeF.g, neeF.b)) > 1e-6) {
            sp = geoPoint(hp, n, 0.01); sdir = L;
#ifdef VIEW_CURVED
            sdir = mnorm(tproj(L, sp));
#endif
            st = 0.02; sSteps = 0;
            phase = SHADOW;
            bounce = false;
          }
        }
      }
    } else {  // SHADOW: does the light get here?
      sSteps++;
      bool blocked = d < max(u_eps * 2.0, 0.002);
      bool outOfSteps = sSteps > u_shadowSteps * (homing ? 4 : neeArea ? 2 : 1);
      bool arrived = homing && !neeArea && length(slp - sp) < max(u_lightRadius, 0.15);  // (Geodesic: a point light reached)
      if (blocked || st > shMax || outOfSteps || arrived) {
        if (neeArea) {
          if (blocked && mm[1].x > 0.0) {
            vec3 Le = mm[0].rgb * clamp(mm[1].x, 0.0, 1.0) * max(mm[2].z, 0.0);
#ifdef VIEW_GEO
            // (the light's solid angle seen from where the shadow ray started, at the distance it travelled)
            float sa = min(neeR / max(st, 1e-3), 1.0);
            accum += capIndirect(neeF * Le * (2.0 * PT_PI * (1.0 - sqrt(1.0 - sa * sa))), depth);
#else
            accum += capIndirect(neeF * Le * lightComp(sp, st + 0.01), depth);
#endif
          }
        } else if (arrived) {
          float a = clamp(1.0 - st / max(u_lcol[neeLi - 1].w, 1e-3), 0.0, 1.0);  // (fades along the surface, as in the simple model)
          accum += neeF * u_lcol[neeLi - 1].rgb * a * a;
        } else if (!homing && !blocked && !outOfSteps) accum += neeF;  // (a ray that ran out of steps cannot confirm its light)
        bounce = true;
      } else {
#ifdef VIEW_GEO
        h = clamp(d, 0.03, u_geoMaxStep);
        if (homing && !neeArea) h = min(h, max(length(slp - sp) * 0.5, 0.03));  // (do not overshoot a point light)
#else
        float remain = max(shMax - st, 0.0);
        h = clamp(d, 0.02, max(0.4, remain / float(max(u_shadowSteps - sSteps, 1))));
#endif
        ap = sp; adir = sdir; advance = true; st += h;
      }
    }

    if (bounce) {
      // the path goes on (Russian roulette after 3 bounces keeps long paths cheap and unbiased)
      if (bounces >= u_maxLayers) { phase = DONE; toSky = false; }
      else if (bounces >= 3) {
        float qs = clamp(max(thr.r, max(thr.g, thr.b)), 0.05, 0.95);
        if (rnd() > qs) { phase = DONE; toSky = false; } else thr /= qs;
      }
      if (phase != DONE) {
        bool ok = true;
        lastDiffuse = false;
        if (glass) {
          // rough glass: a microfacet normal, then Fresnel reflection or refraction about it
          float cm, ior = max(hm[2].y, 1.0);
          vec4 m = g_alpha > 0.003 ? ggxNormal(hp, n, wn, cm) : n;
          float f0 = (ior - 1.0) / (ior + 1.0), c = clamp(-mdot(dir, m), 0.0, 1.0);
          f0 *= f0;
          if (rnd() < f0 + (1.0 - f0) * pow(1.0 - c, 5.0)) {
            dir = mreflect(dir, m);
            if (mdot(dir, n) <= 0.0) dir = mreflect(dir, n);
            p = geoPoint(hp, n, u_eps * 4.0);
          } else {
            vec4 nd2 = mrefract(dir, m, 1.0 / ior);
            if (mdot(nd2, nd2) < 1e-6) { dir = mreflect(dir, m); p = geoPoint(hp, n, u_eps * 4.0); }
            else { dir = mnorm(nd2); inside = true; p = geoPoint(hp, n, -u_eps * 4.0); thr *= mix(vec3(1.0), g_base, 0.5); }
          }
        } else {
          vec4 V = -dir;
          float nvv = mdot(n, V);
          if (rnd() < g_ps) {  // GGX: mirror the view about a microfacet normal
            float cm;
            vec4 m = ggxNormal(hp, n, wn, cm);
            float vm = mdot(V, m);
            vec4 L = mreflect(dir, m);
            float nl = mdot(n, L), a2 = g_alpha * g_alpha;
            if (vm <= 0.0 || nl <= 0.0 || nvv <= 0.0) ok = false;
            else { thr *= fresnel(g_F0, vm) * G1(nvv, a2) * G1(nl, a2) * vm / (nvv * cm) / g_ps; dir = L; }
          } else {  // diffuse
            vec4 L = cosineDir(hp, n, wn);
            thr *= g_kd * g_base / (1.0 - g_ps);
            misPB = (1.0 - g_ps) * max(mdot(n, L), 0.0) / COS_NORM;
            misPL = lightPdf(hp, L, nLights, ncand);
            lastDiffuse = true;
            g_hadDiffuse = true;
            dir = L;
          }
          p = geoPoint(hp, n, u_eps * 4.0);
        }
        if (!ok || max(thr.r, max(thr.g, thr.b)) < 1e-5) { phase = DONE; toSky = false; }
        else {
          phase = MARCH; bounces++; depth++;
#ifdef VIEW_CURVED
          dir = mnorm(tproj(dir, p));
#endif
          ro = p; rd = dir; t0 = t;
          omega = 1.0; prevR = 0.0; stepLen = 0.0;
        }
      }
    }

    if (advance) {
#ifdef VIEW_GEO
      ap += adir * h;
      vec4 nn = projectToWorld(ap);   // the only world-projection site
      if (phase == SHADOW && homing) adir = slp - ap;  // homing in on the light along the surface
      adir = adir - dot(adir, nn) * nn;
      adir = dot(adir, adir) > 1e-12 ? normalize(adir) : sdir;
#elif defined(VIEW_CURVED)
      if (phase == MARCH) { ap = ro; adir = rd; geoMove(ap, adir, t - t0); }
      else { geoMove(ap, adir, h); if (abs(ap.w) < 100.0) { ap = crenorm(ap); adir = mnorm(tproj(adir, ap)); } }
#else
      if (phase == MARCH) ap = ro + rd * (t - t0);
      else ap += adir * h;
#endif
      if (phase == MARCH) {
        p = ap; dir = adir;
#ifdef VIEW_GEO
        wn = nn;
        if (u_geoIters > 0) rayWorldCol = g_projMat[0].rgb;
#endif
      } else { sp = ap; sdir = adir; }
    }
  }

#ifdef PT_GUIDE
  fragColor = vec4(firstT >= 0.0 ? accum : vec3(-1.0), firstT >= 0.0 ? firstT : 1e4);
  return;
#endif
  vec3 col = accum;
  if (toSky) col += thr * sky(p, dir, rayWorldCol);  // the sky's light (Geodesic: the world surface's colour)
  if (any(isnan(col)) || any(isinf(col))) col = vec3(0.0);
  col = min(max(col, vec3(0.0)), vec3(32.0));  // (rare very bright samples would take long to average out)
  col = groundGrid(col, gro, grd, firstT >= 0.0 ? firstT : 1e9);
  fragColor = vec4(col, 1.0);  // linear: samples are averaged as light, gamma comes after (Renderer::resolve)
}
)";


// Overview of a curved space from outside: the Poincare ball (hyperbolic: the whole space in a
// unit ball) or the stereographic projection (spherical: the half of the space around the origin
// fills the unit ball, more out to radius 1.6). Rays are straight in the ball; each step converts
// the scene distance with the conformal factor (lengths in the ball = world / R * (1 -+ |x|^2) / 2).
/** Full-screen post passes of path-traced images (PostOp): inputs u_a, u_b, parameters u_p0, u_p1. */
inline std::string postShader(int op, ShaderTarget t) {
  std::string head = t == ShaderTarget::OpenGL
                         ? "#version 330 core\nuniform sampler2D u_a;\nuniform sampler2D u_b;\nuniform vec4 u_p0;\nuniform vec4 u_p1;\nout vec4 fragColor;\n"
                         : "#version 450\nlayout(set = 0, binding = 0) uniform sampler2D u_a;\nlayout(set = 0, binding = 1) uniform sampler2D u_b;\n"
                           "layout(push_constant) uniform Post { vec4 u_p0; vec4 u_p1; };\nlayout(location = 0) out vec4 fragColor;\n";
  if (op == 0)  // resolve: exposure, tone mapping (filmic ACES fit / clip), gamma (u_p0.z: already display colours)
    return head + R"(
vec3 aces(vec3 x) { return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
  if (u_p0.z > 0.5) { fragColor = vec4(clamp(texelFetch(u_a, ivec2(gl_FragCoord.xy), 0).rgb, 0.0, 1.0), 1.0); return; }
  vec3 c = max(texelFetch(u_a, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0)) * u_p0.x;
  c = u_p0.y > 0.5 ? aces(c) : min(c, vec3(1.0));
  fragColor = vec4(pow(c, vec3(0.4545)), 1.0);
}
)";
  if (op == 2)  // bilateral: radius u_p0.x pixels, stopped by colour (u_p0.y), surface colour and depth
    return head + R"(
vec3 soft(vec3 c) { return c / (1.0 + c); }
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy), sz = textureSize(u_a, 0);
  vec3 c0 = texelFetch(u_a, c, 0).rgb;
  if (any(isnan(c0)) || any(isinf(c0))) c0 = vec3(0.0);
  vec3 s0 = soft(max(c0, vec3(0.0)));
  vec4 g0 = texelFetch(u_b, c, 0);
  float rad = u_p0.x, sc = max(u_p0.y, 1e-4), sa = u_p0.z, sd = u_p0.w;
  int R = int(ceil(rad * 1.5));
  vec4 sum = vec4(0.0);
  for (int y = -R; y <= R; y++)
    for (int x = -R; x <= R; x++) {
      ivec2 q = clamp(c + ivec2(x, y), ivec2(0), sz - 1);
      vec4 ci = vec4(texelFetch(u_a, q, 0).rgb, 1.0);
      if (any(isnan(ci)) || any(isinf(ci))) continue;
      vec4 gi = texelFetch(u_b, q, 0);
      vec3 dc = soft(max(ci.rgb, vec3(0.0))) - s0, da = gi.rgb - g0.rgb;
      float dd = abs(gi.a - g0.a) / max(min(gi.a, g0.a), 1e-3);
      sum += ci * exp(-float(x * x + y * y) / (2.0 * rad * rad) - dot(dc, dc) / (sc * sc) - dot(da, da) / (sa * sa) - dd / sd);
    }
  fragColor = vec4(sum.rgb / max(sum.a, 1e-8), 1.0);
}
)";
  if (op == 3)  // median of the 3x3 neighbourhood by brightness, among pixels of the same surface
    return head + R"(
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy), sz = textureSize(u_a, 0);
  vec4 g0 = texelFetch(u_b, c, 0);
  vec3 col[9];
  float lum[9];
  bool ok[9];
  int n = 0;
  for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
      ivec2 q = clamp(c + ivec2(x, y), ivec2(0), sz - 1);
      vec3 ci = texelFetch(u_a, q, 0).rgb;
      if (any(isnan(ci)) || any(isinf(ci))) ci = vec3(0.0);
      vec4 gi = texelFetch(u_b, q, 0);
      float dd = abs(gi.a - g0.a) / max(min(gi.a, g0.a), 1e-3);
      int k = (y + 1) * 3 + (x + 1);
      col[k] = ci;
      lum[k] = dot(ci, vec3(0.299, 0.587, 0.114));
      ok[k] = dd < u_p0.w * 4.0 && length(gi.rgb - g0.rgb) < u_p0.z * 2.0;  // (the same surface)
      if (ok[k]) n++;
    }
  // the median: the pixel with as many darker as brighter neighbours (ties by index)
  vec3 res = col[4];
  for (int i = 0; i < 9; i++) {
    if (!ok[i]) continue;
    int below = 0;
    for (int j = 0; j < 9; j++)
      if (ok[j] && (lum[j] < lum[i] || (lum[j] == lum[i] && j < i))) below++;
    if (below == (n - 1) / 2) { res = col[i]; break; }
  }
  fragColor = vec4(res, 1.0);
}
)";
  // a-trous wavelet filter (5x5 B3 spline at step u_p0.x), stopped at edges: colour differences
  // (compared after a soft tone curve, relative), the guide's surface colour and its depth
  return head + R"(
vec3 soft(vec3 c) { return c / (1.0 + c); }
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy), sz = textureSize(u_a, 0);
  vec3 c0 = texelFetch(u_a, c, 0).rgb;
  if (any(isnan(c0)) || any(isinf(c0))) c0 = vec3(0.0);
  vec3 s0 = soft(max(c0, vec3(0.0)));
  vec4 g0 = texelFetch(u_b, c, 0);
  int step = int(u_p0.x);
  float sc = max(u_p0.y, 1e-4), sa = u_p0.z, sd = u_p0.w;
  float k[3] = float[3](0.375, 0.25, 0.0625);
  vec4 sum = vec4(0.0);  // (rgb: weighted colours, a: the weights)
  for (int y = -2; y <= 2; y++)
    for (int x = -2; x <= 2; x++) {
      ivec2 q = clamp(c + ivec2(x, y) * step, ivec2(0), sz - 1);
      vec4 ci = vec4(texelFetch(u_a, q, 0).rgb, 1.0);
      if (any(isnan(ci)) || any(isinf(ci))) continue;  // (one bad value would spread over the whole kernel)
      vec4 gi = texelFetch(u_b, q, 0);
      vec3 dc = soft(max(ci.rgb, vec3(0.0))) - s0, da = gi.rgb - g0.rgb;
      float dd = abs(gi.a - g0.a) / max(min(gi.a, g0.a), 1e-3);
      sum += ci * (k[abs(x)] * k[abs(y)] * exp(-dot(dc, dc) / (sc * sc) - dot(da, da) / (sa * sa) - dd / sd));
    }
  fragColor = vec4(sum.rgb / max(sum.a, 1e-8), 1.0);
}
)";
}

inline const char* BALL_MAIN = R"(
vec4 ballToModel(vec3 x) {
  float r2 = dot(x, x);
  return KAPPA < 0.0 ? vec4(2.0 * x, 1.0 + r2) / (1.0 - r2) : vec4(2.0 * x, 1.0 - r2) / (1.0 + r2);
}
float ballMap(vec3 x, out MAT m) {
  float r2 = dot(x, x);
  float d = mapScene(ballToModel(x), m) / u_curvR * (KAPPA < 0.0 ? 1.0 - r2 : 1.0 + r2) * 0.5;
  // you (a small marker with a nose)
  float dm = min(length(x - u_marker.xyz) - u_markerCol.w, length(x - u_marker.xyz - u_markerFwd.xyz * u_markerCol.w) - u_markerCol.w * 0.5);
  if (dm < d) { d = dm; m = MAT(vec4(u_markerCol.rgb, 0.0), vec4(1.0, 0.5, 0.0, 0.0), vec4(0.0)); }
  return d;
}
void main() {
  vec2 uv = v_uv * vec2(u_res.x / u_res.y, 1.0);
  vec3 ro = u_eye.xyz, rd = normalize((u_frame * vec4(uv * u_tanHalf, 1.0, 0.0)).xyz);
  float B = KAPPA < 0.0 ? 0.999 : 1.6;
  vec3 bg = vec3(0.07, 0.075, 0.09);
  // the ball
  float b = dot(ro, rd), c = dot(ro, ro) - B * B, h = b * b - c;
  if (h < 0.0) { fragColor = vec4(pow(bg, vec3(0.4545)), 1.0); return; }
  float t0 = max(-b - sqrt(h), 0.0), t1 = -b + sqrt(h);
  vec3 col = bg;
  vec3 nb = normalize(ro + rd * t0);
  float rim = pow(1.0 - abs(dot(nb, rd)), 3.0);
  float t = t0;
  bool hit = false;
  MAT m;
  for (int i = 0; i < 220; i++) {
    vec3 x = ro + rd * t;
    float d = ballMap(x, m);
    if (d < 0.0005 * (1.0 + t)) { hit = true; break; }
    t += max(d, 0.0004);
    if (t > t1) break;
  }
  vec3 inner = mix(u_skyBottom, u_skyTop, 0.5) * 0.35;
  if (hit) {
    vec3 x = ro + rd * t;
    MAT mm;
    const float e = 0.0008;
    vec3 n = normalize(vec3(ballMap(x + vec3(e, 0, 0), mm) - ballMap(x - vec3(e, 0, 0), mm),
                            ballMap(x + vec3(0, e, 0), mm) - ballMap(x - vec3(0, e, 0), mm),
                            ballMap(x + vec3(0, 0, e), mm) - ballMap(x - vec3(0, 0, e), mm)));
    vec3 L = normalize(vec3(0.5, 0.8, 0.35));
    vec3 base = m[0].rgb;
    float emis = clamp(m[1].x, 0.0, 1.0);
    vec3 lit = base * (0.25 + 0.75 * max(dot(n, L), 0.0));
    col = mix(lit, base, emis);
    col = mix(col, vec3(1.0, 0.6, 0.15), m[0].a * 0.6);
  } else col = inner;
  col = mix(col, vec3(0.75, 0.82, 1.0), rim * 0.35);  // the ball's boundary (infinity / the far half)
  fragColor = vec4(pow(col, vec3(0.4545)), 1.0);
}
)";

inline std::string buildFragmentShader(const std::string& sceneGLSL, bool geo, ShaderTarget t, int curved = 0, bool ball = false, bool pathTrace = false,
                                       bool guide = false) {
  if (ball && curved) return shaderPrelude(t, false, curved) + sceneGLSL + BALL_MAIN;
  return shaderPrelude(t, geo, curved) + (pathTrace && guide ? "#define PT_GUIDE\n" : "") + sceneGLSL + RAYMARCH_COMMON + RAYMARCH_GRID +
         (pathTrace ? PATHTRACE_MAIN : SIMPLE_MAIN);
}

}  // namespace fd
