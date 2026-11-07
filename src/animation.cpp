#include "animation.hpp"

#include "defs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>

namespace fd {

using json = nlohmann::json;

// ============================================================ values
static const char* AX = "xyzw";
static const char* RGB = "rgb";

std::vector<std::string> transformPaths() { return {"pos.x", "pos.y", "pos.z", "pos.w", "scale", "rot"}; }

static void modValues(const Node& n, std::vector<AnimValue>& out) {
  for (size_t k = 0; k < n.mods.size(); k++) {
    const Modifier& m = n.mods[k];
    std::string p = "mods." + std::to_string(k) + ".", L = std::string(MOD_NAMES[m.type]) + " " + std::to_string(k + 1) + ": ";
    switch (m.type) {
      case MOD_MIRROR: out.push_back({p + "offset", L + "offset"}); out.push_back({p + "axis", L + "axis", true}); break;
      case MOD_REPEAT: out.push_back({p + "spacing", L + "spacing"}); out.push_back({p + "count", L + "copies", true}); break;
      case MOD_EXTRUDE: out.push_back({p + "length", L + "length"}); break;
      case MOD_REVOLVE: out.push_back({p + "radius", L + "radius"}); break;
      case MOD_RADIAL:
        out.push_back({p + "radius", L + "radius"});
        out.push_back({p + "angle", L + "angle step"});
        out.push_back({p + "startAngle", L + "start angle"});
        out.push_back({p + "count", L + "copies", true});
        break;
      case MOD_DEF:
        if (auto d = defs().modifier(m.def)) {
          L = d->name + " " + std::to_string(k + 1) + ": ";
          for (auto& pd : d->params) {
            std::string lab = L + (pd.label.empty() ? pd.name : pd.label);
            bool stepped = pd.type != P_FLOAT && pd.type != P_COLOR;
            if (pd.type == P_COLOR) for (int c = 0; c < 3; c++) out.push_back({p + "p." + pd.name + "." + RGB[c], lab + " " + RGB[c]});
            else out.push_back({p + "p." + pd.name, lab, stepped});
          }
        }
        break;
    }
  }
}

std::vector<AnimValue> animatableValues(const Node& n) {
  std::vector<AnimValue> v;
  for (int i = 0; i < 4; i++) v.push_back({std::string("pos.") + AX[i], std::string("Position ") + (char)std::toupper(AX[i])});
  v.push_back({"rot", "Rotation"});
  if (n.anchor) {  // attached to a world surface: motion along the surface (opt-in, see keyNode)
    v.push_back({"surf.travel", "Travel (along the surface)"});
    v.push_back({"surf.heading", "Heading"});
    v.push_back({"surf.spin", "Spin"});
  }
  if (n.isCamera()) {
    v.push_back({"cam.fov", "Field of view"});
    v.push_back({"cam.slider", "Slider axis", true});
    v.push_back({"cam.aperture", "Aperture"});
    v.push_back({"cam.focus", "Focus distance"});
    return v;
  }
  v.push_back({"scale", "Scale"});
  v.push_back({"hidden", "Hidden", true});
  if (n.isLeaf()) {
    for (int i = 0; i < 3; i++) v.push_back({std::string("color.") + RGB[i], std::string("Colour ") + RGB[i]});
    v.push_back({"thickness", "Thickness"});
    v.push_back({"mat.emissive", "Emissive"});
    v.push_back({"mat.roughness", "Roughness"});
    v.push_back({"mat.specular", "Specular"});
    v.push_back({"mat.transparency", "Transparency"});
    v.push_back({"mat.ior", "Refraction index"});
    v.push_back({"mat.metallic", "Metallic"});
    v.push_back({"mat.texScale", "Texture scale"});
    v.push_back({"mat.texStrength", "Texture strength"});
    for (int i = 0; i < 3; i++) v.push_back({std::string("mat.color2.") + RGB[i], std::string("Texture colour ") + RGB[i]});
    if (n.isCustom()) v.push_back({"codeBound", "Bound"});
  } else v.push_back({"k", "Smoothness"});
  modValues(n, v);
  return v;
}

std::vector<AnimValue> animatableLightValues() {
  std::vector<AnimValue> v;
  for (int i = 0; i < 4; i++) v.push_back({std::string("pos.") + AX[i], std::string("Position ") + (char)std::toupper(AX[i])});
  for (int i = 0; i < 3; i++) v.push_back({std::string("color.") + RGB[i], std::string("Colour ") + RGB[i]});
  v.push_back({"intensity", "Intensity"});
  v.push_back({"range", "Range"});
  v.push_back({"enabled", "On", true});
  return v;
}

bool isSteppedPath(const std::string& p) {
  return p == "hidden" || p == "cam.slider" || p == "enabled" || p == "shadows" ||
         (p.rfind("mods.", 0) == 0 && (p.size() > 5 && (p.substr(p.size() - 5) == "count" || p.substr(p.size() - 4) == "axis")));
}

static int axisOf(const std::string& path, size_t at) {
  if (path.size() <= at) return -1;
  const char* f = std::strchr(AX, path[at]);
  return f ? (int)(f - AX) : -1;
}
static int rgbOf(const std::string& path, size_t at) {
  if (path.size() <= at) return -1;
  const char* f = std::strchr(RGB, path[at]);
  return f ? (int)(f - RGB) : -1;
}

// value access: get / set through one function (set when `put` is given)
static bool access(Node& n, const std::string& p, double& v, bool put) {
  auto D = [&](double& x) { if (put) x = v; else v = x; return true; };
  auto I = [&](int& x) { if (put) x = (int)std::lround(v); else v = x; return true; };
  auto B = [&](bool& x) { if (put) x = v >= 0.5; else v = x ? 1 : 0; return true; };
  if (p.rfind("pos.", 0) == 0) { int a = axisOf(p, 4); return a >= 0 && D(n.xf.pos[a]); }
  if (p == "scale") { if (put) n.xf.scale = std::max(1e-3, v); else v = n.xf.scale; return true; }
  if (p == "hidden") return B(n.hidden);
  if (p.rfind("surf.", 0) == 0) {  // surface motion of attached objects
    if (!n.anchor) return false;
    if (p == "surf.travel") return D(n.anchor->travel);
    if (p == "surf.heading") return D(n.anchor->heading);
    if (p == "surf.spin") return D(n.anchor->spin);
    return false;
  }
  if (p == "cam.fov") return n.isCamera() && D(n.camFov);
  if (p == "cam.slider") return n.isCamera() && I(n.camSlider);
  if (p == "cam.aperture") return n.isCamera() && D(n.camAperture);
  if (p == "cam.focus") return n.isCamera() && D(n.camFocus);
  if (p == "k") return n.isGroup() && D(n.k);
  if (p.rfind("mods.", 0) == 0) {
    size_t dot = p.find('.', 5);
    if (dot == std::string::npos) return false;
    size_t k = (size_t)std::atoi(p.substr(5, dot - 5).c_str());
    if (k >= n.mods.size()) return false;
    Modifier& m = n.mods[k];
    std::string f = p.substr(dot + 1);
    if (f.rfind("p.", 0) == 0 && m.type == MOD_DEF) {  // a data-driven modifier's parameter (colour: .r / .g / .b)
      auto d = defs().modifier(m.def);
      if (!d) return false;
      std::string name = f.substr(2);
      int comp = 0;
      if (name.size() > 2 && name[name.size() - 2] == '.') { comp = rgbOf(name, (int)name.size() - 1); name = name.substr(0, name.size() - 2); }
      for (int i = 0; i < (int)d->params.size(); i++)
        if (d->params[i].name == name) {
          if (put) setModParam(m, *d, i, v, std::max(0, comp));
          else v = modParam(m, *d, i, std::max(0, comp));
          return true;
        }
      return false;
    }
    if (f == "offset") return D(m.offset);
    if (f == "axis") return I(m.axis);
    if (f == "spacing") return D(m.spacing);
    if (f == "count") return I(m.count);
    if (f == "length") return D(m.length);
    if (f == "radius") return D(m.radius);
    if (f == "angle") return D(m.angle);
    if (f == "startAngle") return D(m.startAngle);
    return false;
  }
  if (!n.isLeaf()) return false;
  if (p.rfind("color.", 0) == 0) { int c = rgbOf(p, 6); return c >= 0 && D(n.color[c]); }
  if (p == "thickness") return D(n.thickness);
  if (p == "codeBound") return D(n.codeBound);
  if (p == "mat.emissive") return D(n.mat.emissive);
  if (p == "mat.roughness") return D(n.mat.roughness);
  if (p == "mat.specular") return D(n.mat.specular);
  if (p == "mat.transparency") return D(n.mat.transparency);
  if (p == "mat.ior") return D(n.mat.ior);
  if (p == "mat.metallic") return D(n.mat.metallic);
  if (p == "mat.texScale") return D(n.mat.texScale);
  if (p == "mat.texStrength") return D(n.mat.texStrength);
  if (p.rfind("mat.color2.", 0) == 0) { int c = rgbOf(p, 11); return c >= 0 && D(n.mat.color2[c]); }
  return false;
}

bool getValue(const Node& n, const std::string& path, double& v) { return access(const_cast<Node&>(n), path, v, false); }
bool setValue(Node& n, const std::string& path, double v) { return access(n, path, v, true); }

static bool lightAccess(PointLight& l, const std::string& p, double& v, bool put) {
  auto D = [&](double& x) { if (put) x = v; else v = x; return true; };
  auto B = [&](bool& x) { if (put) x = v >= 0.5; else v = x ? 1 : 0; return true; };
  if (p.rfind("pos.", 0) == 0) { int a = axisOf(p, 4); return a >= 0 && D(l.pos[a]); }
  if (p.rfind("color.", 0) == 0) { int c = rgbOf(p, 6); return c >= 0 && D(l.color[c]); }
  if (p == "intensity") return D(l.intensity);
  if (p == "range") { if (put) l.range = std::max(0.01, v); else v = l.range; return true; }
  if (p == "enabled") return B(l.enabled);
  if (p == "shadows") return B(l.shadows);
  return false;
}
bool getLightValue(const PointLight& l, const std::string& path, double& v) { return lightAccess(const_cast<PointLight&>(l), path, v, false); }
bool setLightValue(PointLight& l, const std::string& path, double v) { return lightAccess(l, path, v, true); }

// ============================================================ interpolation
double easeProgress(Interp mode, double u) {
  u = std::clamp(u, 0.0, 1.0);
  switch (mode) {
    case Interp::Constant: return 0;
    case Interp::Linear: return u;
    case Interp::EaseIn: return u * u * u;
    case Interp::EaseOut: return 1 - (1 - u) * (1 - u) * (1 - u);
    case Interp::EaseInOut: return u < 0.5 ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3) / 2;
    default: return u * u * (3 - 2 * u);  // bezier (only used for rotations: smooth step)
  }
}

static double bezier1(double p0, double p1, double p2, double p3, double t) {
  double s = 1 - t;
  return s * s * s * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t * t * t * p3;
}

// segment from key a to key b at frame f (a.frame <= f <= b.frame)
static double segment(const Key& a, const Key& b, double f) {
  double span = b.frame - a.frame;
  if (span <= 1e-12) return b.value;
  double u = (f - a.frame) / span;
  if (a.interp == Interp::Constant) return a.value;
  if (a.interp != Interp::Bezier) return a.value + (b.value - a.value) * easeProgress(a.interp, u);
  // cubic bezier through (frame, value): handles clamped inside the segment so x(t) stays monotonic
  double x0 = a.frame, x1 = a.frame + std::clamp(a.rx, 0.0, span), x2 = b.frame + std::clamp(b.lx, -span, 0.0), x3 = b.frame;
  double y0 = a.value, y1 = a.value + a.ry, y2 = b.value + b.ly, y3 = b.value;
  double lo = 0, hi = 1, t = u;
  for (int i = 0; i < 40; i++) {  // solve x(t) = f by bisection (x is monotonic)
    t = (lo + hi) / 2;
    if (bezier1(x0, x1, x2, x3, t) < f) lo = t; else hi = t;
  }
  return bezier1(y0, y1, y2, y3, t);
}

static size_t keyIndex(const Track& t, double frame) {
  // last key with key.frame <= frame
  size_t i = 0;
  while (i + 1 < t.keys.size() && t.keys[i + 1].frame <= frame) i++;
  return i;
}

double evalTrack(const Track& t, double frame) {
  if (t.keys.empty()) return 0;
  size_t n = t.keys.size();
  bool lin = t.linearExtrap && n >= 2 && !isSteppedPath(t.path);
  if (frame <= t.keys.front().frame) {
    if (!lin) return t.keys.front().value;
    // the slope the first segment starts with (any interpolation; constant / eased ends are flat)
    const Key &a = t.keys[0], &b = t.keys[1];
    double e = std::min(0.01, (b.frame - a.frame) * 0.5);
    double slope = e > 1e-12 ? (segment(a, b, a.frame + e) - a.value) / e : 0;
    return a.value + slope * (frame - a.frame);
  }
  if (frame >= t.keys.back().frame) {
    if (!lin) return t.keys.back().value;
    const Key &a = t.keys[n - 2], &b = t.keys[n - 1];
    double e = std::min(0.01, (b.frame - a.frame) * 0.5);
    double slope = e > 1e-12 ? (b.value - segment(a, b, b.frame - e)) / e : 0;
    if (a.interp == Interp::Constant) slope = 0;
    return b.value + slope * (frame - b.frame);
  }
  size_t i = keyIndex(t, frame);
  double v = segment(t.keys[i], t.keys[i + 1], frame);
  return isSteppedPath(t.path) ? t.keys[i].value : v;
}

// symmetric 4x4 eigen decomposition (Jacobi): A = V diag(w) V^T, columns of V = eigenvectors
static void jacobi4(double A[4][4], double w[4], double V[4][4]) {
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) V[i][j] = i == j;
  for (int sweep = 0; sweep < 60; sweep++) {
    double off = 0;
    for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) off += A[i][j] * A[i][j];
    if (off < 1e-24) break;
    for (int p = 0; p < 4; p++)
      for (int q = p + 1; q < 4; q++) {
        if (std::abs(A[p][q]) < 1e-30) continue;
        double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
        double t = (th >= 0 ? 1 : -1) / (std::abs(th) + std::sqrt(th * th + 1));
        double c = 1 / std::sqrt(t * t + 1), s = t * c;
        for (int k = 0; k < 4; k++) {  // rotate rows / columns p, q
          double akp = A[k][p], akq = A[k][q];
          A[k][p] = c * akp - s * akq;
          A[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 4; k++) {
          double apk = A[p][k], aqk = A[q][k];
          A[p][k] = c * apk - s * aqk;
          A[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 4; k++) {
          double vkp = V[k][p], vkq = V[k][q];
          V[k][p] = c * vkp - s * vkq;
          V[k][q] = s * vkp + c * vkq;
        }
      }
  }
  for (int i = 0; i < 4; i++) w[i] = A[i][i];
}

M4 interpolateRotation(const M4& a, const M4& b, double u) {
  // (u outside 0..1 extrapolates: the plane angles are scaled past the keys, see evalRotation)
  if (u == 0) return a;
  if (u == 1) return b;
  // relative rotation Q = a^T b turns two orthogonal planes by angles t1, t2 (0..180 degrees).
  // The symmetric part C = (Q + Q^T) / 2 has eigenvalues cos t1, cos t2 (each twice): each
  // eigenvector pair spans one rotation plane. The path scales both angles by u.
  M4 Q = mul(transpose(a), b);
  double C[4][4], w[4], V[4][4];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) C[i][j] = (Q[j * 4 + i] + Q[i * 4 + j]) / 2;  // m[col*4+row]
  jacobi4(C, w, V);
  int ord[4] = {0, 1, 2, 3};
  std::sort(ord, ord + 4, [&](int x, int y) { return w[x] < w[y]; });
  auto eig = [&](int c) { return V4{V[0][ord[c]], V[1][ord[c]], V[2][ord[c]], V[3][ord[c]]}; };
  M4 R = identity();
  std::vector<V4> used;
  auto gs = [&](V4 e) { for (auto& x : used) e = reject(e, x); return e; };
  for (int pl = 0; pl < 2; pl++) {
    V4 u0 = gs(eig(pl * 2));
    if (length(u0) < 0.3) u0 = gs(eig(pl * 2 + 1));
    if (length(u0) < 0.3) break;
    u0 = normalize(u0);
    // second vector: where Q turns u0 within the plane; at 0 / 180 degrees, the pair's other eigenvector
    V4 qu = mul(Q, u0), v0 = gs(reject(qu, u0));
    if (length(v0) < 1e-6) v0 = gs(reject(eig(pl * 2 + 1), u0));
    if (length(v0) < 1e-6) v0 = gs(reject(eig(pl * 2), u0));
    if (length(v0) < 1e-6) break;
    v0 = normalize(v0);
    double ang = std::atan2(dot(qu, v0), dot(qu, u0));
    R = mul(R, rotationInPlane(u0, v0, ang * u));
    used.push_back(u0);
    used.push_back(v0);
  }
  return orthonormalize(mul(a, R));
}

M4 evalRotation(const Track& t, double frame) {
  if (t.keys.empty()) return identity();
  size_t n = t.keys.size();
  bool lin = t.linearExtrap && n >= 2;
  // linear extrapolation: keep turning at the end segment's average speed (same rotation planes)
  if (frame <= t.keys.front().frame) {
    if (!lin || t.keys[0].interp == Interp::Constant) return t.keys.front().rot;
    const Key &a = t.keys[0], &b = t.keys[1];
    double span = b.frame - a.frame;
    return span > 1e-12 ? orthonormalize(interpolateRotation(a.rot, b.rot, (frame - a.frame) / span)) : a.rot;
  }
  if (frame >= t.keys.back().frame) {
    const Key &a = t.keys[n > 1 ? n - 2 : 0], &b = t.keys[n - 1];
    if (!lin || a.interp == Interp::Constant) return b.rot;
    double span = b.frame - a.frame;
    return span > 1e-12 ? orthonormalize(interpolateRotation(a.rot, b.rot, 1 + (frame - b.frame) / span)) : b.rot;
  }
  size_t i = keyIndex(t, frame);
  const Key &a = t.keys[i], &b = t.keys[i + 1];
  double span = b.frame - a.frame;
  double u = span > 1e-12 ? (frame - a.frame) / span : 1;
  if (a.interp == Interp::Constant) return a.rot;
  return interpolateRotation(a.rot, b.rot, easeProgress(a.interp, u));
}

void autoHandles(Track& t) {
  size_t n = t.keys.size();
  for (size_t i = 0; i < n; i++) {
    Key& k = t.keys[i];
    double pf = i > 0 ? t.keys[i - 1].frame : k.frame, nf = i + 1 < n ? t.keys[i + 1].frame : k.frame;
    double lspan = (k.frame - pf) / 3, rspan = (nf - k.frame) / 3;
    if (!k.autoHandles) {  // keep the shape, only keep handles inside their segments
      k.lx = std::max(-std::max(lspan * 3, 1e-6), std::min(k.lx, 0.0));
      k.rx = std::min(std::max(rspan * 3, 1e-6), std::max(k.rx, 0.0));
      continue;
    }
    double slope = 0;
    if (i > 0 && i + 1 < n) {
      double pv = t.keys[i - 1].value, nv = t.keys[i + 1].value;
      bool extreme = (k.value >= pv && k.value >= nv) || (k.value <= pv && k.value <= nv);
      slope = extreme ? 0 : (nv - pv) / std::max(nf - pf, 1e-9);  // flat at peaks / valleys (no overshoot)
    } else if (t.linearExtrap && n >= 2) {
      // linear extrapolation: the end keys point along their segment, so the motion carries on
      const Key& o = i == 0 ? t.keys[1] : t.keys[n - 2];
      slope = (o.value - k.value) / ((o.frame - k.frame) != 0 ? (o.frame - k.frame) : 1e-9);
    }
    k.lx = -lspan;
    k.ly = -slope * lspan;
    k.rx = rspan;
    k.ry = slope * rspan;
  }
}

// ============================================================ editing
void sortKeys(Track& t) {
  std::stable_sort(t.keys.begin(), t.keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
  if (!t.rotation()) autoHandles(t);
}

Track* findTrack(Animation& a, const std::string& target, const std::string& path) {
  for (auto& t : a.tracks) if (t.target == target && t.path == path) return &t;
  return nullptr;
}

static Key& keyAt(Track& t, double frame, Interp interp) {
  for (auto& k : t.keys) if (std::abs(k.frame - frame) < 1e-6) return k;
  Key k;
  k.frame = frame;
  k.interp = isSteppedPath(t.path) ? Interp::Constant : interp;
  t.keys.push_back(k);
  return t.keys.back();
}

Track& setKey(Animation& a, const std::string& target, const std::string& path, double frame, double value, Interp interp) {
  Track* t = findTrack(a, target, path);
  if (!t) { a.tracks.push_back({target, path, {}}); t = &a.tracks.back(); }
  keyAt(*t, frame, interp).value = value;
  sortKeys(*t);
  return *t;
}

Track& setRotKey(Animation& a, const std::string& target, double frame, const M4& rot, Interp interp) {
  Track* t = findTrack(a, target, "rot");
  if (!t) { a.tracks.push_back({target, "rot", {}}); t = &a.tracks.back(); }
  keyAt(*t, frame, interp).rot = rot;
  sortKeys(*t);
  return *t;
}

void keyNode(Animation& a, const Node& n, double frame, bool transformOnly) {
  std::vector<AnimValue> vals;
  bool surf = n.anchor && (isAnimated(a, n.id, "surf.travel") || isAnimated(a, n.id, "surf.heading") || isAnimated(a, n.id, "surf.spin"));
  if (transformOnly && surf) {  // attached and moving along the surface: its surface motion and scale
    for (const char* p : {"surf.travel", "surf.heading", "surf.spin"}) vals.push_back({p, p});
    if (!n.isCamera()) vals.push_back({"scale", "scale"});
  } else if (transformOnly) {
    for (auto& p : transformPaths()) if (!(n.isCamera() && p == "scale")) vals.push_back({p, p});
    if (n.isCamera()) vals.push_back({"cam.fov", ""});
  } else {
    vals = animatableValues(n);
    if (!surf) vals.erase(std::remove_if(vals.begin(), vals.end(), [](const AnimValue& v) { return v.path.rfind("surf.", 0) == 0; }), vals.end());
    else vals.erase(std::remove_if(vals.begin(), vals.end(), [](const AnimValue& v) { return v.path == "rot" || v.path.rfind("pos.", 0) == 0; }), vals.end());
  }
  for (auto& v : vals) {
    if (v.path == "rot") { setRotKey(a, n.id, frame, nodeRotation(n)); continue; }
    double x;
    if (getValue(n, v.path, x)) setKey(a, n.id, v.path, frame, x);
  }
}

void keyLight(Animation& a, const PointLight& l, double frame) {
  for (auto& v : animatableLightValues()) {
    double x;
    if (getLightValue(l, v.path, x)) setKey(a, "light:" + l.id, v.path, frame, x);
  }
}

bool hasKeyAt(const Animation& a, const std::string& target, const std::string& path, double frame) {
  for (auto& t : a.tracks)
    if (t.target == target && t.path == path)
      for (auto& k : t.keys) if (std::abs(k.frame - frame) < 1e-6) return true;
  return false;
}

bool isAnimated(const Animation& a, const std::string& target, const std::string& path) {
  for (auto& t : a.tracks) if (t.target == target && t.path == path && !t.keys.empty()) return true;
  return false;
}

void deleteKey(Animation& a, const std::string& target, const std::string& path, double frame) {
  if (Track* t = findTrack(a, target, path)) {
    t->keys.erase(std::remove_if(t->keys.begin(), t->keys.end(), [&](const Key& k) { return std::abs(k.frame - frame) < 1e-6; }), t->keys.end());
    sortKeys(*t);
  }
}

void pruneTracks(Animation& a, const Node& root, const std::vector<PointLight>& lights) {
  a.tracks.erase(std::remove_if(a.tracks.begin(), a.tracks.end(),
                                [&](const Track& t) {
                                  if (t.keys.empty()) return true;
                                  if (t.target == "scene") return false;  // scene-wide values (curvature)
                                  if (t.target.rfind("light:", 0) == 0) {
                                    for (auto& l : lights) if ("light:" + l.id == t.target) return false;
                                    return true;
                                  }
                                  return findNode(const_cast<Node&>(root), t.target) == nullptr;
                                }),
                 a.tracks.end());
}

void applyAnimation(const Animation& a, Node& root, std::vector<PointLight>& lights, double frame) {
  for (auto& t : a.tracks) {
    if (t.keys.empty()) continue;
    if (t.target.rfind("light:", 0) == 0) {
      for (auto& l : lights) if ("light:" + l.id == t.target) setLightValue(l, t.path, evalTrack(t, frame));
      continue;
    }
    Node* n = findNode(root, t.target);
    if (!n) continue;
    if (t.rotation()) {
      M4 r = evalRotation(t, frame);
      // attached nodes: the key is their full frame; App re-attaches them to the surface afterwards
      if (n->anchor) n->xf.rot = orthonormalize(mul(transpose(n->anchor->basis), r));
      else n->xf.rot = r;
    } else setValue(*n, t.path, evalTrack(t, frame));
  }
}

// ============================================================ JSON
std::string animationToJson(const Animation& a) {
  json tr = json::array();
  for (auto& t : a.tracks) {
    json keys = json::array();
    for (auto& k : t.keys) {
      json kj = {{"f", k.frame}, {"i", INTERP_NAMES[(int)k.interp]}};
      if (t.rotation()) {
        json r = json::array();
        for (double x : k.rot) r.push_back(x);
        kj["r"] = r;
      } else {
        kj["v"] = k.value;
        if (!k.autoHandles) kj["h"] = {k.lx, k.ly, k.rx, k.ry};
      }
      keys.push_back(kj);
    }
    json tj = {{"target", t.target}, {"path", t.path}, {"keys", keys}};
    if (t.linearExtrap) tj["extrap"] = "linear";
    tr.push_back(tj);
  }
  return json({{"fps", a.fps}, {"start", a.start}, {"end", a.end}, {"frame", a.frame}, {"tracks", tr}}).dump();
}

Animation animationFromJson(const std::string& text) {
  Animation a;
  json j;
  try { j = json::parse(text); } catch (...) { return a; }
  if (!j.is_object()) return a;
  a.fps = std::clamp(j.value("fps", 24.0), 1.0, 240.0);
  a.start = j.value("start", 1);
  a.end = std::max(a.start, j.value("end", 120));
  a.frame = j.value("frame", (double)a.start);
  for (auto& tj : j.value("tracks", json::array())) {
    if (!tj.is_object()) continue;
    Track t;
    t.target = tj.value("target", std::string());
    t.path = tj.value("path", std::string());
    t.linearExtrap = tj.value("extrap", std::string()) == "linear";
    for (auto& kj : tj.value("keys", json::array())) {
      Key k;
      k.frame = kj.value("f", 1.0);
      std::string in = kj.value("i", std::string("Bezier"));
      for (int i = 0; i < (int)Interp::Count; i++) if (in == INTERP_NAMES[i]) k.interp = (Interp)i;
      if (kj.contains("r") && kj["r"].is_array() && kj["r"].size() == 16) {
        for (int i = 0; i < 16; i++) k.rot[i] = kj["r"][i].get<double>();
        k.rot = orthonormalize(k.rot);
      }
      k.value = kj.value("v", 0.0);
      if (kj.contains("h") && kj["h"].is_array() && kj["h"].size() == 4) {
        k.autoHandles = false;
        k.lx = kj["h"][0]; k.ly = kj["h"][1]; k.rx = kj["h"][2]; k.ry = kj["h"][3];
      }
      t.keys.push_back(k);
    }
    sortKeys(t);
    if (!t.target.empty() && !t.path.empty()) a.tracks.push_back(t);
  }
  return a;
}

}  // namespace fd
