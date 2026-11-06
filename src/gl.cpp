#include "gl.hpp"

namespace gl {
#define FD_DEF(ret, name, args) PFN_##name name = nullptr;
FD_GL_FUNCS(FD_DEF)
FD_GL_OPTIONAL(FD_DEF)
#undef FD_DEF

const char* load(void* (*getProc)(const char*)) {
#define FD_LOAD(ret, name, args)                                     \
  name = reinterpret_cast<PFN_##name>(getProc("gl" #name));          \
  if (!name) return "gl" #name;
  FD_GL_FUNCS(FD_LOAD)
#undef FD_LOAD
#define FD_LOAD_OPT(ret, name, args) name = reinterpret_cast<PFN_##name>(getProc("gl" #name));
  FD_GL_OPTIONAL(FD_LOAD_OPT)
#undef FD_LOAD_OPT
  return nullptr;
}
}  // namespace gl
