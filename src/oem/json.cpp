#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../common/util.h"

namespace bc {
namespace oem {

// ------------------------------------------------------------------------------------------------ JVal
const JVal* JVal::Find(const std::string& key) const {
  if (type != Obj) return nullptr;
  for (const auto& p : obj)
    if (p.first == key) return &p.second;
  return nullptr;
}

const JVal* JVal::FindI(const std::string& key) const {
  if (type != Obj) return nullptr;
  for (const auto& p : obj)
    if (EqNoCase(p.first, key)) return &p.second;
  return nullptr;
}

bool JVal::Numeric() const {
  if (type == Num || type == Bool) return true;
  if (type != Str) return false;
  long long n;
  double d;
  const std::string s = Trim(str);
  return ParseInt(s, n) || ParseDouble(s, d);
}

std::string JVal::AsString(const std::string& def) const {
  switch (type) {
    case Str: return str;
    case Bool: return b ? "true" : "false";
    case Num: {
      if (isInt || (num == std::floor(num) && std::fabs(num) < 1e15)) return Fmt("%lld", static_cast<long long>(num));
      char buf[48];
      std::snprintf(buf, sizeof buf, "%.6g", num);
      return buf;
    }
    default: return def;
  }
}

double JVal::AsNum(double def) const {
  switch (type) {
    case Num: return num;
    case Bool: return b ? 1 : 0;
    case Str: {
      const std::string s = Trim(str);
      long long n;
      if (ParseInt(s, n)) return static_cast<double>(n);
      double d;
      if (ParseDouble(s, d)) return d;
      return def;
    }
    default: return def;
  }
}

long long JVal::AsInt(long long def) const {
  if (type == Str) {
    long long n;
    if (ParseInt(Trim(str), n)) return n;
  }
  if (!Numeric()) return def;
  const double d = AsNum(0);
  if (!(std::fabs(d) < 9e18)) return def;
  return static_cast<long long>(std::llround(d));
}

bool JVal::AsBool(bool def) const {
  switch (type) {
    case Bool: return b;
    case Num: return num != 0;
    case Str: {
      bool v;
      if (ParseBool(Trim(str), v)) return v;
      return def;
    }
    default: return def;
  }
}

// ------------------------------------------------------------------------------------------------ parser
namespace {
struct Parser {
  const char* p;
  const char* end;
  const JsonLimits& lim;
  size_t nodes = 0;
  std::string err;

  Parser(const std::string& s, const JsonLimits& l) : p(s.data()), end(s.data() + s.size()), lim(l) {}

  bool Fail(const char* m) {
    if (err.empty()) err = m;
    return false;
  }
  void Ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  }
  static int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static void PutUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) {
      o += static_cast<char>(0xC0 | (cp >> 6));
      o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      o += static_cast<char>(0xE0 | (cp >> 12));
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      o += static_cast<char>(0xF0 | (cp >> 18));
      o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool Hex4(unsigned* v) {
    if (end - p < 4) return Fail("short \\u escape");
    unsigned r = 0;
    for (int i = 0; i < 4; i++) {
      int h = Hex(p[i]);
      if (h < 0) return Fail("bad \\u escape");
      r = (r << 4) | static_cast<unsigned>(h);
    }
    p += 4;
    *v = r;
    return true;
  }
  bool String(std::string* out) {
    // *p == '"'
    p++;
    out->clear();
    while (p < end) {
      unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') return true;
      if (c < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        if (out->size() > lim.maxBytes) return Fail("string too long");
        continue;
      }
      if (p >= end) return Fail("dangling backslash");
      char e = *p++;
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          unsigned u;
          if (!Hex4(&u)) return false;
          if (u >= 0xD800 && u <= 0xDBFF) {
            if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
              p += 2;
              unsigned lo;
              if (!Hex4(&lo)) return false;
              if (lo < 0xDC00 || lo > 0xDFFF) return Fail("bad surrogate pair");
              u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              return Fail("lone surrogate");
            }
          } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return Fail("lone surrogate");
          }
          PutUtf8(*out, u);
          break;
        }
        default: return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }
  bool Number(JVal* v) {
    const char* s = p;
    if (p < end && *p == '-') p++;
    if (p >= end) return Fail("bad number");
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (p < end && *p >= '0' && *p <= '9') p++;
    else return Fail("bad number");
    bool frac = false;
    if (p < end && *p == '.') {
      frac = true;
      p++;
      if (p >= end || *p < '0' || *p > '9') return Fail("bad fraction");
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      frac = true;
      p++;
      if (p < end && (*p == '+' || *p == '-')) p++;
      if (p >= end || *p < '0' || *p > '9') return Fail("bad exponent");
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    const std::string t(s, p);
    v->type = JVal::Num;
    v->num = std::strtod(t.c_str(), nullptr);
    v->isInt = !frac;
    if (!std::isfinite(v->num)) return Fail("number out of range");
    return true;
  }
  bool Lit(const char* w) {
    size_t n = std::char_traits<char>::length(w);
    if (static_cast<size_t>(end - p) < n || std::string(p, n) != w) return Fail("bad literal");
    p += n;
    return true;
  }
  bool Value(JVal* v, int depth) {
    if (++nodes > lim.maxNodes) return Fail("too many values");
    if (depth > lim.maxDepth) return Fail("nesting too deep");
    Ws();
    if (p >= end) return Fail("unexpected end");
    switch (*p) {
      case '{': {
        p++;
        v->type = JVal::Obj;
        Ws();
        if (p < end && *p == '}') { p++; return true; }
        for (;;) {
          Ws();
          if (p >= end || *p != '"') return Fail("object key expected");
          std::string k;
          if (!String(&k)) return false;
          Ws();
          if (p >= end || *p != ':') return Fail("':' expected");
          p++;
          JVal child;
          if (!Value(&child, depth + 1)) return false;
          v->obj.emplace_back(std::move(k), std::move(child));
          Ws();
          if (p < end && *p == ',') { p++; continue; }
          if (p < end && *p == '}') { p++; return true; }
          return Fail("',' or '}' expected");
        }
      }
      case '[': {
        p++;
        v->type = JVal::Arr;
        Ws();
        if (p < end && *p == ']') { p++; return true; }
        for (;;) {
          JVal child;
          if (!Value(&child, depth + 1)) return false;
          v->arr.push_back(std::move(child));
          Ws();
          if (p < end && *p == ',') { p++; continue; }
          if (p < end && *p == ']') { p++; return true; }
          return Fail("',' or ']' expected");
        }
      }
      case '"':
        v->type = JVal::Str;
        return String(&v->str);
      case 't':
        v->type = JVal::Bool;
        v->b = true;
        return Lit("true");
      case 'f':
        v->type = JVal::Bool;
        v->b = false;
        return Lit("false");
      case 'n':
        v->type = JVal::Null;
        return Lit("null");
      default: return Number(v);
    }
  }
};
}  // namespace

bool ParseJson(const std::string& text, JVal* out, std::string* err, const JsonLimits& lim) {
  if (text.size() > lim.maxBytes) {
    if (err) *err = "payload too large";
    return false;
  }
  std::string t = text;
  if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB && static_cast<unsigned char>(t[2]) == 0xBF)
    t.erase(0, 3);
  Parser ps(t, lim);
  JVal v;
  if (!ps.Value(&v, 0)) {
    if (err) *err = ps.err.empty() ? "parse error" : ps.err;
    return false;
  }
  ps.Ws();
  if (ps.p != ps.end) {
    if (err) *err = "trailing characters";
    return false;
  }
  *out = std::move(v);
  return true;
}

// ------------------------------------------------------------------------------------------------ flatten
namespace {
void FlatRec(const JVal& v, const std::string& prefix, KV* out, size_t maxItems, size_t maxEntries) {
  if (out->Size() >= maxEntries) return;
  switch (v.type) {
    case JVal::Obj:
      for (const auto& p : v.obj) FlatRec(p.second, prefix.empty() ? p.first : prefix + "." + p.first, out, maxItems, maxEntries);
      break;
    case JVal::Arr: {
      size_t n = 0;
      for (const JVal& e : v.arr) {
        if (n >= maxItems) break;
        FlatRec(e, prefix + "." + std::to_string(n), out, maxItems, maxEntries);
        n++;
      }
      out->SetI(prefix + ".#", static_cast<long long>(v.arr.size()));
      break;
    }
    case JVal::Bool: out->Set(prefix, v.b ? "1" : "0"); break;
    case JVal::Num: out->Set(prefix, v.AsString()); break;
    case JVal::Str: out->Set(prefix, v.str.size() > 400 ? v.str.substr(0, 400) : v.str); break;
    case JVal::Null: break;
  }
}
}  // namespace

void Flatten(const JVal& v, const std::string& prefix, KV* out, size_t maxItems, size_t maxEntries) { FlatRec(v, prefix, out, maxItems, maxEntries); }

// ------------------------------------------------------------------------------------------------ writer
std::string JsonOut::Quote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7F) o += Fmt("\\u%04x", c);
        else o += static_cast<char>(c);
    }
  }
  o += '"';
  return o;
}

void JsonOut::Prefix(const char* key) {
  const bool inObj = !stack_.empty() && stack_.back() == '{';
  const bool inArr = !stack_.empty() && stack_.back() == '[';
  if (!stack_.empty()) {
    if (!first_.back()) text_ += ',';
    first_.back() = false;
  } else if (!text_.empty()) {
    ok_ = false;   // a second top-level value
  }
  if (inObj) {
    if (!key) ok_ = false;
    else text_ += Quote(key) + ":";
  } else if (inArr && key) {
    ok_ = false;
  }
}

JsonOut& JsonOut::BeginObj(const char* key) {
  Prefix(key);
  text_ += '{';
  stack_.push_back('{');
  first_.push_back(true);
  return *this;
}
JsonOut& JsonOut::EndObj() {
  if (stack_.empty() || stack_.back() != '{') { ok_ = false; return *this; }
  stack_.pop_back();
  first_.pop_back();
  text_ += '}';
  return *this;
}
JsonOut& JsonOut::BeginArr(const char* key) {
  Prefix(key);
  text_ += '[';
  stack_.push_back('[');
  first_.push_back(true);
  return *this;
}
JsonOut& JsonOut::EndArr() {
  if (stack_.empty() || stack_.back() != '[') { ok_ = false; return *this; }
  stack_.pop_back();
  first_.pop_back();
  text_ += ']';
  return *this;
}
JsonOut& JsonOut::Str(const char* key, const std::string& v) {
  Prefix(key);
  text_ += Quote(v);
  return *this;
}
JsonOut& JsonOut::Int(const char* key, long long v) {
  Prefix(key);
  text_ += std::to_string(v);
  return *this;
}
JsonOut& JsonOut::Num(const char* key, double v) {
  Prefix(key);
  if (!std::isfinite(v)) { text_ += "0"; ok_ = false; return *this; }
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  text_ += buf;
  return *this;
}
JsonOut& JsonOut::Bool(const char* key, bool v) {
  Prefix(key);
  text_ += v ? "true" : "false";
  return *this;
}
JsonOut& JsonOut::Null(const char* key) {
  Prefix(key);
  text_ += "null";
  return *this;
}
JsonOut& JsonOut::ElemInt(long long v) { return Int(nullptr, v); }
JsonOut& JsonOut::ElemStr(const std::string& v) { return Str(nullptr, v); }
std::string JsonOut::Take() {
  std::string t;
  t.swap(text_);
  stack_.clear();
  first_.clear();
  return t;
}

}  // namespace oem
}  // namespace bc
