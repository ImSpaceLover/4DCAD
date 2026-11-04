#include "polytope.hpp"

#include <algorithm>
#include <cmath>

namespace fd {

namespace {
const double PHI = (1 + std::sqrt(5.0)) / 2;

std::vector<V4> normed(std::vector<V4> v) {
  for (auto& x : v) x = normalize(x);
  return v;
}

std::vector<std::vector<V4>> makeFaces() {
  std::vector<std::vector<V4>> f(POLY_COUNT);
  f[POLY_TETRA] = normed({{1, 1, 1, 0}, {1, -1, -1, 0}, {-1, 1, -1, 0}, {-1, -1, 1, 0}});
  f[POLY_CUBE] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
  std::vector<V4> oct;
  for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2)
      for (int c = -1; c <= 1; c += 2) oct.push_back({(double)a, (double)b, (double)c, 0});
  f[POLY_OCTA] = normed(oct);
  // dodecahedron faces = icosahedron vertices: cyclic permutations of (0, +-1, +-phi)
  std::vector<V4> dod;
  for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2) {
      dod.push_back({0, (double)a, b * PHI, 0});
      dod.push_back({(double)a, b * PHI, 0, 0});
      dod.push_back({b * PHI, 0, (double)a, 0});
    }
  f[POLY_DODECA] = normed(dod);
  // icosahedron faces = dodecahedron vertices: (+-1, +-1, +-1) and cyclic (0, +-1/phi, +-phi)
  std::vector<V4> ico = oct;
  for (int a = -1; a <= 1; a += 2)
    for (int b = -1; b <= 1; b += 2) {
      ico.push_back({0, a / PHI, b * PHI, 0});
      ico.push_back({a / PHI, b * PHI, 0, 0});
      ico.push_back({b * PHI, 0, a / PHI, 0});
    }
  f[POLY_ICOSA] = normed(ico);
  return f;
}

const std::vector<std::vector<V4>>& faces() {
  static auto f = makeFaces();
  return f;
}
}  // namespace

const std::vector<V4>& polyFaces(int type) { return faces()[std::clamp(type, 0, POLY_COUNT - 1)]; }

double polyAdjCos(int type) {
  // the largest cos below 1 between two face directions = adjacent faces
  const auto& f = polyFaces(type);
  double best = -2;
  for (size_t i = 0; i < f.size(); i++)
    for (size_t j = i + 1; j < f.size(); j++) {
      double c = dot(f[i], f[j]);
      if (c < 1 - 1e-6) best = std::max(best, c);
    }
  return best;
}

const std::vector<std::pair<int, int>>& polyEdges(int type) {
  static std::vector<std::vector<std::pair<int, int>>> all;
  if (all.empty()) {
    all.resize(POLY_COUNT);
    for (int t = 0; t < POLY_COUNT; t++) {
      const auto& f = polyFaces(t);
      double c = polyAdjCos(t);
      for (size_t i = 0; i < f.size(); i++)
        for (size_t j = i + 1; j < f.size(); j++)
          if (std::abs(dot(f[i], f[j]) - c) < 1e-6) all[t].push_back({(int)i, (int)j});
    }
  }
  return all[std::clamp(type, 0, POLY_COUNT - 1)];
}

double polyCircumRatio(int type) {
  static const double r[POLY_COUNT] = {3.0, std::sqrt(3.0), std::sqrt(3.0), 1.2584, 1.2584};
  return r[std::clamp(type, 0, POLY_COUNT - 1)];
}

double polyFlatDihedral(int type) { return PI - std::acos(polyAdjCos(type)); }

const std::vector<Honeycomb>& honeycombs() {
  static const std::vector<Honeycomb> h = {
      {"Cubic", "{4,3,4}", SpaceKind::Flat, POLY_CUBE, 90, "cubes, 4 around each edge"},
      {"Order-5 cubic", "{4,3,5}", SpaceKind::Hyperbolic, POLY_CUBE, 72, "cubes, 5 around each edge"},
      {"Order-4 dodecahedral", "{5,3,4}", SpaceKind::Hyperbolic, POLY_DODECA, 90, "dodecahedra, 4 around each edge"},
      {"Order-5 dodecahedral", "{5,3,5}", SpaceKind::Hyperbolic, POLY_DODECA, 72, "dodecahedra, 5 around each edge"},
      {"Icosahedral", "{3,5,3}", SpaceKind::Hyperbolic, POLY_ICOSA, 120, "icosahedra, 3 around each edge"},
      {"Tesseract (8-cell)", "{4,3,3}", SpaceKind::Spherical, POLY_CUBE, 120, "8 cubes, 3 around each edge"},
      {"120-cell", "{5,3,3}", SpaceKind::Spherical, POLY_DODECA, 120, "120 dodecahedra, 3 around each edge"},
      {"24-cell", "{3,4,3}", SpaceKind::Spherical, POLY_OCTA, 120, "24 octahedra, 3 around each edge"},
      {"5-cell", "{3,3,3}", SpaceKind::Spherical, POLY_TETRA, 120, "5 tetrahedra, 3 around each edge"},
      {"16-cell", "{3,3,4}", SpaceKind::Spherical, POLY_TETRA, 90, "16 tetrahedra, 4 around each edge"},
      {"600-cell", "{3,3,5}", SpaceKind::Spherical, POLY_TETRA, 72, "600 tetrahedra, 5 around each edge"},
  };
  return h;
}

int firstHoneycomb(SpaceKind s) {
  const auto& h = honeycombs();
  for (size_t i = 0; i < h.size(); i++) if (h[i].space == s) return (int)i;
  return -1;
}

double honeycombInradius(const Honeycomb& h) {
  if (h.space == SpaceKind::Flat) return 0;
  double c = polyAdjCos(h.cell), t = h.dihedralDeg * PI / 180;
  double c2 = (1 + std::cos(t)) / (1 - c);  // cosK(h)^2
  double ch = std::sqrt(c2);
  return h.space == SpaceKind::Spherical ? std::acos(std::min(1.0, ch)) : std::acosh(std::max(1.0, ch));
}

// ---------------------------------------------------------------- distances
double polyDistFlat(int type, double h, const V4& u) {
  double d = -1e30;
  for (auto& f : polyFaces(type)) d = std::max(d, u[0] * f[0] + u[1] * f[1] + u[2] * f[2] - h);
  return d;
}

static double asinK(double k, double x) { return k > 0 ? std::asin(std::clamp(x, -1.0, 1.0)) : std::asinh(x); }

double polyDistCurved(const Space& s, int type, double hWorld, const V4& q) {
  double k = s.k(), t = hWorld / s.R(), Ct = cs::C(k, t), St = cs::S(k, t);
  double d = -1e30;
  for (auto& f : polyFaces(type)) {
    double x = (q[0] * f[0] + q[1] * f[1] + q[2] * f[2]) * Ct - q[3] * St;  // mdot(q, n)
    d = std::max(d, s.R() * asinK(k, x));
  }
  return d;
}

M4 honeycombCellOf(const Space& s, const PolyParams& pp, V4 q, int* parity) {
  M4 g = identity();
  int par = 0;
  const auto& all = honeycombs();
  if (!s.curved() || pp.honey < 0 || pp.honey >= (int)all.size() || all[pp.honey].space != s.kind) { if (parity) *parity = 0; return g; }
  const Honeycomb& hc = all[pp.honey];
  const auto& F = polyFaces(hc.cell);
  double k = s.k(), th = honeycombInradius(hc), Ct = cs::C(k, th), St = cs::S(k, th);
  for (int it = 0; it < 400; it++) {
    int bi = -1;
    double best = 1e-12;
    for (size_t i = 0; i < F.size(); i++) {
      double x = (q[0] * F[i][0] + q[1] * F[i][1] + q[2] * F[i][2]) * Ct - q[3] * St;
      if (x > best) { best = x; bi = (int)i; }
    }
    if (bi < 0) break;
    V4 n{F[bi][0] * Ct, F[bi][1] * Ct, F[bi][2] * Ct, -k * St};
    // reflection matrix: x -> x - 2 mdot(x, n) n
    M4 r = identity();
    for (int c = 0; c < 4; c++) setCol(r, c, unit(c) - n * (2 * cs::mdot(k, unit(c), n)));
    g = mul(r, g);
    q = mul(r, q);
    par ^= 1;
  }
  if (parity) *parity = par;
  return g;
}

double honeycombDist(const Space& s, const PolyParams& pp, V4 q, int* parity) {
  const auto& all = honeycombs();
  if (pp.honey < 0 || pp.honey >= (int)all.size()) return 1e9;
  const Honeycomb& hc = all[pp.honey];
  bool curved = s.curved();
  if ((hc.space == SpaceKind::Flat) == curved || (curved && hc.space != s.kind)) return 1e9;  // another space's tiling
  const auto& F = polyFaces(hc.cell);
  double k = s.k(), th = curved ? honeycombInradius(hc) : 0, Ct = curved ? cs::C(k, th) : 1, St = curved ? cs::S(k, th) : 0;
  double h = curved ? th * s.R() : std::max(pp.size, 1e-3);
  auto faceVal = [&](const V4& f) {  // > 0: outside that face
    if (curved) return (q[0] * f[0] + q[1] * f[1] + q[2] * f[2]) * Ct - q[3] * St;
    return q[0] * f[0] + q[1] * f[1] + q[2] * f[2] - h;
  };
  int par = 0;
  for (int it = 0; it < 96; it++) {  // fold into the central cell
    int bi = -1;
    double best = 1e-12;
    for (size_t i = 0; i < F.size(); i++) { double x = faceVal(F[i]); if (x > best) { best = x; bi = (int)i; } }
    if (bi < 0) break;
    const V4& f = F[bi];
    if (curved) {
      V4 n{f[0] * Ct, f[1] * Ct, f[2] * Ct, -k * St};
      q = q - n * (2 * best);  // reflection across the face (n is a unit space-like vector)
    } else {
      for (int a = 0; a < 3; a++) q[a] -= 2 * best * f[a];
    }
    par ^= 1;
  }
  if (parity) *parity = par;
  // distances to the cell's faces (positive inside)
  std::vector<double> a(F.size());
  for (size_t i = 0; i < F.size(); i++) {
    double x = faceVal(F[i]);
    a[i] = -(curved ? s.R() * asinK(k, x) : x);
  }
  if (pp.style == 1) {  // cells, shrunk to leave gaps
    double m = 1e30;
    for (double x : a) m = std::min(m, x);
    return -m + pp.width;
  }
  // edges: distance to the line where two faces meet, from the two face distances
  double t = hc.dihedralDeg * PI / 180, ct = std::cos(t), st2 = std::max(std::sin(t) * std::sin(t), 1e-6);
  double e = 1e30;
  for (auto& [i, j] : polyEdges(hc.cell)) {
    double x = std::max(a[i], 0.0), y = std::max(a[j], 0.0);
    e = std::min(e, std::sqrt(std::max((x * x + y * y + 2 * x * y * ct) / st2, 0.0)));
  }
  return e - pp.width;
}

}  // namespace fd
