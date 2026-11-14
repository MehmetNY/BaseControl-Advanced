#include "kv.h"

#include "util.h"

namespace bc {

void KV::SetF(const std::string& k, double v, int decimals) {
  m_[k] = Fmt("%.*f", decimals, v);
}

std::string KV::Str(const std::string& k, const std::string& def) const {
  auto it = m_.find(k);
  return it == m_.end() ? def : it->second;
}

long long KV::Int(const std::string& k, long long def) const {
  auto it = m_.find(k);
  long long v;
  if (it != m_.end() && ParseInt(it->second, v)) return v;
  double d;
  if (it != m_.end() && ParseDouble(it->second, d)) return static_cast<long long>(d);
  return def;
}

double KV::Num(const std::string& k, double def) const {
  auto it = m_.find(k);
  double v;
  if (it != m_.end() && ParseDouble(it->second, v)) return v;
  return def;
}

bool KV::Bool(const std::string& k, bool def) const {
  auto it = m_.find(k);
  bool v;
  if (it != m_.end() && ParseBool(it->second, v)) return v;
  return def;
}

std::string KV::Serialize() const {
  std::string out;
  for (auto& p : m_) {
    out += p.first;
    out += '=';
    out += Escape(p.second);
    out += '\n';
  }
  return out;
}

KV KV::Parse(const std::string& text) {
  KV kv;
  size_t i = 0;
  while (i < text.size()) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string line = text.substr(i, j - i);
    i = j + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t e = line.find('=');
    if (e == std::string::npos || e == 0) continue;
    kv.m_[line.substr(0, e)] = Unescape(line.substr(e + 1));
  }
  return kv;
}

}  // namespace bc
