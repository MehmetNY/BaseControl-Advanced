// Controller: the embedded controller, READ ONLY. The live values every slider follows (power limits, temperatures, fan speed and mode,
// battery state, switches) come from here, and so does the check that a change really arrived. Nothing in this file writes: the only EC
// writes of this program are the two cTGP registers of the GPU power engine (ctl_gpu.cpp, through GuardedEc).
#include <algorithm>
#include <cmath>

#include "../mgp/core/hardware_identity_guard.h"
#include "../mgp/core/smbios.h"
#include "controller.h"
#include "i18n_mark.h"
#include "registers.h"
#include "util.h"

namespace bc {

using namespace reg;

// ------------------------------------------------------------------------------------------------ primitive
bool Controller::EcR(uint16_t a, uint8_t* v) {
  if (!gec_) return false;
  mgp::EcByte r = gec_->Read(a);
  if (!r.ok) return false;
  *v = r.value;
  return true;
}

// ------------------------------------------------------------------------------------------------ probe
void Controller::ProbeEc() {
  ecConnected_ = false;
  devClass_ = "unknown";
  projectId_ = -1;
  static const char* kEcCaps[] = {"ec.pl", "ec.tcc", "ec.fan", "ec.batt", "ec.fnlock", "ec.acboot", "ec.usbpower", "fan.read"};
  auto allAbsent = [&](const char* why) {
    for (const char* c : kEcCaps) SetCap(c, "absent", why);
  };
  if (!hw_.ec || !gec_) {
    SetCap("ec", "absent", N_("no EC driver interface"));
    allAbsent(N_("no EC link"));
    return;
  }
  uint32_t werr = 0;
  mgp::DriverState st = hw_.ec->Probe(&werr);
  if (st != mgp::DriverState::Connected) {
    SetCap("ec", st == mgp::DriverState::AccessDenied ? "denied" : st == mgp::DriverState::Absent ? "absent" : "error", Fmt("win32=%u", werr));
    allAbsent("EC driver not reachable (install the OEM ACPI driver; run elevated)");
    return;
  }
  ecConnected_ = true;
  SetCap("ec", "ok");

  // identity: exact SMBIOS match (GM5TG7Y) = verified; same ModelID (project id 26 = IDY_7Y) = family; else unknown
  bool exact = false;
  if (hw_.smbios) {
    std::vector<uint8_t> raw;
    std::string err;
    mgp::SmbiosInfo info;
    bool ok = hw_.smbios->ReadRaw(raw, err) && mgp::ParseRawSmbios(raw.data(), raw.size(), info);
    mgp::IdentityResult id = mgp::EvaluateIdentity(info, ok);
    exact = id.verdict == mgp::IdentityVerdict::Exact;
  }
  uint8_t pid = 0;
  if (EcR(kProjectId, &pid)) projectId_ = pid;
  if (exact) devClass_ = "verified";
  else if (projectId_ == 26) devClass_ = "family";
  {
    std::lock_guard<std::mutex> g(snapMu_);
    sCaps_.SetI("dev.project", projectId_);
  }

  auto rd = [&](uint16_t a, uint8_t* v) { return EcR(a, v); };
  uint8_t b1 = 0, b2 = 0, b4 = 0, t = 0;
  bool plOk = rd(kPl1, &b1) && rd(kPl2, &b2) && rd(kPl4, &b4);
  // What the EC shows is what every change is checked against, so each feature needs its registers to be readable and to look plausible.
  if (!plOk) SetCap("ec.pl", "absent", N_("power-limit registers unreadable"));
  else if (b1 < 5 || b2 < 5 || b4 < 5 || b2 < b1) SetCap("ec.pl", "implausible", Fmt("PL1=%u PL2=%u PL4raw=%u", b1, b2, b4));
  else SetCap("ec.pl", "ok");

  if (!rd(kCpuTempLimitRead, &t)) SetCap("ec.tcc", "absent", N_("0x0463 unreadable"));
  else if ((t & 0x7F) > 40) SetCap("ec.tcc", "implausible", Fmt("0x0463=0x%02X", t));
  else SetCap("ec.tcc", "ok");

  uint8_t m751 = 0, m7c5 = 0, m7c6 = 0, r = 0;
  const bool fanRegs = rd(kFanMode, &m751) && rd(kFanCurveEnable, &m7c5) && rd(kFanCurveLatch, &m7c6) && rd(kFanFixedCpu, &r) && rd(kFanFixedGpu, &r);
  SetCap("fan.read", rd(kCpuFanRpmHi, &r) && rd(kGpuFanRpmHi, &r) ? "ok" : "absent");
  SetCap("ec.fan", fanRegs ? "ok" : "absent");

  uint8_t bm = 0;
  if (!rd(kBatteryMode, &bm)) SetCap("ec.batt", "absent");
  else {
    uint8_t m = bm & 0x38;
    if (m == 0x08 || m == 0x18 || m == 0x28) SetCap("ec.batt", "ok");
    else SetCap("ec.batt", "implausible", Fmt("0x07A6=0x%02X", bm));
  }
  SetCap("ec.fnlock", rd(kFnLock, &r) ? "ok" : "absent");
  SetCap("ec.acboot", rd(kAcAutoBoot, &r) ? "ok" : "absent");
  SetCap("ec.usbpower", rd(kUsbPowerShare, &r) ? "ok" : "absent");
}

// ------------------------------------------------------------------------------------------------ polling
namespace {
int Rpm(uint8_t hi, uint8_t lo) {
  int v = (hi << 8) | lo;
  return v == 0xFFFF ? -1 : v;
}
}  // namespace

void Controller::PollEc(bool full) {
  if (!ecConnected_) return;
  KV k;
  auto get = [&](uint16_t a, uint8_t* v) { return EcR(a, v); };
  uint8_t v = 0, lo = 0;
  if (get(kEcActive, &v)) k.SetB("ec.active", (v & 0x05) == 0x01);
  uint8_t a726 = 0, a727 = 0;
  if (get(kAcAutoBoot, &a726) && get(kCustomProfile, &a727)) {
    k.SetB("ec.peci", (a726 & 0x80) && (a727 & 0x40));
    k.SetI("ec.pl4.scale", (a727 & 0x80) ? 2 : 1);
  }
  const int scale = (a727 & 0x80) ? 2 : 1;
  uint8_t p1, p2, p4;
  if (get(kPl1, &p1) && get(kPl2, &p2) && get(kPl4, &p4)) {
    k.SetI("ec.pl1", p1);
    k.SetI("ec.pl2", p2);
    k.SetI("ec.pl4", p4 * scale);
  }
  if (get(kCpuTempLimitRead, &v)) k.SetI("ec.tcc", 105 - (v & 0x7F));
  if (get(kCpuTemp, &v) && v > 0 && v < 125) k.SetI("cpu.temp.ec", v);
  if (get(kGpuTemp, &v) && v > 0 && v < 125) k.SetI("gpu.temp.ec", v);
  uint8_t hi;
  if (get(kCpuFanRpmHi, &hi) && get(kCpuFanRpmLo, &lo)) {
    int r = Rpm(hi, lo);
    if (r >= 0) k.SetI("fan.cpu.rpm", r);
  }
  if (get(kGpuFanRpmHi, &hi) && get(kGpuFanRpmLo, &lo)) {
    int r = Rpm(hi, lo);
    if (r >= 0) k.SetI("fan.gpu.rpm", r);
  }
  if (get(kCpuFanDuty, &v)) k.SetI("fan.cpu.duty", std::min(100, v / 2));
  if (get(kGpuFanDuty, &v)) k.SetI("fan.gpu.duty", std::min(100, v / 2));
  if (get(kAcStatus, &v)) k.SetB("ec.ac", v & 1);

  // fan mode (derived): a fixed duty (set by another program) > boost > custom curve > auto
  uint8_t m751 = 0, f1 = 0, f2 = 0, c5 = 0, c6 = 0;
  if (get(kFanMode, &m751) && get(kFanFixedCpu, &f1) && get(kFanFixedGpu, &f2) && get(kFanCurveEnable, &c5) && get(kFanCurveLatch, &c6)) {
    const char* mode = (f1 & 0x80) || (f2 & 0x80) ? "fixed" : (m751 & 0x40) ? "boost" : ((c5 & 0x80) && (c6 & 0x04)) ? "custom" : "auto";
    k.Set("fan.mode", mode);
    k.SetI("fan.fixed.cpu", (f1 & 0x80) ? std::min(100, f1 & 0x7F) : 0);
    k.SetI("fan.fixed.gpu", (f2 & 0x80) ? std::min(100, f2 & 0x7F) : 0);
  }
  if (get(kBatteryMode, &v)) {
    uint8_t m = v & 0x38;
    k.Set("batt.mode", m == 0x08 ? "highcap" : m == 0x18 ? "balanced" : m == 0x28 ? "eco" : "unknown");
  }
  if (get(kFnLock, &v)) k.SetB("pref.fnlock", v & 0x10);
  if (full) {
    // slow group
    if (get(kAcAutoBoot, &v)) k.SetB("pref.acboot", v & 0x08);
    if (get(kUsbPowerShare, &v)) k.SetB("pref.usbpower", v & 0x10);
    if (get(kSsdTemp, &v) && v > 5 && v < 120) k.SetI("ssd.temp", v);
    uint8_t soc;
    if (get(kBatterySoc, &soc) && soc <= 100) k.SetI("batt.ec.pct", soc);
    auto le16 = [&](uint16_t a, int* out) {
      uint8_t l, h;
      if (!get(a, &l) || !get(a + 1, &h)) return false;
      *out = l | (h << 8);
      return true;
    };
    int x;
    if (le16(kBatCapDesignLo, &x) && x > 1000 && x < 20000) k.SetI("batt.design", x);
    if (le16(kBatCapFullLo, &x) && x > 1000 && x < 20000) k.SetI("batt.full", x);
    if (le16(kBatCycleLo, &x) && x < 5000) k.SetI("batt.cycles", x);
    if (le16(kBatVoltageLo, &x) && x > 5000 && x < 30000) k.SetI("batt.volt", x);
    if (le16(kBatRateLo, &x)) k.SetI("batt.rate", static_cast<int16_t>(x));
    if (le16(kBatTempLo, &x) && x > 2500 && x < 3600) k.SetF("batt.temp", x / 10.0 - 273.15, 1);
    // fan tables (once per slow cycle at most, cheap enough: 96 reads only when something may have changed)
    static const struct { uint16_t base; const char* key; } kTables[] = {{kFanTableCpuUp, "fan.cpu.curve"}, {kFanTableGpuUp, "fan.gpu.curve"}};
    for (auto& t : kTables) {
      fan::TableImage img;
      bool ok = true;
      for (int i = 0; i < 16 && ok; i++) {
        uint8_t a = 0, b = 0, c = 0;
        ok = get(t.base + i, &a) && get(t.base + 0x10 + i, &b) && get(t.base + 0x20 + i, &c);
        img.up[i] = a;
        img.down[i] = b;
        img.duty[i] = c;
      }
      fan::Curve cv;
      if (ok && fan::Unpack(img, &cv, nullptr)) k.Set(t.key, fan::ToText(cv));
      else k.Set(t.key, "");
    }
  }
  std::lock_guard<std::mutex> g(snapMu_);
  if (full) sEc_ = k;
  else
    for (auto& p : k.All()) sEc_.Set(p.first, p.second);
}

}  // namespace bc
