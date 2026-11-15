// What the UI talks to: the latest merged state and a queue of requests. The only implementation is LocalBackend, which wraps the
// in-process Controller (the window itself, the simulator and the tests); the program has no service and no pipe of its own.
#pragma once
#include <string>

#include "controller.h"

namespace bc {

class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual KV Snapshot() = 0;                       // latest merged state; cheap, never blocks on hardware
  virtual uint64_t Submit(const Request& r) = 0;   // asynchronous; the returned id is > 0
  virtual bool TakeResult(uint64_t id, Result* out) = 0;  // non-blocking
  virtual void SetInterest(bool visible) = 0;
  virtual std::string Mode() const = 0;            // "local" | "sim"
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

}  // namespace bc
