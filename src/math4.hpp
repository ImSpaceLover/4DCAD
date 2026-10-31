// Minimal 4D linear algebra (double precision on the CPU).
// Matrices are 4x4 column-major (GLSL layout): m[col * 4 + row].
#pragma once
#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <vector>

namespace fd {

constexpr double PI = 3.14159265358979323846;

using V4 = std::array<double, 4>;
using M4 = std::array<double, 16>;

inline const char* AXIS = "XYZW";
inline const int PLANES[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
inline const char* PLANE_NAMES[6] = {"XY", "XZ", "XW", "YZ", "YW", "ZW"};

inline V4 operator+(const V4& a, const V4& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}; }
inline V4 operator-(const V4& a, const V4& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]}; }
inline V4 operator*(const V4& a, double s) { return {a[0] * s, a[1] * s, a[2] * s, a[3] * s}; }
inline V4 operator*(double s, const V4& a) { return a * s; }
inline V4 operator-(const V4& a) { return a * -1.0; }
inline double dot(const V4& a, const V4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }
inline double length(const V4& a) { return std::sqrt(dot(a, a)); }
inline V4 normalize(const V4& a) {
  double l = length(a);
  return l > 1e-12 ? a * (1.0 / l) : V4{0, 0, 0, 0};
}
inline V4 reject(const V4& a, const V4& n) { return a - n * dot(a, n); }
inline V4 unit(int i) { V4 v{0, 0, 0, 0}; v[i] = 1; return v; }

inline M4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
inline V4 col(const M4& m, int c) { return {m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]}; }
inline void setCol(M4& m, int c, const V4& v) { for (int i = 0; i < 4; i++) m[c * 4 + i] = v[i]; }
inline M4 fromCols(const V4& a, const V4& b, const V4& c, const V4& d) {
  M4 m{};
  setCol(m, 0, a); setCol(m, 1, b); setCol(m, 2, c); setCol(m, 3, d);
  return m;
}
inline V4 mul(const M4& m, const V4& v) {
  V4 r{0, 0, 0, 0};
  for (int row = 0; row < 4; row++) r[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
  return r;
}
inline M4 mul(const M4& a, const M4& b) {
  M4 r{};
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      double s = 0;
      for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  return r;
}
inline M4 transpose(const M4& m) {
  M4 r{};
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) r[row * 4 + c] = m[c * 4 + row];
  return r;
}
/** Rotation by angle (radians) in the plane of axes i -> j. */
inline M4 planeRotation(int i, int j, double a) {
  M4 m = identity();
  double c = std::cos(a), s = std::sin(a);
  m[i * 4 + i] = c; m[j * 4 + j] = c;
  m[i * 4 + j] = s; m[j * 4 + i] = -s;
  return m;
}
/** Rotation by angle in the plane spanned by orthonormal vectors a -> b (arbitrary plane). */
inline M4 rotationInPlane(const V4& a, const V4& b, double ang) {
  M4 m = identity();
  double c = std::cos(ang) - 1, s = std::sin(ang);
  for (int col_ = 0; col_ < 4; col_++)
    for (int row = 0; row < 4; row++)
      m[col_ * 4 + row] += s * (b[row] * a[col_] - a[row] * b[col_]) + c * (a[row] * a[col_] + b[row] * b[col_]);
  return m;
}
/** Rotation from 6 plane angles in degrees (XY XZ XW YZ YW ZW), legacy format. */
inline M4 rotationFromAngles(const double* deg) {
  M4 m = identity();
  for (int k = 0; k < 6; k++)
    if (deg[k] != 0) m = mul(m, planeRotation(PLANES[k][0], PLANES[k][1], deg[k] * PI / 180));
  return m;
}
inline M4 orthonormalize(const M4& m) {
  V4 c[4] = {col(m, 0), col(m, 1), col(m, 2), col(m, 3)};
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < i; j++) c[i] = c[i] - c[j] * dot(c[i], c[j]);
    c[i] = normalize(c[i]);
  }
  return fromCols(c[0], c[1], c[2], c[3]);
}
/** Extend orthonormal vectors to a full basis of R^4 (inputs first). */
inline std::vector<V4> completeBasis(std::vector<V4> vs) {
  for (int a = 0; a < 4 && vs.size() < 4; a++) {
    V4 v = unit(a);
    for (auto& u : vs) v = v - u * dot(v, u);
    if (length(v) > 1e-4) vs.push_back(normalize(v));
  }
  return vs;
}
inline double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }
inline double mixd(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace fd
