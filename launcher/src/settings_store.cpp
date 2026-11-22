// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace bbport {

namespace {

std::string Trim(std::string_view sv) {
  std::size_t start = 0;
  while (start < sv.size() && (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r' || sv[start] == '\n')) {
    ++start;
  }
  std::size_t end = sv.size();
  while (end > start && (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r' || sv[end - 1] == '\n')) {
    --end;
  }
  return std::string(sv.substr(start, end - start));
}

SettingStoreKind ParseStoreKind(const json& item) {
  if (!item.contains("store") || item["store"].is_null()) {
    return SettingStoreKind::None;
  }
  const std::string s = item["store"].get<std::string>();
  if (s == "ini") return SettingStoreKind::GameSetting;
  if (s == "settings") return SettingStoreKind::LauncherSetting;
  return SettingStoreKind::None;
}

SettingType ParseSettingType(const json& item) {
  const std::string t = item.value("type", std::string("text"));
  if (t == "bool") return SettingType::Bool;
  if (t == "int") return SettingType::Int;
  if (t == "float") return SettingType::Float;
  if (t == "choice") return SettingType::Choice;
  if (t == "password") return SettingType::Password;
  if (t == "special") return SettingType::Special;
  return SettingType::Text;
}

EnvEmitMode ParseEnvMode(std::string_view mode) {
  if (mode == "always") return EnvEmitMode::Always;
  if (mode == "zero_one") return EnvEmitMode::ZeroOne;
  if (mode == "true_only") return EnvEmitMode::TrueOnly;
  if (mode == "positive_int") return EnvEmitMode::PositiveInt;
  return EnvEmitMode::NonEmpty;
}

std::unordered_map<std::string, std::string> ParseLangMap(const json& obj) {
  std::unordered_map<std::string, std::string> result;
  if (!obj.is_object()) {
    return result;
  }
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (it.value().is_string()) {
      result.emplace(it.key(), it.value().get<std::string>());
    }
  }
  return result;
}

SettingValue ParseDefaultValue(const json& item, SettingType type) {
  if (!item.contains("default") || item["default"].is_null()) {
    switch (type) {
      case SettingType::Bool: return false;
      case SettingType::Int: return 0;
      case SettingType::Float: return 0.0;
      default: return std::string{};
    }
  }
  const auto& d = item["default"];
  switch (type) {
    case SettingType::Bool:
      if (d.is_boolean()) return d.get<bool>();
      if (d.is_number_integer()) return d.get<int>() != 0;
      if (d.is_string()) {
        const auto s = d.get<std::string>();
        return s == "1" || s == "true";
      }
      return false;
    case SettingType::Int:
      if (d.is_number_integer()) return d.get<int>();
      if (d.is_number()) return static_cast<int>(std::lround(d.get<double>()));
      if (d.is_string()) {
        try { return std::stoi(d.get<std::string>()); } catch (...) { return 0; }
      }
      return 0;
    case SettingType::Float:
      if (d.is_number()) return d.get<double>();
      if (d.is_string()) {
        try { return std::stod(d.get<std::string>()); } catch (...) { return 0.0; }
      }
      return 0.0;
    default:
      if (d.is_string()) return d.get<std::string>();
      if (d.is_boolean()) return std::string(d.get<bool>() ? "1" : "0");
      if (d.is_number_integer()) return std::to_string(d.get<int>());
      if (d.is_number()) return std::to_string(d.get<double>());
      return std::string{};
  }
}

bool ParseBoolFromString(std::string_view s, bool fallback) {
  if (s == "1" || s == "true" || s == "True" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "False" || s == "no" || s == "off") return false;
  return fallback;
}

std::string FormatFloatIni(double v) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2) << v;
  return ss.str();
}

std::string ValueToIniText(const SettingValue& value, SettingType type) {
  if (type == SettingType::Bool || std::holds_alternative<bool>(value)) {
    const bool b = std::holds_alternative<bool>(value)
                       ? std::get<bool>(value)
                       : (std::holds_alternative<int>(value) ? std::get<int>(value) != 0 : false);
    return b ? "1" : "0";
  }
  if (type == SettingType::Float || std::holds_alternative<double>(value)) {
    const double d = std::holds_alternative<double>(value)
                         ? std::get<double>(value)
                         : (std::holds_alternative<int>(value) ? static_cast<double>(std::get<int>(value)) : 0.0);
    return FormatFloatIni(d);
  }
  if (std::holds_alternative<int>(value)) {
    return std::to_string(std::get<int>(value));
  }
  return std::get<std::string>(value);
}

bool WriteFileAtomic(const fs::path& target, const std::string& content, bool mode_0600, std::string* error) {
  std::error_code ec;
  if (target.has_parent_path()) {
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
      if (error) *error = "Failed to create directory: " + target.parent_path().string();
      return false;
    }
  }

#ifdef _WIN32
  (void)mode_0600;
  const fs::path tmp = target.string() + ".tmp";
  {
    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      if (error) *error = "Failed to open temporary file for writing: " + tmp.string();
      return false;
    }
    if (!content.empty() && !ofs.write(content.data(), static_cast<std::streamsize>(content.size()))) {
      ofs.close();
      fs::remove(tmp, ec);
      if (error) *error = "Failed writing file: " + target.string();
      return false;
    }
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(target, ec);
    ec.clear();
    fs::rename(tmp, target, ec);
    if (ec) {
      fs::remove(tmp, ec);
      if (error) *error = "Failed renaming temporary file to: " + target.string();
      return false;
    }
  }
  return true;
#else
  const fs::path tmp = target.string() + ".tmp." + std::to_string(::getpid());
  const int flags = O_WRONLY | O_CREAT | O_TRUNC;
  const mode_t mode = mode_0600 ? 0600 : 0644;
  int fd = ::open(tmp.c_str(), flags, mode);
  if (fd < 0) {
    if (error) *error = "Failed to open temporary file for writing: " + tmp.string();
    return false;
  }
  if (mode_0600) {
    ::fchmod(fd, 0600);
  }

  const char* ptr = content.data();
  std::size_t remaining = content.size();
  while (remaining > 0) {
    ssize_t written = ::write(fd, ptr, remaining);
    if (written < 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      if (error) *error = "Failed writing file: " + target.string();
      return false;
    }
    ptr += written;
    remaining -= static_cast<std::size_t>(written);
  }

  if (::close(fd) != 0) {
    ::unlink(tmp.c_str());
    if (error) *error = "Failed closing temporary file: " + tmp.string();
    return false;
  }

  if (::rename(tmp.c_str(), target.c_str()) != 0) {
    ::unlink(tmp.c_str());
    if (error) *error = "Failed renaming temporary file to: " + target.string();
    return false;
  }
  return true;
#endif
}

// Non-schema auxiliary keys used by GTK/WPF/C++ launchers for complete parity.
struct ExtraKeySpec {
  const char* key;
  SettingStoreKind store;
  SettingType type;
  const char* default_str;
};

constexpr ExtraKeySpec kExtraKeys[] = {
    {"mods_dir", SettingStoreKind::LauncherSetting, SettingType::Special, ""},
    {"patches_dir", SettingStoreKind::LauncherSetting, SettingType::Special, ""},
    {"trophy_key", SettingStoreKind::LauncherSetting, SettingType::Text, ""},
    {"thd_online_id", SettingStoreKind::LauncherSetting, SettingType::Text, ""},
    {"thd_token", SettingStoreKind::LauncherSetting, SettingType::Password, ""},
    {"frame_generation_multiplier", SettingStoreKind::GameSetting, SettingType::Text, "2"},
};

} // namespace

bool SettingsSchema::LoadFromFile(const fs::path& path, SettingsSchema& out, std::string* error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    if (error) *error = "Unable to open settings schema: " + path.string();
    return false;
  }

  json doc;
  try {
    in >> doc;
  } catch (const std::exception& ex) {
    if (error) *error = std::string("Failed to parse settings schema JSON: ") + ex.what();
    return false;
  }

  if (!doc.is_object() || !doc.contains("sections") || !doc["sections"].is_array()) {
    if (error) *error = "Settings schema missing 'sections' array";
    return false;
  }

  SettingsSchema parsed;
  parsed.version_ = doc.value("version", 1);

  const json lists = doc.contains("lists") && doc["lists"].is_object() ? doc["lists"] : json::object();

  auto parse_choices = [](const json& arr) -> std::vector<SettingChoice> {
    std::vector<SettingChoice> out_choices;
    if (!arr.is_array()) {
      return out_choices;
    }
    for (const auto& entry : arr) {
      if (entry.is_array() && !entry.empty()) {
        SettingChoice c;
        c.value = entry[0].is_string() ? entry[0].get<std::string>() : entry[0].dump();
        if (entry.size() >= 2 && entry[1].is_object()) {
          c.labels = ParseLangMap(entry[1]);
        }
        out_choices.push_back(std::move(c));
      } else if (entry.is_object() && entry.contains("value")) {
        SettingChoice c;
        const auto& v = entry["value"];
        c.value = v.is_string() ? v.get<std::string>() : v.dump();
        if (entry.contains("label") && entry["label"].is_object()) {
          c.labels = ParseLangMap(entry["label"]);
        }
        out_choices.push_back(std::move(c));
      } else if (entry.is_string()) {
        SettingChoice c;
        c.value = entry.get<std::string>();
        out_choices.push_back(std::move(c));
      }
    }
    return out_choices;
  };

  for (const auto& sec : doc["sections"]) {
    if (!sec.is_object()) {
      continue;
    }
    SectionDef sdef;
    sdef.id = sec.value("id", std::string{});
    sdef.icon = sec.value("icon", std::string{});
    if (sec.contains("title")) sdef.title = ParseLangMap(sec["title"]);
    if (sec.contains("description")) sdef.description = ParseLangMap(sec["description"]);

    if (sec.contains("items") && sec["items"].is_array()) {
      for (const auto& item : sec["items"]) {
        if (!item.is_object() || !item.contains("key") || !item["key"].is_string()) {
          continue;
        }
        SettingDef def;
        def.key = item["key"].get<std::string>();
        def.section_id = sdef.id;
        def.store = ParseStoreKind(item);
        def.type = ParseSettingType(item);
        def.default_value = ParseDefaultValue(item, def.type);
        if (item.contains("min") && item["min"].is_number()) def.min_value = item["min"].get<double>();
        if (item.contains("max") && item["max"].is_number()) def.max_value = item["max"].get<double>();
        if (item.contains("step") && item["step"].is_number()) def.step = item["step"].get<double>();
        if (item.contains("choices")) {
          def.choices = parse_choices(item["choices"]);
        } else if (item.contains("choices_ref") && item["choices_ref"].is_string()) {
          const std::string ref = item["choices_ref"].get<std::string>();
          if (lists.contains(ref)) {
            def.choices = parse_choices(lists[ref]);
          }
        }
        if (item.contains("env") && item["env"].is_object() && item["env"].contains("var")) {
          EnvMappingSpec spec;
          spec.var = item["env"]["var"].get<std::string>();
          spec.mode = ParseEnvMode(item["env"].value("mode", std::string("non_empty")));
          def.env = std::move(spec);
        }
        if (item.contains("title")) def.title = ParseLangMap(item["title"]);
        if (item.contains("description")) def.description = ParseLangMap(item["description"]);

        sdef.item_keys.push_back(def.key);
        parsed.key_index_[def.key] = parsed.items_.size();
        parsed.items_.push_back(std::move(def));
      }
    }
    parsed.sections_.push_back(std::move(sdef));
  }

  // Register auxiliary non-schema keys used by GTK/WPF/RmlUi if not already in schema.
  for (const auto& extra : kExtraKeys) {
    if (parsed.key_index_.find(extra.key) == parsed.key_index_.end()) {
      SettingDef def;
      def.key = extra.key;
      def.store = extra.store;
      def.type = extra.type;
      def.default_value = std::string(extra.default_str);
      parsed.key_index_[def.key] = parsed.items_.size();
      parsed.items_.push_back(std::move(def));
    }
  }

  out = std::move(parsed);
  return true;
}

const SettingDef* SettingsSchema::Find(std::string_view key) const noexcept {
  auto it = key_index_.find(std::string(key));
  if (it == key_index_.end()) {
    return nullptr;
  }
  return &items_[it->second];
}

struct SettingsStore::Impl {
  SettingsSchema schema;
  PortPaths paths;
  std::unordered_map<std::string, SettingValue> values;
  json raw_settings_json = json::object();
  std::vector<std::string> raw_ini_lines;
  std::unordered_set<std::string> modified_ini_keys;

  void ResetToDefaults() {
    values.clear();
    for (const auto& def : schema.Items()) {
      if (def.store != SettingStoreKind::None) {
        values[def.key] = def.default_value;
      }
    }
  }

  SettingValue CoerceAndClamp(const SettingDef& def, const SettingValue& raw, bool* clamped = nullptr) const {
    if (clamped) *clamped = false;
    switch (def.type) {
      case SettingType::Bool: {
        if (std::holds_alternative<bool>(raw)) return std::get<bool>(raw);
        if (std::holds_alternative<int>(raw)) return std::get<int>(raw) != 0;
        if (std::holds_alternative<double>(raw)) return std::get<double>(raw) != 0.0;
        if (std::holds_alternative<std::string>(raw)) {
          const bool fallback = std::holds_alternative<bool>(def.default_value) ? std::get<bool>(def.default_value) : false;
          return ParseBoolFromString(std::get<std::string>(raw), fallback);
        }
        return def.default_value;
      }
      case SettingType::Int: {
        int v = 0;
        if (std::holds_alternative<int>(raw)) {
          v = std::get<int>(raw);
        } else if (std::holds_alternative<double>(raw)) {
          v = static_cast<int>(std::lround(std::get<double>(raw)));
        } else if (std::holds_alternative<bool>(raw)) {
          v = std::get<bool>(raw) ? 1 : 0;
        } else {
          try {
            v = std::stoi(std::get<std::string>(raw));
          } catch (...) {
            v = std::holds_alternative<int>(def.default_value) ? std::get<int>(def.default_value) : 0;
          }
        }
        if (def.min_value && v < static_cast<int>(*def.min_value)) {
          v = static_cast<int>(*def.min_value);
          if (clamped) *clamped = true;
        }
        if (def.max_value && v > static_cast<int>(*def.max_value)) {
          v = static_cast<int>(*def.max_value);
          if (clamped) *clamped = true;
        }
        return v;
      }
      case SettingType::Float: {
        double v = 0.0;
        if (std::holds_alternative<double>(raw)) {
          v = std::get<double>(raw);
        } else if (std::holds_alternative<int>(raw)) {
          v = static_cast<double>(std::get<int>(raw));
        } else if (std::holds_alternative<std::string>(raw)) {
          try {
            v = std::stod(std::get<std::string>(raw));
          } catch (...) {
            v = std::holds_alternative<double>(def.default_value) ? std::get<double>(def.default_value) : 0.0;
          }
        }
        if (def.min_value && v < *def.min_value) {
          v = *def.min_value;
          if (clamped) *clamped = true;
        }
        if (def.max_value && v > *def.max_value) {
          v = *def.max_value;
          if (clamped) *clamped = true;
        }
        return v;
      }
      default: {
        std::string s;
        if (std::holds_alternative<std::string>(raw)) {
          s = std::get<std::string>(raw);
        } else if (std::holds_alternative<int>(raw)) {
          s = std::to_string(std::get<int>(raw));
        } else if (std::holds_alternative<bool>(raw)) {
          s = std::get<bool>(raw) ? "1" : "0";
        } else {
          s = FormatFloatIni(std::get<double>(raw));
        }
        if (def.type == SettingType::Choice && !def.choices.empty()) {
          const bool ok = std::any_of(def.choices.begin(), def.choices.end(),
                                      [&](const SettingChoice& c) { return c.value == s; });
          if (!ok) {
            if (clamped) *clamped = true;
            return def.default_value;
          }
        }
        return s;
      }
    }
  }

  SettingValue JsonToSettingValue(const SettingDef& def, const json& j) const {
    if (j.is_boolean()) return CoerceAndClamp(def, j.get<bool>());
    if (j.is_number_integer()) return CoerceAndClamp(def, j.get<int>());
    if (j.is_number()) return CoerceAndClamp(def, j.get<double>());
    if (j.is_string()) return CoerceAndClamp(def, j.get<std::string>());
    return def.default_value;
  }

  void ApplyLegacyMigrations() {
    auto migrate = [&](std::string_view key, const std::vector<std::string_view>& old_vals, std::string_view replacement) {
      auto it = values.find(std::string(key));
      if (it == values.end() || !std::holds_alternative<std::string>(it->second)) return;
      const auto& cur = std::get<std::string>(it->second);
      for (auto old_v : old_vals) {
        if (cur == old_v) {
          it->second = std::string(replacement);
          return;
        }
      }
    };
    migrate("online_server", {"srv.shadps4.net:31313"}, "play.seamlesshunt.com:31313");
    migrate("online_community", {"https://thehuntersdream.com", ""}, "http://play.seamlesshunt.com");
    migrate("online_webapi", {""}, "http://play.seamlesshunt.com:31315");
  }
};

SettingsStore::SettingsStore(SettingsSchema schema, PortPaths paths)
    : impl_(std::make_unique<Impl>()) {
  impl_->schema = std::move(schema);
  impl_->paths = std::move(paths);
  impl_->ResetToDefaults();
}

SettingsStore::~SettingsStore() = default;
SettingsStore::SettingsStore(SettingsStore&&) noexcept = default;
SettingsStore& SettingsStore::operator=(SettingsStore&&) noexcept = default;

bool SettingsStore::Load(LoadReport* report, std::string* /*error*/) {
  LoadReport local_report;
  impl_->ResetToDefaults();
  impl_->raw_settings_json = json::object();
  impl_->raw_ini_lines.clear();
  impl_->modified_ini_keys.clear();

  // 1. Load Config Directory / settings.json (Launcher Settings + preserved unknown keys)
  const fs::path settings_path = impl_->paths.SettingsJsonPath();
  if (std::ifstream in(settings_path); in.is_open()) {
    try {
      json parsed;
      in >> parsed;
      if (parsed.is_object()) {
        // Drop obsolete "borderless" key if present from an older C++ launcher build.
        parsed.erase("borderless");
        impl_->raw_settings_json = parsed;
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
          const SettingDef* def = impl_->schema.Find(it.key());
          if (!def || def->store != SettingStoreKind::LauncherSetting) {
            continue;
          }
          impl_->values[def->key] = impl_->JsonToSettingValue(*def, it.value());
          ++local_report.loaded_launcher_settings;
        }
      }
    } catch (const std::exception& ex) {
      local_report.warnings.push_back(std::string("Ignoring malformed settings.json: ") + ex.what());
    }
  }

  // 2. Load Data Directory / bbport.ini (Game Settings + preserved unknown lines/comments)
  const fs::path ini_path = impl_->paths.IniPath();
  if (std::ifstream in(ini_path); in.is_open()) {
    std::string line;
    while (std::getline(in, line)) {
      impl_->raw_ini_lines.push_back(line);
      const std::string trimmed = Trim(line);
      if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
        continue;
      }
      const auto eq = trimmed.find('=');
      if (eq == std::string::npos || eq == 0) {
        continue;
      }
      const std::string key = Trim(std::string_view(trimmed).substr(0, eq));
      const std::string val = Trim(std::string_view(trimmed).substr(eq + 1));
      const SettingDef* def = impl_->schema.Find(key);
      if (!def || def->store != SettingStoreKind::GameSetting) {
        continue;
      }
      bool clamped = false;
      impl_->values[def->key] = impl_->CoerceAndClamp(*def, val, &clamped);
      if (clamped) {
        local_report.warnings.push_back("Clamped out-of-range bbport.ini setting: " + key);
      }
      ++local_report.loaded_game_settings;
    }
  }

  impl_->ApplyLegacyMigrations();

  if (report) {
    *report = std::move(local_report);
  }
  return true;
}

bool SettingsStore::Save(std::string* error) const {
  // 1. Save settings.json (0600, atomic, preserving unknown keys from GTK/WPF)
  json out_json = impl_->raw_settings_json.is_object() ? impl_->raw_settings_json : json::object();
  out_json.erase("borderless");

  for (const auto& def : impl_->schema.Items()) {
    if (def.store != SettingStoreKind::LauncherSetting) {
      continue;
    }
    auto it = impl_->values.find(def.key);
    if (it == impl_->values.end()) {
      continue;
    }
    const auto& val = it->second;
    if (def.type == SettingType::Bool && std::holds_alternative<bool>(val)) {
      out_json[def.key] = std::get<bool>(val);
    } else if (def.type == SettingType::Int && std::holds_alternative<int>(val)) {
      out_json[def.key] = std::get<int>(val);
    } else if (def.type == SettingType::Float && std::holds_alternative<double>(val)) {
      out_json[def.key] = std::get<double>(val);
    } else if (std::holds_alternative<std::string>(val)) {
      out_json[def.key] = std::get<std::string>(val);
    }
  }

  impl_->raw_settings_json = out_json;
  const std::string json_text = out_json.dump(2) + "\n";
  if (!WriteFileAtomic(impl_->paths.SettingsJsonPath(), json_text, /*mode_0600=*/true, error)) {
    return false;
  }

  // 2. Save bbport.ini (preserving comments, blank lines, and unknown keys)
  std::unordered_map<std::string, std::string> ini_values;
  for (const auto& def : impl_->schema.Items()) {
    if (def.store != SettingStoreKind::GameSetting) {
      continue;
    }
    auto it = impl_->values.find(def.key);
    if (it != impl_->values.end()) {
      ini_values[def.key] = ValueToIniText(it->second, def.type);
    }
  }

  std::vector<std::string> new_lines;
  new_lines.reserve(impl_->raw_ini_lines.size() + ini_values.size());
  std::unordered_set<std::string> written_keys;

  for (const auto& line : impl_->raw_ini_lines) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
      new_lines.push_back(line);
      continue;
    }
    const auto eq = trimmed.find('=');
    if (eq == std::string::npos || eq == 0) {
      new_lines.push_back(line);
      continue;
    }
    const std::string key = Trim(std::string_view(trimmed).substr(0, eq));
    auto it = ini_values.find(key);
    if (it != ini_values.end()) {
      new_lines.push_back(key + " = " + it->second);
      written_keys.insert(key);
    } else {
      new_lines.push_back(line);
    }
  }

  // Append newly set or missing Game Settings in stable schema order.
  for (const auto& def : impl_->schema.Items()) {
    if (def.store != SettingStoreKind::GameSetting) {
      continue;
    }
    if (written_keys.count(def.key) == 0) {
      auto it = ini_values.find(def.key);
      if (it != ini_values.end()) {
        // Avoid writing auxiliary frame_generation_multiplier unless explicitly modified.
        if (def.key == "frame_generation_multiplier" && impl_->modified_ini_keys.count(def.key) == 0) {
          continue;
        }
        new_lines.push_back(def.key + " = " + it->second);
        written_keys.insert(def.key);
      }
    }
  }

  std::ostringstream ini_buf;
  for (const auto& line : new_lines) {
    ini_buf << line << "\n";
  }

  impl_->raw_ini_lines = std::move(new_lines);
  return WriteFileAtomic(impl_->paths.IniPath(), ini_buf.str(), /*mode_0600=*/false, error);
}

const SettingsSchema& SettingsStore::Schema() const noexcept { return impl_->schema; }
const PortPaths& SettingsStore::Paths() const noexcept { return impl_->paths; }

std::optional<SettingValue> SettingsStore::Get(std::string_view key) const {
  auto it = impl_->values.find(std::string(key));
  if (it == impl_->values.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool SettingsStore::GetBool(std::string_view key, bool fallback) const {
  auto v = Get(key);
  if (!v) return fallback;
  if (std::holds_alternative<bool>(*v)) return std::get<bool>(*v);
  if (std::holds_alternative<int>(*v)) return std::get<int>(*v) != 0;
  if (std::holds_alternative<std::string>(*v)) return ParseBoolFromString(std::get<std::string>(*v), fallback);
  return fallback;
}

int SettingsStore::GetInt(std::string_view key, int fallback) const {
  auto v = Get(key);
  if (!v) return fallback;
  if (std::holds_alternative<int>(*v)) return std::get<int>(*v);
  if (std::holds_alternative<double>(*v)) return static_cast<int>(std::lround(std::get<double>(*v)));
  if (std::holds_alternative<bool>(*v)) return std::get<bool>(*v) ? 1 : 0;
  try {
    return std::stoi(std::get<std::string>(*v));
  } catch (...) {
    return fallback;
  }
}

double SettingsStore::GetDouble(std::string_view key, double fallback) const {
  auto v = Get(key);
  if (!v) return fallback;
  if (std::holds_alternative<double>(*v)) return std::get<double>(*v);
  if (std::holds_alternative<int>(*v)) return static_cast<double>(std::get<int>(*v));
  try {
    return std::stod(std::get<std::string>(*v));
  } catch (...) {
    return fallback;
  }
}

std::string SettingsStore::GetString(std::string_view key, std::string_view fallback) const {
  auto v = Get(key);
  if (!v) return std::string(fallback);
  if (std::holds_alternative<std::string>(*v)) return std::get<std::string>(*v);
  if (std::holds_alternative<bool>(*v)) return std::get<bool>(*v) ? "1" : "0";
  if (std::holds_alternative<int>(*v)) return std::to_string(std::get<int>(*v));
  return FormatFloatIni(std::get<double>(*v));
}

bool SettingsStore::Set(std::string_view key, SettingValue value, std::string* error) {
  const SettingDef* def = impl_->schema.Find(key);
  if (!def || def->store == SettingStoreKind::None) {
    if (error) *error = "Unknown or non-storable setting key: " + std::string(key);
    return false;
  }
  impl_->values[def->key] = impl_->CoerceAndClamp(*def, value);
  if (def->store == SettingStoreKind::GameSetting) {
    impl_->modified_ini_keys.insert(def->key);
  }
  return true;
}

fs::path SettingsStore::ResolvedGameDir() const {
  return ExpandTilde(GetString("game_dir"));
}

fs::path SettingsStore::ResolvedSavesDir() const {
  const std::string raw = Trim(GetString("user_dir"));
  if (raw.empty()) {
    return impl_->paths.data_dir / "user";
  }
  fs::path p = ExpandTilde(raw);
  return p.is_absolute() ? p : (impl_->paths.data_dir / p);
}

fs::path SettingsStore::ResolvedModsDir() const {
  const std::string raw = Trim(GetString("mods_dir"));
  if (raw.empty()) {
    return impl_->paths.data_dir / "mods";
  }
  fs::path p = ExpandTilde(raw);
  return p.is_absolute() ? p : (impl_->paths.data_dir / p);
}

fs::path SettingsStore::ResolvedPatchesDir() const {
  const std::string raw = Trim(GetString("patches_dir"));
  if (raw.empty()) {
    return impl_->paths.data_dir / "patches";
  }
  fs::path p = ExpandTilde(raw);
  return p.is_absolute() ? p : (impl_->paths.data_dir / p);
}

std::string SettingsStore::ResolvedUiLanguage(const SystemProbe& probe) const {
  const std::string configured = Trim(GetString("ui_language"));
  if (!configured.empty()) {
    return configured;
  }
  return probe.DetectSystemLanguage();
}

} // namespace bbport
