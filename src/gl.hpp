// Minimal OpenGL 3.3 core loader. Functions live in namespace gl (gl::CreateShader, ...)
// so they never clash with the system libGL symbols. Loaded via glfwGetProcAddress.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define FD_APIENTRY __stdcall
#else
#define FD_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef unsigned int GLbitfield;
typedef std::ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2

#define FD_GL_FUNCS(X)                                                                                  \
  X(GLuint, CreateShader, (GLenum))                                                                     \
  X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                           \
  X(void, CompileShader, (GLuint))                                                                      \
  X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                        \
  X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                       \
  X(void, DeleteShader, (GLuint))                                                                       \
  X(GLuint, CreateProgram, (void))                                                                      \
  X(void, AttachShader, (GLuint, GLuint))                                                               \
  X(void, LinkProgram, (GLuint))                                                                        \
  X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                       \
  X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                      \
  X(void, DeleteProgram, (GLuint))                                                                      \
  X(void, UseProgram, (GLuint))                                                                         \
  X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                 \
  X(void, Uniform1i, (GLint, GLint))                                                                    \
  X(void, Uniform1f, (GLint, GLfloat))                                                                  \
  X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                                         \
  X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                                 \
  X(void, Uniform1fv, (GLint, GLsizei, const GLfloat*))                                                 \
  X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                                \
  X(void, GenVertexArrays, (GLsizei, GLuint*))                                                          \
  X(void, BindVertexArray, (GLuint))                                                                    \
  X(void, GenBuffers, (GLsizei, GLuint*))                                                               \
  X(void, BindBuffer, (GLenum, GLuint))                                                                 \
  X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                        \
  X(void, EnableVertexAttribArray, (GLuint))                                                            \
  X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                \
  X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                         \
  X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                   \
  X(void, Clear, (GLbitfield))                                                                          \
  X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                             \
  X(void, Disable, (GLenum))                                                                            \
  X(void, GenFramebuffers, (GLsizei, GLuint*))                                                          \
  X(void, BindFramebuffer, (GLenum, GLuint))                                                            \
  X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                 \
  X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                \
  X(GLenum, CheckFramebufferStatus, (GLenum))                                                           \
  X(void, GenTextures, (GLsizei, GLuint*))                                                              \
  X(void, DeleteTextures, (GLsizei, const GLuint*))                                                     \
  X(void, BindTexture, (GLenum, GLuint))                                                                \
  X(void, ActiveTexture, (GLenum))                                                                      \
  X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))     \
  X(void, TexParameteri, (GLenum, GLenum, GLint))                                                       \
  X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                          \
  X(void, PixelStorei, (GLenum, GLint))                                                                 \
  X(void, Finish, (void))                                                                               \
  X(const GLubyte*, GetString, (GLenum))

namespace gl {
#define FD_DECL(ret, name, args) typedef ret(FD_APIENTRY* PFN_##name) args; extern PFN_##name name;
FD_GL_FUNCS(FD_DECL)
#undef FD_DECL
/** Loads all functions; returns the name of the first missing one, or nullptr on success. */
const char* load(void* (*getProc)(const char*));
}  // namespace gl
