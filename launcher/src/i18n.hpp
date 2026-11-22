// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace bbport {

enum class Language {
  English,
  Portuguese,
  Russian,
  Chinese,
  French
};

struct LanguageOption {
  std::string code;
  std::string native_name;
  Language lang;
};

class I18n {
public:
  I18n();

  static I18n& Instance();

  void SetLanguage(Language lang);
  void SetLanguageByCode(const std::string& code);
  Language GetLanguage() const { return current_lang_; }
  std::string GetLanguageCode() const;

  const std::string& Translate(const std::string& key) const;

  static const std::vector<LanguageOption>& GetAvailableLanguages();

private:
  void LoadTranslations();

  Language current_lang_ = Language::English;
  std::unordered_map<std::string, std::unordered_map<Language, std::string>> strings_;
};

} // namespace bbport
