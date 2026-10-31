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
  static I18n& Instance();

  void SetLanguage(Language lang);
  void SetLanguageByCode(const std::string& code);
  Language GetLanguage() const { return current_lang_; }
  std::string GetLanguageCode() const;

  const std::string& Translate(const std::string& key) const;

  static const std::vector<LanguageOption>& GetAvailableLanguages();

private:
  I18n();
  void LoadTranslations();

  Language current_lang_ = Language::English;
  // Key -> (Language -> Translated string)
  std::unordered_map<std::string, std::unordered_map<Language, std::string>> strings_;
};

inline const std::string& tr(const std::string& key) {
  return I18n::Instance().Translate(key);
}

} // namespace bbport
