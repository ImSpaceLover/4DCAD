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
constexpr int NODE_VEC4S = 12;
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
  vec4 f_e;          // lens aperture, focus distance, (accumulation weight: used by the renderer), 0
  vec4 u_lpos[8];    // point lights: 4D position
  vec4 u_lcol[8];    // rgb * intensity, range
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
#define u_lightShadowMask f_k.x
#define u_maxLayers f_k.y
#define u_relax f_k.z
#define u_skyTop f_skyTop.rgb
#define u_skyBottom f_skyBottom.rgb
#define u_skyCol f_skyCol.rgb
#define u_dbgEndCol f_dbgEndCol.rgb
)";

inline std::string shaderPrelude(ShaderTarget t, bool geo) {
  std::string s;
  if (t == ShaderTarget::OpenGL) {
    s = "#version 330 core\n";
    if (geo) s += "#define VIEW_GEO\n";
    s += std::string("layout(std140) uniform Frame {") + FRAME_BLOCK + "};\n";
    s += "uniform sampler2D u_nodes;  // node data: row = node, 12 RGBA32F texels\n"
         "vec4 ndat(int i, int k) { return texelFetch(u_nodes, ivec2(k, i), 0); }\n"
         "in vec2 v_uv;\nout vec4 fragColor;\n";
  } else {
    s = "#version 450\n";
    if (geo) s += "#define VIEW_GEO\n";
    s += std::string("layout(std140, set = 0, binding = 0) uniform Frame {") + FRAME_BLOCK + "};\n";
    s += "layout(std430, set = 0, binding = 1) readonly buffer Nodes { vec4 nodeData[]; };\n"
         "vec4 ndat(int i, int k) { return nodeData[i * 12 + k]; }\n"
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

inline const char* RAYMARCH_MAIN = R"(
bool g_noBody = false;     // primary ray still inside the player's own body
bool g_noDbg = false;      // primary ray still next to the eye (the debug ray may pass through it)

// scene + the player's body (a sphere with a small "nose" showing where it faces) + the debug ray
float map(vec4 p, out MAT m) {
  float d = mapScene(p, m);
  if (u_markerCol.w > 0.0 && !g_noBody) {
    float r = u_markerCol.w;
    float body = length(p - u_marker) - r;
    float nose = length(p - (u_marker + u_markerFwd * r * 0.95)) - r * 0.4;
    float dm = min(body, nose);
    if (dm < d) { d = dm; m = MAT(vec4(nose < body ? u_markerCol.rgb * 0.55 : u_markerCol.rgb, 0.0), vec4(0.0, 0.6, 0.3, 0.0), vec4(0.0)); }
  }
  // debug ray: a thin glowing tube along its path (emissive: no shadows), end marker coloured by the stop reason
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
vec3 sky(vec4 dir, vec3 surf) {
#ifdef VIEW_GEO
  // the sky of a world surface has the colour of the surface the ray travelled to
  return surf * (0.9 + 0.1 * dot(dir, normalize(u_light)));
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

const int MARCH = 0, NORMAL = 1, AO = 2, SHADOW = 3, DONE = 4;

void main() {
  bool stoch = u_stochastic == 1;
  g_seed = pcg(uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + uint(u_sample) * 26699u) | 1u;
  // stochastic: a random point inside the pixel (anti-aliasing as the samples add up)
  vec2 jit = stoch ? (vec2(rnd(), rnd()) - 0.5) * 2.0 / u_res.y : vec2(0.0);
  vec2 uv = v_uv * vec2(u_res.x / u_res.y, 1.0) + jit;
  vec4 ro = u_eye;
  vec4 rd = normalize(u_frame * vec4(uv * u_tanHalf, 1.0, 0.0));
#ifndef VIEW_GEO
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
  float nv[5];
  float occ = 0.0, aow = 1.0;
  vec4 sp = vec4(0.0), sdir = vec4(0.0);
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
    if (phase == NORMAL) { q = hp; q[k - 1] += 1e-3; }
    else if (phase == AO) {
      vec4 ad = n;
      if (stoch) {  // sampled ambient occlusion: random directions around the normal
        ad = n + rndDir4() * 0.8;
#ifdef VIEW_GEO
        ad -= dot(ad, wn) * wn;
#endif
        ad = normalize(ad);
        if (dot(ad, n) < 0.0) ad = reflect(ad, n);
      }
      q = hp + ad * (0.03 + 0.12 * float(k + 1));
    }
    else if (phase == SHADOW) q = sp;

    MAT mm;
    g_exact = phase == AO || phase == SHADOW;
    g_noEmissive = g_exact;
    g_noBody = phase == MARCH && t < u_markerSkip;
    g_noDbg = phase == MARCH && t < 0.12;
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
      nv[k] = d; k++;
      if (k == 5) {
        n = normalize(vec4(nv[1], nv[2], nv[3], nv[4]) - nv[0]);
#ifdef VIEW_GEO
        n = normalize(n - dot(n, wn) * wn + 1e-6);
#endif
        k = 0;
        if (exiting) {
          // leaving a transparent object: refract outward (total internal reflection keeps it inside)
          exiting = false;
          float ior = max(hm[2].y, 1.0);
          vec4 nd2 = refract(dir, -n, ior);
          phase = MARCH;
          if (dot(nd2, nd2) < 1e-6) { dir = reflect(dir, -n); p = hp - n * (u_eps * 3.0); }
          else { dir = normalize(nd2); inside = false; p = hp + n * (u_eps * 3.0); }
          ro = p; rd = dir; t0 = t;
          omega = 1.0; prevR = 0.0; stepLen = 0.0;
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
      if (sh < 0.02 || st > shMax || sSteps > u_shadowSteps) {
        // this light is done: add its contribution
        float shv = clamp(sh, 0.0, 1.0);
        diffAcc += lc * dif * shv;
        vec4 H = normalize(Ll - dir);
        float rough = clamp(hm[1].y, 0.02, 1.0);
        specAcc += lc * pow(max(dot(n, H), 0.0), mix(160.0, 6.0, rough)) * hm[1].z * shv * float(u_specular);
        startLights = true;
      } else {
#ifdef VIEW_GEO
        h = clamp(d, 0.03, u_geoMaxStep);
#else
        h = clamp(d, 0.02, 0.4);
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
        dif = max(dot(n, Ll), 0.0);
        if (dif <= 0.0 || max(lc.r, max(lc.g, lc.b)) < 1e-4) continue;
        bool shadowed = li == 0 ? u_shadows == 1 : ((u_lightShadowMask >> (li - 1)) & 1) == 1;
        if (shadowed) {
          sp = hp + n * 0.01; sdir = Ll; st = 0.02; sh = 1.0; sSteps = 0; shMax = min(lmax, li == 0 ? u_shadowMax : 1e9);
          phase = SHADOW;
          break;
        }
        diffAcc += lc * dif;  // unshadowed light
        vec4 H = normalize(Ll - dir);
        float rough = clamp(hm[1].y, 0.02, 1.0);
        specAcc += lc * pow(max(dot(n, H), 0.0), mix(160.0, 6.0, rough)) * hm[1].z * float(u_specular);
      }
      if (li > u_nLights) {
        // all lights done: shade this surface
        vec3 base = hm[0].rgb;
        float emissive = clamp(hm[1].x, 0.0, 1.0);
        float ao = u_ao == 1 ? clamp(1.0 - u_aoStrength * occ, 0.0, 1.0) : 1.0;
        vec4 V = -dir;
        float fre = pow(1.0 - max(dot(n, V), 0.0), 3.0);
#ifdef VIEW_GEO
        vec3 rim = rayWorldCol;
#else
        vec3 rim = sky(n, rayWorldCol);
#endif
        vec3 lit = base * (u_ambient * ao + 0.9 * diffAcc) + specAcc + 0.12 * fre * ao * rim;
        vec3 c = mix(lit, base, emissive);  // emissive: always fully lit, unshadowed
        c = mix(c, vec3(1.0, 0.6, 0.15), hm[0].a * (0.25 + 0.5 * fre));  // selection highlight
        float f = t / u_maxDist;
        c = mix(c, sky(dir, rayWorldCol), clamp(u_fog * f * f * 1.5, 0.0, 1.0));
        // no bounces left (or bounces off): mirrors and glass are drawn as plain surfaces, not dark
        bool canBounce = layers < u_maxLayers;
        float transp = canBounce ? clamp(hm[1].w, 0.0, 1.0) : 0.0;
        float metal = canBounce ? clamp(hm[2].x, 0.0, 1.0) : 0.0;
        float wSurf = (1.0 - transp) * (1.0 - metal);  // the surface's own colour
        int path = 0;                                   // 0: stop here, 1: reflect, 2: refract
        vec3 tint = vec3(1.0);
        if (stoch) {
          // one path per sample, chosen by its share: the surface, a mirror reflection, or (glass)
          // a Fresnel reflection / refraction; the average over samples mixes them correctly
          float ior = max(hm[2].y, 1.0), f0 = (ior - 1.0) / (ior + 1.0);
          float F = f0 * f0 + (1.0 - f0 * f0) * pow(1.0 - abs(dot(dir, n)), 5.0);
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
            dir = reflect(dir, n);
            if (stoch && hm[1].y > 0.02) {
              dir = normalize(dir + rndDir4() * hm[1].y * hm[1].y);
#ifdef VIEW_GEO
              dir = normalize(dir - dot(dir, wn) * wn);
#endif
              if (dot(dir, n) < 0.0) dir = reflect(dir, n);
            }
            p = hp + n * (u_eps * 4.0);
          } else {
            // glass: bend into the object by its index of refraction
            vec4 nd2 = refract(dir, n, 1.0 / max(hm[2].y, 1.0));
            dir = dot(nd2, nd2) < 1e-6 ? reflect(dir, n) : normalize(nd2);
            inside = dot(nd2, nd2) >= 1e-6;
            p = inside ? hp - n * (u_eps * 4.0) : hp + n * (u_eps * 4.0);
          }
          ro = p; rd = dir; t0 = t;
          omega = 1.0; prevR = 0.0; stepLen = 0.0;
        } else { phase = DONE; toSky = false; }
      }
    }

    if (advance) {
#ifdef VIEW_GEO
      ap += adir * h;
      vec4 nn = projectToWorld(ap);   // the only world-projection site
      adir = normalize(adir - dot(adir, nn) * nn);
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
  if (toSky) col += thr * sky(dir, rayWorldCol);  // the ray (or what continued of it) ended in the sky
  float tGrid = firstT >= 0.0 ? firstT : 1e9;

#ifndef VIEW_GEO
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

  col = pow(col, vec3(0.4545));
  fragColor = vec4(col, 1.0);
}
)";

inline std::string buildFragmentShader(const std::string& sceneGLSL, bool geo, ShaderTarget t) {
  return shaderPrelude(t, geo) + sceneGLSL + RAYMARCH_MAIN;
}

}  // namespace fd
