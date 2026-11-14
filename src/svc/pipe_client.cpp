#include "pipe_client.h"

#include <algorithm>
#include <chrono>

#include "../common/util.h"

namespace bc {

PipeBackend::PipeBackend(std::string clientName) : client_(std::move(clientName)) {}

PipeBackend::~PipeBackend() { Stop(); }

void PipeBackend::Start() {
  if (started_) return;
  started_ = true;
  stop_ = false;
  th_ = std::thread([this] { Loop(); });
}

void PipeBackend::Stop() {
  if (!started_) return;
  stop_ = true;
  cv_.notify_all();
  if (th_.joinable()) {
    CancelSynchronousIo(th_.native_handle());
    th_.join();
  }
  started_ = false;
}

void PipeBackend::SetStatus(const std::string& s) {
  std::lock_guard<std::mutex> g(mu_);
  status_ = s;
}

std::string PipeBackend::Status() const {
  std::lock_guard<std::mutex> g(mu_);
  return status_;
}

std::string PipeBackend::Mode() const {
  std::lock_guard<std::mutex> g(mu_);
  return connected_.load() ? mode_ : "offline";
}

std::string PipeBackend::ServiceVersion() const {
  std::lock_guard<std::mutex> g(mu_);
  return svcVersion_;
}

KV PipeBackend::Snapshot() {
  std::lock_guard<std::mutex> g(mu_);
  return snap_;
}

uint64_t PipeBackend::Submit(const Request& r) {
  std::lock_guard<std::mutex> g(mu_);
  const uint64_t id = next_++;
  if (!connected_.load()) {
    Result res;
    res.code = "internal";
    res.msg = "the Control Center Advanced service is not connected";
    done_[id] = res;
    return id;
  }
  jobs_.push_back(Job{id, r});
  cv_.notify_all();
  return id;
}

bool PipeBackend::TakeResult(uint64_t id, Result* out) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = done_.find(id);
  if (it == done_.end()) return false;
  if (out) *out = it->second;
  done_.erase(it);
  return true;
}

void PipeBackend::SetInterest(bool visible) {
  visible_ = visible;
  cv_.notify_all();
}

bool PipeBackend::Connect() {
  const std::wstring name = hw::W(proto::kPipeNameUtf8);
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 3; ++i) {
    h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h != INVALID_HANDLE_VALUE) break;
    const DWORD e = GetLastError();
    if (e == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(name.c_str(), 1500);
      continue;
    }
    if (e == ERROR_FILE_NOT_FOUND) SetStatus("The Control Center Advanced service is not running.");
    else if (e == ERROR_ACCESS_DENIED) SetStatus("Access to the Control Center Advanced service was denied.");
    else SetStatus("Cannot reach the Control Center Advanced service: " + hw::Win32Text(e));
    return false;
  }
  if (h == INVALID_HANDLE_VALUE) {
    SetStatus("The Control Center Advanced service is busy.");
    return false;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
    CloseHandle(h);
    SetStatus("Cannot switch the pipe to message mode.");
    return false;
  }
  pipe_ = h;
  KV body, reply;
  body.SetI("proto", proto::kVersion);
  body.Set("client", client_);
  if (!Roundtrip("hello", body, &reply, 5000)) {
    Disconnect("handshake failed");
    return false;
  }
  if (!reply.Bool("_ok", false)) {
    const std::string why = reply.Str("_msg", "handshake refused");
    Disconnect(why);
    SetStatus(reply.Str("_code") == "proto" ? "The service speaks a different protocol version - please update Control Center Advanced." : why);
    return false;
  }
  {
    std::lock_guard<std::mutex> g(mu_);
    mode_ = reply.Str("mode", "service");
    svcVersion_ = reply.Str("version");
    crc_.clear();
    status_ = "connected";
  }
  connected_ = true;
  return true;
}

void PipeBackend::Disconnect(const std::string& why) {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  const bool was = connected_.exchange(false);
  std::lock_guard<std::mutex> g(mu_);
  if (was) status_ = "Connection to the Control Center Advanced service lost (" + why + ").";
  // fail whatever was waiting
  for (Job& j : jobs_) {
    Result r;
    r.code = "internal";
    r.msg = "connection to the service was lost";
    done_[j.id] = r;
  }
  jobs_.clear();
}

bool PipeBackend::Roundtrip(const std::string& op, const KV& body, KV* reply, DWORD) {
  if (pipe_ == INVALID_HANDLE_VALUE) return false;
  const std::string msg = proto::Encode(op, 1, body);
  DWORD n = 0;
  if (!WriteFile(pipe_, msg.data(), static_cast<DWORD>(msg.size()), &n, nullptr) || n != msg.size()) return false;
  std::string in;
  std::vector<char> buf(64 * 1024);
  for (;;) {
    DWORD got = 0;
    const BOOL ok = ReadFile(pipe_, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
    const DWORD e = ok ? 0 : GetLastError();
    if (ok || e == ERROR_MORE_DATA) {
      in.append(buf.data(), got);
      if (in.size() > proto::kMaxResponseBytes) return false;
      if (ok) break;
      continue;
    }
    return false;
  }
  proto::Frame f;
  if (!proto::DecodeResponse(in, &f)) return false;
  if (reply) *reply = f.kv;
  return true;
}

void PipeBackend::Loop() {
  uint64_t lastSnap = 0;
  uint32_t backoff = 300;
  bool lastVis = true;
  while (!stop_) {
    if (!connected_.load()) {
      if (!Connect()) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(backoff), [this] { return stop_.load(); });
        backoff = std::min<uint32_t>(backoff * 2, 2000);
        continue;
      }
      backoff = 300;
      lastSnap = 0;
      lastVis = !visible_.load();  // force an "interest" announcement right after (re)connecting
    }
    // 1) queued requests, one at a time (the service answers in order)
    for (;;) {
      Job j;
      {
        std::lock_guard<std::mutex> g(mu_);
        if (jobs_.empty()) break;
        j = jobs_.front();
        jobs_.pop_front();
      }
      KV reply;
      if (Roundtrip("req", proto::RequestToKv(j.req), &reply, 60000)) {
        const Result r = proto::KvToResult(reply);
        std::lock_guard<std::mutex> g(mu_);
        done_[j.id] = r;
      } else {
        Result r;
        r.code = "internal";
        r.msg = "no answer from the service";
        {
          std::lock_guard<std::mutex> g(mu_);
          done_[j.id] = r;
        }
        Disconnect("request failed");  // takes the lock itself; also fails the rest of the queue
        break;
      }
    }
    if (!connected_.load()) continue;
    // 2) tell the service whether anybody is looking (fast vs. slow polling)
    const bool vis = visible_.load();
    if (vis != lastVis) {
      KV b, rep;
      b.SetB("visible", vis);
      if (!Roundtrip("interest", b, &rep, 5000)) {
        Disconnect("interest failed");
        continue;
      }
      lastVis = vis;
    }
    // 3) snapshot
    const uint64_t now = GetTickCount64();
    if (now - lastSnap >= (vis ? 400u : 2500u)) {
      KV b, rep;
      {
        std::lock_guard<std::mutex> g(mu_);
        if (!crc_.empty()) b.Set("have", crc_);
      }
      if (!Roundtrip("snap", b, &rep, 10000)) {
        Disconnect("snapshot failed");
        continue;
      }
      lastSnap = now;
      if (rep.Bool("_ok", false) && !rep.Bool("_unchanged", false)) {
        KV s;
        std::string crc;
        for (auto& p : rep.All()) {
          if (!p.first.empty() && p.first[0] == '_') {
            if (p.first == "_crc") crc = p.second;
            continue;
          }
          s.Set(p.first, p.second);
        }
        std::lock_guard<std::mutex> g(mu_);
        snap_ = s;
        crc_ = crc;
      }
    }
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(40), [this] { return stop_.load() || !jobs_.empty(); });
  }
  Disconnect("closing");
}

}  // namespace bc
