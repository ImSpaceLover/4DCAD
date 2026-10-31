// Ray-marching shaders. The body is plain GLSL shared between GLSL ES 3.00
// (WebGL2) and desktop GLSL 3.30: only the header differs, so a native port can
// call buildFragmentShader(mode, '#version 330 core').
//
// The scene is NOT compiled into GLSL. A fixed interpreter (runProg) executes
// the bytecode produced by core/program.ts, stored in the float texture u_data.
// So the shader is compiled once per view mode, whatever the scene.
// main() is a single loop / state machine (march -> normal -> AO -> shadow) with
// one scene-evaluation site, to keep the compiled code small.

export const GLSL_ES_HEADER = '#version 300 es\nprecision highp float;\nprecision highp int;';

export const VERTEX_SHADER = `#version 300 es
in vec2 a_pos;
out vec2 v_uv;
void main() { v_uv = a_pos; gl_Position = vec4(a_pos, 0.0, 1.0); }`;

const MAIN = /* glsl */ `
in vec2 v_uv;
out vec4 fragColor;

uniform vec2 u_res;
uniform vec4 u_eye;
uniform mat4 u_frame;     // columns: right, up, forward, ana/normal
uniform float u_tanHalf;  // tan(fov/2)
uniform int u_maxSteps;
uniform int u_shadows;
uniform int u_ao;
uniform int u_grid;
uniform float u_geoMaxStep;
uniform int u_geoIters;
uniform vec4 u_light;

const float EPS = 0.0015;

// ================================================================ SDF interpreter
uniform highp sampler2D u_data;
uniform ivec2 u_main;           // (start, len) of the program for this view
uniform int u_worldStart[4];
uniform int u_worldLen[4];
uniform int u_nWorlds;          // world programs to precompute for 'hug' nodes
uniform int u_activeWorld;      // slot of the active world (-1 none)

#define INF 1e9
#define TEX_W 1024

vec4 T(int i) { return texelFetch(u_data, ivec2(i % TEX_W, i / TEX_W), 0); }

float opExtrude(float d, float wy) {
  vec2 w = vec2(d, wy);
  return min(max(w.x, w.y), 0.0) + length(max(w, 0.0));
}

// Primitive recipe: outside-in coordinate pass, then inside-out distance pass.
float primEval(int base, vec4 c) {
  int n = int(T(base).x);
  float tv[16];
  for (int k = 15; k >= 0; k--) {
    if (k >= n) continue;
    vec4 s = T(base + 1 + k);
    int op = int(s.x);
    if (op == 1) tv[k] = c[int(s.w)];
    else if (op == 2) { int a = int(s.z); c[a] = length(vec2(c[a], c[int(s.w)])) - s.y; }
    else if (op == 3) {
      int i = int(s.y), j = int(s.z);
      float ci = c[i], cj = c[j], cs = cos(s.w), sn = sin(s.w);
      c[i] = cs * ci - sn * cj;
      c[j] = sn * ci + cs * cj;
    }
  }
  float d = -1e9;
  for (int k = 0; k < 16; k++) {
    if (k >= n) break;
    vec4 s = T(base + 1 + k);
    if (int(s.x) == 1 && int(s.y) != 1) d = opExtrude(d, int(s.y) == 2 ? tv[k] : abs(tv[k]) - 0.5 * s.z);
  }
  return d;
}

float g_wv[4];

float runProg(int start, int len, vec4 p, out vec4 mOut) {
  vec4 fq[16]; float fs[16]; float fnd[16];
  float vd[16]; vec4 vm[16];
  int fp = 0, vp = 0;
  fq[0] = p; fs[0] = 1.0; fnd[0] = 0.0;
  int pc = start, end = start + len;
  for (int it = 0; it < 16384; it++) {
    if (pc >= end) break;
    vec4 ins = T(pc);
    pc++;
    int op = int(ins.x), nb = int(ins.y);
    if (op == 1) {                                   // BEGIN
      vec4 P = fq[fp];
      vec4 b1 = T(nb + 1);
      if (ins.w >= 0.0) {
        float bd = length(P - T(nb)) - b1.x;
        if (bd > T(nb + 9).w) { vd[vp] = bd; vm[vp] = vec4(0.0); vp++; pc = int(ins.w); continue; }
      }
      vec4 par = T(nb + 3);
      mat4 R = mat4(T(nb + 4), T(nb + 5), T(nb + 6), T(nb + 7));
      vec4 q = R * (P - T(nb + 2)) / par.x;
      float sAcc = fs[fp] * par.x;
      float nd = q.w;
      int role = int(ins.z);
      if (role == 1) q.w = 0.0;
      else if (role == 2) q.w = g_wv[int(T(nb + 9).z)] / sAcc;
      int nm = int(b1.y);
      for (int mi = 0; mi < 4; mi++) {
        if (mi >= nm) break;
        vec4 md = T(nb + 10 + mi);
        int ax = int(md.y);
        if (md.x == 1.0) q[ax] = abs(q[ax]);
        else if (md.z > 0.0) {
          float rr = floor(q[ax] / md.z + 0.5);
          if (md.w > 0.0) rr = clamp(rr, -md.w, md.w);
          q[ax] -= md.z * rr;
        }
      }
      fp++;
      fq[fp] = q; fs[fp] = sAcc; fnd[fp] = nd;
    } else if (op == 2) {                            // LEAF
      vec4 q = fq[fp];
      vec4 b1 = T(nb + 1), par = T(nb + 3), b9 = T(nb + 9);
      float d = primEval(int(b1.z), q);
      for (int a = 0; a < 4; a++) if (a >= int(b1.w)) d = opExtrude(d, abs(q[a]) - 0.5 * par.z);
      if (b9.y > 0.0) d = abs(d) - b9.y;
      vd[vp] = d - b9.x; vm[vp] = T(nb + 8); vp++;
    } else if (op == 3) {                            // COMBINE
      vp--;
      float b = vd[vp], a = vd[vp - 1];
      vec4 mb = vm[vp], ma = vm[vp - 1];
      float k = T(nb + 3).y;
      int o = int(ins.z);
      float d; vec4 m;
      if (o == 0) { d = min(a, b); m = b < a ? mb : ma; }
      else if (o == 1) { d = max(a, b); m = b > a ? mb : ma; }
      else if (o == 2) { d = max(a, -b); m = -b > a ? mb : ma; }
      else if (o == 3) { float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0); d = mix(b, a, h) - k * h * (1.0 - h); m = mix(mb, ma, h); }
      else if (o == 4) { float h = clamp(0.5 - 0.5 * (b - a) / k, 0.0, 1.0); d = mix(b, a, h) + k * h * (1.0 - h); m = mix(mb, ma, h); }
      else { float h = clamp(0.5 - 0.5 * (a + b) / k, 0.0, 1.0); d = mix(a, -b, h) + k * h * (1.0 - h); m = mix(ma, mb, h); }
      vd[vp - 1] = d; vm[vp - 1] = m;
    } else if (op == 4) {                            // END
      float nd = fnd[fp];
      fp--;
      float d = vd[vp - 1];
      if (d < INF * 0.5) {
        vec4 par = T(nb + 3);
        d *= par.x;
        if (ins.z > 0.0) d = max(d, -nd * par.x - par.w);
      }
      vd[vp - 1] = d;
    } else {                                         // EMPTY
      vd[vp] = INF; vm[vp] = vec4(0.0); vp++;
    }
  }
  mOut = vp > 0 ? vm[vp - 1] : vec4(0.0);
  return vp > 0 ? vd[vp - 1] : INF;
}

// Evaluate the scene (worldOnly = false) or only the active world surface.
float evalScene(vec4 p, bool worldOnly, out vec4 m) {
  float d = INF;
  int nw = worldOnly ? 0 : u_nWorlds;
  for (int w = 0; w <= 4; w++) {
    if (w > nw) break;
    bool last = w == nw;
    int st = last ? (worldOnly ? u_worldStart[u_activeWorld < 0 ? 0 : u_activeWorld] : u_main.x) : u_worldStart[w];
    int ln = last ? (worldOnly ? u_worldLen[u_activeWorld < 0 ? 0 : u_activeWorld] : u_main.y) : u_worldLen[w];
    d = runProg(st, ln, p, m);
    if (!last) g_wv[w] = d;
  }
  return d;
}

float mapScene(vec4 p, out vec4 m) { return evalScene(p, false, m); }
float worldSDF(vec4 p) { vec4 m; return evalScene(p, true, m); }


#ifdef VIEW_GEO
const float MAX_DIST = 60.0;
float map(vec4 p, out vec4 m) { return mapScene(p, m); }

// Pull p back onto the world surface (Newton steps on worldSDF); returns the unit normal.
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
#else
const float MAX_DIST = 120.0;
float map(vec4 p, out vec4 m) { return mapScene(p, m); }
#endif

vec3 sky(vec4 dir) {
#ifdef VIEW_GEO
  // direction-coded sky: makes curvature / holonomy of the world visible
  vec3 c = 0.5 + 0.5 * dir.xyz;
  return mix(vec3(0.07, 0.08, 0.12), c * vec3(0.35, 0.4, 0.55), 0.6 + 0.2 * dir.w);
#else
  return mix(vec3(0.16, 0.17, 0.2), vec3(0.32, 0.38, 0.5), clamp(dir.y * 0.5 + 0.5, 0.0, 1.0)) + 0.05 * dir.w;
#endif
}

float gridLine(float x) {
  float f = abs(fract(x - 0.5) - 0.5);
  return 1.0 - smoothstep(0.0, fwidth(x), f);
}

// phases of the per-pixel state machine
const int MARCH = 0, NORMAL = 1, AO = 2, SHADOW = 3, DONE = 4;

void main() {
  vec2 uv = v_uv * vec2(u_res.x / u_res.y, 1.0);
  vec4 ro = u_eye;
  vec4 rd = normalize(u_frame * vec4(uv * u_tanHalf, 1.0, 0.0));

  vec4 p = ro, dir = rd;       // march state
  float t = 0.0;
  int steps = 0;
  vec4 wn = u_frame[3];        // world normal at p (geodesic view)
  int phase = MARCH, k = 0;
  bool hit = false;
  vec4 hm = vec4(0.0), hp = p, n = vec4(0.0);
  float nv[5];
  float occ = 0.0, aow = 1.0;
  vec4 sp = vec4(0.0), sdir = vec4(0.0);   // shadow ray
  float st = 0.0, sh = 1.0;
  int sSteps = 0;
  vec4 L = normalize(u_light), Ll = L;
  float dif = 0.0;

  for (int it = 0; it < 1400; it++) {
    if (phase == DONE) break;
    vec4 q = p;
    if (phase == NORMAL) { q = hp; q[k - 1] += 1e-3; }
    else if (phase == AO) q = hp + n * (0.03 + 0.12 * float(k + 1));
    else if (phase == SHADOW) q = sp;

    vec4 mm;
    float d = map(q, mm);              // the ONLY scene evaluation site

    bool advance = false;
    vec4 ap = vec4(0.0), adir = vec4(0.0);
    float h = 0.0;

    if (phase == MARCH) {
      if (d < EPS * (1.0 + t * 0.02)) {
        hit = true; hm = mm; hp = p; nv[0] = d;
        phase = NORMAL; k = 1;
      } else if (t > MAX_DIST || steps >= u_maxSteps) {
        phase = DONE;
      } else {
#ifdef VIEW_GEO
        h = min(d, u_geoMaxStep);
#else
        h = d;
#endif
        ap = p; adir = dir; advance = true;
        t += h; steps++;
      }
    } else if (phase == NORMAL) {
      nv[k] = d; k++;
      if (k == 5) {
        n = normalize(vec4(nv[1], nv[2], nv[3], nv[4]) - nv[0]);
#ifdef VIEW_GEO
        // restrict normal and light to the 3D tangent space of the world
        n = normalize(n - dot(n, wn) * wn + 1e-6);
        Ll = L - dot(L, wn) * wn;
        Ll = dot(Ll, Ll) < 1e-4 ? -dir : normalize(Ll);
#endif
        dif = max(dot(n, Ll), 0.0);
        k = 0;
        sp = hp + n * 0.01; sdir = Ll; st = 0.02;
        phase = u_ao == 1 ? AO : (u_shadows == 1 && dif > 0.0 ? SHADOW : DONE);
      }
    } else if (phase == AO) {
      float hh = 0.03 + 0.12 * float(k + 1);
      occ += (hh - d) * aow; aow *= 0.7; k++;
      if (k == 5) phase = u_shadows == 1 && dif > 0.0 ? SHADOW : DONE;
    } else { // SHADOW
      sh = min(sh, 10.0 * d / st);
      sSteps++;
      if (sh < 0.02 || st > 12.0 || sSteps > 48) phase = DONE;
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
      vec4 nn = projectToWorld(ap);     // the ONLY world-projection site
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
    float ao = u_ao == 1 ? clamp(1.0 - 1.5 * occ, 0.0, 1.0) : 1.0;
    sh = clamp(sh, 0.0, 1.0);
    vec4 V = -dir;
    vec4 H = normalize(Ll + V);
    float spec = pow(max(dot(n, H), 0.0), 40.0) * 0.35;
    float fre = pow(1.0 - max(dot(n, V), 0.0), 3.0);
#ifdef VIEW_GEO
    vec3 rim = vec3(0.25, 0.27, 0.32);
#else
    vec3 rim = sky(n);
#endif
    col = hm.rgb * (0.18 * ao + 0.9 * dif * sh) + spec * sh + 0.12 * fre * ao * rim;
    col = mix(col, vec3(1.0, 0.6, 0.15), hm.a * (0.25 + 0.5 * fre));   // selection highlight
    float f = t / MAX_DIST;
    col = mix(col, sky(dir), clamp(f * f * 1.5, 0.0, 1.0));
  } else {
    col = sky(dir);
    t = 1e9;
  }

#ifndef VIEW_GEO
  // ground grid on the y = 0 hyperplane (Edit mode)
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
`;

export function buildFragmentShader(mode: 'full' | 'geo', header = GLSL_ES_HEADER): string {
  return `${header}\n${mode === 'geo' ? '#define VIEW_GEO\n' : ''}${MAIN}`;
}
