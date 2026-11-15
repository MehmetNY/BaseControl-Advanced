// WriteGuard: the single gatekeeper for writes to the embedded controller.
//
// Control Center Advanced changes almost nothing in the EC itself (profile values, fans, battery and keyboard go through the vendor's own
// service). The two registers the GPU power engine uses for the extended GPU power (cTGP) are the only addresses on the allow-list;
// everything else - including every other register this program reads - is refused, and a never-write list wins over everything.
//
//  * allow-list of (address, bit mask, value rule); everything else is refused
//  * refuses (never clamps) out-of-range values
//  * masked registers are read-modify-write: bits outside the mask must be unchanged
//  * a global kill switch (read-only mode)
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
  static GuardVerdict Check(uint16_t addr, uint8_t value, uint8_t current, bool haveCurrent);
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

  void SetReadOnly(bool ro) { readOnly_ = ro; }
  bool ReadOnly() const { return readOnly_; }
  uint32_t writes() const { return writes_; }
  uint32_t refused() const { return refused_; }
  std::string lastRefusal() const { return lastRefusal_; }

 private:
  mgp::IEcDriver& inner_;
  mgp::ILog& log_;
  bool readOnly_ = false;
  uint32_t writes_ = 0, refused_ = 0;
  std::string lastRefusal_;
};

}  // namespace bc
