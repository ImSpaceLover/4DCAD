// User library: folders of saved objects (built-in read-only folders + user folders),
// persisted in the user data directory; app data paths.
#pragma once
#include <string>
#include <vector>

#include "scene.hpp"

namespace fd {

std::string dataDir();
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& text);

/** The user's own modifier / texture definitions (definitions.json in the data directory): loaded
 *  into the registry at start, saved whenever one is created, edited or deleted. */
void loadUserDefs();
void saveUserDefs();
/** Registers the definition a .4dmod.json / .4dtex.json file holds; returns its name (throws). */
std::string importDefinition(const std::string& text);
/** A free id for a new definition named `name` ("user_<name>", numbered if taken). */
std::string newDefId(const std::string& name, bool texture);

class LibraryStore {
 public:
  LibraryStore();
  std::vector<LibraryFolder> folders;  // built-in folders first, then user folders ("Custom" always exists)
  static constexpr const char* CUSTOM = "custom";

  const LibraryObject* find(const std::string& objectId) const;
  LibraryFolder* folder(const std::string& id);
  /** Saves into a folder (default: Custom). Returns the stored object's id. */
  std::string saveObject(LibraryObject o, const std::string& folderId = CUSTOM);
  void deleteObject(const std::string& objectId);
  void renameObject(const std::string& objectId, const std::string& name);
  void moveObject(const std::string& objectId, const std::string& folderId);
  std::string addFolder(const std::string& name);
  void renameFolder(const std::string& id, const std::string& name);
  void deleteFolder(const std::string& id);  // its objects move to Custom
  std::string importText(const std::string& text);  // throws on error
  std::string exportText() const { return libraryToJson(folders); }

 private:
  void persist();
};

}  // namespace fd
