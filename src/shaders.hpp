// Ray-marching shader (GLSL 3.30 core). The scene functions from codegen.cpp
// are inserted before MAIN. main() is a single-loop state machine
// (march -> normal -> AO -> shadow) with exactly one call to map() and one to
// the world projection, so compiled code stays proportional to the scene size.
#pragma once
#include <string>

namespace fd {

inline const char* VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
out vec2 v_uv;
void main() { v_uv = a_pos; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";

inline const char* RAYMARCH_MAIN = R"(
in vec2 v_uv;
out vec4 fragColor;

uniform vec2 u_res;
uniform vec4 u_eye;
uniform mat4 u_frame;     // columns: right, up, forward, ana/normal
uniform float u_tanHalf;
uniform int u_maxSteps;
uniform float u_maxDist;   // rays travelling further show the sky
uniform float u_eps;       // surface hit precision
uniform float u_stepScale; // < 1: safer (slower) marching for non-exact SDFs
uniform int u_shadows;
uniform float u_shadowK;   // shadow sharpness
uniform float u_shadowMax; // shadow ray length
uniform int u_ao;
uniform float u_aoStrength;
uniform float u_fog;       // 0 = no distance fog
uniform float u_ambient;
uniform int u_grid;
uniform float u_geoMaxStep;
uniform int u_geoIters;
uniform vec4 u_light;
uniform vec3 u_skyTop, u_skyBottom;  // Full 4D sky gradient
uniform vec4 u_marker;     // player body centre (4D)
uniform vec4 u_markerCol;  // rgb, radius (0 = hidden)
uniform vec4 u_markerFwd;
uniform float u_markerSkip;
uniform vec3 u_skyCol;     // Geodesic sky colour (= world surface colour)

bool g_noBody = false;     // primary ray still inside the player's own body

// scene + the player's body (a sphere with a small "nose" showing where it faces)
float map(vec4 p, out MAT m) {
  float d = mapScene(p, m);
  if (u_markerCol.w > 0.0 && !g_noBody) {
    float r = u_markerCol.w;
    float body = length(p - u_marker) - r;
    float nose = length(p - (u_marker + u_markerFwd * r * 0.95)) - r * 0.4;
    float dm = min(body, nose);
    if (dm < d) { d = dm; m = MAT(vec4(nose < body ? u_markerCol.rgb * 0.55 : u_markerCol.rgb, 0.0), vec4(0.0)); }
  }
  return d;
}

#ifdef VIEW_GEO
vec4 projectToWorld(inout vec4 p) {
  const float e = 1e-3;
  vec4 n = vec4(0.0, 1.0, 0.0, 0.0);
  for (int k = 0; k < 4; k++) {
    if (k >= u_geoIters) break;
    float v[5];
    for (int i = 0; i < 5; i++) {
      vec4 o = vec4(0.0);
      if (i > 0) o[i - 1] = e;
      v[i] = worldSDF(p + o);
    }
    vec4 g = (vec4(v[1], v[2], v[3], v[4]) - v[0]) / e;
    float gg = max(dot(g, g), 1e-8);
    p -= v[0] * g / gg;
    n = g * inversesqrt(gg);
  }
  return n;
}
#endif

vec3 sky(vec4 dir) {
#ifdef VIEW_GEO
  // the sky of a world surface has the colour of the surface itself
  return u_skyCol * (0.9 + 0.1 * dot(dir, normalize(u_light)));
#else
  return mix(u_skyBottom, u_skyTop, clamp(dir.y * 0.5 + 0.5, 0.0, 1.0)) + 0.05 * dir.w;
#endif
}

float gridLine(float x) {
  float f = abs(fract(x - 0.5) - 0.5);
  return 1.0 - smoothstep(0.0, fwidth(x), f);
}

const int MARCH = 0, NORMAL = 1, AO = 2, SHADOW = 3, DONE = 4;

void main() {
  vec2 uv = v_uv * vec2(u_res.x / u_res.y, 1.0);
  vec4 ro = u_eye;
  vec4 rd = normalize(u_frame * vec4(uv * u_tanHalf, 1.0, 0.0));

  vec4 p = ro, dir = rd;
  float t = 0.0;
  int steps = 0;
  vec4 wn = u_frame[3];
  int phase = MARCH, k = 0;
  bool hit = false;
  MAT hm = MAT(0.0);
  vec4 hp = p, n = vec4(0.0);
  float nv[5];
  float occ = 0.0, aow = 1.0;
  vec4 sp = vec4(0.0), sdir = vec4(0.0);
  float st = 0.0, sh = 1.0;
  int sSteps = 0;
  vec4 L = normalize(u_light), Ll = L;
  float dif = 0.0;

  for (int it = 0; it < 2000; it++) {
    if (phase == DONE) break;
    vec4 q = p;
    if (phase == NORMAL) { q = hp; q[k - 1] += 1e-3; }
    else if (phase == AO) q = hp + n * (0.03 + 0.12 * float(k + 1));
    else if (phase == SHADOW) q = sp;

    MAT mm;
    g_exact = phase == AO || phase == SHADOW;
    g_noEmissive = g_exact;
    g_noBody = phase == MARCH && t < u_markerSkip;
    float d = map(q, mm);   // the only scene evaluation site

    bool advance = false;
    vec4 ap = vec4(0.0), adir = vec4(0.0);
    float h = 0.0;

    if (phase == MARCH) {
      if (d < u_eps * (1.0 + t * 0.02)) {
        hit = true; hm = mm; hp = p; nv[0] = d;
        phase = NORMAL; k = 1;
      } else if (t > u_maxDist || steps >= u_maxSteps) {
        phase = DONE;
      } else {
#ifdef VIEW_GEO
        h = min(d * u_stepScale, u_geoMaxStep);
#else
        h = d * u_stepScale;
#endif
        ap = p; adir = dir; advance = true;
        t += h; steps++;
      }
    } else if (phase == NORMAL) {
      nv[k] = d; k++;
      if (k == 5) {
        n = normalize(vec4(nv[1], nv[2], nv[3], nv[4]) - nv[0]);
#ifdef VIEW_GEO
        n = normalize(n - dot(n, wn) * wn + 1e-6);
        Ll = L - dot(L, wn) * wn;
        Ll = dot(Ll, Ll) < 1e-4 ? -dir : normalize(Ll);
#endif
        dif = max(dot(n, Ll), 0.0);
        k = 0;
        sp = hp + n * 0.01; sdir = Ll; st = 0.02;
        bool emissive = hm[1].x > 0.999;  // fully emissive: no shading needed
        phase = emissive ? DONE : u_ao == 1 ? AO : (u_shadows == 1 && dif > 0.0 ? SHADOW : DONE);
      }
    } else if (phase == AO) {
      float hh = 0.03 + 0.12 * float(k + 1);
      occ += (hh - d) * aow; aow *= 0.7; k++;
      if (k == 5) phase = u_shadows == 1 && dif > 0.0 ? SHADOW : DONE;
    } else {
      sh = min(sh, u_shadowK * d / st);
      sSteps++;
      if (sh < 0.02 || st > u_shadowMax || sSteps > 64) phase = DONE;
      else {
#ifdef VIEW_GEO
        h = clamp(d, 0.03, u_geoMaxStep);
#else
        h = clamp(d, 0.02, 0.4);
#endif
        ap = sp; adir = sdir; advance = true; st += h;
      }
    }

    if (advance) {
      ap += adir * h;
#ifdef VIEW_GEO
      vec4 nn = projectToWorld(ap);   // the only world-projection site
      adir = normalize(adir - dot(adir, nn) * nn);
#endif
      if (phase == MARCH) {
        p = ap; dir = adir;
#ifdef VIEW_GEO
        wn = nn;
#endif
      } else { sp = ap; sdir = adir; }
    }
  }

  vec3 col;
  if (hit) {
    vec3 base = hm[0].rgb;
    float emissive = clamp(hm[1].x, 0.0, 1.0);
    float ao = u_ao == 1 ? clamp(1.0 - u_aoStrength * occ, 0.0, 1.0) : 1.0;
    sh = clamp(sh, 0.0, 1.0);
    vec4 V = -dir;
    vec4 H = normalize(Ll + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * 0.35;
    float fre = pow(1.0 - max(dot(n, V), 0.0), 3.0);
#ifdef VIEW_GEO
    vec3 rim = u_skyCol;
#else
    vec3 rim = sky(n);
#endif
    vec3 lit = base * (u_ambient * ao + 0.9 * dif * sh) + spec * sh + 0.12 * fre * ao * rim;
    col = mix(lit, base, emissive);  // emissive: always fully lit, unshadowed
    col = mix(col, vec3(1.0, 0.6, 0.15), hm[0].a * (0.25 + 0.5 * fre));
    float f = t / u_maxDist;
    col = mix(col, sky(dir), clamp(u_fog * f * f * 1.5, 0.0, 1.0));
  } else {
    col = sky(dir);
    t = 1e9;
  }

#ifndef VIEW_GEO
  if (u_grid == 1 && abs(rd.y) > 1e-4) {
    float tg = -ro.y / rd.y;
    if (tg > 0.0 && tg < t) {
      vec4 g = ro + rd * tg;
      float fade = exp(-tg * 0.05);
      float gl = max(gridLine(g.x), gridLine(g.z)) * 0.25;
      float gw = gridLine(g.w) * 0.25;
      vec3 gc = vec3(0.8) * gl + vec3(0.8, 0.3, 0.9) * gw;
      float ax = 1.0 - smoothstep(0.0, fwidth(g.z) * 1.5, abs(g.z));
      float az = 1.0 - smoothstep(0.0, fwidth(g.x) * 1.5, abs(g.x));
      col = mix(col, gc + col * 0.5, max(gl, gw) * fade);
      col = mix(col, vec3(0.9, 0.25, 0.25), ax * fade * 0.8);
      col = mix(col, vec3(0.25, 0.45, 0.95), az * fade * 0.8);
    }
  }
#endif

  col = pow(col, vec3(0.4545));
  fragColor = vec4(col, 1.0);
}
)";

inline std::string buildFragmentShader(const std::string& sceneGLSL, bool geo) {
  return std::string("#version 330 core\n") + (geo ? "#define VIEW_GEO\n" : "") + sceneGLSL + RAYMARCH_MAIN;
}

}  // namespace fd
