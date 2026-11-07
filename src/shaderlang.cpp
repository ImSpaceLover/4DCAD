#include "shaderlang.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>

namespace fd {
namespace sl {

const char* typeName(Type t) {
  static const char* n[] = {"void", "bool", "int", "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "?"};
  return n[std::clamp((int)t, 0, (int)T_ERROR)];
}
int typeSize(Type t) {
  switch (t) {
    case T_BOOL: case T_INT: case T_FLOAT: return 1;
    case T_VEC2: return 2; case T_VEC3: return 3; case T_VEC4: return 4;
    case T_MAT2: return 4; case T_MAT3: return 9; case T_MAT4: return 16;
    default: return 0;
  }
}
static bool isScalar(Type t) { return t == T_INT || t == T_FLOAT || t == T_BOOL; }
static bool isNum(Type t) { return t == T_INT || t == T_FLOAT; }
static bool isVec(Type t) { return t >= T_VEC2 && t <= T_VEC4; }
static bool isMat(Type t) { return t >= T_MAT2 && t <= T_MAT4; }
static bool isFloaty(Type t) { return t == T_FLOAT || isVec(t) || isMat(t); }
static int vecN(Type t) { return isVec(t) ? t - T_VEC2 + 2 : 1; }
static int matN(Type t) { return isMat(t) ? t - T_MAT2 + 2 : 0; }
static Type vecType(int n) { return n == 1 ? T_FLOAT : (Type)(T_VEC2 + n - 2); }

// ================================================================ lexer
struct Tok {
  enum K { ID, NUM, OP, END } k = END;
  std::string s;
  double num = 0;
  bool isInt = false;
  int line = 1, col = 1;
};

static bool lex(const std::string& src, std::vector<Tok>& out, std::string& err) {
  std::map<std::string, std::vector<Tok>> macros;
  std::vector<Tok> raw;
  size_t i = 0, n = src.size();
  int line = 1, col = 1;
  auto adv = [&](size_t k) { for (size_t j = 0; j < k && i < n; j++, i++) { if (src[i] == '\n') { line++; col = 1; } else col++; } };
  bool lineStart = true;
  while (i < n) {
    char c = src[i];
    if (c == '\n') { adv(1); lineStart = true; continue; }
    if (std::isspace((unsigned char)c)) { adv(1); continue; }
    if (src.compare(i, 2, "//") == 0) { while (i < n && src[i] != '\n') adv(1); continue; }
    if (src.compare(i, 2, "/*") == 0) {
      size_t e = src.find("*/", i + 2);
      adv((e == std::string::npos ? n : e + 2) - i);
      continue;
    }
    if (c == '#' && lineStart) {  // preprocessor: object-like #define, other directives ignored
      size_t e = src.find('\n', i);
      if (e == std::string::npos) e = n;
      std::string ln = src.substr(i + 1, e - i - 1);
      int dl = line;
      adv(e - i);
      size_t p = ln.find_first_not_of(" \t");
      if (p != std::string::npos && ln.compare(p, 6, "define") == 0) {
        size_t a = ln.find_first_not_of(" \t", p + 6), b = a;
        while (b < ln.size() && (std::isalnum((unsigned char)ln[b]) || ln[b] == '_')) b++;
        if (a == std::string::npos || b == a) { err = std::to_string(dl) + ":1: bad #define"; return false; }
        std::string name = ln.substr(a, b - a);
        if (b < ln.size() && ln[b] == '(') { err = std::to_string(dl) + ":1: function-like macros are not supported"; return false; }
        std::vector<Tok> body;
        std::string e2;
        if (!lex(ln.substr(b), body, e2)) { err = std::to_string(dl) + ":1: " + e2; return false; }
        body.pop_back();  // END
        for (auto& t : body) { t.line = dl; }
        macros[name] = body;
      }
      continue;
    }
    lineStart = false;
    Tok t;
    t.line = line;
    t.col = col;
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t j = i;
      while (j < n && (std::isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
      t.k = Tok::ID;
      t.s = src.substr(i, j - i);
      adv(j - i);
    } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1]))) {
      size_t j = i;
      bool flt = false;
      while (j < n && std::isdigit((unsigned char)src[j])) j++;
      if (j < n && src[j] == '.') { flt = true; j++; while (j < n && std::isdigit((unsigned char)src[j])) j++; }
      if (j < n && (src[j] == 'e' || src[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (src[k] == '+' || src[k] == '-')) k++;
        if (k < n && std::isdigit((unsigned char)src[k])) { flt = true; j = k; while (j < n && std::isdigit((unsigned char)src[j])) j++; }
      }
      t.k = Tok::NUM;
      t.num = std::strtod(src.substr(i, j - i).c_str(), nullptr);
      if (j < n && (src[j] == 'f' || src[j] == 'F')) { flt = true; j++; }
      if (j < n && (src[j] == 'u' || src[j] == 'U')) j++;
      t.isInt = !flt;
      adv(j - i);
    } else {
      static const char* ops[] = {"<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "&&", "||", "^^", "<<", ">>"};
      t.k = Tok::OP;
      for (auto o : ops) if (src.compare(i, std::strlen(o), o) == 0) { t.s = o; break; }
      if (t.s.empty()) {
        if (std::strchr("+-*/%=<>!&|^?:;,.(){}[]~", c)) t.s = std::string(1, c);
        else { err = std::to_string(line) + ":" + std::to_string(col) + ": unexpected character '" + std::string(1, c) + "'"; return false; }
      }
      adv(t.s.size());
    }
    raw.push_back(t);
  }
  // macro substitution (nested macros expanded, no recursion)
  std::function<void(const Tok&, int)> emit = [&](const Tok& t, int depth) {
    if (t.k == Tok::ID && depth < 16) {
      auto it = macros.find(t.s);
      if (it != macros.end()) {
        for (Tok m : it->second) { m.line = t.line; m.col = t.col; emit(m, depth + 1); }
        return;
      }
    }
    out.push_back(t);
  };
  for (auto& t : raw) emit(t, 0);
  Tok e;
  e.k = Tok::END;
  e.line = line;
  e.col = col;
  out.push_back(e);
  return true;
}

// ================================================================ AST
enum Builtin {
  B_NONE = 0, B_ABS, B_SIGN, B_FLOOR, B_CEIL, B_FRACT, B_SQRT, B_ISQRT, B_EXP, B_EXP2, B_LOG, B_LOG2, B_SIN, B_COS, B_TAN,
  B_ASIN, B_ACOS, B_ATAN, B_SINH, B_COSH, B_TANH, B_RAD, B_DEG, B_ROUND, B_TRUNC,
  B_POW, B_MOD, B_MIN, B_MAX, B_STEP, B_ATAN2, B_CLAMP, B_MIX, B_SMOOTH, B_LENGTH, B_DISTANCE, B_DOT, B_CROSS, B_NORMALIZE,
  B_TRANSPOSE
};

struct Expr {
  enum K { NUM, LVAR, GVAR, CALL, BUILTIN, CTOR, NEG, NOT, BIN, AND, OR, TERN, ASSIGN, SWZ, INDEX, PREINC, POSTINC } k = NUM;
  Type t = T_FLOAT;
  int line = 0, col = 0;
  Value num;
  int slot = 0;        // LVAR / GVAR
  int op = 0;          // BIN / ASSIGN: operator char ('+', '-', '*', '/', '%', '<', 'l' (<=), '>', 'g' (>=), 'e' (==), 'n' (!=), '=' plain assign)
  int fn = 0;          // CALL: function index; BUILTIN: Builtin
  int sw[4] = {0, 0, 0, 0}, nsw = 0;
  int delta = 0;       // ++ / --
  std::vector<std::unique_ptr<Expr>> a;
};

struct Stmt {
  enum K { BLOCK, DECL, EXPR, IF, FOR, WHILE, DO, RET, BRK, CONT, NOP } k = NOP;
  std::vector<std::unique_ptr<Stmt>> b;
  std::unique_ptr<Expr> e, e2;
  std::vector<std::pair<int, std::unique_ptr<Expr>>> decls;  // slot, initialiser (may be null)
  Type declT = T_FLOAT;
};

struct Func {
  std::string name;
  Type ret = T_VOID;
  std::vector<Type> params;
  std::vector<int> paramSlots;
  int nlocals = 0;
  std::unique_ptr<Stmt> body;
  bool defined = false;
  size_t bodyTok = 0;  // token index of '{'
};

}  // namespace sl

class sl::Program {
 public:
  std::vector<sl::Func> funcs;
  std::vector<sl::Type> globalTypes;
  std::vector<std::pair<int, std::unique_ptr<sl::Expr>>> globalInits;  // in order
};

namespace sl {

// ================================================================ parser
struct Parser {
  std::vector<Tok> tk;
  size_t p = 0;
  std::string err;
  Program* prog = nullptr;
  std::map<std::string, int> globals;
  std::vector<std::map<std::string, std::pair<int, Type>>> scopes;  // locals: slot, type
  Func* cur = nullptr;
  int loopDepth = 0;

  const Tok& peek(int k = 0) { return tk[std::min(p + k, tk.size() - 1)]; }
  bool isOp(const char* s, int k = 0) { return peek(k).k == Tok::OP && peek(k).s == s; }
  bool isId(const char* s, int k = 0) { return peek(k).k == Tok::ID && peek(k).s == s; }
  bool fail(const std::string& m, const Tok* t = nullptr) {
    if (err.empty()) {
      const Tok& x = t ? *t : peek();
      err = std::to_string(x.line) + ":" + std::to_string(x.col) + ": " + m;
    }
    return false;
  }
  bool expect(const char* s) {
    if (!isOp(s)) return fail(std::string("expected '") + s + "'" + (peek().s.empty() ? "" : " before '" + peek().s + "'"));
    p++;
    return true;
  }
  static Type typeOf(const std::string& s) {
    static const std::map<std::string, Type> m = {{"void", T_VOID}, {"bool", T_BOOL}, {"int", T_INT}, {"float", T_FLOAT}, {"vec2", T_VEC2},
                                                   {"vec3", T_VEC3}, {"vec4", T_VEC4}, {"mat2", T_MAT2}, {"mat3", T_MAT3}, {"mat4", T_MAT4}};
    auto it = m.find(s);
    return it == m.end() ? T_ERROR : it->second;
  }
  bool isTypeTok(int k = 0) { return peek(k).k == Tok::ID && typeOf(peek(k).s) != T_ERROR; }
  void skipQualifiers() {
    while (peek().k == Tok::ID && (peek().s == "const" || peek().s == "in" || peek().s == "highp" || peek().s == "mediump" || peek().s == "lowp")) p++;
  }

  // ---------------------------------------------------------- program
  bool parseProgram() {
    // pass 1: function signatures (bodies skipped) and globals, so calls may precede definitions
    size_t save = p;
    while (peek().k != Tok::END) {
      skipQualifiers();
      if (!isTypeTok()) return fail("expected a declaration (types: float, int, bool, vec2-4, mat2-4, void)");
      Type t = typeOf(peek().s);
      if (peek(1).k == Tok::ID && isOp("(", 2)) {
        Func f;
        f.ret = t;
        f.name = peek(1).s;
        p += 3;
        if (isId("void") && isOp(")", 1)) p++;
        while (!isOp(")")) {
          skipQualifiers();
          if (isId("out") || isId("inout")) return fail("out / inout parameters are not supported");
          if (!isTypeTok()) return fail("expected a parameter type");
          f.params.push_back(typeOf(peek().s));
          p++;
          if (peek().k == Tok::ID) p++;
          if (isOp("[")) return fail("array parameters are not supported");
          if (isOp(",")) p++;
          else if (!isOp(")")) return fail("expected ',' or ')'");
        }
        p++;
        int idx = findExact(f.name, f.params);
        if (isOp(";")) { p++; if (idx < 0) prog->funcs.push_back(std::move(f)); continue; }
        if (!isOp("{")) return fail("expected '{'");
        if (idx >= 0 && prog->funcs[idx].defined) return fail("function '" + f.name + "' defined twice");
        f.bodyTok = p;
        f.defined = true;
        if (idx >= 0) { prog->funcs[idx].bodyTok = p; prog->funcs[idx].defined = true; }
        else prog->funcs.push_back(std::move(f));
        int depth = 0;
        do {
          if (isOp("{")) depth++;
          else if (isOp("}")) depth--;
          if (peek().k == Tok::END) return fail("missing '}'");
          p++;
        } while (depth > 0);
      } else {
        // global variables: skip to ';' (parsed in pass 2, in order)
        while (!isOp(";")) { if (peek().k == Tok::END) return fail("expected ';'"); p++; }
        p++;
      }
    }
    // pass 2: globals (with initialisers) and bodies, in source order
    p = save;
    while (peek().k != Tok::END) {
      skipQualifiers();
      Type t = typeOf(peek().s);
      if (peek(1).k == Tok::ID && isOp("(", 2)) {
        std::string name = peek(1).s;
        // find the function by its body position
        size_t q = p;
        while (!isOp("{") && !isOp(";")) p++;
        if (isOp(";")) { p++; continue; }
        size_t body = p;
        Func* f = nullptr;
        for (auto& fn : prog->funcs) if (fn.bodyTok == body) f = &fn;
        if (!f) return fail("internal: function body lost");
        // parameter names
        p = q + 3;
        cur = f;
        f->nlocals = 0;
        scopes.assign(1, {});
        if (isId("void") && isOp(")", 1)) p++;
        while (!isOp(")")) {
          skipQualifiers();
          Type pt = typeOf(peek().s);
          p++;
          int slot = f->nlocals++;
          f->paramSlots.push_back(slot);
          if (peek().k == Tok::ID) { scopes.back()[peek().s] = {slot, pt}; p++; }
          if (isOp(",")) p++;
        }
        p++;
        f->body = parseBlock();
        if (!f->body) return false;
        cur = nullptr;
        (void)t;
        (void)name;
      } else {
        // global declaration list
        p++;
        do {
          if (peek().k != Tok::ID) return fail("expected a name");
          std::string nm = peek().s;
          p++;
          if (isOp("[")) return fail("arrays are not supported");
          int slot = (int)prog->globalTypes.size();
          prog->globalTypes.push_back(t);
          std::unique_ptr<Expr> init;
          if (isOp("=")) {
            p++;
            init = convert(parseAssign(), t);
            if (!init) return false;
          }
          globals[nm] = slot;
          prog->globalInits.push_back({slot, std::move(init)});
        } while (isOp(",") && (p++, true));
        if (!expect(";")) return false;
      }
    }
    for (auto& f : prog->funcs) if (!f.defined) return fail("function '" + f.name + "' is declared but not defined");
    return true;
  }

  int findExact(const std::string& name, const std::vector<Type>& params) {
    for (size_t i = 0; i < prog->funcs.size(); i++)
      if (prog->funcs[i].name == name && prog->funcs[i].params == params) return (int)i;
    return -1;
  }

  // ---------------------------------------------------------- statements
  std::unique_ptr<Stmt> parseBlock() {
    if (!expect("{")) return nullptr;
    auto s = std::make_unique<Stmt>();
    s->k = Stmt::BLOCK;
    scopes.push_back({});
    while (!isOp("}")) {
      if (peek().k == Tok::END) { fail("missing '}'"); return nullptr; }
      auto c = parseStmt();
      if (!c) return nullptr;
      s->b.push_back(std::move(c));
    }
    p++;
    scopes.pop_back();
    return s;
  }

  std::unique_ptr<Stmt> parseStmt() {
    if (isOp("{")) return parseBlock();
    if (isOp(";")) { p++; return std::make_unique<Stmt>(); }
    auto s = std::make_unique<Stmt>();
    if (isId("if")) {
      p++;
      if (!expect("(")) return nullptr;
      s->k = Stmt::IF;
      s->e = convert(parseExpr(), T_BOOL);
      if (!s->e || !expect(")")) return nullptr;
      auto th = scoped([&] { return parseStmt(); });
      if (!th) return nullptr;
      s->b.push_back(std::move(th));
      if (isId("else")) {
        p++;
        auto el = scoped([&] { return parseStmt(); });
        if (!el) return nullptr;
        s->b.push_back(std::move(el));
      }
      return s;
    }
    if (isId("for")) {
      p++;
      if (!expect("(")) return nullptr;
      s->k = Stmt::FOR;
      scopes.push_back({});
      std::unique_ptr<Stmt> init = isOp(";") ? (p++, std::make_unique<Stmt>()) : parseSimple(true);
      if (!init) return nullptr;
      if (!isOp(";")) { s->e = convert(parseExpr(), T_BOOL); if (!s->e) return nullptr; }
      if (!expect(";")) return nullptr;
      if (!isOp(")")) { s->e2 = parseExpr(); if (!s->e2) return nullptr; }
      if (!expect(")")) return nullptr;
      loopDepth++;
      auto body = scoped([&] { return parseStmt(); });
      loopDepth--;
      scopes.pop_back();
      if (!body) return nullptr;
      s->b.push_back(std::move(init));
      s->b.push_back(std::move(body));
      return s;
    }
    if (isId("while")) {
      p++;
      if (!expect("(")) return nullptr;
      s->k = Stmt::WHILE;
      s->e = convert(parseExpr(), T_BOOL);
      if (!s->e || !expect(")")) return nullptr;
      loopDepth++;
      auto body = scoped([&] { return parseStmt(); });
      loopDepth--;
      if (!body) return nullptr;
      s->b.push_back(std::move(body));
      return s;
    }
    if (isId("do")) {
      p++;
      s->k = Stmt::DO;
      loopDepth++;
      auto body = scoped([&] { return parseStmt(); });
      loopDepth--;
      if (!body) return nullptr;
      if (!isId("while")) { fail("expected 'while'"); return nullptr; }
      p++;
      if (!expect("(")) return nullptr;
      s->e = convert(parseExpr(), T_BOOL);
      if (!s->e || !expect(")") || !expect(";")) return nullptr;
      s->b.push_back(std::move(body));
      return s;
    }
    if (isId("return")) {
      Tok at = peek();
      p++;
      s->k = Stmt::RET;
      if (!isOp(";")) {
        if (cur->ret == T_VOID) { fail("a void function cannot return a value", &at); return nullptr; }
        s->e = convert(parseExpr(), cur->ret);
        if (!s->e) return nullptr;
      } else if (cur->ret != T_VOID) { fail(std::string("missing return value (") + typeName(cur->ret) + ")", &at); return nullptr; }
      if (!expect(";")) return nullptr;
      return s;
    }
    if (isId("break") || isId("continue")) {
      if (!loopDepth) { fail("'" + peek().s + "' outside a loop"); return nullptr; }
      s->k = isId("break") ? Stmt::BRK : Stmt::CONT;
      p++;
      if (!expect(";")) return nullptr;
      return s;
    }
    if (isId("discard")) { fail("'discard' is not allowed here"); return nullptr; }
    auto st = parseSimple(false);
    return st;
  }

  template <class F> std::unique_ptr<Stmt> scoped(F f) {
    scopes.push_back({});
    auto r = f();
    scopes.pop_back();
    return r;
  }

  // declaration or expression statement, ending with ';' (consumed)
  std::unique_ptr<Stmt> parseSimple(bool inFor) {
    (void)inFor;
    skipQualifiers();
    auto s = std::make_unique<Stmt>();
    if (isTypeTok() && peek(1).k == Tok::ID) {
      Type t = typeOf(peek().s);
      if (t == T_VOID) { fail("variables cannot be void"); return nullptr; }
      p++;
      s->k = Stmt::DECL;
      s->declT = t;
      do {
        if (peek().k != Tok::ID) { fail("expected a name"); return nullptr; }
        std::string nm = peek().s;
        p++;
        if (isOp("[")) { fail("arrays are not supported"); return nullptr; }
        std::unique_ptr<Expr> init;
        if (isOp("=")) {
          p++;
          init = convert(parseAssign(), t);
          if (!init) return nullptr;
        }
        int slot = cur->nlocals++;
        scopes.back()[nm] = {slot, t};  // (visible after its initialiser, as in GLSL)
        s->decls.push_back({slot, std::move(init)});
      } while (isOp(",") && (p++, true));
      if (!expect(";")) return nullptr;
      return s;
    }
    s->k = Stmt::EXPR;
    s->e = parseExpr();
    if (!s->e || !expect(";")) return nullptr;
    return s;
  }

  // ---------------------------------------------------------- expressions
  std::unique_ptr<Expr> mk(Expr::K k, Type t, const Tok& at) {
    auto e = std::make_unique<Expr>();
    e->k = k;
    e->t = t;
    e->line = at.line;
    e->col = at.col;
    return e;
  }

  // implicit conversion (int -> float, int -> vec splat is NOT implicit in GLSL)
  std::unique_ptr<Expr> convert(std::unique_ptr<Expr> e, Type to) {
    if (!e) return nullptr;
    if (e->t == to) return e;
    if (e->t == T_INT && to == T_FLOAT) {
      Tok at;
      at.line = e->line;
      at.col = e->col;
      auto c = mk(Expr::CTOR, T_FLOAT, at);
      c->a.push_back(std::move(e));
      return c;
    }
    Tok at;
    at.line = e->line;
    at.col = e->col;
    fail(std::string("cannot convert ") + typeName(e->t) + " to " + typeName(to), &at);
    return nullptr;
  }

  std::unique_ptr<Expr> parseExpr() {
    auto e = parseAssign();
    while (e && isOp(",")) {  // comma operator (for loop steps)
      Tok at = peek();
      p++;
      auto r = parseAssign();
      if (!r) return nullptr;
      auto c = mk(Expr::BIN, r->t, at);
      c->op = ',';
      c->a.push_back(std::move(e));
      c->a.push_back(std::move(r));
      e = std::move(c);
    }
    return e;
  }

  static bool isLvalue(const Expr* e) {
    if (e->k == Expr::LVAR || e->k == Expr::GVAR) return true;
    if (e->k == Expr::SWZ || e->k == Expr::INDEX) return isLvalue(e->a[0].get());
    return false;
  }

  std::unique_ptr<Expr> parseAssign() {
    auto l = parseTernary();
    if (!l) return nullptr;
    static const char* aops[] = {"=", "+=", "-=", "*=", "/="};
    for (auto o : aops)
      if (isOp(o)) {
        Tok at = peek();
        if (!isLvalue(l.get())) { fail("cannot assign to this expression", &at); return nullptr; }
        p++;
        auto r = parseAssign();
        if (!r) return nullptr;
        if (o[0] != '=') {
          auto b = binary(o[0], cloneLv(l.get()), std::move(r), at);
          if (!b) return nullptr;
          r = std::move(b);
        }
        r = convert(std::move(r), l->t);
        if (!r) return nullptr;
        auto e = mk(Expr::ASSIGN, l->t, at);
        e->a.push_back(std::move(l));
        e->a.push_back(std::move(r));
        return e;
      }
    return l;
  }

  // a copy of an lvalue expression (compound assignment reads it again)
  std::unique_ptr<Expr> cloneLv(const Expr* e) {
    auto c = std::make_unique<Expr>();
    c->k = e->k; c->t = e->t; c->line = e->line; c->col = e->col; c->slot = e->slot; c->nsw = e->nsw;
    for (int k = 0; k < 4; k++) c->sw[k] = e->sw[k];
    c->num = e->num; c->op = e->op; c->fn = e->fn; c->delta = e->delta;
    for (auto& a : e->a) c->a.push_back(cloneLv(a.get()));
    return c;
  }

  std::unique_ptr<Expr> parseTernary() {
    auto c = parseOr();
    if (!c || !isOp("?")) return c;
    Tok at = peek();
    p++;
    c = convert(std::move(c), T_BOOL);
    if (!c) return nullptr;
    auto a = parseAssign();
    if (!a || !expect(":")) return nullptr;
    auto b = parseAssign();
    if (!b) return nullptr;
    if (a->t != b->t) {
      if (a->t == T_INT && b->t == T_FLOAT) a = convert(std::move(a), T_FLOAT);
      else if (b->t == T_INT && a->t == T_FLOAT) b = convert(std::move(b), T_FLOAT);
      else { fail("the two branches of ?: have different types", &at); return nullptr; }
    }
    auto e = mk(Expr::TERN, a->t, at);
    e->a.push_back(std::move(c));
    e->a.push_back(std::move(a));
    e->a.push_back(std::move(b));
    return e;
  }

  std::unique_ptr<Expr> parseOr() {
    auto l = parseAnd();
    while (l && (isOp("||") || isOp("^^"))) {
      Tok at = peek();
      bool x = isOp("^^");
      p++;
      auto r = parseAnd();
      l = convert(std::move(l), T_BOOL);
      r = convert(std::move(r), T_BOOL);
      if (!l || !r) return nullptr;
      auto e = mk(x ? Expr::BIN : Expr::OR, T_BOOL, at);
      e->op = 'n';  // (^^ = !=)
      e->a.push_back(std::move(l));
      e->a.push_back(std::move(r));
      l = std::move(e);
    }
    return l;
  }
  std::unique_ptr<Expr> parseAnd() {
    auto l = parseBits(0);
    while (l && isOp("&&")) {
      Tok at = peek();
      p++;
      auto r = parseBits(0);
      l = convert(std::move(l), T_BOOL);
      r = convert(std::move(r), T_BOOL);
      if (!l || !r) return nullptr;
      auto e = mk(Expr::AND, T_BOOL, at);
      e->a.push_back(std::move(l));
      e->a.push_back(std::move(r));
      l = std::move(e);
    }
    return l;
  }
  // integer bit operations: | (level 0), ^ (1), & (2), then equality
  std::unique_ptr<Expr> parseBits(int level) {
    if (level == 3) return parseEq();
    static const char* ops[3] = {"|", "^", "&"};
    auto l = parseBits(level + 1);
    while (l && isOp(ops[level])) {
      Tok at = peek();
      p++;
      auto r = parseBits(level + 1);
      l = binary(ops[level][0], std::move(l), std::move(r), at);
    }
    return l;
  }
  std::unique_ptr<Expr> parseEq() {
    auto l = parseRel();
    while (l && (isOp("==") || isOp("!="))) {
      Tok at = peek();
      char op = isOp("==") ? 'e' : 'n';
      p++;
      auto r = parseRel();
      l = binary(op, std::move(l), std::move(r), at);
    }
    return l;
  }
  std::unique_ptr<Expr> parseRel() {
    auto l = parseShift();
    while (l && (isOp("<") || isOp(">") || isOp("<=") || isOp(">="))) {
      Tok at = peek();
      char op = isOp("<") ? '<' : isOp(">") ? '>' : isOp("<=") ? 'l' : 'g';
      p++;
      auto r = parseShift();
      l = binary(op, std::move(l), std::move(r), at);
    }
    return l;
  }
  std::unique_ptr<Expr> parseShift() {
    auto l = parseAdd();
    while (l && (isOp("<<") || isOp(">>"))) {
      Tok at = peek();
      char op = isOp("<<") ? 'L' : 'R';
      p++;
      auto r = parseAdd();
      l = binary(op, std::move(l), std::move(r), at);
    }
    return l;
  }
  std::unique_ptr<Expr> parseAdd() {
    auto l = parseMul();
    while (l && (isOp("+") || isOp("-"))) {
      Tok at = peek();
      char op = peek().s[0];
      p++;
      auto r = parseMul();
      l = binary(op, std::move(l), std::move(r), at);
    }
    return l;
  }
  std::unique_ptr<Expr> parseMul() {
    auto l = parseUnary();
    while (l && (isOp("*") || isOp("/") || isOp("%"))) {
      Tok at = peek();
      char op = peek().s[0];
      p++;
      auto r = parseUnary();
      l = binary(op, std::move(l), std::move(r), at);
    }
    return l;
  }

  // result type of a binary operation (with the operands converted as needed)
  std::unique_ptr<Expr> binary(char op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r, const Tok& at) {
    if (!l || !r) return nullptr;
    Type a = l->t, b = r->t;
    auto bad = [&]() -> std::unique_ptr<Expr> {
      fail(std::string("operator '") + (op == 'l' ? "<=" : op == 'g' ? ">=" : op == 'e' ? "==" : op == 'n' ? "!=" : std::string(1, op)) +
               "' cannot combine " + typeName(a) + " and " + typeName(b), &at);
      return nullptr;
    };
    Type res = T_ERROR;
    if (op == 'e' || op == 'n') {
      if (a == T_INT && b == T_FLOAT) { l = convert(std::move(l), T_FLOAT); a = T_FLOAT; }
      if (b == T_INT && a == T_FLOAT) { r = convert(std::move(r), T_FLOAT); b = T_FLOAT; }
      if (a != b) return bad();
      res = T_BOOL;
    } else if (op == '<' || op == '>' || op == 'l' || op == 'g') {
      if (!isNum(a) || !isNum(b)) return bad();
      if (a != b) { l = convert(std::move(l), T_FLOAT); r = convert(std::move(r), T_FLOAT); }
      res = T_BOOL;
    } else if (op == '%' || op == '&' || op == '|' || op == '^' || op == 'L' || op == 'R') {
      if (a != T_INT || b != T_INT) return bad();
      res = T_INT;
    } else {
      if (a == T_BOOL || b == T_BOOL) return bad();
      // int operands next to float-like ones become float
      if (a == T_INT && isFloaty(b)) { l = convert(std::move(l), T_FLOAT); a = T_FLOAT; }
      if (b == T_INT && isFloaty(a)) { r = convert(std::move(r), T_FLOAT); b = T_FLOAT; }
      if (a == b && !isMat(a)) res = a;
      else if (isScalar(a) && (isVec(b) || isMat(b))) res = b;
      else if (isScalar(b) && (isVec(a) || isMat(a))) res = a;
      else if (isMat(a) && a == b) res = a;  // (* = matrix product, others component-wise)
      else if (op == '*' && isMat(a) && isVec(b) && matN(a) == vecN(b)) res = b;
      else if (op == '*' && isVec(a) && isMat(b) && matN(b) == vecN(a)) res = a;
      else return bad();
    }
    auto e = mk(Expr::BIN, res, at);
    e->op = op;
    e->a.push_back(std::move(l));
    e->a.push_back(std::move(r));
    return e;
  }

  std::unique_ptr<Expr> parseUnary() {
    Tok at = peek();
    if (isOp("-") || isOp("+")) {
      bool neg = isOp("-");
      p++;
      auto e = parseUnary();
      if (!e) return nullptr;
      if (!isNum(e->t) && !isVec(e->t) && !isMat(e->t)) { fail("cannot negate this", &at); return nullptr; }
      if (!neg) return e;
      auto n = mk(Expr::NEG, e->t, at);
      n->a.push_back(std::move(e));
      return n;
    }
    if (isOp("!")) {
      p++;
      auto e = convert(parseUnary(), T_BOOL);
      if (!e) return nullptr;
      auto n = mk(Expr::NOT, T_BOOL, at);
      n->a.push_back(std::move(e));
      return n;
    }
    if (isOp("++") || isOp("--")) {
      int d = isOp("++") ? 1 : -1;
      p++;
      auto e = parseUnary();
      if (!e) return nullptr;
      if (!isLvalue(e.get()) || !isNum(e->t)) { fail("++ / -- need a number variable", &at); return nullptr; }
      auto n = mk(Expr::PREINC, e->t, at);
      n->delta = d;
      n->a.push_back(std::move(e));
      return n;
    }
    return parsePostfix();
  }

  std::unique_ptr<Expr> parsePostfix() {
    auto e = parsePrimary();
    while (e) {
      Tok at = peek();
      if (isOp(".")) {
        p++;
        if (peek().k != Tok::ID) { fail("expected a component name"); return nullptr; }
        std::string s = peek().s;
        p++;
        if (!isVec(e->t)) { fail(std::string("components (.") + s + ") need a vector, not " + typeName(e->t), &at); return nullptr; }
        if (s.size() > 4) { fail("too many components", &at); return nullptr; }
        auto w = mk(Expr::SWZ, vecType((int)s.size()), at);
        w->nsw = (int)s.size();
        for (size_t k = 0; k < s.size(); k++) {
          int c = -1;
          const char* sets[3] = {"xyzw", "rgba", "stpq"};
          for (auto set : sets) { const char* f = std::strchr(set, s[k]); if (f && *f) c = (int)(f - set); }
          if (c < 0 || c >= vecN(e->t)) { fail(std::string("no component '") + s[k] + "' in " + typeName(e->t), &at); return nullptr; }
          w->sw[k] = c;
        }
        w->a.push_back(std::move(e));
        e = std::move(w);
      } else if (isOp("[")) {
        p++;
        auto ix = parseExpr();
        if (!ix || !expect("]")) return nullptr;
        if (ix->t != T_INT) { fail("an index must be an int", &at); return nullptr; }
        Type rt = isVec(e->t) ? T_FLOAT : isMat(e->t) ? vecType(matN(e->t)) : T_ERROR;
        if (rt == T_ERROR) { fail("[] needs a vector or a matrix", &at); return nullptr; }
        auto x = mk(Expr::INDEX, rt, at);
        x->a.push_back(std::move(e));
        x->a.push_back(std::move(ix));
        e = std::move(x);
      } else if (isOp("++") || isOp("--")) {
        if (!isLvalue(e.get()) || !isNum(e->t)) { fail("++ / -- need a number variable", &at); return nullptr; }
        auto n = mk(Expr::POSTINC, e->t, at);
        n->delta = isOp("++") ? 1 : -1;
        p++;
        n->a.push_back(std::move(e));
        e = std::move(n);
      } else break;
    }
    return e;
  }

  std::unique_ptr<Expr> parsePrimary() {
    Tok at = peek();
    if (at.k == Tok::NUM) {
      p++;
      auto e = mk(Expr::NUM, at.isInt ? T_INT : T_FLOAT, at);
      e->num = at.isInt ? Value::i((long)at.num) : Value::f(at.num);
      return e;
    }
    if (isOp("(")) {
      p++;
      auto e = parseExpr();
      if (!e || !expect(")")) return nullptr;
      return e;
    }
    if (at.k != Tok::ID) { fail("expected an expression" + (at.s.empty() ? std::string() : " before '" + at.s + "'")); return nullptr; }
    if (at.s == "true" || at.s == "false") {
      p++;
      auto e = mk(Expr::NUM, T_BOOL, at);
      e->num = Value::b(at.s == "true");
      return e;
    }
    if (isOp("(", 1)) return parseCall();
    p++;
    for (int s = (int)scopes.size() - 1; s >= 0; s--) {
      auto it = scopes[s].find(at.s);
      if (it != scopes[s].end()) {
        auto e = mk(Expr::LVAR, it->second.second, at);
        e->slot = it->second.first;
        return e;
      }
    }
    auto g = globals.find(at.s);
    if (g != globals.end()) {
      auto e = mk(Expr::GVAR, prog->globalTypes[g->second], at);
      e->slot = g->second;
      return e;
    }
    fail("unknown name '" + at.s + "'", &at);
    return nullptr;
  }

  std::unique_ptr<Expr> parseCall() {
    Tok at = peek();
    std::string name = at.s;
    p += 2;
    std::vector<std::unique_ptr<Expr>> args;
    while (!isOp(")")) {
      auto a = parseAssign();
      if (!a) return nullptr;
      args.push_back(std::move(a));
      if (isOp(",")) p++;
      else if (!isOp(")")) { fail("expected ',' or ')'"); return nullptr; }
    }
    p++;
    Type ct = typeOf(name);
    if (ct != T_ERROR && ct != T_VOID) return constructor(ct, std::move(args), at);
    // user functions (exact, then with int -> float conversions)
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
      for (size_t i = 0; i < prog->funcs.size(); i++) {
        auto& f = prog->funcs[i];
        if (f.name != name || f.params.size() != args.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < args.size(); k++)
          if (!(args[k]->t == f.params[k] || (pass == 1 && args[k]->t == T_INT && f.params[k] == T_FLOAT))) ok = false;
        if (ok) { best = (int)i; break; }
      }
    if (best >= 0) {
      auto e = mk(Expr::CALL, prog->funcs[best].ret, at);
      e->fn = best;
      for (size_t k = 0; k < args.size(); k++) {
        auto c = convert(std::move(args[k]), prog->funcs[best].params[k]);
        if (!c) return nullptr;
        e->a.push_back(std::move(c));
      }
      return e;
    }
    return builtin(name, std::move(args), at);
  }

  std::unique_ptr<Expr> constructor(Type t, std::vector<std::unique_ptr<Expr>> args, const Tok& at) {
    auto e = mk(Expr::CTOR, t, at);
    if (args.empty()) { fail(std::string(typeName(t)) + "() needs arguments", &at); return nullptr; }
    int comps = 0;
    for (auto& a : args) {
      if (a->t == T_VOID) { fail("a void value cannot be used", &at); return nullptr; }
      comps += typeSize(a->t);
    }
    int need = typeSize(t);
    bool single = args.size() == 1;
    if (!(single && (isScalar(args[0]->t) || (isMat(t) && isMat(args[0]->t)))) && comps < need) {
      fail(std::string("not enough values for ") + typeName(t), &at);
      return nullptr;
    }
    if (!single && comps > need && comps - typeSize(args.back()->t) >= need) {
      fail(std::string("too many values for ") + typeName(t), &at);
      return nullptr;
    }
    for (auto& a : args) e->a.push_back(std::move(a));
    return e;
  }

  std::unique_ptr<Expr> builtin(const std::string& name, std::vector<std::unique_ptr<Expr>> args, const Tok& at) {
    struct B { const char* n; Builtin b; int nargs; };
    static const B table[] = {
        {"abs", B_ABS, 1}, {"sign", B_SIGN, 1}, {"floor", B_FLOOR, 1}, {"ceil", B_CEIL, 1}, {"fract", B_FRACT, 1}, {"sqrt", B_SQRT, 1},
        {"inversesqrt", B_ISQRT, 1}, {"exp", B_EXP, 1}, {"exp2", B_EXP2, 1}, {"log", B_LOG, 1}, {"log2", B_LOG2, 1}, {"sin", B_SIN, 1},
        {"cos", B_COS, 1}, {"tan", B_TAN, 1}, {"asin", B_ASIN, 1}, {"acos", B_ACOS, 1}, {"atan", B_ATAN, 1}, {"sinh", B_SINH, 1},
        {"cosh", B_COSH, 1}, {"tanh", B_TANH, 1}, {"radians", B_RAD, 1}, {"degrees", B_DEG, 1}, {"round", B_ROUND, 1}, {"trunc", B_TRUNC, 1},
        {"pow", B_POW, 2}, {"mod", B_MOD, 2}, {"min", B_MIN, 2}, {"max", B_MAX, 2}, {"step", B_STEP, 2}, {"atan", B_ATAN2, 2},
        {"clamp", B_CLAMP, 3}, {"mix", B_MIX, 3}, {"smoothstep", B_SMOOTH, 3}, {"length", B_LENGTH, 1}, {"distance", B_DISTANCE, 2},
        {"dot", B_DOT, 2}, {"cross", B_CROSS, 2}, {"normalize", B_NORMALIZE, 1}, {"transpose", B_TRANSPOSE, 1}};
    const B* b = nullptr;
    bool known = false;
    for (auto& x : table)
      if (name == x.n) { known = true; if ((size_t)x.nargs == args.size()) b = &x; }
    if (!b) {
      fail(known ? "wrong number of arguments for '" + name + "'" : "unknown function '" + name + "'", &at);
      return nullptr;
    }
    Type res = T_ERROR;
    Type t0 = args[0]->t;
    auto allowInt = [&](Builtin bb) { return bb == B_ABS || bb == B_SIGN || bb == B_MIN || bb == B_MAX || bb == B_CLAMP; };
    // integer forms (all arguments int)
    bool allInt = true;
    for (auto& a : args) if (a->t != T_INT) allInt = false;
    if (allInt && allowInt(b->b)) res = T_INT;
    else {
      // float forms: ints become float
      for (auto& a : args) if (a->t == T_INT) { a = convert(std::move(a), T_FLOAT); if (!a) return nullptr; }
      t0 = args[0]->t;
      auto gen = [&](Type t) { return t == T_FLOAT || isVec(t); };
      switch (b->b) {
        case B_LENGTH: if (gen(t0)) res = T_FLOAT; break;
        case B_NORMALIZE: if (gen(t0)) res = t0; break;
        case B_DISTANCE: case B_DOT: if (gen(t0) && args[1]->t == t0) res = T_FLOAT; break;
        case B_CROSS: if (t0 == T_VEC3 && args[1]->t == T_VEC3) res = T_VEC3; break;
        case B_TRANSPOSE: if (isMat(t0)) res = t0; break;
        case B_STEP: if (gen(args[1]->t) && (args[0]->t == args[1]->t || args[0]->t == T_FLOAT)) res = args[1]->t; break;
        case B_SMOOTH:
          if (gen(args[2]->t) && ((args[0]->t == args[2]->t && args[1]->t == args[2]->t) || (args[0]->t == T_FLOAT && args[1]->t == T_FLOAT)))
            res = args[2]->t;
          break;
        case B_POW: case B_ATAN2: if (gen(t0) && args[1]->t == t0) res = t0; break;
        case B_MOD: case B_MIN: case B_MAX: if (gen(t0) && (args[1]->t == t0 || args[1]->t == T_FLOAT)) res = t0; break;
        case B_CLAMP: if (gen(t0) && args[1]->t == args[2]->t && (args[1]->t == t0 || args[1]->t == T_FLOAT)) res = t0; break;
        case B_MIX: if (gen(t0) && args[1]->t == t0 && (args[2]->t == t0 || args[2]->t == T_FLOAT)) res = t0; break;
        default: if (gen(t0)) res = t0; break;  // component-wise one-argument functions
      }
    }
    if (res == T_ERROR) {
      std::string sig = name + "(";
      for (size_t k = 0; k < args.size(); k++) sig += std::string(k ? ", " : "") + typeName(args[k]->t);
      fail("no '" + sig + ")'", &at);
      return nullptr;
    }
    auto e = mk(Expr::BUILTIN, res, at);
    e->fn = b->b;
    for (auto& a : args) e->a.push_back(std::move(a));
    return e;
  }
};

// ================================================================ execution
namespace {
struct Ctx {
  const Program* prog;
  std::vector<Value> globals;
  long steps = 0;
  int depth = 0;
  std::string err;
  bool failed = false;
  void error(const Expr* e, const std::string& m) {
    if (failed) return;
    failed = true;
    err = (e ? std::to_string(e->line) + ":" + std::to_string(e->col) + ": " : std::string()) + m;
  }
};
enum Flow { F_NORMAL, F_BREAK, F_CONT, F_RET };
constexpr long MAX_STEPS = 20000000;

Value eval(const Expr* e, std::vector<Value>& fr, Ctx& c);
Value callFunc(int fi, std::vector<Value>& args, Ctx& c, const Expr* at);

Value convertTo(const Value& v, Type t) {
  Value r;
  r.t = t;
  if (t == T_INT) r.v[0] = std::trunc(v.v[0]);
  else if (t == T_BOOL) r.v[0] = v.v[0] != 0 ? 1 : 0;
  else for (int k = 0; k < typeSize(v.t) && k < 16; k++) r.v[k] = v.v[k];
  return r;
}

// a value's components, appended (constructors)
void flatten(const Value& v, std::vector<double>& out) { for (int k = 0; k < typeSize(v.t); k++) out.push_back(v.v[k]); }

Value construct(Type t, const std::vector<Value>& args) {
  Value r;
  r.t = t;
  int n = typeSize(t);
  if (isScalar(t)) {
    double x = args[0].v[0];
    r.v[0] = t == T_INT ? std::trunc(x) : t == T_BOOL ? (x != 0 ? 1 : 0) : x;
    return r;
  }
  if (args.size() == 1 && isScalar(args[0].t)) {
    double x = args[0].v[0];
    if (isVec(t)) for (int k = 0; k < n; k++) r.v[k] = x;
    else { int m = matN(t); for (int k = 0; k < m; k++) r.v[k * m + k] = x; }  // diagonal
    return r;
  }
  if (args.size() == 1 && isMat(t) && isMat(args[0].t)) {
    int m = matN(t), s = matN(args[0].t);
    for (int col = 0; col < m; col++)
      for (int row = 0; row < m; row++)
        r.v[col * m + row] = (col < s && row < s) ? args[0].v[col * s + row] : (col == row ? 1 : 0);
    return r;
  }
  std::vector<double> comps;
  for (auto& a : args) flatten(a, comps);
  for (int k = 0; k < n && k < (int)comps.size(); k++) r.v[k] = comps[k];
  return r;
}

double comp(const Value& v, int k) { return typeSize(v.t) == 1 ? v.v[0] : v.v[k]; }

Value binop(int op, const Value& a, const Value& b, Type rt, Ctx& c, const Expr* at) {
  Value r;
  r.t = rt;
  if (op == 'e' || op == 'n') {
    bool eq = true;
    for (int k = 0; k < typeSize(a.t); k++) if (a.v[k] != b.v[k]) eq = false;
    return Value::b(op == 'e' ? eq : !eq);
  }
  if (op == '<') return Value::b(a.v[0] < b.v[0]);
  if (op == '>') return Value::b(a.v[0] > b.v[0]);
  if (op == 'l') return Value::b(a.v[0] <= b.v[0]);
  if (op == 'g') return Value::b(a.v[0] >= b.v[0]);
  if (op == '*' && isMat(a.t) && isMat(b.t)) {  // matrix product (column-major)
    int m = matN(a.t);
    for (int col = 0; col < m; col++)
      for (int row = 0; row < m; row++) {
        double s = 0;
        for (int k = 0; k < m; k++) s += a.v[k * m + row] * b.v[col * m + k];
        r.v[col * m + row] = s;
      }
    return r;
  }
  if (op == '*' && isMat(a.t) && isVec(b.t)) {
    int m = matN(a.t);
    for (int row = 0; row < m; row++) { double s = 0; for (int k = 0; k < m; k++) s += a.v[k * m + row] * b.v[k]; r.v[row] = s; }
    return r;
  }
  if (op == '*' && isVec(a.t) && isMat(b.t)) {
    int m = matN(b.t);
    for (int col = 0; col < m; col++) { double s = 0; for (int k = 0; k < m; k++) s += a.v[k] * b.v[col * m + k]; r.v[col] = s; }
    return r;
  }
  int n = typeSize(rt);
  for (int k = 0; k < n; k++) {
    double x = comp(a, k), y = comp(b, k), z = 0;
    switch (op) {
      case '+': z = x + y; break;
      case '-': z = x - y; break;
      case '*': z = x * y; break;
      case '/':
        if (rt == T_INT) { if (y == 0) { c.error(at, "integer division by zero"); return r; } z = std::trunc(x / y); }
        else z = x / y;
        break;
      case '%': if (y == 0) { c.error(at, "integer modulo by zero"); return r; } z = std::fmod(x, y); break;
      case '&': z = (double)((long long)x & (long long)y); break;
      case '|': z = (double)((long long)x | (long long)y); break;
      case '^': z = (double)((long long)x ^ (long long)y); break;
      case 'L': z = (double)((long long)x << (int)y); break;
      case 'R': z = (double)((long long)x >> (int)y); break;
      case ',': return b;
    }
    r.v[k] = z;
  }
  return r;
}

double smoothstepd(double a, double b, double x) { double t = std::clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3 - 2 * t); }

Value builtinCall(int b, const std::vector<Value>& a, Type rt) {
  Value r;
  r.t = rt;
  int n = typeSize(rt);
  auto per = [&](auto f) { for (int k = 0; k < n; k++) r.v[k] = f(k); };
  switch (b) {
    case B_ABS: per([&](int k) { return std::abs(a[0].v[k]); }); break;
    case B_SIGN: per([&](int k) { double x = a[0].v[k]; return (double)((x > 0) - (x < 0)); }); break;
    case B_FLOOR: per([&](int k) { return std::floor(a[0].v[k]); }); break;
    case B_CEIL: per([&](int k) { return std::ceil(a[0].v[k]); }); break;
    case B_FRACT: per([&](int k) { return a[0].v[k] - std::floor(a[0].v[k]); }); break;
    case B_SQRT: per([&](int k) { return std::sqrt(a[0].v[k]); }); break;
    case B_ISQRT: per([&](int k) { return 1 / std::sqrt(a[0].v[k]); }); break;
    case B_EXP: per([&](int k) { return std::exp(a[0].v[k]); }); break;
    case B_EXP2: per([&](int k) { return std::exp2(a[0].v[k]); }); break;
    case B_LOG: per([&](int k) { return std::log(a[0].v[k]); }); break;
    case B_LOG2: per([&](int k) { return std::log2(a[0].v[k]); }); break;
    case B_SIN: per([&](int k) { return std::sin(a[0].v[k]); }); break;
    case B_COS: per([&](int k) { return std::cos(a[0].v[k]); }); break;
    case B_TAN: per([&](int k) { return std::tan(a[0].v[k]); }); break;
    case B_ASIN: per([&](int k) { return std::asin(a[0].v[k]); }); break;
    case B_ACOS: per([&](int k) { return std::acos(a[0].v[k]); }); break;
    case B_ATAN: per([&](int k) { return std::atan(a[0].v[k]); }); break;
    case B_SINH: per([&](int k) { return std::sinh(a[0].v[k]); }); break;
    case B_COSH: per([&](int k) { return std::cosh(a[0].v[k]); }); break;
    case B_TANH: per([&](int k) { return std::tanh(a[0].v[k]); }); break;
    case B_RAD: per([&](int k) { return a[0].v[k] * 3.14159265358979323846 / 180; }); break;
    case B_DEG: per([&](int k) { return a[0].v[k] * 180 / 3.14159265358979323846; }); break;
    case B_ROUND: per([&](int k) { return std::round(a[0].v[k]); }); break;
    case B_TRUNC: per([&](int k) { return std::trunc(a[0].v[k]); }); break;
    case B_POW: per([&](int k) { return std::pow(a[0].v[k], a[1].v[k]); }); break;
    case B_ATAN2: per([&](int k) { return std::atan2(a[0].v[k], a[1].v[k]); }); break;
    case B_MOD: per([&](int k) { double x = a[0].v[k], y = comp(a[1], k); return x - y * std::floor(x / y); }); break;
    case B_MIN: per([&](int k) { return std::min(a[0].v[k], comp(a[1], k)); }); break;
    case B_MAX: per([&](int k) { return std::max(a[0].v[k], comp(a[1], k)); }); break;
    case B_STEP: per([&](int k) { return a[1].v[k] < comp(a[0], k) ? 0.0 : 1.0; }); break;
    case B_CLAMP: per([&](int k) { return std::min(std::max(a[0].v[k], comp(a[1], k)), comp(a[2], k)); }); break;
    case B_MIX: per([&](int k) { double t = comp(a[2], k); return a[0].v[k] * (1 - t) + a[1].v[k] * t; }); break;
    case B_SMOOTH: per([&](int k) { return smoothstepd(comp(a[0], k), comp(a[1], k), a[2].v[k]); }); break;
    case B_LENGTH: { double s = 0; for (int k = 0; k < typeSize(a[0].t); k++) s += a[0].v[k] * a[0].v[k]; r.v[0] = std::sqrt(s); break; }
    case B_DISTANCE: { double s = 0; for (int k = 0; k < typeSize(a[0].t); k++) { double d = a[0].v[k] - a[1].v[k]; s += d * d; } r.v[0] = std::sqrt(s); break; }
    case B_DOT: { double s = 0; for (int k = 0; k < typeSize(a[0].t); k++) s += a[0].v[k] * a[1].v[k]; r.v[0] = s; break; }
    case B_CROSS:
      r.v[0] = a[0].v[1] * a[1].v[2] - a[0].v[2] * a[1].v[1];
      r.v[1] = a[0].v[2] * a[1].v[0] - a[0].v[0] * a[1].v[2];
      r.v[2] = a[0].v[0] * a[1].v[1] - a[0].v[1] * a[1].v[0];
      break;
    case B_NORMALIZE: {
      double s = 0;
      for (int k = 0; k < n; k++) s += a[0].v[k] * a[0].v[k];
      s = std::sqrt(s);
      per([&](int k) { return a[0].v[k] / s; });
      break;
    }
    case B_TRANSPOSE: { int m = matN(rt); for (int i = 0; i < m; i++) for (int j = 0; j < m; j++) r.v[i * m + j] = a[0].v[j * m + i]; break; }
  }
  if (rt == T_INT) for (int k = 0; k < n; k++) r.v[k] = std::trunc(r.v[k]);
  return r;
}

// an lvalue: the variable it lives in and which of its components it names
struct LRef {
  Value* v = nullptr;
  int idx[16];
  int n = 0;
};
bool lref(const Expr* e, std::vector<Value>& fr, Ctx& c, LRef& out) {
  if (e->k == Expr::LVAR || e->k == Expr::GVAR) {
    out.v = e->k == Expr::LVAR ? &fr[e->slot] : &c.globals[e->slot];
    out.n = typeSize(e->t);
    for (int k = 0; k < out.n; k++) out.idx[k] = k;
    return true;
  }
  LRef base;
  if (!lref(e->a[0].get(), fr, c, base)) return false;
  out.v = base.v;
  if (e->k == Expr::SWZ) {
    out.n = e->nsw;
    for (int k = 0; k < e->nsw; k++) out.idx[k] = base.idx[e->sw[k]];
    return true;
  }
  // INDEX
  Value ix = eval(e->a[1].get(), fr, c);
  int i = (int)ix.v[0];
  Type bt = e->a[0]->t;
  if (isVec(bt)) {
    if (i < 0 || i >= vecN(bt)) { c.error(e, "index out of range"); return false; }
    out.n = 1;
    out.idx[0] = base.idx[i];
  } else {
    int m = matN(bt);
    if (i < 0 || i >= m) { c.error(e, "index out of range"); return false; }
    out.n = m;
    for (int k = 0; k < m; k++) out.idx[k] = base.idx[i * m + k];
  }
  return true;
}
Value readRef(const LRef& r, Type t) {
  Value v;
  v.t = t;
  for (int k = 0; k < r.n; k++) v.v[k] = r.v->v[r.idx[k]];
  return v;
}
void writeRef(const LRef& r, const Value& v) { for (int k = 0; k < r.n; k++) r.v->v[r.idx[k]] = typeSize(v.t) == 1 ? v.v[0] : v.v[k]; }

Value eval(const Expr* e, std::vector<Value>& fr, Ctx& c) {
  if (c.failed) return Value{};
  switch (e->k) {
    case Expr::NUM: return e->num;
    case Expr::LVAR: return fr[e->slot];
    case Expr::GVAR: return c.globals[e->slot];
    case Expr::CTOR: {
      std::vector<Value> args;
      for (auto& a : e->a) args.push_back(eval(a.get(), fr, c));
      return construct(e->t, args);
    }
    case Expr::NEG: {
      Value v = eval(e->a[0].get(), fr, c);
      for (int k = 0; k < typeSize(v.t); k++) v.v[k] = -v.v[k];
      return v;
    }
    case Expr::NOT: return Value::b(eval(e->a[0].get(), fr, c).v[0] == 0);
    case Expr::AND: return Value::b(eval(e->a[0].get(), fr, c).v[0] != 0 && eval(e->a[1].get(), fr, c).v[0] != 0);
    case Expr::OR: return Value::b(eval(e->a[0].get(), fr, c).v[0] != 0 || eval(e->a[1].get(), fr, c).v[0] != 0);
    case Expr::TERN: return eval(e->a[0].get(), fr, c).v[0] != 0 ? eval(e->a[1].get(), fr, c) : eval(e->a[2].get(), fr, c);
    case Expr::BIN: {
      Value a = eval(e->a[0].get(), fr, c), b = eval(e->a[1].get(), fr, c);
      return binop(e->op, a, b, e->t, c, e);
    }
    case Expr::ASSIGN: {
      Value v = eval(e->a[1].get(), fr, c);
      LRef r;
      if (!lref(e->a[0].get(), fr, c, r)) return Value{};
      writeRef(r, v);
      return v;
    }
    case Expr::PREINC: case Expr::POSTINC: {
      LRef r;
      if (!lref(e->a[0].get(), fr, c, r)) return Value{};
      Value old = readRef(r, e->t), nw = old;
      nw.v[0] += e->delta;
      writeRef(r, nw);
      return e->k == Expr::PREINC ? nw : old;
    }
    case Expr::SWZ: {
      Value b = eval(e->a[0].get(), fr, c), v;
      v.t = e->t;
      for (int k = 0; k < e->nsw; k++) v.v[k] = b.v[e->sw[k]];
      return v;
    }
    case Expr::INDEX: {
      Value b = eval(e->a[0].get(), fr, c);
      int i = (int)eval(e->a[1].get(), fr, c).v[0];
      Value v;
      v.t = e->t;
      if (isVec(b.t)) {
        if (i < 0 || i >= vecN(b.t)) { c.error(e, "index out of range"); return v; }
        v.v[0] = b.v[i];
      } else {
        int m = matN(b.t);
        if (i < 0 || i >= m) { c.error(e, "index out of range"); return v; }
        for (int k = 0; k < m; k++) v.v[k] = b.v[i * m + k];
      }
      return v;
    }
    case Expr::BUILTIN: {
      std::vector<Value> args;
      for (auto& a : e->a) args.push_back(eval(a.get(), fr, c));
      return builtinCall(e->fn, args, e->t);
    }
    case Expr::CALL: {
      std::vector<Value> args;
      for (auto& a : e->a) args.push_back(eval(a.get(), fr, c));
      return callFunc(e->fn, args, c, e);
    }
  }
  return Value{};
}

Flow exec(const Stmt* s, std::vector<Value>& fr, Ctx& c, Value& ret) {
  if (c.failed) return F_RET;
  if (++c.steps > MAX_STEPS) { c.error(nullptr, "too many steps (endless loop?)"); return F_RET; }
  switch (s->k) {
    case Stmt::NOP: return F_NORMAL;
    case Stmt::BLOCK:
      for (auto& b : s->b) { Flow f = exec(b.get(), fr, c, ret); if (f != F_NORMAL) return f; }
      return F_NORMAL;
    case Stmt::DECL:
      for (auto& [slot, init] : s->decls) {
        Value v;
        v.t = s->declT;
        if (init) v = eval(init.get(), fr, c);
        fr[slot] = v;
      }
      return F_NORMAL;
    case Stmt::EXPR: eval(s->e.get(), fr, c); return F_NORMAL;
    case Stmt::IF:
      if (eval(s->e.get(), fr, c).v[0] != 0) return exec(s->b[0].get(), fr, c, ret);
      if (s->b.size() > 1) return exec(s->b[1].get(), fr, c, ret);
      return F_NORMAL;
    case Stmt::FOR: {
      Flow f = exec(s->b[0].get(), fr, c, ret);
      if (f == F_RET) return f;
      for (;;) {
        if (c.failed) return F_RET;
        if (s->e && eval(s->e.get(), fr, c).v[0] == 0) break;
        f = exec(s->b[1].get(), fr, c, ret);
        if (f == F_RET) return f;
        if (f == F_BREAK) break;
        if (s->e2) eval(s->e2.get(), fr, c);
        if (++c.steps > MAX_STEPS) { c.error(nullptr, "too many steps (endless loop?)"); return F_RET; }
      }
      return F_NORMAL;
    }
    case Stmt::WHILE:
      while (!c.failed && eval(s->e.get(), fr, c).v[0] != 0) {
        Flow f = exec(s->b[0].get(), fr, c, ret);
        if (f == F_RET) return f;
        if (f == F_BREAK) break;
        if (++c.steps > MAX_STEPS) { c.error(nullptr, "too many steps (endless loop?)"); return F_RET; }
      }
      return F_NORMAL;
    case Stmt::DO:
      do {
        Flow f = exec(s->b[0].get(), fr, c, ret);
        if (f == F_RET) return f;
        if (f == F_BREAK) break;
        if (++c.steps > MAX_STEPS) { c.error(nullptr, "too many steps (endless loop?)"); return F_RET; }
      } while (!c.failed && eval(s->e.get(), fr, c).v[0] != 0);
      return F_NORMAL;
    case Stmt::RET:
      if (s->e) ret = eval(s->e.get(), fr, c);
      return F_RET;
    case Stmt::BRK: return F_BREAK;
    case Stmt::CONT: return F_CONT;
  }
  return F_NORMAL;
}

Value callFunc(int fi, std::vector<Value>& args, Ctx& c, const Expr* at) {
  const Func& f = c.prog->funcs[fi];
  if (++c.depth > 48) { c.error(at, "calls nested too deeply (recursion is not allowed)"); c.depth--; return Value{}; }
  std::vector<Value> fr(std::max(1, f.nlocals));
  for (size_t k = 0; k < f.paramSlots.size() && k < args.size(); k++) fr[f.paramSlots[k]] = args[k];
  Value ret;
  ret.t = f.ret;
  exec(f.body.get(), fr, c, ret);
  c.depth--;
  return ret;
}
}  // namespace

// ================================================================ API
std::shared_ptr<const Program> compile(const std::string& source, std::string& err) {
  Parser ps;
  if (!lex(source, ps.tk, err)) return nullptr;
  auto prog = std::make_shared<Program>();
  ps.prog = prog.get();
  if (!ps.parseProgram()) { err = ps.err; return nullptr; }
  return prog;
}

bool hasFunction(const Program& p, const std::string& name, const std::vector<Type>& params, Type* ret) {
  for (auto& f : p.funcs)
    if (f.name == name && f.params == params) { if (ret) *ret = f.ret; return true; }
  return false;
}

bool call(const Program& p, const std::string& name, const std::vector<Value>& args, Value& out, std::string* err) {
  int fi = -1;
  for (int pass = 0; pass < 2 && fi < 0; pass++)
    for (size_t i = 0; i < p.funcs.size(); i++) {
      auto& f = p.funcs[i];
      if (f.name != name || f.params.size() != args.size()) continue;
      bool ok = true;
      for (size_t k = 0; k < args.size(); k++)
        if (!(args[k].t == f.params[k] || (pass == 1 && args[k].t == T_INT && f.params[k] == T_FLOAT))) ok = false;
      if (ok) { fi = (int)i; break; }
    }
  if (fi < 0) { if (err) *err = "no function '" + name + "' with these arguments"; return false; }
  Ctx c;
  c.prog = &p;
  c.globals.resize(p.globalTypes.size());
  for (size_t g = 0; g < p.globalTypes.size(); g++) c.globals[g].t = p.globalTypes[g];
  std::vector<Value> none;
  for (auto& [slot, init] : p.globalInits)
    if (init) c.globals[slot] = eval(init.get(), none, c);
  std::vector<Value> a;
  for (size_t k = 0; k < args.size(); k++) a.push_back(convertTo(args[k], p.funcs[fi].params[k]));
  out = callFunc(fi, a, c, nullptr);
  if (c.failed) { if (err) *err = c.err; return false; }
  return true;
}

std::shared_ptr<const Program> compileCached(const std::string& source, std::string* err) {
  static std::mutex mu;
  static std::unordered_map<std::string, std::pair<std::shared_ptr<const Program>, std::string>> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(source);
    if (it != cache.end()) { if (err) *err = it->second.second; return it->second.first; }
  }
  std::string e;
  auto p = compile(source, e);
  std::lock_guard<std::mutex> lk(mu);
  if (cache.size() > 512) cache.clear();
  cache[source] = {p, e};
  if (err) *err = e;
  return p;
}

}  // namespace sl
}  // namespace fd
