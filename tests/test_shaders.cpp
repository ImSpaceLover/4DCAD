// Shader compile tests: generates the ray-marching shader of every example and test scene in every
// view it supports (Full 4D, Geodesic, curved spaces, the ball overview; simple and path-traced
// lighting) and compiles it with glslang, for the Vulkan (GLSL 4.50 -> SPIR-V) and the OpenGL
// (GLSL 3.30) targets. No GPU needed: a shader the generator gets wrong fails here, not at run time.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include "codegen.hpp"
#include "examples.hpp"
#include "scene.hpp"
#include "shaders.hpp"

using namespace fd;

static int checks = 0, failures = 0;
constexpr int POST_COUNT_TEST = 2;  // (PostOp count: resolve, a-trous; renderer.hpp is not part of fdcore)

static bool compile(const std::string& src, ShaderTarget t, std::string& log) {
  glslang::TShader sh(EShLangFragment);
  const char* s = src.c_str();
  sh.setStrings(&s, 1);
  if (t == ShaderTarget::Vulkan) {
    sh.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  }
  if (!sh.parse(GetDefaultResources(), t == ShaderTarget::Vulkan ? 450 : 330, false, EShMsgDefault)) { log = sh.getInfoLog(); return false; }
  glslang::TProgram prog;
  prog.addShader(&sh);
  if (!prog.link(EShMsgDefault)) { log = prog.getInfoLog(); return false; }
  if (t == ShaderTarget::Vulkan) {
    std::vector<uint32_t> spv;
    glslang::GlslangToSpv(*prog.getIntermediate(EShLangFragment), spv);
    if (spv.empty()) { log = "no SPIR-V"; return false; }
  }
  return true;
}

static void check(const std::string& name, const std::string& src, ShaderTarget t) {
  checks++;
  std::string log;
  if (compile(src, t, log)) return;
  failures++;
  std::printf("FAIL %s (%s)\n%s\n", name.c_str(), t == ShaderTarget::Vulkan ? "Vulkan" : "OpenGL", log.substr(0, 2000).c_str());
}

// custom SDF code that does not compile is drawn as its bounding box (as the app does)
static std::unordered_set<std::string> badCustomCode(Node& root) {
  std::unordered_set<std::string> bad;
  walk(root, [&](Node& n, Node*, int) {
    if (!n.isCustom()) return;
    std::string src = shaderPrelude(ShaderTarget::Vulkan, false) + sdfHelpersGLSL() + guardLoops(renameCustomCode(n.code, "_t")) +
                      "\nvoid main() { fragColor = vec4(sdf_t(vec4(v_uv, 0.0, 1.0))); }\n";
    std::string log;
    if (!compile(src, ShaderTarget::Vulkan, log)) bad.insert(n.code);
  });
  return bad;
}

static void scene(const std::string& name, SceneFile f) {
  PrimitiveRegistry reg;
  for (auto& p : f.primitives) reg.set(p);
  Node& root = f.root;
  auto bad = badCustomCode(root);
  bool curved = f.space.curved();
  std::string world = !curved && !f.world.empty() && findNode(root, f.world) ? f.world : "";
  struct View { const char* tag; bool geo, ball, pt, guide; };
  std::vector<View> views;
  for (int pt : {0, 1, 2}) {  // simple, path traced, the denoiser's guide
    const char* tags[3] = {"simple", "pt", "pt-guide"};
    const char* gtags[3] = {"geo", "geo-pt", "geo-pt-guide"};
    views.push_back({tags[pt], false, false, pt > 0, pt == 2});
    if (!world.empty()) views.push_back({gtags[pt], true, false, pt > 0, pt == 2});
  }
  if (curved) views.push_back({"ball", false, true, false, false});
  for (auto& v : views) {
    CompiledScene cs = compileScene(root, reg, world, v.geo, &bad, f.space.kind);
    for (ShaderTarget t : {ShaderTarget::Vulkan, ShaderTarget::OpenGL})
      check(name + " [" + v.tag + "]", buildFragmentShader(cs.glsl, v.geo, t, (int)f.space.kind, v.ball, v.pt, v.guide), t);
  }
}

static std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int main() {
  glslang::InitializeProcess();
  for (auto& ex : EXAMPLES) scene(std::string("example:") + ex.id, ex.make());
  for (const char* dir : {"examples", "tests/scenes"}) {
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(std::filesystem::path(FD_SOURCE_DIR) / dir))
      if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& p : files) {
      try {
        scene(p.filename().string(), sceneFromJson(readFile(p)));
      } catch (const std::exception& e) {
        checks++; failures++;
        std::printf("FAIL %s: cannot load (%s)\n", p.filename().string().c_str(), e.what());
      }
    }
  }
  // the path tracer's post passes (resolve / tone mapping, denoiser)
  for (int op = 0; op < POST_COUNT_TEST; op++)
    for (ShaderTarget t : {ShaderTarget::Vulkan, ShaderTarget::OpenGL}) check("post " + std::to_string(op), postShader(op, t), t);
  glslang::FinalizeProcess();
  std::printf("%d shaders compiled, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
