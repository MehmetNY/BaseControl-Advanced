// Ordered key=value store used for telemetry snapshots, IPC payloads and config files.
#pragma once
#include <map>
#include <string>

namespace bc {

class KV {
 public:
  void Set(const std::string& k, const std::string& v) { m_[k] = v; }
  void SetI(const std::string& k, long long v) { m_[k] = std::to_string(v); }
  void SetF(const std::string& k, double v, int decimals = 1);
  void SetB(const std::string& k, bool v) { m_[k] = v ? "1" : "0"; }
  bool Has(const std::string& k) const { return m_.count(k) != 0; }
  void Erase(const std::string& k) { m_.erase(k); }
  void Clear() { m_.clear(); }
  size_t Size() const { return m_.size(); }

  std::string Str(const std::string& k, const std::string& def = "") const;
  long long Int(const std::string& k, long long def = 0) const;
  double Num(const std::string& k, double def = 0.0) const;
  bool Bool(const std::string& k, bool def = false) const;

  const std::map<std::string, std::string>& All() const { return m_; }
  void Merge(const KV& o) { for (auto& p : o.m_) m_[p.first] = p.second; }

  // "k=v\n" lines; values are Escape()d. Lines starting with '#' are comments when parsing.
  std::string Serialize() const;
  static KV Parse(const std::string& text);

 private:
  std::map<std::string, std::string> m_;
};

}  // namespace bc
