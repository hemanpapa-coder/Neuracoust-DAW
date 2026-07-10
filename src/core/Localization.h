#pragma once

#include <string>

namespace neuracoust::daw {

enum class UiLanguage {
    Korean,
    English,
    Japanese,
    ChineseSimplified
};

UiLanguage uiLanguageFromLocaleTag(const std::string& localeTag);
void setUiLanguageFromLocaleTag(const std::string& localeTag);
void setUiLanguage(UiLanguage language);
UiLanguage currentUiLanguage();
std::string currentUiLanguageCode();
std::string tr(const std::string& key);

} // namespace neuracoust::daw
