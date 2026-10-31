#include "trophy_manager.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace bbport {

static const std::string kBloodborneNpId = "NPWR05818_00";

TrophyManager& TrophyManager::Instance() {
  static TrophyManager instance;
  return instance;
}

std::string TrophyManager::FindTrophyFolder(const std::string& user_dir) const {
  if (user_dir.empty()) return "";
  fs::path root = fs::path(user_dir) / "trophy";
  if (!fs::exists(root) || !fs::is_directory(root)) return "";

  fs::path bb_folder = root / kBloodborneNpId;
  if (fs::exists(bb_folder) && fs::is_directory(bb_folder)) {
    return bb_folder.string();
  }

  // Fallback: search for any directory containing unlocked.txt or TROP.XML
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.is_directory()) {
      if (fs::exists(entry.path() / "unlocked.txt") || fs::exists(entry.path() / "TROP.XML")) {
        return entry.path().string();
      }
    }
  }
  return "";
}

void TrophyManager::Refresh(const std::string& user_dir, const std::string& lang_code) {
  trophies_.clear();
  stats_ = TrophyStats{};

  std::string folder = FindTrophyFolder(user_dir);
  if (folder.empty()) {
    return;
  }

  // 1. Pick appropriate XML: TROP_XX.XML or TROP.XML
  fs::path xml_path;
  if (!lang_code.empty()) {
    std::string prefix = lang_code.substr(0, 2);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::toupper);
    fs::path lang_xml = fs::path(folder) / ("TROP_" + prefix + ".XML");
    if (fs::exists(lang_xml)) {
      xml_path = lang_xml;
    }
  }
  if (xml_path.empty()) {
    xml_path = fs::path(folder) / "TROP.XML";
  }

  if (fs::exists(xml_path)) {
    ParseTrophyXml(xml_path.string());
  }

  // 2. Read unlocked.txt
  fs::path unlocked_path = fs::path(folder) / "unlocked.txt";
  if (fs::exists(unlocked_path)) {
    LoadUnlockedTxt(unlocked_path.string());
  }

  // 3. Read trophies.jsonl for unlock timestamps
  LoadTrophiesJsonl(user_dir);

  // 4. Fill icon paths
  for (auto& item : trophies_) {
    std::ostringstream ss;
    ss << "TROP" << std::setw(3) << std::setfill('0') << item.id << ".PNG";
    fs::path icon = fs::path(folder) / ss.str();
    if (fs::exists(icon)) {
      item.icon_path = icon.string();
    }
  }

  CalculateStats();
}

void TrophyManager::ParseTrophyXml(const std::string& xml_path) {
  std::ifstream file(xml_path);
  if (!file.is_open()) return;

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  size_t pos = 0;
  while ((pos = content.find("<trophy ", pos)) != std::string::npos) {
    size_t end_trophy = content.find("</trophy>", pos);
    if (end_trophy == std::string::npos) break;

    std::string block = content.substr(pos, end_trophy - pos + 9);

    TrophyItem item;

    // Parse attributes: id="X" hidden="yes/no" ttype="P/G/S/B"
    auto get_attr = [&](const std::string& attr) -> std::string {
      std::string needle = attr + "=\"";
      size_t a = block.find(needle);
      if (a == std::string::npos) return "";
      a += needle.length();
      size_t b = block.find('"', a);
      if (b == std::string::npos) return "";
      return block.substr(a, b - a);
    };

    std::string id_str = get_attr("id");
    std::string hidden_str = get_attr("hidden");
    std::string ttype_str = get_attr("ttype");

    if (!id_str.empty()) {
      try { item.id = std::stoi(id_str); } catch (...) {}
    }
    item.hidden = (hidden_str == "yes");

    if (ttype_str == "P") {
      item.grade = TrophyGrade::Platinum;
      item.grade_str = "Platinum";
    } else if (ttype_str == "G") {
      item.grade = TrophyGrade::Gold;
      item.grade_str = "Gold";
    } else if (ttype_str == "S") {
      item.grade = TrophyGrade::Silver;
      item.grade_str = "Silver";
    } else {
      item.grade = TrophyGrade::Bronze;
      item.grade_str = "Bronze";
    }

    // Parse tags: <name>...</name>, <detail>...</detail>
    auto get_tag = [&](const std::string& tag) -> std::string {
      std::string open = "<" + tag + ">";
      std::string close = "</" + tag + ">";
      size_t a = block.find(open);
      if (a == std::string::npos) return "";
      a += open.length();
      size_t b = block.find(close, a);
      if (b == std::string::npos) return "";
      return block.substr(a, b - a);
    };

    item.name = get_tag("name");
    item.detail = get_tag("detail");

    trophies_.push_back(item);
    pos = end_trophy + 9;
  }
}

void TrophyManager::LoadUnlockedTxt(const std::string& txt_path) {
  std::ifstream file(txt_path);
  if (!file.is_open()) return;

  std::unordered_set<int> unlocked_ids;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    try {
      unlocked_ids.insert(std::stoi(line));
    } catch (...) {}
  }

  for (auto& item : trophies_) {
    if (unlocked_ids.count(item.id)) {
      item.unlocked = true;
    }
  }
}

void TrophyManager::LoadTrophiesJsonl(const std::string& user_dir) {
  fs::path jsonl_path = fs::path(user_dir) / "trophies.jsonl";
  if (!fs::exists(jsonl_path)) return;

  std::ifstream file(jsonl_path);
  if (!file.is_open()) return;

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    // Extract "id": X and "time": "..."
    size_t id_pos = line.find("\"id\":");
    size_t time_pos = line.find("\"time\":");
    if (id_pos != std::string::npos) {
      int id = -1;
      try {
        id = std::stoi(line.substr(id_pos + 5));
      } catch (...) {}

      std::string time_str;
      if (time_pos != std::string::npos) {
        size_t q1 = line.find('"', time_pos + 7);
        if (q1 != std::string::npos) {
          size_t q2 = line.find('"', q1 + 1);
          if (q2 != std::string::npos) {
            time_str = line.substr(q1 + 1, q2 - q1 - 1);
          }
        }
      }

      if (id >= 0) {
        for (auto& item : trophies_) {
          if (item.id == id) {
            item.unlocked = true;
            if (!time_str.empty()) {
              item.date_str = time_str;
            }
            break;
          }
        }
      }
    }
  }
}

void TrophyManager::CalculateStats() {
  stats_.total = static_cast<int>(trophies_.size());
  stats_.unlocked = 0;
  stats_.platinum_total = 0;
  stats_.platinum_unlocked = 0;
  stats_.gold_total = 0;
  stats_.gold_unlocked = 0;
  stats_.silver_total = 0;
  stats_.silver_unlocked = 0;
  stats_.bronze_total = 0;
  stats_.bronze_unlocked = 0;

  for (const auto& item : trophies_) {
    switch (item.grade) {
      case TrophyGrade::Platinum:
        stats_.platinum_total++;
        if (item.unlocked) stats_.platinum_unlocked++;
        break;
      case TrophyGrade::Gold:
        stats_.gold_total++;
        if (item.unlocked) stats_.gold_unlocked++;
        break;
      case TrophyGrade::Silver:
        stats_.silver_total++;
        if (item.unlocked) stats_.silver_unlocked++;
        break;
      case TrophyGrade::Bronze:
        stats_.bronze_total++;
        if (item.unlocked) stats_.bronze_unlocked++;
        break;
    }
    if (item.unlocked) {
      stats_.unlocked++;
    }
  }

  if (stats_.total > 0) {
    stats_.progress_pct = (stats_.unlocked * 100) / stats_.total;
  }
}

} // namespace bbport
