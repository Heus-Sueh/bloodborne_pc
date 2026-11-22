// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bbport {

struct GameInspection {
  bool valid = false;
  bool has_eboot = false;
  bool has_dvdroot = false;
  bool is_109 = false;
  std::string title_id;
  std::string version;
  std::filesystem::path resolved_dir;
  std::string status_key = "status_invalid"; // "status_ready" | "status_update_needed" | "status_invalid"
};

struct ModItem {
  std::string id;
  std::string name;
  std::string author;
  std::string path;
  bool enabled = true;
  int order = 0;
};

struct PatchItem {
  std::string id; // "<filename>/<Metadata Name>"
  std::string name;
  std::string author;
  std::string path;
  bool default_enabled = false;
  bool enabled = false;
};

enum class TrophyGrade { Bronze, Silver, Gold, Platinum, Unknown };

struct TrophyItem {
  int id = 0;
  std::string name;
  std::string detail;
  std::string grade_str;
  std::string icon_path;
  std::string date_str;
  bool unlocked = false;
  bool hidden = false;
  TrophyGrade grade = TrophyGrade::Unknown;
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

struct TrophyCatalog {
  std::filesystem::path folder;
  bool named = false;
  std::vector<TrophyItem> trophies;
  TrophyStats stats;
};

// Single-pass Game Directory inspection (eboot.bin, dvdroot_ps4, binary sce_sys/param.sfo).
GameInspection InspectGameDirectory(const std::filesystem::path& game_dir);

// Real on-disk Mods contract (scripts/mods.py content_root + Data Directory/mods.json).
std::vector<ModItem> LoadMods(const std::filesystem::path& mods_dir,
                              const std::filesystem::path& mods_config_path);
bool SaveModsConfig(const std::filesystem::path& mods_config_path, const std::vector<ModItem>& mods);

// Real on-disk External Patches contract (shadPS4 XML Metadata + Data Directory/patches.json deltas).
std::vector<PatchItem> LoadExternalPatches(const std::filesystem::path& patches_dir,
                                           const std::filesystem::path& patches_config_path,
                                           const std::filesystem::path& builtin_xml_to_exclude);
bool SaveExternalPatchesConfig(const std::filesystem::path& patches_config_path,
                               const std::vector<PatchItem>& patches);

// Real on-disk Trophies contract (BbTrophy XML parser + PS4 TROP_XX.XML + unlocked.txt + trophies.jsonl).
TrophyCatalog LoadTrophies(const std::filesystem::path& saves_dir, std::string_view ps4_language_id);

struct SaveSlotItem {
  std::string id;             // "<user_id>/<title_id>/<file_name>", e.g. "1/CUSA00207/userdata0000"
  std::string user_id;        // e.g. "1"
  std::string title_id;       // e.g. "CUSA00207"
  std::string slot_label;     // e.g. "Character Slot 0" or "System Profile"
  std::string file_name;      // e.g. "userdata0000"
  std::string backup_name;    // e.g. "backup0000"
  std::string size_str;       // e.g. "1.25 MB"
  std::string modified_str;   // e.g. "2026-11-22 14:54:12"
  std::string backup_status;  // e.g. "Internal backup0000 ready (2026-11-22 14:53:09)"
  bool has_internal_backup = false;
  bool is_system_slot = false;
  bool sound_flag_ok = true;
};

struct SaveBackupItem {
  std::string id;             // directory name under savedata-backups/, e.g. "20261122-172500"
  std::string label;          // human-readable label
  std::string path;           // full path on disk
  std::string date_str;       // formatted timestamp
  std::string details_str;    // e.g. "CUSA00207 • 2 character slots • 2.75 MB"
};

struct SaveSummary {
  bool has_saves = false;
  std::string active_title_id;
  std::string total_size_str = "0 B";
  std::string last_modified = "Never";
  int character_slots = 0;
  int system_slots = 0;
  int backup_count = 0;
  bool sound_flag_ok = true;
  std::string status_line = "No active save profile found";
};

// Real on-disk Bloodborne Save Data Management contract (<saves_dir>/savedata/<uid>/<CUSA*>/SPRJ0005).
SaveSummary InspectSaves(const std::filesystem::path& saves_dir,
                         std::vector<SaveSlotItem>& out_slots,
                         std::vector<SaveBackupItem>& out_backups);

bool CreateSaveBackup(const std::filesystem::path& saves_dir,
                      std::string_view tag,
                      std::string* out_backup_id = nullptr,
                      std::string* out_error = nullptr);

bool RestoreSaveBackup(const std::filesystem::path& saves_dir,
                       std::string_view backup_id,
                       std::string* out_error = nullptr);

bool DeleteSaveBackup(const std::filesystem::path& saves_dir,
                      std::string_view backup_id,
                      std::string* out_error = nullptr);

bool RestoreInternalSlotBackup(const std::filesystem::path& saves_dir,
                               std::string_view slot_id,
                               std::string* out_error = nullptr);

int RepairSaveSoundFlags(const std::filesystem::path& saves_dir);

bool ImportExternalSaveFolder(const std::filesystem::path& saves_dir,
                              const std::filesystem::path& external_dir,
                              std::string_view default_title_id,
                              std::string* out_error = nullptr);

} // namespace bbport
