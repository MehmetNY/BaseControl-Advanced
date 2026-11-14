// What the UI talks to. Three implementations:
//   LocalBackend      wraps an in-process Controller (elevated "local" mode, simulator, tests)
//   PipeBackend       talks to the SYSTEM service over the named pipe            (pipe_client.h, Windows only)
//   CompositeBackend  PipeBackend + a user-session Controller for the display/touchpad keys
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../common/controller.h"

namespace bc {

class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual KV Snapshot() = 0;                       // latest merged state; cheap, never blocks on hardware
  virtual uint64_t Submit(const Request& r) = 0;   // asynchronous; the returned id is > 0
  virtual bool TakeResult(uint64_t id, Result* out) = 0;  // non-blocking
  virtual void SetInterest(bool visible) = 0;
  virtual std::string Mode() const = 0;            // "service" | "local" | "sim" | "offline"
  virtual bool Connected() const = 0;
  virtual std::string Status() const { return ""; }  // human text while disconnected
};

class LocalBackend : public IBackend {
 public:
  LocalBackend(Controller& c, std::string mode) : c_(c), mode_(std::move(mode)) {}
  KV Snapshot() override { return c_.Snapshot(); }
  uint64_t Submit(const Request& r) override { return c_.Submit(r); }
  bool TakeResult(uint64_t id, Result* out) override { return c_.TakeResult(id, out); }
  void SetInterest(bool visible) override { c_.SetInterest(visible); }
  std::string Mode() const override { return mode_; }
  bool Connected() const override { return true; }

 private:
  Controller& c_;
  std::string mode_;
};

// Service snapshot + whitelisted keys from the user-session controller (display, touchpad).
class CompositeBackend : public IBackend {
 public:
  CompositeBackend(IBackend& remote, Controller& userSide) : remote_(remote), user_(userSide) {}
  KV Snapshot() override;
  uint64_t Submit(const Request& r) override;
  bool TakeResult(uint64_t id, Result* out) override;
  void SetInterest(bool visible) override {
    remote_.SetInterest(visible);
    user_.SetInterest(visible);
  }
  std::string Mode() const override { return remote_.Mode(); }
  bool Connected() const override { return remote_.Connected(); }
  std::string Status() const override { return remote_.Status(); }

 private:
  struct Route { bool user; uint64_t inner; };
  IBackend& remote_;
  Controller& user_;
  std::mutex mu_;
  std::map<uint64_t, Route> routes_;
  uint64_t next_ = 1;
};

}  // namespace bc
