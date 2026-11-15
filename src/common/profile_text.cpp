#include "profile_text.h"

#include "util.h"

namespace bc {

const std::vector<std::string>& BuiltinProfiles() {
  static const std::vector<std::string> v = {"Balanced", "Enthusiast", "Overboost", "Customized"};
  return v;
}

bool IsBuiltinProfile(const std::string& name) {
  for (const std::string& b : BuiltinProfiles())
    if (EqNoCase(name, b)) return true;
  return false;
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
