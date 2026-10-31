#pragma once

#include <string>
#include <vector>

namespace bbport {

struct ModItem {
  std::string id;
  std::string name;
  std::string author;
  std::string path;
  bool enabled = true;
  int order = 0;
};

struct PatchItem {
  std::string id;
  std::string name;
  std::string author;
  std::string path;
  bool enabled = false;
};

class ModsManager {
public:
  static ModsManager& Instance();

  void Refresh(const std::string& mods_dir, const std::string& patches_dir);
  bool SaveModsConfig(const std::string& mods_dir);
  bool SavePatchesConfig(const std::string& patches_dir);

  std::vector<ModItem>& GetMods() { return mods_; }
  const std::vector<ModItem>& GetMods() const { return mods_; }

  std::vector<PatchItem>& GetPatches() { return patches_; }
  const std::vector<PatchItem>& GetPatches() const { return patches_; }

  bool ToggleMod(const std::string& mod_id);
  bool MoveModUp(const std::string& mod_id);
  bool MoveModDown(const std::string& mod_id);

  bool TogglePatch(const std::string& patch_id);

private:
  ModsManager() = default;

  void LoadModsJson(const std::string& mods_dir);
  void LoadPatchesJson(const std::string& patches_dir);

  std::vector<ModItem> mods_;
  std::vector<PatchItem> patches_;
};

} // namespace bbport
