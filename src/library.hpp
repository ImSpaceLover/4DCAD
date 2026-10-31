// User library (custom primitives + saved objects) and app data paths.
#pragma once
#include <string>
#include <vector>

#include "scene.hpp"

namespace fd {

/** Per-user data directory (%APPDATA%\4DCAD or ~/.config/4dcad), created on demand. */
std::string dataDir();
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& text);

class LibraryStore {
 public:
  explicit LibraryStore(PrimitiveRegistry& reg);
  std::vector<PrimitiveDef> primitives;
  std::vector<LibraryObject> objects;
  void savePrimitive(const PrimitiveDef& p);
  void deletePrimitive(const std::string& id);
  void saveObject(const LibraryObject& o);
  void deleteObject(const std::string& id);
  std::string importText(const std::string& text);  // throws on error
  std::string exportText() const { return libraryToJson(primitives, objects); }

 private:
  PrimitiveRegistry& reg_;
  void persist();
  void sync();
};

}  // namespace fd
