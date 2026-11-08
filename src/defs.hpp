// Definitions of data-driven modifiers (and, later, textures): code in the shader language plus a
// list of parameters edited with sliders / drop-down lists. The same code runs in the shader and,
// through the interpreter (shaderlang), on the CPU.
//
// A modifier definition may define these hooks (all optional):
//   vec4  warp(vec4 p)            moves the point before the inner shape is evaluated
//   float shape(float d, vec4 p)  changes the inner distance (p = the point before warp)
//   float bound(float r)          the new bounding radius for an inner radius r (default: r)
// Inside the hooks, every parameter is a variable of its name (float, int, bool, int for enum /
// axis, vec3 for colour), and `newAxis` / `srcAxis` are the axes a dimension-raising definition
// adds / spins (-1 otherwise).
//
// The five engine modifiers (mirror, repeat, extrude, revolve, radial repeat) are built-in
// definitions with a `role`: the engine keeps its own code for them (curved-space exact repeat,
// re-anchoring, symmetry features); their code is the equivalent reference, used by copies.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace fd {

struct Modifier;
namespace sl { class Program; }

enum ParamType { P_FLOAT = 0, P_INT, P_BOOL, P_ENUM, P_AXIS, P_COLOR, P_TYPE_COUNT };
inline const char* PARAM_TYPE_NAMES[P_TYPE_COUNT] = {"float", "int", "bool", "enum", "axis", "colour"};

struct ParamDef {
  std::string name;   // variable name in the code
  std::string label;  // shown in the UI (empty: the name)
  std::string tip;
  ParamType type = P_FLOAT;
  double min = 0, max = 1, def = 0;
  std::array<double, 3> defColor{1, 1, 1};
  std::vector<std::string> options;  // enum labels
  std::string field;                 // built-in engine modifiers: the Modifier field holding the value
  bool operator==(const ParamDef& o) const {
    return name == o.name && label == o.label && tip == o.tip && type == o.type && min == o.min && max == o.max && def == o.def &&
           defColor == o.defColor && options == o.options && field == o.field;
  }
};
int paramScalars(const ParamDef& p);                     // 1 (colour: 3)
int paramsScalars(const std::vector<ParamDef>& ps);
int paramsVec4s(const std::vector<ParamDef>& ps);        // at least 1
int paramOffset(const std::vector<ParamDef>& ps, int i); // scalar index of parameter i

struct TextureDef;

struct ModifierDef {
  std::string id, name, description;
  std::string category;  // "Basic", "Deform", ... (menus)
  bool builtin = false;
  std::vector<ParamDef> params;
  std::string code;
  bool raisesAxis = false;  // adds a dimension (the engine tracks the new axis)
  double safety = 1;        // distance scale for warps that stretch space (1 = exact)
  int role = 0;             // engine modifiers: MOD_MIRROR..MOD_RADIAL (native code); 0 = generic
  uint64_t hash() const;    // what the generated shader depends on (code, parameter layout, flags)
};

/** Registry of all definitions (built-in + user / scene ones), shared by the whole program.
 *  Definitions are immutable snapshots: editing one replaces it. Thread-safe. */
class DefRegistry {
 public:
  std::shared_ptr<const ModifierDef> modifier(const std::string& id) const;
  std::vector<std::shared_ptr<const ModifierDef>> modifiers() const;
  void setModifier(const ModifierDef& d);  // adds / replaces (built-ins cannot be replaced)
  void eraseModifier(const std::string& id);
  std::shared_ptr<const TextureDef> texture(const std::string& id) const;
  std::vector<std::shared_ptr<const TextureDef>> textures() const;
  void setTexture(const TextureDef& d);  // adds / replaces (built-ins cannot be replaced)
  void eraseTexture(const std::string& id);
  DefRegistry();
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
DefRegistry& defs();

/** Built-in modifier definitions (the five engine ones, Round, Onion, ...). */
const std::vector<ModifierDef>& builtinModifierDefs();
/** The definition a modifier uses (engine modifiers: by type), nullptr if unknown. */
std::shared_ptr<const ModifierDef> modifierDefOf(const Modifier& m);

// ---- parameter values: a list of scalars matching a parameter list (colours take 3) ----
double paramValue(const std::vector<double>& vals, const std::vector<ParamDef>& ps, int i, int comp = 0);
void setParamValue(std::vector<double>& vals, const std::vector<ParamDef>& ps, int i, double v, int comp = 0);
void conformValues(std::vector<double>& vals, const std::vector<ParamDef>& ps);
std::vector<double> packValues(const std::vector<double>& vals, const std::vector<ParamDef>& ps);  // paramsVec4s * 4
nlohmann::json valuesToJson(const std::vector<double>& vals, const std::vector<ParamDef>& ps);     // by name
void valuesFromJson(std::vector<double>& vals, const std::vector<ParamDef>& ps, const nlohmann::json& j);
/** Declarations of the parameter variables, read from the packed vec4s P_0 .. (GLSL). */
std::string paramDeclsGLSL(const std::vector<ParamDef>& ps);
/** Checks parameter names and options; "" if fine. */
std::string checkParams(const std::vector<ParamDef>& ps);

// ---- parameter values of a modifier ----
/** A new modifier of this definition with default parameters. */
Modifier makeModifier(const ModifierDef& d);
double modParam(const Modifier& m, const ModifierDef& d, int i, int comp = 0);
void setModParam(Modifier& m, const ModifierDef& d, int i, double v, int comp = 0);
/** All parameters packed as vec4s (paramsVec4s * 4 values), as the shader / interpreter get them. */
std::vector<double> packedParams(const Modifier& m, const ModifierDef& d);
/** Makes m.params fit the definition (after loading, or after the definition changed). */
void conformParams(Modifier& m, const ModifierDef& d);

// ---- code ----
struct HookInfo { bool warp = false, shape = false, bound = false; };
/** The definition's code with its hooks renamed (warp<suffix> ...) and taking the packed
 *  parameters (vec4 P_0 ..) plus `int newAxis, int srcAxis`, which declare the parameter variables.
 *  Other top-level functions are renamed with the suffix too. */
std::string transformedModifierCode(const ModifierDef& d, const std::string& suffix, HookInfo* hooks = nullptr);
/** Checks a definition (names, code in the interpreter, hook signatures); "" if fine. */
std::string checkModifierDef(const ModifierDef& d);

/** CPU runtime of a definition (compiled once per definition hash). */
struct ModRuntime {
  std::shared_ptr<const sl::Program> prog;
  HookInfo hooks;
  std::string error;
  int vec4s = 1;
};
std::shared_ptr<const ModRuntime> modRuntime(const ModifierDef& d);

// ---- files ----
nlohmann::json modifierDefToJson(const ModifierDef& d);
ModifierDef modifierDefFromJson(const nlohmann::json& j);  // throws on bad data
/** Parameter values by name (files: robust against definitions whose parameter list changed). */
nlohmann::json modParamsToJson(const Modifier& m, const ModifierDef& d);
void modParamsFromJson(Modifier& m, const ModifierDef& d, const nlohmann::json& j);
/** The modifiers Onion (shell > 0) and Round (round != 0) that replace the old node fields. */
void appendRoundShell(std::vector<Modifier>& mods, double round, double shell);

// ================================================================ textures
// A texture definition defines one of
//   float tex(vec4 p)   a mask 0..1
//   vec3  tex(vec4 p)   a colour
//   vec4  tex(vec4 p)   a colour (rgb) + coverage (a)
// p is the texture coordinate (the slot's mapping). Parameters are variables, as in modifiers.
enum TexKind { TEXK_MASK = 0, TEXK_RGB, TEXK_RGBA };

struct TextureDef {
  std::string id, name, description;
  std::string category;  // "Patterns", "Colour", ...
  bool builtin = false;
  std::vector<ParamDef> params;
  std::string code;
  std::string source = "code";  // "shapes": made in the texture editing mode (code generated from `shapes`)
  std::string shapes;           // shapes: the composition and its settings (JSON, see ShapeTexture in textures.hpp)
  uint64_t hash() const;
};
/** What tex returns (from its declaration; a mask if not found). */
TexKind textureKind(const TextureDef& d);
const std::vector<TextureDef>& builtinTextureDefs();
/** The built-in texture that replaces an old texture type (1..12), "" for none. */
const char* legacyTextureId(int type);
/** The code with tex renamed to tex<suffix>(vec4 p, vec4 P_0 ..) declaring the parameters, and
 *  other top-level functions renamed with the suffix. */
std::string transformedTextureCode(const TextureDef& d, const std::string& suffix);
std::string checkTextureDef(const TextureDef& d);

struct TexRuntime {
  std::shared_ptr<const sl::Program> prog;
  TexKind kind = TEXK_MASK;
  std::string error;
  int vec4s = 1;
};
std::shared_ptr<const TexRuntime> texRuntime(const TextureDef& d);
/** Runs the texture at p with packed parameters; out = (mask, 0, 0, 1) / (rgb, 1) / rgba. false on errors. */
bool runTexture(const TextureDef& d, const std::vector<double>& packed, const double p[4], double out[4]);

nlohmann::json textureDefToJson(const TextureDef& d);
TextureDef textureDefFromJson(const nlohmann::json& j);  // throws on bad data

}  // namespace fd
