#include "protocol.h"

#include "../common/util.h"

namespace bc {
namespace proto {
namespace {

bool KeyOk(const std::string& k, size_t maxLen = 64) {
  if (k.empty() || k.size() > maxLen) return false;
  for (char c : k)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || (c >= 'A' && c <= 'Z'))) return false;
  return true;
}

bool Decode(const std::string& bytes, Frame* out, size_t cap) {
  if (bytes.empty() || bytes.size() > cap) return false;
  if (bytes.find('\0') != std::string::npos) return false;
  KV kv = KV::Parse(bytes);
  if (kv.Size() == 0) return false;
  for (auto& p : kv.All())
    if (!KeyOk(p.first, 96)) return false;
  Frame f;
  f.op = kv.Str("_op");
  long long id = 0;
  if (!ParseInt(kv.Str("_id", "0"), id) || id < 0) return false;
  f.id = static_cast<uint64_t>(id);
  f.kv = kv;
  *out = f;
  return true;
}

}  // namespace

std::string Encode(const std::string& op, uint64_t id, const KV& body) {
  KV kv = body;
  if (!op.empty()) kv.Set("_op", op);
  kv.SetI("_id", static_cast<long long>(id));
  return kv.Serialize();
}

bool DecodeRequest(const std::string& bytes, Frame* out) {
  Frame f;
  if (!Decode(bytes, &f, kMaxRequestBytes)) return false;
  if (!KeyOk(f.op, 16)) return false;
  *out = f;
  return true;
}

bool DecodeResponse(const std::string& bytes, Frame* out) { return Decode(bytes, out, kMaxResponseBytes); }

KV RequestToKv(const Request& r) {
  KV kv;
  kv.Set("op", r.op);
  kv.Set("key", r.key);
  kv.Set("value", r.value);
  for (auto& p : r.args.All()) kv.Set("a." + p.first, p.second);
  return kv;
}

bool KvToRequest(const KV& kv, Request* r, std::string* why) {
  Request q;
  q.op = kv.Str("op");
  q.key = kv.Str("key");
  q.value = kv.Str("value");
  if (q.op != "set" && q.op != "cmd" && q.op != "cfg") {
    if (why) *why = "bad op";
    return false;
  }
  if (!KeyOk(q.key, 64)) {
    if (why) *why = "bad key";
    return false;
  }
  // "cfg" writes the controller's own preferences (kill switch, experimental toggle ...): keys live under "cfg."
  if (q.op == "cfg" && (!StartsWith(q.key, "cfg.") || q.value.size() > 512)) {
    if (why) *why = "bad config request";
    return false;
  }
  if (q.value.size() > 4096) {
    if (why) *why = "value too long";
    return false;
  }
  size_t nargs = 0;
  for (auto& p : kv.All()) {
    if (!StartsWith(p.first, "a.")) continue;
    const std::string k = p.first.substr(2);
    if (!KeyOk(k, 48) || p.second.size() > 4096 || ++nargs > 32) {
      if (why) *why = "bad argument";
      return false;
    }
    q.args.Set(k, p.second);
  }
  *r = q;
  return true;
}

KV ResultToKv(const Result& r) {
  KV kv;
  kv.SetB("_ok", r.ok);
  kv.Set("_code", r.code);
  kv.Set("_msg", r.msg);
  for (auto& p : r.data.All()) kv.Set("d." + p.first, p.second);
  return kv;
}

Result KvToResult(const KV& kv) {
  Result r;
  r.ok = kv.Bool("_ok", false);
  r.code = kv.Str("_code", "internal");
  r.msg = kv.Str("_msg");
  for (auto& p : kv.All())
    if (StartsWith(p.first, "d.")) r.data.Set(p.first.substr(2), p.second);
  return r;
}

bool IsUserSessionKey(const std::string& key) {
  return StartsWith(key, "disp.") || key == "dev.touchpad";
}

}  // namespace proto
}  // namespace bc
