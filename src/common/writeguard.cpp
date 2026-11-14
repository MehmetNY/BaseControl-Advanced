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

constexpr Rule kRules[] = {
    {kEcActive, kEcActive, 0xFF},
    {kPl1, kPl1, 0xFF},
    {kPl2, kPl2, 0xFF},
    {kPl4, kPl4, 0xFF},
    {kTccWrite, kTccWrite, 0xFF},
    {kGpuCtrl, kGpuCtrl, 0x07},
    {kGpuCtgpOffset, kGpuCtgpOffset, 0xFF},
    {kGpuTpp, kGpuTpp, 0xFF},
    {kGpuDbMax, kGpuDbMax, 0xFF},
    {kFanMode, kFanMode, 0x40},
    {kFanFixedCpu, kFanFixedGpu, 0xFF},
    {kFanSwitchSpeed, kFanSwitchSpeed, 0xFF},
    {kFanCurveEnable, kFanCurveEnable, 0xE0},
    {kFanCurveLatch, kFanCurveLatch, 0x04},
    {kFanTableCpuUp, kFanTableEnd, 0xFF},
    {kFnLock, kFnLock, 0x10},
    {kAcAutoBoot, kAcAutoBoot, 0x88},
    {kCustomProfile, kCustomProfile, 0x40},
    {kUsbPowerShare, kUsbPowerShare, 0x10},
    {kBatteryMode, kBatteryMode, 0x38},
    {kLightbarR, kLightbarB, 0xFF},
    // bring-up registers (init scope only, exact values)
    {kBiosCtrl, kBiosCtrl, 0xFF},
    {kEcHandshake2, kEcHandshake1, 0xFF},
    {kFeatureEnable, kFeatureEnable, 0xFF},
    {kOcpl, kOcpl, 0xFF},
};
}  // namespace

uint8_t WriteGuard::MaskFor(uint16_t addr) {
  if (IsNeverWrite(addr)) return 0;
  for (const Rule& r : kRules)
    if (addr >= r.lo && addr <= r.hi) return r.mask;
  return 0;
}

GuardVerdict WriteGuard::Check(uint16_t addr, uint8_t v, uint8_t cur, bool haveCur, bool initScope) {
  GuardVerdict g;
  auto no = [&](const std::string& w) { g.ok = false; g.why = Fmt("EC 0x%04X <- 0x%02X refused: %s", addr, v, w.c_str()); return g; };
  if (IsNeverWrite(addr)) return no("address is on the never-write list");
  uint8_t mask = MaskFor(addr);
  if (!mask) return no("address is not on the write allowlist");
  if (mask != 0xFF) {
    if (!haveCur) return no("masked register needs a fresh read-modify-write");
    if (((cur ^ v) & ~mask & 0xFF) != 0) return no("bits outside the permitted mask would change");
  }
  auto inRange = [&](int lo, int hi) { return v >= lo && v <= hi; };
  const bool isInit = addr == kBiosCtrl || addr == kEcHandshake1 || addr == kEcHandshake2 || addr == kFeatureEnable || addr == kOcpl;
  if (isInit) {
    if (!initScope) return no("bring-up register: only allowed inside an explicit EC bring-up");
    uint8_t want = addr == kBiosCtrl ? 0x41 : addr == kEcHandshake1 ? 0x05 : addr == kEcHandshake2 ? 0x01 : addr == kFeatureEnable ? 0x81 : 0x0C;
    if (v != want) return no("bring-up register accepts only its documented value");
    g.ok = true;
    return g;
  }
  switch (addr) {
    case kEcActive:
      if (v != 0x05) return no("only the bring-up value 0x05 is allowed");
      break;
    case kPl1: case kPl2:
      if (!inRange(10, 210)) return no("PL1/PL2 must be 10..210 W");
      break;
    case kPl4:
      if (!inRange(5, 250)) return no("PL4 raw must be 5..250");
      break;
    case kTccWrite:
      if (!(v & 0x80) || (v & 0x7F) > 35) return no("TCC register must be 0x80 | 0..35");
      break;
    case kGpuCtrl:
      if (v != 0x01 && v != 0x05 && v != 0x07) return no("0x0743 may only be 0x01, 0x05 or 0x07");
      break;
    case kGpuCtgpOffset:
      if (v > 25 || (v % 5) != 0) return no("cTGP increment must be 0..25 in 5 W steps");
      break;
    case kGpuTpp:
      if (v != 0 && v != 255) return no("TPP target must be 0 or 255");
      break;
    case kGpuDbMax:
      if (v > 25) return no("DB max must be 0..25");
      break;
    case kFanFixedCpu: case kFanFixedGpu:
      if (v != 0 && (!(v & 0x80) || (v & 0x7F) > 100)) return no("fixed duty must be 0 or 0x80|1..100");
      break;
    case kFanSwitchSpeed:
      if (v != 0 && (!(v & 0x80) || (v & 0x7F) == 0)) return no("switch speed must be 0 or 0x80|1..127");
      break;
    case kBatteryMode: {
      uint8_t m = v & 0x38;
      if (m != 0x08 && m != 0x18 && m != 0x28) return no("battery mode must be 0x08, 0x18 or 0x28");
      break;
    }
    case kLightbarR: case kLightbarG: case kLightbarB:
      if (v > 0x24) return no("light bar level must be 0..0x24");
      break;
    default:
      break;
  }
  if (addr >= kFanTableCpuUp && addr <= kFanTableEnd) {
    // table rows: temperatures 0..105 or 0xFF padding, duty raw 0..200 or 0xFF is never written
    bool isDuty = (addr >= kFanTableCpuDuty && addr < kFanTableCpuDuty + 16) || (addr >= kFanTableGpuDuty && addr < kFanTableGpuDuty + 16);
    if (isDuty && v > 200) return no("fan duty raw must be 0..200");
    if (!isDuty && v != 0xFF && v > 105) return no("fan temperature must be 0..105 or 0xFF padding");
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
  GuardVerdict g = WriteGuard::Check(addr, value, cur, haveCur, initScope_);
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

mgp::EcByte GuardedEc::WriteRollback(uint16_t addr, uint8_t value) {
  mgp::EcByte r;
  const uint8_t mask = WriteGuard::MaskFor(addr);
  if (readOnly_ || mask == 0) {
    r.error = "rollback refused";
    refused_++;
    return r;
  }
  mgp::EcByte c = inner_.Read(addr);
  if (!c.ok || ((c.value ^ value) & ~mask & 0xFF) != 0) {
    r.error = "rollback refused: bits outside the register mask differ";
    refused_++;
    return r;
  }
  writes_++;
  return inner_.Write(addr, value);
}

}  // namespace bc
