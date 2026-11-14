#include "pipe_server.h"

#include <sddl.h>

#include <algorithm>

#include "../common/util.h"

namespace bc {
namespace {
constexpr DWORD kBuf = 64 * 1024;
constexpr int kMaxClients = 8;
}  // namespace

struct PipeServer::Conn {
  uint32_t pid = 0;
  bool admin = false;
  bool visible = true;
  bool helloOk = false;
  double tokens = 40;
  uint64_t lastRefill = 0;
};

PipeServer::PipeServer(Controller& c, mgp::ILog& log, std::string mode, std::string version) : c_(c), log_(log), mode_(std::move(mode)), version_(std::move(version)) {}

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(std::string* why) {
  if (started_) return true;
  stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEv_) {
    if (why) *why = "cannot create the stop event";
    return false;
  }
  // Probe the first instance synchronously so a squatted pipe name is reported to the caller instead of being retried silently.
  stop_ = false;
  started_ = true;
  accept_ = std::thread([this] { AcceptLoop(); });
  return true;
}

void PipeServer::Stop() {
  if (!started_) return;
  stop_ = true;
  SetEvent(stopEv_);
  if (accept_.joinable()) accept_.join();
  std::list<std::thread> w;
  {
    std::lock_guard<std::mutex> g(mu_);
    w.swap(workers_);
  }
  for (auto& t : w)
    if (t.joinable()) t.join();
  CloseHandle(stopEv_);
  stopEv_ = nullptr;
  started_ = false;
}

void PipeServer::AcceptLoop() {
  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, FALSE};
  // SY / BA: full; AU: read + write data (GRGW). Low-integrity or anonymous callers are not "authenticated users" and are refused.
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd, nullptr)) {
    log_.Write(mgp::LogLevel::Error, "pipe: cannot build the security descriptor; the pipe is not served");
    return;
  }
  sa.lpSecurityDescriptor = sd;
  const std::wstring name = hw::W(proto::kPipeNameUtf8);
  bool first = true;
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  int squatRetries = 0;
  while (!stop_) {
    HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, kMaxClients, kBuf, kBuf, 0, &sa);
    if (p == INVALID_HANDLE_VALUE) {
      const DWORD e = GetLastError();
      if (first && e == ERROR_ACCESS_DENIED) {
        log_.Write(mgp::LogLevel::Error, "pipe: the pipe name is already owned by another process (possible squatter); retrying");
        if (++squatRetries > 20) break;
      } else {
        log_.Write(mgp::LogLevel::Warn, "pipe: CreateNamedPipe failed: " + hw::Win32Text(e));
      }
      if (WaitForSingleObject(stopEv_, 1000) == WAIT_OBJECT_0) break;
      continue;
    }
    first = false;
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ResetEvent(ev);
    BOOL ok = ConnectNamedPipe(p, &ov);
    DWORD err = ok ? 0 : GetLastError();
    bool connected = ok || err == ERROR_PIPE_CONNECTED;
    if (!connected && err == ERROR_IO_PENDING) {
      HANDLE h[2] = {ev, stopEv_};
      const DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
      if (w == WAIT_OBJECT_0) {
        DWORD n = 0;
        connected = GetOverlappedResult(p, &ov, &n, FALSE) != 0;
      } else {
        CancelIoEx(p, &ov);
        DWORD n = 0;
        GetOverlappedResult(p, &ov, &n, TRUE);
      }
    }
    if (!connected) {
      CloseHandle(p);
      continue;
    }
    if (clients_.load() >= kMaxClients - 1) {
      DisconnectNamedPipe(p);
      CloseHandle(p);
      continue;
    }
    std::lock_guard<std::mutex> g(mu_);
    workers_.emplace_back([this, p] { ClientLoop(p); });
  }
  if (ev) CloseHandle(ev);
  LocalFree(sd);
}

bool PipeServer::ReadMessage(HANDLE pipe, std::string* out) {
  out->clear();
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<char> buf(kBuf);
  bool ok = false;
  for (;;) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD n = 0;
    BOOL r = ReadFile(pipe, buf.data(), kBuf, &n, &ov);
    DWORD err = r ? 0 : GetLastError();
    if (!r && err == ERROR_IO_PENDING) {
      HANDLE h[2] = {ev, stopEv_};
      const DWORD w = WaitForMultipleObjects(2, h, FALSE, 120000);  // idle clients are dropped after two minutes
      if (w != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &n, TRUE);
        break;
      }
      r = GetOverlappedResult(pipe, &ov, &n, FALSE);
      err = r ? 0 : GetLastError();
    }
    if (r || err == ERROR_MORE_DATA) {
      out->append(buf.data(), n);
      if (out->size() > proto::kMaxRequestBytes) break;  // oversized: refuse
      if (r) {
        ok = true;
        break;
      }
      continue;
    }
    break;  // broken pipe etc.
  }
  CloseHandle(ev);
  return ok;
}

bool PipeServer::WriteMessage(HANDLE pipe, const std::string& msg) {
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  OVERLAPPED ov = {};
  ov.hEvent = ev;
  DWORD n = 0;
  BOOL r = WriteFile(pipe, msg.data(), static_cast<DWORD>(msg.size()), &n, &ov);
  if (!r && GetLastError() == ERROR_IO_PENDING) {
    HANDLE h[2] = {ev, stopEv_};
    const DWORD w = WaitForMultipleObjects(2, h, FALSE, 10000);
    if (w != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &ov);
      GetOverlappedResult(pipe, &ov, &n, TRUE);
      CloseHandle(ev);
      return false;
    }
    r = GetOverlappedResult(pipe, &ov, &n, FALSE);
  }
  CloseHandle(ev);
  return r && n == msg.size();
}

bool PipeServer::ClientIsAdmin(HANDLE pipe) {
  if (!ImpersonateNamedPipeClient(pipe)) return false;
  bool admin = false;
  HANDLE tok = nullptr;
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok)) {
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    if (GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n)) admin = e.TokenIsElevated != 0;
    CloseHandle(tok);
  }
  RevertToSelf();
  return admin;
}

void PipeServer::UpdateInterest() {
  bool any = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (Conn* c : conns_) any = any || c->visible;
  }
  c_.SetInterest(any);
}

std::string PipeServer::Handle(Conn& cn, const proto::Frame& f, bool* close) {
  KV out;
  auto reply = [&](bool ok, const std::string& code = "ok", const std::string& msg = "") {
    out.SetB("_ok", ok);
    if (!ok || !code.empty()) out.Set("_code", code);
    if (!msg.empty()) out.Set("_msg", msg);
    return proto::Encode("", f.id, out);
  };
  // token bucket: 20 tokens/s, burst 40; set/cmd cost 4
  const uint64_t now = GetTickCount64();
  if (cn.lastRefill) cn.tokens = std::min(40.0, cn.tokens + (now - cn.lastRefill) * 0.02);
  cn.lastRefill = now;
  const double cost = f.op == "req" ? 4.0 : 1.0;
  if (cn.tokens < cost) return reply(false, "busy", "rate limited");
  cn.tokens -= cost;

  if (f.op == "hello") {
    long long v = 0;
    ParseInt(f.kv.Str("proto"), v);
    if (v != proto::kVersion) return reply(false, "proto", Fmt("protocol %d required", proto::kVersion));
    out.SetI("proto", proto::kVersion);
    out.Set("mode", mode_);
    out.Set("version", version_);
    cn.helloOk = true;
    return reply(true);
  }
  if (f.op == "snap") {
    KV s = c_.Snapshot();
    const std::string text = s.Serialize();
    const std::string crc = Hex(Crc32(text.data(), text.size()), 8);
    if (f.kv.Str("have") == crc) {
      out.SetB("_unchanged", true);
      return reply(true);
    }
    out = s;
    out.Set("_crc", crc);
    return reply(true);
  }
  if (f.op == "interest") {
    cn.visible = f.kv.Bool("visible", true);
    UpdateInterest();
    return reply(true);
  }
  if (f.op == "req") {
    Request q;
    std::string why;
    if (!proto::KvToRequest(f.kv, &q, &why)) return reply(false, "unknown", why);
    if (!cn.admin && c_.Snapshot().Bool("cfg.adminOnly", false)) return reply(false, "gate.admin", "administrator rights are required to change settings");
    Result r = c_.Execute(q);
    KV rk = proto::ResultToKv(r);
    out = rk;
    return proto::Encode("", f.id, out);
  }
  if (f.op == "bye") {
    *close = true;
    return "";
  }
  return reply(false, "unknown", "unknown operation");
}

void PipeServer::ClientLoop(HANDLE pipe) {
  Conn cn;
  {
    ULONG pid = 0;
    if (GetNamedPipeClientProcessId(pipe, &pid)) cn.pid = pid;
  }
  cn.admin = ClientIsAdmin(pipe);
  {
    std::lock_guard<std::mutex> g(mu_);
    conns_.push_back(&cn);
  }
  clients_++;
  log_.Write(mgp::LogLevel::Info, Fmt("pipe: client connected (pid %u%s)", cn.pid, cn.admin ? ", admin" : ""));
  for (;;) {
    std::string msg;
    if (!ReadMessage(pipe, &msg)) break;
    proto::Frame f;
    if (!proto::DecodeRequest(msg, &f)) {
      log_.Write(mgp::LogLevel::Warn, "pipe: malformed request dropped");
      break;
    }
    if (!cn.helloOk && f.op != "hello") {
      WriteMessage(pipe, proto::Encode("", f.id, [] { KV k; k.SetB("_ok", false); k.Set("_code", "proto"); k.Set("_msg", "hello first"); return k; }()));
      break;
    }
    bool close = false;
    const std::string reply = Handle(cn, f, &close);
    if (close) break;
    if (reply.size() > proto::kMaxResponseBytes || !WriteMessage(pipe, reply)) break;
    if (f.op == "hello" && !cn.helloOk) break;  // protocol mismatch: the client got the reason, now hang up
  }
  {
    std::lock_guard<std::mutex> g(mu_);
    conns_.remove(&cn);
  }
  clients_--;
  UpdateInterest();
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  log_.Write(mgp::LogLevel::Info, Fmt("pipe: client left (pid %u)", cn.pid));
}

}  // namespace bc
