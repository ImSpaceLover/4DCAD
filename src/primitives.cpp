#include "primitives.hpp"

#include <cstdio>
#include <limits>
#include <sstream>

namespace fd {

int primitiveDim(const PrimitiveDef& d) {
  int n = 0;
  for (auto& s : d.steps) if (s.op != StepOp::Rotate) n++;
  return n;
}

std::vector<std::string> validatePrimitive(const PrimitiveDef& d) {
  std::vector<std::string> errs;
  int dim = 0;
  for (size_t k = 0; k < d.steps.size(); k++) {
    auto& s = d.steps[k];
    std::string at = "step " + std::to_string(k + 1) + ": ";
    if (s.op == StepOp::Extrude) {
      if (dim >= 4) errs.push_back(at + "cannot exceed 4 dimensions");
      if (s.mode == ExtrudeMode::Finite && !(s.length > 0)) errs.push_back(at + "length must be > 0");
      dim++;
    } else if (s.op == StepOp::Revolve) {
      if (dim == 0) errs.push_back(at + "revolve needs at least a 1D profile");
      if (dim >= 4) errs.push_back(at + "cannot exceed 4 dimensions");
      int a = s.axis < 0 ? dim - 1 : s.axis;
      if (dim > 0 && (a < 0 || a >= dim)) errs.push_back(at + "revolve axis must be an existing axis");
      dim++;
    } else {
      if (s.i == s.j || s.i >= dim || s.j >= dim || s.i < 0 || s.j < 0) errs.push_back(at + "rotation plane must use two existing axes");
    }
  }
  if (d.steps.size() > 16) errs.push_back("at most 16 steps");
  return errs;
}

static std::vector<int> stepDims(const PrimitiveDef& d) {
  std::vector<int> dims;
  int dim = 0;
  for (auto& s : d.steps) { dims.push_back(dim); if (s.op != StepOp::Rotate) dim++; }
  return dims;
}

static double opExtrude(double d, double wy) {
  return std::min(std::max(d, wy), 0.0) + std::hypot(std::max(d, 0.0), std::max(wy, 0.0));
}

double evalPrimitive(const PrimitiveDef& def, const V4& p) {
  double c[4] = {p[0], p[1], p[2], p[3]};
  auto dims = stepDims(def);
  std::vector<double> t(def.steps.size(), 0);
  for (int k = (int)def.steps.size() - 1; k >= 0; k--) {
    auto& s = def.steps[k];
    int n = dims[k];
    if (s.op == StepOp::Extrude) t[k] = c[n];
    else if (s.op == StepOp::Revolve) {
      int a = s.axis < 0 ? n - 1 : s.axis;
      c[a] = std::hypot(c[a], c[n]) - s.radius;
    } else {
      double ang = -s.angle * PI / 180, ci = c[s.i], cj = c[s.j];
      c[s.i] = std::cos(ang) * ci - std::sin(ang) * cj;
      c[s.j] = std::sin(ang) * ci + std::cos(ang) * cj;
    }
  }
  double d = NEG_INF;
  for (size_t k = 0; k < def.steps.size(); k++) {
    auto& s = def.steps[k];
    if (s.op != StepOp::Extrude || s.mode == ExtrudeMode::Infinite) continue;
    double wy = s.mode == ExtrudeMode::Half ? t[k] : std::abs(t[k]) - s.length / 2;
    d = opExtrude(d, wy);
  }
  return d;
}

std::array<double, 4> primitiveExtents(const PrimitiveDef& def) {
  std::array<double, 4> e{0, 0, 0, 0};
  int n = 0;
  for (auto& s : def.steps) {
    if (s.op == StepOp::Extrude) { e[n] = s.mode == ExtrudeMode::Finite ? s.length / 2 : INFINITY; n++; }
    else if (s.op == StepOp::Revolve) { int a = s.axis < 0 ? n - 1 : s.axis; double v = s.radius + e[a]; e[a] = v; e[n] = v; n++; }
    else { double v = std::hypot(e[s.i], e[s.j]); e[s.i] = v; e[s.j] = v; }
  }
  return e;
}

std::string glslFloat(double x) {
  if (!std::isfinite(x)) return "0.0";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.9g", x);
  std::string s = buf;
  if (s.find_first_of(".eE") == std::string::npos) s += ".0";
  return s;
}

std::string primitiveGLSL(const PrimitiveDef& def, const std::string& fn) {
  static const char* C = "xyzw";
  auto dims = stepDims(def);
  std::ostringstream o;
  o << "float " << fn << "(vec4 c) {\n";
  for (int k = (int)def.steps.size() - 1; k >= 0; k--) {
    auto& s = def.steps[k];
    int n = dims[k];
    if (s.op == StepOp::Extrude) {
      if (s.mode != ExtrudeMode::Infinite) o << "  float t" << k << " = c." << C[n] << ";\n";
    } else if (s.op == StepOp::Revolve) {
      int a = s.axis < 0 ? n - 1 : s.axis;
      o << "  c." << C[a] << " = length(vec2(c." << C[a] << ", c." << C[n] << ")) - " << glslFloat(s.radius) << ";\n";
    } else {
      double ang = -s.angle * PI / 180;
      o << "  c." << C[s.i] << C[s.j] << " = mat2(" << glslFloat(std::cos(ang)) << ", " << glslFloat(std::sin(ang)) << ", "
        << glslFloat(-std::sin(ang)) << ", " << glslFloat(std::cos(ang)) << ") * c." << C[s.i] << C[s.j] << ";\n";
    }
  }
  o << "  float d = -1e9;\n";
  for (size_t k = 0; k < def.steps.size(); k++) {
    auto& s = def.steps[k];
    if (s.op != StepOp::Extrude || s.mode == ExtrudeMode::Infinite) continue;
    if (s.mode == ExtrudeMode::Half) o << "  d = opExtrude(d, t" << k << ");\n";
    else o << "  d = opExtrude(d, abs(t" << k << ") - " << glslFloat(s.length / 2) << ");\n";
  }
  o << "  return d;\n}\n";
  return o.str();
}

// ---------------------------------------------------------------- built-ins
static Step ext(double L) { Step s; s.op = StepOp::Extrude; s.mode = ExtrudeMode::Finite; s.length = L; return s; }
static Step inf() { Step s; s.op = StepOp::Extrude; s.mode = ExtrudeMode::Infinite; s.length = 0; return s; }
static Step half() { Step s; s.op = StepOp::Extrude; s.mode = ExtrudeMode::Half; s.length = 0; return s; }
static Step rev(double R = 0, int axis = -1) { Step s; s.op = StepOp::Revolve; s.radius = R; s.axis = axis; return s; }

static PrimitiveDef B(const char* id, const char* name, std::vector<Step> steps, const char* desc, double round = 0) {
  PrimitiveDef p;
  p.id = id; p.name = name; p.steps = std::move(steps); p.description = desc; p.builtin = true; p.round = round;
  return p;
}

const std::vector<PrimitiveDef>& builtinPrimitives() {
  static const std::vector<PrimitiveDef> list = {
      B("point", "Point", {}, "0D point (rounded to be visible)", 0.08),
      B("segment", "Segment", {ext(2)}, "1D line segment along X", 0.05),
      B("square", "Square", {ext(2), ext(2)}, "2D square in XY", 0.02),
      B("disk", "Disk", {ext(2), rev()}, "2D disk in XY: segment revolved", 0.02),
      B("cube", "Cube", {ext(2), ext(2), ext(2)}, "segment -> square -> cube"),
      B("sphere", "Sphere", {ext(2), rev(), rev(0, 0)}, "segment -> disk -> sphere"),
      B("cylinder", "Cylinder", {ext(2), rev(), ext(2)}, "disk extruded along Z"),
      B("rod", "Infinite rod", {ext(0.5), rev(), inf()}, "disk extruded infinitely along Z"),
      B("torus", "Torus", {ext(0.6), rev(), rev(1, 0)}, "disk revolved around Y at radius 1"),
      B("plane", "Plane", {inf(), half(), inf()}, "infinite ground half-space y <= 0 (3D)"),
      B("slab", "Slab", {inf(), ext(0.2), inf()}, "infinite thin slab, normal Y (3D)"),
      B("tesseract", "Tesseract", {ext(2), ext(2), ext(2), ext(2)}, "cube extruded along W"),
      B("hypersphere", "Hypersphere", {ext(2), rev(), rev(0, 0), rev(0, 0)}, "sphere revolved into W (glome)"),
      B("spherinder", "Spherinder", {ext(2), rev(), rev(0, 0), ext(2)}, "sphere extruded along W"),
      B("cubinder", "Cubinder", {ext(2), rev(), ext(2), ext(2)}, "cylinder extruded along W"),
      B("duocylinder", "Duocylinder", {ext(2), rev(), ext(2), rev(0, 2)}, "disk x disk (XY and ZW)"),
      B("spheritorus", "Spheritorus", {ext(0.8), rev(), rev(0, 0), rev(1.2, 0)}, "sphere revolved into W at radius 1.2"),
      B("torisphere", "Torisphere", {ext(0.6), rev(), rev(1.2, 0), rev(0, 0)}, "torus revolved into W"),
      B("tiger", "Tiger", {ext(0.6), rev(), rev(1.2, 0), rev(1.2, 1)}, "disk with two revolutions (XZ and YW)"),
      B("ditorus", "Ditorus", {ext(0.4), rev(), rev(0.8, 0), rev(1.6, 0)}, "torus revolved into W at radius 1.6"),
      B("hyperplane", "Hyperplane", {inf(), half(), inf(), inf()}, "infinite 4D ground half-space y <= 0"),
  };
  return list;
}

}  // namespace fd
