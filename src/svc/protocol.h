// Wire protocol between the UI (client) and the SYSTEM service (server), carried in message-mode named-pipe messages.
// A message is a UTF-8 KV blob ("key=value" lines, values Escape()d). Protocol keys start with '_' (snapshot keys never do).
//
//   client -> server                                   server -> client
//   _op=hello _id=N proto=1 client=ui                  _id=N _ok=1 proto=1 mode=service version=...
//   _op=snap  _id=N have=<crc32 of last snapshot>      _id=N _ok=1 _unchanged=1   |   _id=N _ok=1 _crc=... <snapshot keys>
//   _op=req   _id=N op=set|cmd|cfg key=... value=... a.X=  _id=N _ok=0|1 _code=... _msg=... d.X=...
//   _op=interest _id=N visible=0|1                     _id=N _ok=1
//   _op=bye
//
// Only TYPED settings travel: `key` must exist in the settings registry (or be a known command); there is no way to
// name an EC/MSR address on the wire.
#pragma once
#include <string>

#include "../common/controller.h"
#include "../common/kv.h"

namespace bc {
namespace proto {

inline constexpr int kVersion = 1;
inline constexpr char kPipeNameUtf8[] = "\\\\.\\pipe\\BaseControl.v1";
inline constexpr size_t kMaxRequestBytes = 16 * 1024;
inline constexpr size_t kMaxResponseBytes = 512 * 1024;

struct Frame {
  std::string op;
  uint64_t id = 0;
  KV kv;  // everything else (protocol keys included)
};

std::string Encode(const std::string& op, uint64_t id, const KV& body);
// Strict decoder: size cap, key charset [a-z0-9._-], `_op` required for requests. Returns false on anything odd.
bool DecodeRequest(const std::string& bytes, Frame* out);
bool DecodeResponse(const std::string& bytes, Frame* out);

// Request <-> KV
KV RequestToKv(const Request& r);
bool KvToRequest(const KV& kv, Request* r, std::string* why);  // validates op/key charset and lengths
KV ResultToKv(const Result& r);
Result KvToResult(const KV& kv);

// Display-side keys served by the user-session controller instead of the service.
bool IsUserSessionKey(const std::string& key);

}  // namespace proto
}  // namespace bc
