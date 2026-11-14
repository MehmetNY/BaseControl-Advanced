// Tiny localisation layer: the English text IS the key. A missing translation falls back to the English text, so the
// UI is always readable; tools/check_i18n.py lists every T("...") literal that has no Turkish entry yet.
#pragma once
#include <string>

namespace bc {
namespace ui {

enum class Lang { En, Tr };

void SetLang(Lang l);
Lang GetLang();
Lang LangFromName(const std::string& name, Lang fallback);   // "en" | "tr" | "auto"
Lang SystemLang();                                           // by the Windows UI language

const std::string& T(const std::string& en);
std::string Tf(const std::string& en, ...);                  // printf-style on the translated format string

}  // namespace ui
}  // namespace bc
