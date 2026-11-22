// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "port_paths.hpp"
#include "system_probe.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bbport {

using SettingValue = std::variant<bool, int, double, std::string>;

enum class SettingStoreKind { GameSetting, LauncherSetting, None };
enum class SettingType { Bool, Int, Float, Choice, Text, Password, Special };
enum class EnvEmitMode { Always, NonEmpty, ZeroOne, TrueOnly, PositiveInt };

struct EnvMappingSpec {
  std::string var;
  EnvEmitMode mode = EnvEmitMode::NonEmpty;
};

struct SettingChoice {
  std::string value;
  std::unordered_map<std::string, std::string> labels;
};

struct SettingDef {
  std::string key;
  std::string section_id;
  SettingStoreKind store = SettingStoreKind::LauncherSetting;
  SettingType type = SettingType::Text;
  SettingValue default_value;
  std::optional<double> min_value;
  std::optional<double> max_value;
  std::optional<double> step;
  std::vector<SettingChoice> choices;
  std::optional<EnvMappingSpec> env;
  std::unordered_map<std::string, std::string> title;
  std::unordered_map<std::string, std::string> description;
};

struct SectionDef {
  std::string id;
  std::string icon;
  std::unordered_map<std::string, std::string> title;
  std::unordered_map<std::string, std::string> description;
  std::vector<std::string> item_keys;
};

class SettingsSchema {
public:
  static bool LoadFromFile(const std::filesystem::path& path, SettingsSchema& out, std::string* error = nullptr);

  int Version() const noexcept { return version_; }
  const std::vector<SectionDef>& Sections() const noexcept { return sections_; }
  const std::vector<SettingDef>& Items() const noexcept { return items_; }
  const SettingDef* Find(std::string_view key) const noexcept;

private:
  int version_ = 1;
  std::vector<SectionDef> sections_;
  std::vector<SettingDef> items_;
  std::unordered_map<std::string, std::size_t> key_index_;
};

struct LoadReport {
  std::size_t loaded_game_settings = 0;
  std::size_t loaded_launcher_settings = 0;
  std::vector<std::string> warnings;
};

// Deep standalone module for schema-backed Game Settings (bbport.ini) and Launcher Settings (settings.json).
class SettingsStore {
public:
  SettingsStore(SettingsSchema schema, PortPaths paths);
  ~SettingsStore();
  SettingsStore(SettingsStore&&) noexcept;
  SettingsStore& operator=(SettingsStore&&) noexcept;
  SettingsStore(const SettingsStore&) = delete;
  SettingsStore& operator=(const SettingsStore&) = delete;

  bool Load(LoadReport* report = nullptr, std::string* error = nullptr);
  bool Save(std::string* error = nullptr) const;

  const SettingsSchema& Schema() const noexcept;
  const PortPaths& Paths() const noexcept;

  std::optional<SettingValue> Get(std::string_view key) const;
  bool GetBool(std::string_view key, bool fallback = false) const;
  int GetInt(std::string_view key, int fallback = 0) const;
  double GetDouble(std::string_view key, double fallback = 0.0) const;
  std::string GetString(std::string_view key, std::string_view fallback = {}) const;

  // Validates, coerces, and clamps against SettingsSchema. Returns false for unknown keys.
  bool Set(std::string_view key, SettingValue value, std::string* error = nullptr);

  // Derived canonical directories (GLOSSARY.md).
  std::filesystem::path ResolvedGameDir() const;
  std::filesystem::path ResolvedSavesDir() const;
  std::filesystem::path ResolvedModsDir() const;
  std::filesystem::path ResolvedPatchesDir() const;
  std::string ResolvedUiLanguage(const SystemProbe& probe) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace bbport
