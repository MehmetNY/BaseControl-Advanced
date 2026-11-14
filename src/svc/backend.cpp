#include "backend.h"

#include "../common/util.h"
#include "protocol.h"

namespace bc {
namespace {
// Snapshot keys that come from the user-session controller.
bool UserKey(const std::string& k) {
  return StartsWith(k, "disp.") || StartsWith(k, "cap.disp.") || StartsWith(k, "capwhy.disp.") || k == "dev.touchpad" || k == "cap.dev.touchpad" ||
         k == "capwhy.dev.touchpad";
}
}  // namespace

KV CompositeBackend::Snapshot() {
  KV s = remote_.Snapshot();
  // the service has no desktop: whatever it says about display/touchpad is replaced by the user-session view
  std::vector<std::string> drop;
  for (auto& p : s.All())
    if (UserKey(p.first)) drop.push_back(p.first);
  for (auto& k : drop) s.Erase(k);
  KV u = user_.Snapshot();
  for (auto& p : u.All())
    if (UserKey(p.first)) s.Set(p.first, p.second);
  return s;
}

uint64_t CompositeBackend::Submit(const Request& r) {
  const bool user = r.op == "set" && proto::IsUserSessionKey(r.key);
  const uint64_t inner = user ? user_.Submit(r) : remote_.Submit(r);
  if (inner == 0) return 0;
  std::lock_guard<std::mutex> g(mu_);
  const uint64_t id = next_++;
  routes_[id] = Route{user, inner};
  return id;
}

bool CompositeBackend::TakeResult(uint64_t id, Result* out) {
  Route rt;
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = routes_.find(id);
    if (it == routes_.end()) return false;
    rt = it->second;
  }
  const bool got = rt.user ? user_.TakeResult(rt.inner, out) : remote_.TakeResult(rt.inner, out);
  if (got) {
    std::lock_guard<std::mutex> g(mu_);
    routes_.erase(id);
  }
  return got;
}

}  // namespace bc
