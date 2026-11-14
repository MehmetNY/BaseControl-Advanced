// IBackend that talks to the service over the named pipe. One background thread owns the connection: it reconnects
// by itself, sends queued requests, polls snapshots (fast while the UI is visible) and never blocks the UI thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "../hw/winutil.h"
#include "backend.h"
#include "protocol.h"

namespace bc {

class PipeBackend : public IBackend {
 public:
  explicit PipeBackend(std::string clientName = "ui");
  ~PipeBackend() override;
  void Start();
  void Stop();

  KV Snapshot() override;
  uint64_t Submit(const Request& r) override;
  bool TakeResult(uint64_t id, Result* out) override;
  void SetInterest(bool visible) override;
  std::string Mode() const override;
  bool Connected() const override { return connected_.load(); }
  std::string Status() const override;
  std::string ServiceVersion() const;

 private:
  void Loop();
  bool Connect();
  void Disconnect(const std::string& why);
  bool Roundtrip(const std::string& op, const KV& body, KV* reply, DWORD timeoutMs);
  void SetStatus(const std::string& s);

  std::string client_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> visible_{true};
  std::thread th_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  KV snap_;
  std::string crc_, status_ = "starting", mode_ = "offline", svcVersion_;
  struct Job { uint64_t id; Request req; };
  std::deque<Job> jobs_;
  std::map<uint64_t, Result> done_;
  uint64_t next_ = 1;
  bool started_ = false;
};

}  // namespace bc
