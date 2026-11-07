// A small interpreter for a GLSL subset, so that code written for the shader (custom SDFs,
// modifier and texture definitions) also runs exactly on the CPU (picking, placement, bounds).
//
// The language is valid GLSL: the same source is emitted into the shader as text and executed
// here. Supported:
//   types       float int bool vec2 vec3 vec4 mat2 mat3 mat4 void
//   functions   user functions (in parameters; no recursion), overloads
//   statements  declarations, = += -= *= /=, ++ --, if / else, for, while, break, continue,
//               return, blocks; object-like #define constants
//   expressions arithmetic (int and float, int -> float promotion), comparisons, && || !, ?:,
//               swizzles (read / write, xyzw rgba), [] on vectors / matrices, constructors,
//               matrix * vector / matrix, vector * matrix
//   built-ins   abs sign floor ceil fract mod min max clamp mix step smoothstep sqrt inversesqrt
//               pow exp exp2 log log2 sin cos tan asin acos atan(1|2) sinh cosh tanh length
//               distance dot cross normalize radians degrees round trunc transpose
// Programs are immutable after compile() and can be called from several threads at once.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace fd {
namespace sl {

enum Type { T_VOID = 0, T_BOOL, T_INT, T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_MAT2, T_MAT3, T_MAT4, T_ERROR };
const char* typeName(Type t);
int typeSize(Type t);  // components (mat: columns x rows)

struct Value {
  Type t = T_FLOAT;
  double v[16] = {0};
  static Value f(double x) { Value r; r.t = T_FLOAT; r.v[0] = x; return r; }
  static Value i(long x) { Value r; r.t = T_INT; r.v[0] = (double)x; return r; }
  static Value b(bool x) { Value r; r.t = T_BOOL; r.v[0] = x ? 1 : 0; return r; }
  static Value vec(int n, const double* c) { Value r; r.t = (Type)(T_VEC2 + n - 2); for (int k = 0; k < n; k++) r.v[k] = c[k]; return r; }
  static Value vec4(double x, double y, double z, double w) { double c[4] = {x, y, z, w}; return vec(4, c); }
};

class Program;

/** Compiles source (the shared prelude is NOT included automatically: pass it as part of the
 *  source if the code uses its helpers). Returns nullptr and sets err ("line:col: message"). */
std::shared_ptr<const Program> compile(const std::string& source, std::string& err);

/** Calls a function by name with the given arguments (converted to the parameter types when
 *  possible: int -> float). False + err if there is no such function or execution fails. */
bool call(const Program& p, const std::string& name, const std::vector<Value>& args, Value& out, std::string* err = nullptr);

/** True if the program defines a function `name` taking exactly these parameter types. */
bool hasFunction(const Program& p, const std::string& name, const std::vector<Type>& params, Type* ret = nullptr);

/** Compiles once per distinct source (cached, thread-safe). err is set on failure. */
std::shared_ptr<const Program> compileCached(const std::string& source, std::string* err = nullptr);

}  // namespace sl
}  // namespace fd
