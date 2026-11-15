// Small JSON toolkit for the OEM service link: a tolerant reader (the service is a .NET program that writes numbers as numbers or as
// numeric strings depending on the field) and a writer that only ever produces well-formed, escaped text.
// No platform headers: the whole link runs against a fake service on any host.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "../common/kv.h"

namespace bc {
namespace oem {

class JVal {
 public:
  enum Type { Null, Bool, Num, Str, Arr, Obj };
  Type type = Null;
  bool b = false;
  double num = 0;
  bool isInt = false;                                   // the number was written without fraction / exponent
  std::string str;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;        // insertion order; objects are small

  bool IsObj() const { return type == Obj; }
  bool IsArr() const { return type == Arr; }
  const JVal* Find(const std::string& key) const;       // exact key
  const JVal* FindI(const std::string& key) const;      // case-insensitive key

  // Tolerant conversions: numbers, numeric strings ("125", "0x7D", " 7 ") and booleans all work; anything else gives the default.
  std::string AsString(const std::string& def = "") const;
  long long AsInt(long long def = 0) const;
  double AsNum(double def = 0) const;
  bool AsBool(bool def = false) const;
  bool Numeric() const;                                 // AsInt()/AsNum() would not fall back to the default
};

struct JsonLimits {
  int maxDepth = 12;
  size_t maxBytes = 1u << 20;
  size_t maxNodes = 20000;
};

// Strict syntax (RFC 8259), one value, nothing after it except whitespace. A UTF-8 byte-order mark is skipped.
bool ParseJson(const std::string& text, JVal* out, std::string* err, const JsonLimits& lim = JsonLimits());

// Flattens a value into "prefix.key.child" = text entries (objects by key, arrays by index; booleans become 1/0, null is skipped).
// Arrays are cut at maxItems and the whole walk at maxEntries, so a hostile or broken payload cannot flood the snapshot.
void Flatten(const JVal& v, const std::string& prefix, KV* out, size_t maxItems = 64, size_t maxEntries = 600);

// Builds one JSON text. Keys and strings are escaped; control characters never survive. Misuse (a value without a key inside an
// object, unbalanced End) is a programming error: the builder keeps going but Valid() turns false.
class JsonOut {
 public:
  JsonOut& BeginObj(const char* key = nullptr);
  JsonOut& EndObj();
  JsonOut& BeginArr(const char* key = nullptr);
  JsonOut& EndArr();
  JsonOut& Str(const char* key, const std::string& v);
  JsonOut& Int(const char* key, long long v);
  JsonOut& Num(const char* key, double v);
  JsonOut& Bool(const char* key, bool v);
  JsonOut& Null(const char* key);
  // Array elements
  JsonOut& ElemInt(long long v);
  JsonOut& ElemStr(const std::string& v);
  bool Valid() const { return ok_ && stack_.empty() && !text_.empty(); }
  std::string Take();

  static std::string Quote(const std::string& s);

 private:
  void Prefix(const char* key);
  std::string text_;
  std::vector<char> stack_;    // '{' or '['
  std::vector<bool> first_;
  bool ok_ = true;
};

}  // namespace oem
}  // namespace bc
