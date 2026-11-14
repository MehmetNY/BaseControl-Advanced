// WriteGuard: the single gatekeeper for EC writes.
//
//  * allowlist of (address, bit mask, value rule); everything else is refused
//  * refuses (never clamps) out-of-range values
//  * masked registers are read-modify-write: bits outside the mask must be unchanged
//  * a global kill switch (read-only mode) and a never-write list that wins over everything
#pragma once
#include <cstdint>
#include <string>

#include "../mgp/core/interfaces.h"

namespace bc {

struct GuardVerdict {
  bool ok = false;
  std::string why;
};

class WriteGuard {
 public:
  // Pure rule check. `current` is the byte currently in the EC (needed for masked registers).
  static GuardVerdict Check(uint16_t addr, uint8_t value, uint8_t current, bool haveCurrent, bool initScope = false);
  // Mask the guard applies to this register (0xFF = whole byte, 0 = not allowlisted).
  static uint8_t MaskFor(uint16_t addr);
  static bool IsAllowlisted(uint16_t addr) { return MaskFor(addr) != 0; }
};

// IEcDriver decorator: every write goes through WriteGuard (including writes issued by the vendored
// MONSTER GPU POWER engine). Reads pass through. Thread-safe only if the wrapped driver is; the controller
// serialises all access on one worker thread anyway.
class GuardedEc : public mgp::IEcDriver {
 public:
  GuardedEc(mgp::IEcDriver& inner, mgp::ILog& log) : inner_(inner), log_(log) {}
  mgp::DriverState Probe(uint32_t* winError) override { return inner_.Probe(winError); }
  mgp::EcByte Read(uint16_t addr) override;
  mgp::EcByte Write(uint16_t addr, uint8_t value) override;
  // Used only to restore a byte we read earlier: the address must still be allowlisted and bits outside the
  // register's mask must be unchanged, but the value itself is not range-checked (the EC may hold any value).
  mgp::EcByte WriteRollback(uint16_t addr, uint8_t value);

  // RAII scope that permits the documented EC bring-up writes (and nothing else new).
  class InitScope {
   public:
    explicit InitScope(GuardedEc& g) : g_(g), prev_(g.initScope_) { g.initScope_ = true; }
    ~InitScope() { g_.initScope_ = prev_; }
    InitScope(const InitScope&) = delete;
    InitScope& operator=(const InitScope&) = delete;
   private:
    GuardedEc& g_;
    bool prev_;
  };

  void SetReadOnly(bool ro) { readOnly_ = ro; }
  bool ReadOnly() const { return readOnly_; }
  uint32_t writes() const { return writes_; }
  uint32_t refused() const { return refused_; }
  std::string lastRefusal() const { return lastRefusal_; }

 private:
  mgp::IEcDriver& inner_;
  mgp::ILog& log_;
  bool readOnly_ = false;
  bool initScope_ = false;
  uint32_t writes_ = 0, refused_ = 0;
  std::string lastRefusal_;
};

}  // namespace bc
