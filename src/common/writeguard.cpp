#include "writeguard.h"

#include "registers.h"
#include "util.h"

namespace bc {

using namespace reg;

namespace {
struct Rule {
  uint16_t lo, hi;  // inclusive address range
  uint8_t mask;     // 0xFF whole byte
};

// The extended GPU power (cTGP) registers of the verified GM5TG7Y, written only by the vendored GPU power engine.
constexpr Rule kRules[] = {
    {kGpuCtrl, kGpuCtrl, 0x07},
    {kGpuCtgpOffset, kGpuCtgpOffset, 0xFF},
};
}  // namespace

uint8_t WriteGuard::MaskFor(uint16_t addr) {
  if (IsNeverWrite(addr)) return 0;
  for (const Rule& r : kRules)
    if (addr >= r.lo && addr <= r.hi) return r.mask;
  return 0;
}

GuardVerdict WriteGuard::Check(uint16_t addr, uint8_t v, uint8_t cur, bool haveCur) {
  GuardVerdict g;
  auto no = [&](const std::string& w) { g.ok = false; g.why = Fmt("EC 0x%04X <- 0x%02X refused: %s", addr, v, w.c_str()); return g; };
  if (IsNeverWrite(addr)) return no("address is on the never-write list");
  uint8_t mask = MaskFor(addr);
  if (!mask) return no("address is not on the write allowlist");
  if (mask != 0xFF) {
    if (!haveCur) return no("masked register needs a fresh read-modify-write");
    if (((cur ^ v) & ~mask & 0xFF) != 0) return no("bits outside the permitted mask would change");
  }
  switch (addr) {
    case kGpuCtrl:
      if (v != 0x01 && v != 0x05) return no("0x0743 may only be 0x01 or 0x05");
      break;
    case kGpuCtgpOffset:
      if (v > 25 || (v % 5) != 0) return no("cTGP increment must be 0..25 in 5 W steps");
      break;
    default:
      return no("address is not on the write allowlist");
  }
  g.ok = true;
  return g;
}

mgp::EcByte GuardedEc::Read(uint16_t addr) { return inner_.Read(addr); }

mgp::EcByte GuardedEc::Write(uint16_t addr, uint8_t value) {
  mgp::EcByte r;
  if (readOnly_) {
    r.error = "read-only mode (kill switch)";
    refused_++;
    lastRefusal_ = r.error;
    log_.Write(mgp::LogLevel::Warn, Fmt("EC write 0x%04X refused: read-only mode", addr));
    return r;
  }
  uint8_t cur = 0;
  bool haveCur = false;
  if (WriteGuard::MaskFor(addr) != 0xFF && WriteGuard::MaskFor(addr) != 0) {
    mgp::EcByte c = inner_.Read(addr);
    if (c.ok) { cur = c.value; haveCur = true; }
  }
  GuardVerdict g = WriteGuard::Check(addr, value, cur, haveCur);
  if (!g.ok) {
    r.error = g.why;
    refused_++;
    lastRefusal_ = g.why;
    log_.Write(mgp::LogLevel::Warn, g.why);
    return r;
  }
  writes_++;
  return inner_.Write(addr, value);
}

}  // namespace bc
