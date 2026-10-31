#pragma once

#include <string>
#include <vector>

namespace bbport {

enum class TrophyGrade {
  Bronze,
  Silver,
  Gold,
  Platinum
};

struct TrophyItem {
  int id = 0;
  std::string name;
  std::string detail;
  std::string grade_str; // "Platinum", "Gold", "Silver", "Bronze"
  std::string icon_path;
  std::string date_str;
  bool unlocked = false;
  bool hidden = false;
  TrophyGrade grade = TrophyGrade::Bronze;
};

struct TrophyStats {
  int total = 0;
  int unlocked = 0;
  int platinum_total = 0;
  int platinum_unlocked = 0;
  int gold_total = 0;
  int gold_unlocked = 0;
  int silver_total = 0;
  int silver_unlocked = 0;
  int bronze_total = 0;
  int bronze_unlocked = 0;
  int progress_pct = 0;
};

class TrophyManager {
public:
  static TrophyManager& Instance();

  void Refresh(const std::string& user_dir, const std::string& lang_code = "en");

  const std::vector<TrophyItem>& GetTrophies() const { return trophies_; }
  const TrophyStats& GetStats() const { return stats_; }

private:
  TrophyManager() = default;

  std::string FindTrophyFolder(const std::string& user_dir) const;
  void ParseTrophyXml(const std::string& xml_path);
  void LoadUnlockedTxt(const std::string& txt_path);
  void LoadTrophiesJsonl(const std::string& user_dir);
  void CalculateStats();

  std::vector<TrophyItem> trophies_;
  TrophyStats stats_;
};

} // namespace bbport
