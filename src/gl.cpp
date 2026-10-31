#include "gl.hpp"

namespace gl {
#define FD_DEF(ret, name, args) PFN_##name name = nullptr;
FD_GL_FUNCS(FD_DEF)
#undef FD_DEF

const char* load(void* (*getProc)(const char*)) {
#define FD_LOAD(ret, name, args)                                     \
  name = reinterpret_cast<PFN_##name>(getProc("gl" #name));          \
  if (!name) return "gl" #name;
  FD_GL_FUNCS(FD_LOAD)
#undef FD_LOAD
  return nullptr;
}
}  // namespace gl
