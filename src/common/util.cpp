#include "util.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bc {

std::string Fmt(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string out;
  if (n > 0) {
    out.resize(static_cast<size_t>(n));
    vsnprintf(&out[0], static_cast<size_t>(n) + 1, fmt, ap2);
  }
  va_end(ap2);
  return out;
}

std::vector<std::string> Split(const std::string& s, char sep, bool keepEmpty) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(sep, i);
    if (j == std::string::npos) j = s.size();
    std::string part = s.substr(i, j - i);
    if (keepEmpty || !part.empty()) out.push_back(part);
    i = j + 1;
  }
  return out;
}

std::string Join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

bool StartsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool EndsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool EqNoCase(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

bool ParseInt(const std::string& in, long long& out) {
  std::string s = Trim(in);
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  int base = 10;
  size_t off = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; off = 1; }
  if (s.size() > off + 2 && s[off] == '0' && (s[off + 1] == 'x' || s[off + 1] == 'X')) { base = 16; off += 2; }
  if (off >= s.size()) return false;
  unsigned long long v = std::strtoull(s.c_str() + off, &end, base);
  if (errno != 0 || end == s.c_str() + off || *end != '\0') return false;
  if (v > 0x7FFFFFFFFFFFFFFFULL) return false;
  out = neg ? -static_cast<long long>(v) : static_cast<long long>(v);
  return true;
}

bool ParseDouble(const std::string& in, double& out) {
  std::string s = Trim(in);
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end == s.c_str() || *end != '\0' || std::isnan(v) || std::isinf(v)) return false;
  out = v;
  return true;
}

bool ParseBool(const std::string& in, bool& out) {
  std::string s = Lower(Trim(in));
  if (s == "1" || s == "true" || s == "on" || s == "yes") { out = true; return true; }
  if (s == "0" || s == "false" || s == "off" || s == "no") { out = false; return true; }
  return false;
}

std::string Escape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      case '=': o += "\\e"; break;
      case '|': o += "\\p"; break;
      default: o += c;
    }
  }
  return o;
}

std::string Unescape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      switch (n) {
        case 'n': o += '\n'; break;
        case 'r': o += '\r'; break;
        case 't': o += '\t'; break;
        case 'e': o += '='; break;
        case 'p': o += '|'; break;
        default: o += n;
      }
    } else {
      o += s[i];
    }
  }
  return o;
}

std::u16string Utf8ToUtf16(const std::string& s) {
  std::u16string o;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    if (c < 0x80) { cp = c; }
    else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); len = 2; }
    else if ((c >> 4) == 0xE && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); len = 3; }
    else if ((c >> 3) == 0x1E && i + 3 < s.size()) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu); len = 4; }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      o.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      o.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      o.push_back(static_cast<char16_t>(cp));
    }
  }
  return o;
}

std::string Utf16ToUtf8(const std::u16string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    if (cp < 0x80) o.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { o.push_back(static_cast<char>(0xC0 | (cp >> 6))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { o.push_back(static_cast<char>(0xE0 | (cp >> 12))); o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { o.push_back(static_cast<char>(0xF0 | (cp >> 18))); o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); o.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
  }
  return o;
}

uint32_t Crc32(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::string Hex(uint64_t v, int width) {
  char buf[32];
  snprintf(buf, sizeof buf, "%0*llX", width, static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace bc
