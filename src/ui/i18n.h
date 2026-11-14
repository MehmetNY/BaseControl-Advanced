// Tiny localisation layer: the English text IS the key. A missing translation falls back to the English text, so the
// UI is always readable; tools/check_i18n.py lists every T("...") literal that has no Turkish entry yet.
#pragma once
#include <string>

#include "../common/i18n_mark.h"

namespace bc {
namespace ui {

enum class Lang { En, Tr };

void SetLang(Lang l);
Lang GetLang();
Lang LangFromName(const std::string& name, Lang fallback);   // "en" | "tr" | "auto"
Lang SystemLang();                                           // by the Windows UI language

const std::string& T(const std::string& en);
std::string Tf(const std::string& en, ...);                  // printf-style on the translated format string

// A message the controller produced (why a setting is unavailable, why a change was refused). Most are plain table entries; the ones
// built with Fmt(N_("... %d ...")) are matched against the table entries that hold printf specifiers and their Turkish text is filled
// with the pieces that matched. A message without an entry (a driver's own error text) stays as it is. Its first word is
// capitalised, because the controller writes them in the lower-case style of a log line.
std::string TMsg(const std::string& msg);

}  // namespace ui
}  // namespace bc
