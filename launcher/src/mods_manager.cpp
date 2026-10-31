#include "mods_manager.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace bbport {

ModsManager& ModsManager::Instance() {
  static ModsManager instance;
  return instance;
}

void ModsManager::Refresh(const std::string& mods_dir, const std::string& patches_dir) {
  mods_.clear();
  patches_.clear();

  // 1. Discover mods in mods_dir
  if (!mods_dir.empty() && fs::exists(mods_dir) && fs::is_directory(mods_dir)) {
    for (const auto& entry : fs::directory_iterator(mods_dir)) {
      if (entry.is_directory()) {
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        ModItem item;
        item.id = name;
        item.name = name;
        item.path = entry.path().string();
        item.enabled = true;
        mods_.push_back(item);
      }
    }
    LoadModsJson(mods_dir);
  }

  // 2. Discover XML patches in patches_dir
  if (!patches_dir.empty() && fs::exists(patches_dir) && fs::is_directory(patches_dir)) {
    for (const auto& entry : fs::directory_iterator(patches_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".xml") {
        std::string filename = entry.path().filename().string();
        PatchItem item;
        item.id = filename;
        item.name = filename;
        item.path = entry.path().string();

        // Read XML title / author
        std::ifstream f(item.path);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto extract_tag = [&](const std::string& tag) -> std::string {
          std::string open_tag = "<" + tag + ">";
          std::string close_tag = "</" + tag + ">";
          size_t s = content.find(open_tag);
          if (s != std::string::npos) {
            s += open_tag.length();
            size_t e = content.find(close_tag, s);
            if (e != std::string::npos) {
              return content.substr(s, e - s);
            }
          }
          return "";
        };

        std::string title = extract_tag("title");
        std::string author = extract_tag("author");
        if (!title.empty()) item.name = title;
        if (!author.empty()) item.author = author;

        item.enabled = (content.find("isEnabled=\"true\"") != std::string::npos ||
                        content.find("isEnabled=\"1\"") != std::string::npos);

        patches_.push_back(item);
      }
    }
    LoadPatchesJson(patches_dir);
  }
}

void ModsManager::LoadModsJson(const std::string& mods_dir) {
  fs::path config_path = fs::path(mods_dir) / "mods.json";
  if (!fs::exists(config_path)) return;

  std::ifstream file(config_path);
  if (!file.is_open()) return;

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  std::vector<std::string> order;
  std::unordered_set<std::string> disabled;

  // Simple parser for "order": [...] and "disabled": [...]
  auto parse_array = [&](const std::string& key, auto&& sink) {
    size_t key_pos = content.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return;
    size_t open_bracket = content.find('[', key_pos);
    size_t close_bracket = content.find(']', open_bracket);
    if (open_bracket == std::string::npos || close_bracket == std::string::npos) return;

    std::string arr = content.substr(open_bracket + 1, close_bracket - open_bracket - 1);
    size_t pos = 0;
    while ((pos = arr.find('"', pos)) != std::string::npos) {
      size_t end_str = arr.find('"', pos + 1);
      if (end_str == std::string::npos) break;
      sink(arr.substr(pos + 1, end_str - pos - 1));
      pos = end_str + 1;
    }
  };

  parse_array("order", [&](const std::string& s) { order.push_back(s); });
  parse_array("disabled", [&](const std::string& s) { disabled.insert(s); });

  // Update disabled state
  for (auto& mod : mods_) {
    if (disabled.count(mod.id)) {
      mod.enabled = false;
    }
  }

  // Sort by order list
  std::vector<ModItem> sorted;
  std::unordered_set<std::string> placed;
  for (const auto& ord_id : order) {
    for (const auto& mod : mods_) {
      if (mod.id == ord_id && !placed.count(mod.id)) {
        sorted.push_back(mod);
        placed.insert(mod.id);
        break;
      }
    }
  }
  // Append any newly discovered mods not in order list
  for (const auto& mod : mods_) {
    if (!placed.count(mod.id)) {
      sorted.push_back(mod);
    }
  }
  mods_ = std::move(sorted);

  for (size_t i = 0; i < mods_.size(); ++i) {
    mods_[i].order = static_cast<int>(i);
  }
}

void ModsManager::LoadPatchesJson(const std::string& patches_dir) {
  fs::path config_path = fs::path(patches_dir) / "patches.json";
  if (!fs::exists(config_path)) return;

  std::ifstream file(config_path);
  if (!file.is_open()) return;

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  for (auto& patch : patches_) {
    std::string needle = "\"" + patch.id + "\"";
    size_t pos = content.find(needle);
    if (pos != std::string::npos) {
      // Find colon and value
      size_t colon = content.find(':', pos + needle.length());
      if (colon != std::string::npos) {
        size_t true_pos = content.find("true", colon);
        size_t false_pos = content.find("false", colon);
        if (true_pos != std::string::npos && (false_pos == std::string::npos || true_pos < false_pos)) {
          patch.enabled = true;
        } else if (false_pos != std::string::npos) {
          patch.enabled = false;
        }
      }
    }
  }
}

bool ModsManager::SaveModsConfig(const std::string& mods_dir) {
  if (mods_dir.empty() || !fs::exists(mods_dir)) return false;
  fs::path out_path = fs::path(mods_dir) / "mods.json";

  std::ostringstream ss;
  ss << "{\n  \"order\": [\n";
  for (size_t i = 0; i < mods_.size(); ++i) {
    ss << "    \"" << mods_[i].id << "\"" << (i + 1 < mods_.size() ? "," : "") << "\n";
  }
  ss << "  ],\n  \"disabled\": [\n";
  bool first = true;
  for (const auto& mod : mods_) {
    if (!mod.enabled) {
      if (!first) ss << ",\n";
      ss << "    \"" << mod.id << "\"";
      first = false;
    }
  }
  ss << "\n  ]\n}\n";

  std::ofstream file(out_path);
  if (!file.is_open()) return false;
  file << ss.str();
  return true;
}

bool ModsManager::SavePatchesConfig(const std::string& patches_dir) {
  if (patches_dir.empty() || !fs::exists(patches_dir)) return false;
  fs::path out_path = fs::path(patches_dir) / "patches.json";

  std::ostringstream ss;
  ss << "{\n";
  for (size_t i = 0; i < patches_.size(); ++i) {
    ss << "  \"" << patches_[i].id << "\": " << (patches_[i].enabled ? "true" : "false")
       << (i + 1 < patches_.size() ? "," : "") << "\n";
  }
  ss << "}\n";

  std::ofstream file(out_path);
  if (!file.is_open()) return false;
  file << ss.str();
  return true;
}

bool ModsManager::ToggleMod(const std::string& mod_id) {
  for (auto& m : mods_) {
    if (m.id == mod_id) {
      m.enabled = !m.enabled;
      return true;
    }
  }
  return false;
}

bool ModsManager::MoveModUp(const std::string& mod_id) {
  for (size_t i = 1; i < mods_.size(); ++i) {
    if (mods_[i].id == mod_id) {
      std::swap(mods_[i], mods_[i - 1]);
      for (size_t j = 0; j < mods_.size(); ++j) mods_[j].order = static_cast<int>(j);
      return true;
    }
  }
  return false;
}

bool ModsManager::MoveModDown(const std::string& mod_id) {
  for (size_t i = 0; i + 1 < mods_.size(); ++i) {
    if (mods_[i].id == mod_id) {
      std::swap(mods_[i], mods_[i + 1]);
      for (size_t j = 0; j < mods_.size(); ++j) mods_[j].order = static_cast<int>(j);
      return true;
    }
  }
  return false;
}

bool ModsManager::TogglePatch(const std::string& patch_id) {
  for (auto& p : patches_) {
    if (p.id == patch_id) {
      p.enabled = !p.enabled;
      return true;
    }
  }
  return false;
}

} // namespace bbport
