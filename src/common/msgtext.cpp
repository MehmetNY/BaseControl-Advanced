#include "msgtext.h"

#include <algorithm>
#include <cstring>

namespace bc {
namespace {

bool IsConversion(char c) { return c != 0 && std::strchr("diuxXfFeEgGcsp", c) != nullptr; }

// Finds the next conversion specifier at or after `from`. "%%" is a literal percent sign and is skipped.
bool NextSpec(const std::string& s, size_t from, size_t* at, size_t* len) {
  for (size_t i = from; i < s.size(); ++i) {
    if (s[i] != '%') continue;
    if (i + 1 < s.size() && s[i + 1] == '%') {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < s.size() && s[j] != 0 && std::strchr("-+ #0123456789.", s[j])) ++j;
    while (j < s.size() && (s[j] == 'l' || s[j] == 'h' || s[j] == 'z')) ++j;
    if (j < s.size() && IsConversion(s[j])) {
      *at = i;
      *len = j + 1 - i;
      return true;
    }
  }
  return false;
}

std::string Unescape(const std::string& s) {   // "%%" -> "%"
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    o += s[i];
    if (s[i] == '%' && i + 1 < s.size() && s[i + 1] == '%') ++i;
  }
  return o;
}

}  // namespace

void MsgTemplates::Add(const std::string& english, const std::string& translated) {
  Tmpl t;
  size_t pos = 0, at = 0, len = 0;
  while (NextSpec(english, pos, &at, &len)) {
    t.lits.push_back(Unescape(english.substr(pos, at - pos)));
    pos = at + len;
  }
  if (t.lits.empty()) return;
  t.lits.push_back(Unescape(english.substr(pos)));
  for (const std::string& l : t.lits) t.weight += l.size();
  if (t.weight < 10) return;
  t.translated = translated;
  const auto at_ = std::upper_bound(t_.begin(), t_.end(), t, [](const Tmpl& a, const Tmpl& b) { return a.weight > b.weight; });
  t_.insert(at_, std::move(t));
}

bool MsgTemplates::Translate(const std::string& message, std::string* out) const {
  std::vector<std::string> parts;
  for (const Tmpl& t : t_) {
    const std::vector<std::string>& L = t.lits;
    parts.clear();
    if (message.size() < L[0].size() || message.compare(0, L[0].size(), L[0]) != 0) continue;
    size_t pos = L[0].size();
    bool ok = true;
    for (size_t i = 1; i < L.size() && ok; ++i) {
      if (i + 1 == L.size()) {   // the last literal ends the message
        if (message.size() < pos + L[i].size() || message.compare(message.size() - L[i].size(), L[i].size(), L[i]) != 0) {
          ok = false;
          break;
        }
        parts.push_back(message.substr(pos, message.size() - L[i].size() - pos));
      } else {
        if (L[i].empty()) {      // two specifiers in a row cannot be told apart
          ok = false;
          break;
        }
        const size_t f = message.find(L[i], pos);
        if (f == std::string::npos) {
          ok = false;
          break;
        }
        parts.push_back(message.substr(pos, f - pos));
        pos = f + L[i].size();
      }
    }
    if (!ok) continue;
    // fill the translated text: each of its specifiers takes the next matched piece
    std::string o;
    size_t p = 0, at = 0, len = 0, k = 0;
    while (NextSpec(t.translated, p, &at, &len)) {
      o += Unescape(t.translated.substr(p, at - p));
      if (k < parts.size()) o += parts[k];
      ++k;
      p = at + len;
    }
    o += Unescape(t.translated.substr(p));
    *out = o;
    return true;
  }
  return false;
}

std::string CapitalizeMessage(std::string s) {
  size_t i = 0;
  while (i < s.size() && s[i] != ' ' && s[i] != '\n') {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return s;
    ++i;
  }
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
  return s;
}

}  // namespace bc
