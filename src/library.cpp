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

LibraryStore::LibraryStore() {
  folders = builtinFolders();
  std::string text;
  std::vector<LibraryFolder> user;
  std::vector<LibraryObject> loose;
  if (readFile(libPath(), text)) {
    try {
      auto imp = importLibraryJson(text);
      user = imp.folders;
      loose = imp.objects;  // old (v1) library: primitives + objects
    } catch (...) { /* corrupt library: start empty */ }
  }
  bool hasCustom = false;
  for (auto& f : user) if (f.id == CUSTOM) hasCustom = true;
  if (!hasCustom) user.insert(user.begin(), LibraryFolder{CUSTOM, "Custom", false, {}});
  for (auto& f : user) folders.push_back(f);
  for (auto& o : loose) folder(CUSTOM)->objects.push_back(o);
  if (!loose.empty()) persist();
}

const LibraryObject* LibraryStore::find(const std::string& id) const {
  for (auto& f : folders) for (auto& o : f.objects) if (o.id == id) return &o;
  return nullptr;
}
LibraryFolder* LibraryStore::folder(const std::string& id) {
  for (auto& f : folders) if (f.id == id) return &f;
  return nullptr;
}

void LibraryStore::persist() { writeFile(libPath(), exportText()); }

std::string LibraryStore::saveObject(LibraryObject o, const std::string& folderId) {
  LibraryFolder* f = folder(folderId);
  if (!f || f->builtin) f = folder(CUSTOM);
  if (o.id.empty() || find(o.id)) o.id = newId("o");
  std::string id = o.id;
  f->objects.push_back(std::move(o));
  persist();
  return id;
}
void LibraryStore::deleteObject(const std::string& id) {
  for (auto& f : folders) {
    if (f.builtin) continue;
    f.objects.erase(std::remove_if(f.objects.begin(), f.objects.end(), [&](auto& o) { return o.id == id; }), f.objects.end());
  }
  persist();
}
void LibraryStore::renameObject(const std::string& id, const std::string& name) {
  for (auto& f : folders) if (!f.builtin) for (auto& o : f.objects) if (o.id == id) o.name = name;
  persist();
}
void LibraryStore::moveObject(const std::string& id, const std::string& folderId) {
  LibraryFolder* dst = folder(folderId);
  if (!dst || dst->builtin) return;
  const LibraryObject* src = find(id);
  if (!src) return;
  LibraryObject copy = *src;
  bool fromBuiltin = false;
  for (auto& f : folders) for (auto& o : f.objects) if (o.id == id && f.builtin) fromBuiltin = true;
  if (fromBuiltin) copy.id = newId("o");  // dragging a built-in copies it
  else deleteObject(id);
  dst->objects.push_back(copy);
  persist();
}
std::string LibraryStore::addFolder(const std::string& name) {
  LibraryFolder f{newId("f"), name, false, {}};
  folders.push_back(f);
  persist();
  return f.id;
}
void LibraryStore::renameFolder(const std::string& id, const std::string& name) {
  if (LibraryFolder* f = folder(id); f && !f->builtin) { f->name = name; persist(); }
}
void LibraryStore::deleteFolder(const std::string& id) {
  if (id == CUSTOM) return;
  for (size_t i = 0; i < folders.size(); i++) {
    if (folders[i].id != id || folders[i].builtin) continue;
    auto objs = folders[i].objects;
    folders.erase(folders.begin() + i);
    for (auto& o : objs) folder(CUSTOM)->objects.push_back(o);
    persist();
    return;
  }
}
std::string LibraryStore::importText(const std::string& text) {
  auto imp = importLibraryJson(text);
  for (auto& f : imp.folders) {
    LibraryFolder* dst = folder(f.id);
    if (!dst || dst->builtin) { std::string id = addFolder(f.name); dst = folder(id); }
    for (auto& o : f.objects) { LibraryObject c = o; if (find(c.id)) c.id = newId("o"); dst->objects.push_back(c); }
  }
  for (auto& o : imp.objects) { LibraryObject c = o; if (find(c.id)) c.id = newId("o"); folder(CUSTOM)->objects.push_back(c); }
  persist();
  return imp.description;
}

}  // namespace fd
