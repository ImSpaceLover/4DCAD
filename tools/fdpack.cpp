// Release packaging helper: writes the built-in definitions and objects as files of a library folder
//   fdpack OUT_DIR   ->  OUT_DIR/Modifiers/<id>.4dmod.json, OUT_DIR/Textures/<id>.4dtex.json,
//                        OUT_DIR/Objects/4dcad-builtin-library.json
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "defs.hpp"
#include "examples.hpp"
#include "scene.hpp"
#include "version.hpp"

using namespace fd;
namespace fs = std::filesystem;

static bool put(const fs::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary);
  f << text;
  return (bool)f;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: fdpack OUT_DIR\n"); return 2; }
  fs::path out = argv[1];
  fs::create_directories(out / "Modifiers");
  fs::create_directories(out / "Textures");
  fs::create_directories(out / "Objects");
  int n = 0;
  for (auto& d : builtinModifierDefs()) n += put(out / "Modifiers" / (d.id + ".4dmod.json"), modifierDefToJson(d).dump(1));
  for (auto& d : builtinTextureDefs()) n += put(out / "Textures" / (d.id + ".4dtex.json"), textureDefToJson(d).dump(1));
  n += put(out / "Objects" / "4dcad-builtin-library.json", libraryToJson(builtinFolders()));
  // the example scenes built into File > Examples
  fs::create_directories(out / "Examples");
  for (auto& ex : EXAMPLES) {
    SceneFile f = ex.make();
    PrimitiveRegistry reg;
    std::vector<PointLight> lights = f.lights;
    n += put(out / "Examples" / (std::string(ex.id) + ".4dscene.json"),
             sceneToJson(f.root, reg, f.world, f.camera ? &*f.camera : nullptr, nullptr, &lights, f.activeCamera, nullptr, &f.space, nullptr));
  }
  std::printf("4DCAD %s: wrote %d library files to %s\n", FD_VERSION, n, out.string().c_str());
  return 0;
}
