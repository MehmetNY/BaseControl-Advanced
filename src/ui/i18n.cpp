#include "i18n.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace bc {
namespace ui {
namespace {
Lang g_lang = Lang::En;

struct Pair {
  const char* en;
  const char* tr;
};

const Pair kTr[] = {
#include "i18n_tr.inc"
};

const std::unordered_map<std::string, std::string>& Table() {
  static const std::unordered_map<std::string, std::string> m = [] {
    std::unordered_map<std::string, std::string> t;
    for (const Pair& p : kTr) t[p.en] = p.tr;
    return t;
  }();
  return m;
}
}  // namespace

void SetLang(Lang l) { g_lang = l; }
Lang GetLang() { return g_lang; }

Lang SystemLang() {
  const LANGID id = GetUserDefaultUILanguage();
  return PRIMARYLANGID(id) == LANG_TURKISH ? Lang::Tr : Lang::En;
}

Lang LangFromName(const std::string& name, Lang fallback) {
  if (name == "tr") return Lang::Tr;
  if (name == "en") return Lang::En;
  if (name == "auto" || name.empty()) return SystemLang();
  return fallback;
}

const std::string& T(const std::string& en) {
  if (g_lang == Lang::En) return en;
  const auto& t = Table();
  auto it = t.find(en);
  return it == t.end() ? en : it->second;
}

std::string Tf(const std::string& en, ...) {
  const std::string& fmt = T(en);
  char buf[1024];
  va_list ap;
  va_start(ap, en);
  std::vsnprintf(buf, sizeof buf, fmt.c_str(), ap);
  va_end(ap);
  return buf;
}

}  // namespace ui
}  // namespace bc
