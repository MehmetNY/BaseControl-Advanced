#include "profile_text.h"

#include "util.h"

namespace bc {

const std::vector<std::string>& BuiltinProfiles() {
  static const std::vector<std::string> v = {"Quiet", "Balanced", "Enthusiast", "Overboost"};
  return v;
}

bool IsBuiltinProfile(const std::string& name) {
  for (const std::string& b : BuiltinProfiles())
    if (EqNoCase(name, b)) return true;
  return false;
}

std::string FormatProfileValues(const KV& values) {
  std::string out;
  for (auto& kv : values.All()) {
    if (!out.empty()) out += '|';
    out += Escape(kv.first) + "=" + Escape(kv.second);
  }
  return out;
}

KV ParseProfileValues(const std::string& text) {
  KV out;
  for (const std::string& part : Split(text, '|')) {
    const size_t eq = part.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string k = Unescape(part.substr(0, eq));
    if (!k.empty()) out.Set(k, Unescape(part.substr(eq + 1)));
  }
  return out;
}

std::vector<std::string> ProfileList(const KV& snap) {
  std::vector<std::string> out;
  for (const std::string& n : Split(snap.Str("profile.list"), ',')) {
    const std::string t = Trim(n);
    if (t.empty()) continue;
    bool dup = false;
    for (const std::string& o : out) dup = dup || o == t;
    if (!dup) out.push_back(t);
  }
  return out;
}

}  // namespace bc
