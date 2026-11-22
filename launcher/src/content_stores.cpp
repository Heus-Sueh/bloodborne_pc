// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "content_stores.hpp"
#include "bbport_trophy_data.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace bbport {

namespace {

constexpr std::array<std::string_view, 9> kBloodborneSerials = {
    "CUSA03173", "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299",
    "CUSA01363", "CUSA03179", "CUSA03014", "CUSA03023",
};

constexpr std::array<std::string_view, 21> kGameFolders = {
    "action", "adhoc", "chr", "event", "facegen", "font", "map",
    "menu", "movie", "msg", "mtd", "obj", "other", "param",
    "paramdef", "parts", "remo", "script", "sfx", "shader", "sound",
};

std::string ToLower(std::string_view sv) {
  std::string out(sv);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string Trim(std::string_view sv) {
  std::size_t a = 0;
  while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a]))) ++a;
  std::size_t b = sv.size();
  while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) --b;
  return std::string(sv.substr(a, b - a));
}

std::uint16_t ReadLe16(const std::vector<std::uint8_t>& d, std::size_t at) {
  return static_cast<std::uint16_t>(d[at]) | (static_cast<std::uint16_t>(d[at + 1]) << 8);
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& d, std::size_t at) {
  return static_cast<std::uint32_t>(d[at]) |
         (static_cast<std::uint32_t>(d[at + 1]) << 8) |
         (static_cast<std::uint32_t>(d[at + 2]) << 16) |
         (static_cast<std::uint32_t>(d[at + 3]) << 24);
}

// Binary PS4 param.sfo parser matching scripts/prepare.py::sfo().
std::unordered_map<std::string, std::string> ParseParamSfo(const fs::path& sfo_path) {
  std::unordered_map<std::string, std::string> out;
  std::ifstream in(sfo_path, std::ios::binary);
  if (!in.is_open()) {
    return out;
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < 20 || data[0] != 0x00 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F') {
    return out;
  }

  const std::uint32_t key_table = ReadLe32(data, 8);
  const std::uint32_t val_table = ReadLe32(data, 12);
  const std::uint32_t count = ReadLe32(data, 16);

  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t entry = 20 + static_cast<std::size_t>(i) * 16;
    if (entry + 16 > data.size()) break;

    const std::uint16_t key_off = ReadLe16(data, entry);
    const std::uint16_t fmt = ReadLe16(data, entry + 2);
    const std::uint32_t len = ReadLe32(data, entry + 4);
    const std::uint32_t val_off = ReadLe32(data, entry + 12);

    const std::size_t k_start = static_cast<std::size_t>(key_table) + key_off;
    if (k_start >= data.size()) continue;
    std::size_t k_end = k_start;
    while (k_end < data.size() && data[k_end] != 0) ++k_end;
    std::string key(reinterpret_cast<const char*>(data.data() + k_start), k_end - k_start);

    const std::size_t v_start = static_cast<std::size_t>(val_table) + val_off;
    if (v_start > data.size() || len > data.size() - v_start) continue;

    if (fmt == 0x0404 && len >= 4) {
      out[key] = std::to_string(ReadLe32(data, v_start));
    } else {
      std::size_t actual_len = len;
      while (actual_len > 0 && data[v_start + actual_len - 1] == 0) --actual_len;
      out[key] = std::string(reinterpret_cast<const char*>(data.data() + v_start), actual_len);
    }
  }
  return out;
}

fs::path ChildCaseInsensitive(const fs::path& folder, std::string_view name) {
  std::error_code ec;
  const std::string target = ToLower(name);
  if (fs::is_directory(folder, ec)) {
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
      if (ToLower(entry.path().filename().string()) == target) {
        return entry.path();
      }
    }
  }
  return folder / std::string(name);
}

// Mirrors scripts/mods.py::content_root() layout detection.
bool HasValidModLayout(const fs::path& folder, int depth = 0) {
  if (depth > 3) return false;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return false;

  // Check '', 'app0', and Bloodborne serial wrappers for dvdroot_ps4
  if (fs::is_directory(ChildCaseInsensitive(folder, "dvdroot_ps4"), ec)) return true;
  if (fs::is_directory(ChildCaseInsensitive(ChildCaseInsensitive(folder, "app0"), "dvdroot_ps4"), ec)) return true;
  for (auto serial : kBloodborneSerials) {
    if (fs::is_directory(ChildCaseInsensitive(ChildCaseInsensitive(folder, serial), "dvdroot_ps4"), ec)) {
      return true;
    }
  }

  std::vector<fs::path> subdirs;
  bool has_non_doc_files = false;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    const std::string fname = entry.path().filename().string();
    if (!fname.empty() && fname[0] == '.') continue;
    if (entry.is_directory(ec)) {
      subdirs.push_back(entry.path());
    } else if (entry.is_regular_file(ec)) {
      const std::string ext = ToLower(entry.path().extension().string());
      if (ext != ".txt" && ext != ".md" && ext != ".jpg" && ext != ".png" && ext != ".ini") {
        has_non_doc_files = true;
      }
    }
  }

  if (!subdirs.empty()) {
    const bool all_game_folders = std::all_of(subdirs.begin(), subdirs.end(), [](const fs::path& p) {
      const std::string lower = ToLower(p.filename().string());
      return std::find(kGameFolders.begin(), kGameFolders.end(), lower) != kGameFolders.end();
    });
    if (all_game_folders) {
      return true;
    }
  }

  if (subdirs.size() == 1 && !has_non_doc_files) {
    return HasValidModLayout(subdirs[0], depth + 1);
  }
  return false;
}

std::string XmlAttribute(std::string_view tag, std::string_view name) {
  const std::string key = " " + std::string(name) + "=\"";
  const auto at = tag.find(key);
  if (at == std::string_view::npos) return {};
  const auto from = at + key.size();
  const auto to = tag.find('"', from);
  if (to == std::string_view::npos) return {};
  return std::string(tag.substr(from, to - from));
}

bool XmlMatchesBloodborneIds(std::string_view xml) {
  std::set<std::string> ids;
  std::size_t pos = 0;
  while ((pos = xml.find("<ID>", pos)) != std::string_view::npos) {
    const auto end = xml.find("</ID>", pos + 4);
    if (end == std::string_view::npos) break;
    ids.insert(Trim(xml.substr(pos + 4, end - (pos + 4))));
    pos = end + 5;
  }
  if (ids.empty()) return true;
  for (const auto& id : ids) {
    if (std::find(kBloodborneSerials.begin(), kBloodborneSerials.end(), id) != kBloodborneSerials.end()) {
      return true;
    }
  }
  return false;
}

std::string ReadWholeFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TrophyGrade CharToGrade(char g) {
  switch (g) {
    case 'P': return TrophyGrade::Platinum;
    case 'G': return TrophyGrade::Gold;
    case 'S': return TrophyGrade::Silver;
    case 'B': return TrophyGrade::Bronze;
    default: return TrophyGrade::Unknown;
  }
}

std::string GradeToString(TrophyGrade g) {
  switch (g) {
    case TrophyGrade::Platinum: return "Platinum";
    case TrophyGrade::Gold: return "Gold";
    case TrophyGrade::Silver: return "Silver";
    case TrophyGrade::Bronze: return "Bronze";
    default: return "Unknown";
  }
}

} // namespace

GameInspection InspectGameDirectory(const fs::path& game_dir) {
  GameInspection res;
  if (game_dir.empty()) {
    return res;
  }

  std::error_code ec;
  fs::path candidate = game_dir;
  auto has_both = [&](const fs::path& p) {
    return fs::is_regular_file(p / "eboot.bin", ec) && fs::is_directory(p / "dvdroot_ps4", ec);
  };

  if (!has_both(candidate) && fs::is_directory(candidate, ec)) {
    for (auto serial : kBloodborneSerials) {
      if (has_both(candidate / std::string(serial))) {
        candidate = candidate / std::string(serial);
        break;
      }
    }
  }

  res.resolved_dir = candidate;
  res.has_eboot = fs::is_regular_file(candidate / "eboot.bin", ec);
  res.has_dvdroot = fs::is_directory(candidate / "dvdroot_ps4", ec);
  res.valid = res.has_eboot && res.has_dvdroot;

  const auto sfo = ParseParamSfo(candidate / "sce_sys" / "param.sfo");
  if (auto it = sfo.find("TITLE_ID"); it != sfo.end()) {
    res.title_id = it->second;
  }
  if (auto it = sfo.find("APP_VER"); it != sfo.end()) {
    res.version = it->second;
  }

  res.is_109 = (res.version == "01.09");
  if (!res.valid) {
    res.status_key = "status_invalid";
  } else if (!res.is_109) {
    res.status_key = "status_update_needed";
  } else {
    res.status_key = "status_ready";
  }
  return res;
}

std::vector<ModItem> LoadMods(const fs::path& mods_dir, const fs::path& mods_config_path) {
  std::vector<std::string> available;
  std::error_code ec;
  if (fs::is_directory(mods_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(mods_dir, ec)) {
      if (!entry.is_directory(ec)) continue;
      const std::string name = entry.path().filename().string();
      if (name.empty() || name[0] == '.') continue;
      if (HasValidModLayout(entry.path())) {
        available.push_back(name);
      }
    }
  }
  std::sort(available.begin(), available.end(), [](const std::string& a, const std::string& b) {
    const std::string la = ToLower(a);
    const std::string lb = ToLower(b);
    return la != lb ? la < lb : a < b;
  });

  std::vector<std::string> saved_order;
  std::unordered_set<std::string> disabled;
  if (std::ifstream in(mods_config_path); in.is_open()) {
    try {
      json profile;
      in >> profile;
      if (profile.is_object()) {
        if (profile.contains("order") && profile["order"].is_array()) {
          for (const auto& item : profile["order"]) {
            if (item.is_string()) saved_order.push_back(item.get<std::string>());
          }
        }
        if (profile.contains("disabled") && profile["disabled"].is_array()) {
          for (const auto& item : profile["disabled"]) {
            if (item.is_string()) disabled.insert(item.get<std::string>());
          }
        }
      }
    } catch (...) {
    }
  }

  std::unordered_set<std::string> available_set(available.begin(), available.end());
  std::vector<std::string> final_order;
  std::unordered_set<std::string> seen;

  for (const auto& name : saved_order) {
    if (available_set.count(name) && seen.insert(name).second) {
      final_order.push_back(name);
    }
  }
  for (const auto& name : available) {
    if (seen.insert(name).second) {
      final_order.push_back(name);
    }
  }

  std::vector<ModItem> result;
  result.reserve(final_order.size());
  for (std::size_t i = 0; i < final_order.size(); ++i) {
    ModItem m;
    m.id = final_order[i];
    m.name = final_order[i];
    m.path = (mods_dir / final_order[i]).string();
    m.enabled = disabled.count(final_order[i]) == 0;
    m.order = static_cast<int>(i);
    result.push_back(std::move(m));
  }
  return result;
}

bool SaveModsConfig(const fs::path& mods_config_path, const std::vector<ModItem>& mods) {
  std::error_code ec;
  if (mods_config_path.has_parent_path()) {
    fs::create_directories(mods_config_path.parent_path(), ec);
  }
  json profile = json::object();
  json order = json::array();
  json disabled = json::array();
  for (const auto& m : mods) {
    order.push_back(m.id);
    if (!m.enabled) {
      disabled.push_back(m.id);
    }
  }
  profile["order"] = std::move(order);
  profile["disabled"] = std::move(disabled);

  std::ofstream out(mods_config_path);
  if (!out.is_open()) return false;
  out << profile.dump(2) << "\n";
  return out.good();
}

std::vector<PatchItem> LoadExternalPatches(const fs::path& patches_dir,
                                           const fs::path& patches_config_path,
                                           const fs::path& builtin_xml_to_exclude) {
  std::unordered_set<std::string> enabled_overrides;
  std::unordered_set<std::string> disabled_overrides;
  if (std::ifstream in(patches_config_path); in.is_open()) {
    try {
      json profile;
      in >> profile;
      if (profile.is_object()) {
        if (profile.contains("enabled") && profile["enabled"].is_array()) {
          for (const auto& k : profile["enabled"]) {
            if (k.is_string()) enabled_overrides.insert(k.get<std::string>());
          }
        }
        if (profile.contains("disabled") && profile["disabled"].is_array()) {
          for (const auto& k : profile["disabled"]) {
            if (k.is_string()) disabled_overrides.insert(k.get<std::string>());
          }
        }
      }
    } catch (...) {
    }
  }

  std::error_code ec;
  fs::path excluded_canon;
  if (!builtin_xml_to_exclude.empty()) {
    excluded_canon = fs::weakly_canonical(builtin_xml_to_exclude, ec);
  }

  std::vector<fs::path> xml_files;
  if (fs::is_directory(patches_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(patches_dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      if (ToLower(entry.path().extension().string()) != ".xml") continue;
      if (!excluded_canon.empty()) {
        auto canon = fs::weakly_canonical(entry.path(), ec);
        if (!ec && canon == excluded_canon) continue;
      }
      xml_files.push_back(entry.path());
    }
  }
  std::sort(xml_files.begin(), xml_files.end());

  std::vector<PatchItem> result;
  for (const auto& file : xml_files) {
    const std::string xml = ReadWholeFile(file);
    if (xml.empty() || !XmlMatchesBloodborneIds(xml)) {
      continue;
    }

    const std::string fname = file.filename().string();
    std::size_t pos = 0;
    while ((pos = xml.find("<Metadata ", pos)) != std::string::npos) {
      const auto end = xml.find('>', pos);
      if (end == std::string::npos) break;
      const std::string_view tag(xml.data() + pos, end - pos);
      pos = end + 1;

      const std::string app_ver = XmlAttribute(tag, "AppVer");
      const std::string app_elf = XmlAttribute(tag, "AppElf");
      if (app_ver != "01.09") continue;
      if (!app_elf.empty() && app_elf != "eboot.bin") continue;

      const std::string name = XmlAttribute(tag, "Name");
      if (name.empty()) continue;

      PatchItem p;
      p.id = fname + "/" + name;
      p.name = name;
      p.author = XmlAttribute(tag, "Author");
      p.path = file.string();
      p.default_enabled = (ToLower(XmlAttribute(tag, "isEnabled")) == "true");
      p.enabled = enabled_overrides.count(p.id) > 0 ||
                  (p.default_enabled && disabled_overrides.count(p.id) == 0);
      result.push_back(std::move(p));
    }
  }
  return result;
}

bool SaveExternalPatchesConfig(const fs::path& patches_config_path, const std::vector<PatchItem>& patches) {
  std::set<std::string> enabled_set;
  std::set<std::string> disabled_set;
  std::unordered_set<std::string> shown;
  for (const auto& p : patches) {
    shown.insert(p.id);
  }

  // Preserve overrides for patches from other directories (matching bbport_launcher_content.py:75-78).
  if (std::ifstream in(patches_config_path); in.is_open()) {
    try {
      json existing;
      in >> existing;
      if (existing.is_object()) {
        if (existing.contains("enabled") && existing["enabled"].is_array()) {
          for (const auto& k : existing["enabled"]) {
            if (k.is_string() && shown.count(k.get<std::string>()) == 0) {
              enabled_set.insert(k.get<std::string>());
            }
          }
        }
        if (existing.contains("disabled") && existing["disabled"].is_array()) {
          for (const auto& k : existing["disabled"]) {
            if (k.is_string() && shown.count(k.get<std::string>()) == 0) {
              disabled_set.insert(k.get<std::string>());
            }
          }
        }
      }
    } catch (...) {
    }
  }

  for (const auto& p : patches) {
    if (p.enabled != p.default_enabled) {
      if (p.enabled) {
        enabled_set.insert(p.id);
      } else {
        disabled_set.insert(p.id);
      }
    }
  }

  std::error_code ec;
  if (patches_config_path.has_parent_path()) {
    fs::create_directories(patches_config_path.parent_path(), ec);
  }
  json profile = json::object();
  profile["enabled"] = json::array();
  profile["disabled"] = json::array();
  for (const auto& k : enabled_set) profile["enabled"].push_back(k);
  for (const auto& k : disabled_set) profile["disabled"].push_back(k);

  std::ofstream out(patches_config_path);
  if (!out.is_open()) return false;
  out << profile.dump(2) << "\n";
  return out.good();
}

TrophyCatalog LoadTrophies(const fs::path& saves_dir, std::string_view ps4_language_id) {
  TrophyCatalog catalog;
  std::error_code ec;

  // 1. Locate <Saves Directory>/trophy/<NPWR05818_00 or latest>
  const fs::path trophy_root = saves_dir / "trophy";
  if (fs::is_directory(trophy_root, ec)) {
    fs::path latest;
    fs::file_time_type latest_time{};
    for (const auto& entry : fs::directory_iterator(trophy_root, ec)) {
      if (!entry.is_directory(ec)) continue;
      bool has_marker = fs::is_regular_file(entry.path() / "unlocked.txt", ec) ||
                        fs::is_regular_file(entry.path() / "TROP.XML", ec);
      if (!has_marker) {
        for (const auto& child : fs::directory_iterator(entry.path(), ec)) {
          const std::string fname = child.path().filename().string();
          if (fname.rfind("TROP_", 0) == 0 && ToLower(child.path().extension().string()) == ".xml") {
            has_marker = true;
            break;
          }
        }
      }
      if (!has_marker) continue;
      if (entry.path().filename() == "NPWR05818_00") {
        catalog.folder = entry.path();
        break;
      }
      auto mtime = fs::last_write_time(entry.path(), ec);
      if (latest.empty() || (!ec && mtime > latest_time)) {
        latest = entry.path();
        latest_time = mtime;
      }
    }
    if (catalog.folder.empty()) {
      catalog.folder = latest;
    }
  }

  // 2. Read <Saves Directory>/trophies.jsonl (real runtime_trophy.c format: trophy_id, unlocked_at)
  std::unordered_map<int, std::string> unlock_times;
  std::set<int> unlocked_ids;
  if (std::ifstream in(saves_dir / "trophies.jsonl"); in.is_open()) {
    std::string line;
    while (std::getline(in, line)) {
      if (Trim(line).empty()) continue;
      try {
        json ev = json::parse(line);
        if (!ev.is_object() || !ev.contains("trophy_id")) continue;
        const int id = ev["trophy_id"].get<int>();
        unlocked_ids.insert(id);
        if (ev.contains("unlocked_at") && ev["unlocked_at"].is_string()) {
          const std::string stamp = ev["unlocked_at"].get<std::string>();
          auto it = unlock_times.find(id);
          if (it == unlock_times.end() || stamp < it->second) {
            unlock_times[id] = stamp;
          }
        }
      } catch (...) {
      }
    }
  }

  // 3. Merge unlocked.txt if present
  if (!catalog.folder.empty()) {
    if (std::ifstream in(catalog.folder / "unlocked.txt"); in.is_open()) {
      std::string line;
      while (std::getline(in, line)) {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        try {
          unlocked_ids.insert(std::stoi(t));
        } catch (...) {
        }
      }
    }
  }

  // 4. Parse TROP_XX.XML (PS4 numeric language ID) -> TROP.XML via shared BbTrophy::ParseTrophies
  std::vector<BbTrophy::Trophy> parsed;
  if (!catalog.folder.empty()) {
    int lang_num = 1;
    try {
      lang_num = std::stoi(std::string(ps4_language_id));
    } catch (...) {
      lang_num = 1;
    }
    std::ostringstream lang_file;
    lang_file << "TROP_" << std::setw(2) << std::setfill('0') << lang_num << ".XML";

    for (const std::string& candidate : {lang_file.str(), std::string("TROP.XML")}) {
      const fs::path xml_path = catalog.folder / candidate;
      if (fs::is_regular_file(xml_path, ec)) {
        parsed = BbTrophy::ParseTrophies(ReadWholeFile(xml_path));
        if (!parsed.empty()) {
          break;
        }
      }
    }
  }

  catalog.named = !parsed.empty();
  if (!catalog.named) {
    // Fallback when no decrypted TROP.XML exists yet: still report unlocked trophy IDs.
    for (int id : unlocked_ids) {
      BbTrophy::Trophy t;
      t.id = id;
      t.grade = '?';
      t.name = "Trophy #" + std::to_string(id);
      parsed.push_back(std::move(t));
    }
  }

  // 5. Build items & compute TrophyStats
  for (const auto& t : parsed) {
    TrophyItem item;
    item.id = t.id;
    item.name = t.name;
    item.detail = t.detail;
    item.hidden = t.hidden;
    item.grade = CharToGrade(t.grade);
    item.grade_str = GradeToString(item.grade);
    item.unlocked = unlocked_ids.count(t.id) > 0;
    if (auto it = unlock_times.find(t.id); it != unlock_times.end()) {
      item.date_str = it->second;
    }
    if (!catalog.folder.empty()) {
      std::ostringstream icon_name;
      icon_name << "TROP" << std::setw(3) << std::setfill('0') << t.id << ".PNG";
      const fs::path icon = catalog.folder / icon_name.str();
      if (fs::is_regular_file(icon, ec)) {
        item.icon_path = icon.string();
      }
    }

    ++catalog.stats.total;
    if (item.unlocked) ++catalog.stats.unlocked;
    switch (item.grade) {
      case TrophyGrade::Platinum:
        ++catalog.stats.platinum_total;
        if (item.unlocked) ++catalog.stats.platinum_unlocked;
        break;
      case TrophyGrade::Gold:
        ++catalog.stats.gold_total;
        if (item.unlocked) ++catalog.stats.gold_unlocked;
        break;
      case TrophyGrade::Silver:
        ++catalog.stats.silver_total;
        if (item.unlocked) ++catalog.stats.silver_unlocked;
        break;
      case TrophyGrade::Bronze:
        ++catalog.stats.bronze_total;
        if (item.unlocked) ++catalog.stats.bronze_unlocked;
        break;
      case TrophyGrade::Unknown:
        break;
    }

    catalog.trophies.push_back(std::move(item));
  }

  if (catalog.stats.total > 0) {
    catalog.stats.progress_pct = (catalog.stats.unlocked * 100) / catalog.stats.total;
  }

  return catalog;
}

} // namespace bbport
