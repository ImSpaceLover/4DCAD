#include "library.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fd {

std::string dataDir() {
  namespace fs = std::filesystem;
  fs::path p;
#ifdef _WIN32
  if (const char* a = std::getenv("APPDATA")) p = fs::path(a) / "4DCAD";
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME")) p = fs::path(x) / "4dcad";
  else if (const char* h = std::getenv("HOME")) p = fs::path(h) / ".config" / "4dcad";
#endif
  if (p.empty()) p = fs::current_path() / "4dcad-data";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p.string();
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream s;
  s << f.rdbuf();
  out = s.str();
  return true;
}

bool writeFile(const std::string& path, const std::string& text) {
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) return false;
  f << text;
  return (bool)f;
}

static std::string libPath() { return (std::filesystem::path(dataDir()) / "library.json").string(); }

LibraryStore::LibraryStore(PrimitiveRegistry& reg) : reg_(reg) {
  std::string text;
  if (readFile(libPath(), text)) {
    try {
      auto imp = importLibraryJson(text);
      primitives = imp.primitives;
      objects = imp.objects;
    } catch (...) { /* corrupt library: start empty */ }
  }
  sync();
}

void LibraryStore::sync() {
  for (auto& p : primitives) { PrimitiveDef q = p; q.builtin = false; reg_.set(q); }
  for (auto& o : objects) for (auto& p : o.primitives) reg_.set(p);
}

void LibraryStore::persist() {
  sync();
  writeFile(libPath(), exportText());
}

void LibraryStore::savePrimitive(const PrimitiveDef& p) {
  for (auto& q : primitives) if (q.id == p.id) { q = p; persist(); return; }
  primitives.push_back(p);
  persist();
}
void LibraryStore::deletePrimitive(const std::string& id) {
  primitives.erase(std::remove_if(primitives.begin(), primitives.end(), [&](auto& p) { return p.id == id; }), primitives.end());
  persist();
}
void LibraryStore::saveObject(const LibraryObject& o) {
  for (auto& q : objects) if (q.id == o.id) { q = o; persist(); return; }
  objects.push_back(o);
  persist();
}
void LibraryStore::deleteObject(const std::string& id) {
  objects.erase(std::remove_if(objects.begin(), objects.end(), [&](auto& o) { return o.id == id; }), objects.end());
  persist();
}
std::string LibraryStore::importText(const std::string& text) {
  auto imp = importLibraryJson(text);
  for (auto& p : imp.primitives) savePrimitive(p);
  for (auto& o : imp.objects) saveObject(o);
  persist();
  return imp.description;
}

}  // namespace fd
