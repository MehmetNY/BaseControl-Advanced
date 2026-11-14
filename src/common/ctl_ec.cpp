// Controller: everything that talks to the Embedded Controller. Every write goes through GuardedEc/WriteGuard and is
// verified by reading it back; failures roll back to the bytes we read before touching anything.
#include <algorithm>
#include <cmath>

#include "../mgp/core/hardware_identity_guard.h"
#include "../mgp/core/smbios.h"
#include "controller.h"
#include "registers.h"
#include "util.h"

namespace bc {

using namespace reg;

// ------------------------------------------------------------------------------------------------ primitives
bool Controller::EcR(uint16_t a, uint8_t* v) {
  if (!gec_) return false;
  mgp::EcByte r = gec_->Read(a);
  if (!r.ok) return false;
  *v = r.value;
  return true;
}

bool Controller::EcW(uint16_t a, uint8_t v) {
  if (!gec_) return false;
  mgp::EcByte r = gec_->Write(a, v);
  if (!r.ok) {
    lastError_ = r.error;
    return false;
  }
  return true;
}

bool Controller::EcRmw(uint16_t a, uint8_t mask, uint8_t bits) {
  uint8_t cur;
  if (!EcR(a, &cur)) return false;
  uint8_t nv = static_cast<uint8_t>((cur & ~mask) | (bits & mask));
  if (nv == cur) return true;
  return EcW(a, nv);
}

bool Controller::EcVerify(uint16_t a, uint8_t mask, uint8_t want, int tries, uint32_t delayMs) {
  for (int i = 0; i < tries; i++) {
    if (hw_.clock) hw_.clock->SleepMs(delayMs);
    uint8_t v;
    if (EcR(a, &v) && (v & mask) == (want & mask)) return true;
  }
  return false;
}

bool Controller::EcActive() {
  uint8_t v;
  return EcR(kEcActive, &v) && (v & 0x05) == 0x01;
}

int Controller::Pl4Scale() {
  uint8_t v;
  if (EcR(kCustomProfile, &v) && (v & 0x80)) return 2;
  return 1;
}

// ------------------------------------------------------------------------------------------------ probe
void Controller::ProbeEc() {
  ecConnected_ = false;
  devClass_ = "unknown";
  projectId_ = -1;
  static const char* kEcCaps[] = {"pl.ec", "tcc", "fan.read", "fan.ctl", "fan.curve", "fan.fixed", "fan.ramp", "batt.mode", "fnlock",
                                  "acboot", "usbpower", "gpu.db"};
  auto allAbsent = [&](const char* why) {
    for (const char* c : kEcCaps) SetCap(c, "absent", why);
  };
  if (!hw_.ec || !gec_) {
    SetCap("ec", "absent", "no EC driver interface");
    allAbsent("no EC link");
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
  if (!plOk) SetCap("pl.ec", "absent", "power-limit registers unreadable");
  else if (b1 < 5 || b2 < 5 || b4 < 5 || b2 < b1) SetCap("pl.ec", "implausible", Fmt("PL1=%u PL2=%u PL4raw=%u", b1, b2, b4));
  else SetCap("pl.ec", "ok");

  if (!rd(kCpuTempLimitRead, &t)) SetCap("tcc", "absent", "0x0463 unreadable");
  else if ((t & 0x7F) > 40) SetCap("tcc", "implausible", Fmt("0x0463=0x%02X", t));
  else SetCap("tcc", "ok");

  uint8_t m751 = 0, m7c5 = 0, m7c6 = 0, r = 0;
  bool fanCtl = rd(kFanMode, &m751) && rd(kFanCurveEnable, &m7c5) && rd(kFanCurveLatch, &m7c6);
  SetCap("fan.read", rd(kCpuFanRpmHi, &r) && rd(kGpuFanRpmHi, &r) ? "ok" : "absent");
  const char* fs = fanCtl ? "ok" : "absent";
  SetCap("fan.ctl", fs);
  SetCap("fan.curve", fs);
  SetCap("fan.fixed", fs);
  SetCap("fan.ramp", rd(kFanSwitchSpeed, &r) ? "ok" : "absent");

  uint8_t bm = 0;
  if (!rd(kBatteryMode, &bm)) SetCap("batt.mode", "absent");
  else {
    uint8_t m = bm & 0x38;
    if (m == 0x08 || m == 0x18 || m == 0x28) SetCap("batt.mode", "ok");
    else SetCap("batt.mode", "implausible", Fmt("0x07A6=0x%02X", bm));
  }
  SetCap("fnlock", rd(kFnLock, &r) ? "ok" : "absent");
  SetCap("acboot", rd(kAcAutoBoot, &r) ? "ok" : "absent");
  SetCap("usbpower", rd(kUsbPowerShare, &r) ? "ok" : "absent");
  SetCap("gpu.db", "unsupported", "Dynamic Boost register semantics are unverified on this model");
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

  // fan mode (derived): fixed override > boost > custom curve > auto
  uint8_t m751 = 0, f1 = 0, f2 = 0, c5 = 0, c6 = 0;
  if (get(kFanMode, &m751) && get(kFanFixedCpu, &f1) && get(kFanFixedGpu, &f2) && get(kFanCurveEnable, &c5) && get(kFanCurveLatch, &c6)) {
    const char* mode = (f1 & 0x80) || (f2 & 0x80) ? "fixed" : (m751 & 0x40) ? "boost" : ((c5 & 0x80) && (c6 & 0x04)) ? "custom" : "auto";
    k.Set("fan.mode", mode);
    k.SetI("fan.fixed.cpu", (f1 & 0x80) ? std::min(100, f1 & 0x7F) : 0);
    k.SetI("fan.fixed.gpu", (f2 & 0x80) ? std::min(100, f2 & 0x7F) : 0);
  }
  if (get(kFanSwitchSpeed, &v)) k.SetI("fan.ramp", v & 0x7F);
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

// ------------------------------------------------------------------------------------------------ activation
Result Controller::EcBringUp() {
  if (!ecConnected_) return Fail("unsupported", "no EC link");
  if (hw_.oem && hw_.oem->Probe().running) return Fail("gate.oem", "the OEM service is running; take over from it first (Settings > Safety)");
  L(mgp::LogLevel::Info, "EC bring-up: activation byte + PECI power loop bits");
  GuardedEc::InitScope scope(*gec_);
  if (cfg_.Bool("cfg.fullInit", false)) {
    // documented full standalone bring-up (captured from the OEM service on the same ModelID); opt-in only
    bool ok = EcW(kEcHandshake1, 0x05) && EcW(kEcHandshake2, 0x01);
    if (!ok) return Fail("failed.hw", "EC handshake write failed: " + lastError_);
  }
  uint8_t a741 = 0;
  if (!EcR(kEcActive, &a741)) return Fail("failed.hw", "cannot read the activation byte");
  if (!EcW(kEcActive, 0x05)) return Fail("failed.hw", "activation write failed: " + lastError_);
  if (!EcRmw(kAcAutoBoot, 0x80, 0x80) || !EcRmw(kCustomProfile, 0x40, 0x40)) return Fail("failed.hw", "PECI power-loop enable failed: " + lastError_);
  // the EC clears bit 2 within ~60 ms but was seen pending for ~900 ms: allow up to 3 s
  bool active = false;
  for (int i = 0; i < 20 && !active; i++) {
    if (hw_.clock) hw_.clock->SleepMs(150);
    active = EcActive();
  }
  if (!active) return Fail("failed.verify", "EC did not report active after bring-up (0x0741 bit0/bit2)");
  return Ok("EC active");
}

Result Controller::EnsureEcActive() {
  if (!ecConnected_) return Fail("unsupported", "no EC link");
  uint8_t a726 = 0, a727 = 0;
  bool peci = EcR(kAcAutoBoot, &a726) && EcR(kCustomProfile, &a727) && (a726 & 0x80) && (a727 & 0x40);
  if (EcActive() && peci) return Ok();
  if (!cfg_.Bool("cfg.takeover", false))
    return Fail("needs.activation", "the EC power-management loop is not active; the OEM service normally enables it. "
                                    "Use Settings > Safety > Take over from the OEM Control Center");
  return EcBringUp();
}

// ------------------------------------------------------------------------------------------------ CPU power
Result Controller::SetEcPower(const std::string& key, int value, const KV&) {
  Result act = EnsureEcActive();
  if (!act.ok) return act;
  uint8_t r1, r2, r4;
  if (!EcR(kPl1, &r1) || !EcR(kPl2, &r2) || !EcR(kPl4, &r4)) return Fail("failed.hw", "cannot read the current power limits");
  const int scale = Pl4Scale();
  int pl1 = r1, pl2 = r2, pl4 = r4 * scale;
  if (key == "ec.pl1") pl1 = value;
  else if (key == "ec.pl2") pl2 = value;
  else pl4 = value;
  return WriteEcTriple(pl1, pl2, pl4);
}

// Writes PL1/PL2/PL4 as one group (the way the OEM service does), verifies, and undoes the change if the EC disagrees.
Result Controller::WriteEcTriple(int pl1, int pl2, int pl4) {
  Result act = EnsureEcActive();
  if (!act.ok) return act;
  uint8_t r1, r2, r4, rt = 0;
  if (!EcR(kPl1, &r1) || !EcR(kPl2, &r2) || !EcR(kPl4, &r4)) return Fail("failed.hw", "cannot read the current power limits");
  EcR(kCpuTempLimitRead, &rt);
  const int scale = Pl4Scale();
  const int o1 = r1, o2 = r2, o4 = r4 * scale;
  if (pl1 < 10 || pl2 < 10 || pl4 < 20) return Fail("range", "power limit below the minimum");
  if (pl2 < pl1) return Fail("range", "PL2 must be >= PL1");
  if (pl4 < pl2) return Fail("range", "PL4 must be >= PL2");
  if (pl4 % scale) return Fail("range", "PL4 must be a multiple of 2 W on this firmware");
  if (pl1 > 200 || pl2 > 200 || pl4 / scale > 250) return Fail("range", "power limit above the hard cap");
  // consent is about *raising* a value beyond the OEM maximum profile; keeping or lowering a value that is already high needs none
  const bool high = (pl1 > 120 && pl1 > o1) || (pl2 > 120 && pl2 > o2) || (pl4 > 200 && pl4 > o4);
  if (high && !cfg_.Bool("cfg.consent.ec_pl_high", false)) {
    Result r = Fail("gate.consent", "values above the OEM maximum profile (120/120/200 W) need your confirmation");
    r.data.Set("consent", "ec_pl_high");
    return r;
  }
  const bool raising = pl1 > o1 || pl2 > o2 || pl4 > o4;
  int temp = 0;
  { std::lock_guard<std::mutex> g(snapMu_); temp = static_cast<int>(sEc_.Int("cpu.temp.ec", 0)); }
  if (raising && temp >= 92) return Fail("gate.thermal", Fmt("CPU is at %d C; refusing to raise power limits above 92 C", temp));
  if (pl1 == o1 && pl2 == o2 && pl4 == o4) return Ok("already set");

  int off = rt & 0x7F;
  if (off > 40 || off == 0) off = 5;  // 5 = 100 C, the OEM default target
  auto writeGroup = [&]() { return EcW(kPl1, static_cast<uint8_t>(pl1)) && EcW(kPl2, static_cast<uint8_t>(pl2)) &&
                                   EcW(kPl4, static_cast<uint8_t>(pl4 / scale)) && EcW(kTccWrite, static_cast<uint8_t>(0x80 | off)); };
  auto verifyGroup = [&]() { return EcVerify(kPl1, 0xFF, static_cast<uint8_t>(pl1), 3, 150) && EcVerify(kPl2, 0xFF, static_cast<uint8_t>(pl2), 2, 50) &&
                                    EcVerify(kPl4, 0xFF, static_cast<uint8_t>(pl4 / scale), 2, 50); };
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; attempt++) {  // the OEM service writes the group twice; so do we if the first read-back disagrees
    if (!writeGroup()) break;
    ok = verifyGroup();
  }
  if (!ok) {
    gec_->WriteRollback(kPl1, r1);
    gec_->WriteRollback(kPl2, r2);
    gec_->WriteRollback(kPl4, r4);
    return Fail("reverted", "EC did not keep the new limits; previous values restored");
  }
  KV d;
  d.SetI("pl1", pl1); d.SetI("pl2", pl2); d.SetI("pl4", pl4);
  // diagnostics: does the CPU actually follow? (needs the MSR backend)
  if (msrOk_ && hw_.msr) {
    bool followed = false;
    for (int i = 0; i < 10 && !followed; i++) {
      if (hw_.clock) hw_.clock->SleepMs(250);
      uint64_t v;
      if (MsrR(rapl::kMsrPkgPowerLimit, &v)) {
        rapl::PkgLimit pk = rapl::DecodePkgLimit(v, units_);
        followed = std::fabs(pk.pl1W - pl1) <= 1.0;
      }
    }
    d.SetB("cpu.applied", followed);
    if (!followed) {
      L(mgp::LogLevel::Warn, "EC accepted the limits but MSR 0x610 PL1 does not follow (another tool may own the package power limit)");
      return Ok("EC values written; the CPU package limit (MSR 0x610) does not follow yet", d);
    }
  }
  return Ok("", d);
}

Result Controller::SetTcc(int celsius) {
  Result act = EnsureEcActive();
  if (!act.ok) return act;
  const int off = 105 - celsius;
  if (off < 5 || off > 35) return Fail("range", "temperature target out of range");
  uint8_t prev = 0;
  EcR(kCpuTempLimitRead, &prev);
  if (!EcW(kTccWrite, static_cast<uint8_t>(0x80 | off))) return Fail("failed.hw", "TCC write failed: " + lastError_);
  // 0x0463 refreshes on the EC polling cycle (~300-900 ms)
  if (!EcVerify(kCpuTempLimitRead, 0x7F, static_cast<uint8_t>(off), 10, 250)) {
    if ((prev & 0x7F) <= 35) gec_->WriteRollback(kTccWrite, static_cast<uint8_t>(0x80 | (prev & 0x7F)));
    return Fail("reverted", "EC did not report the new temperature target; previous value restored");
  }
  return Ok();
}

// ------------------------------------------------------------------------------------------------ fans
Result Controller::WriteFanTable(const fan::Curve* cpu, const fan::Curve* gpu) {
  struct Part { uint16_t base; fan::TableImage img; bool write; uint8_t old[48]; };
  Part parts[2];
  parts[0].base = kFanTableCpuUp;
  parts[1].base = kFanTableGpuUp;
  parts[0].write = cpu != nullptr;
  parts[1].write = gpu != nullptr;
  if (cpu) parts[0].img = fan::Pack(*cpu);
  if (gpu) parts[1].img = fan::Pack(*gpu);
  for (Part& p : parts) {
    if (!p.write) continue;
    for (int i = 0; i < 48; i++)
      if (!EcR(p.base + i, &p.old[i])) return Fail("failed.hw", "cannot read the current fan table");
  }
  auto rollback = [&]() {
    for (Part& p : parts)
      if (p.write)
        for (int i = 0; i < 48; i++) gec_->WriteRollback(p.base + i, p.old[i]);
  };
  auto writeAll = [&]() {
    for (Part& p : parts) {
      if (!p.write) continue;
      for (int i = 0; i < 16; i++)
        if (!EcW(p.base + i, p.img.up[i]) || !EcW(p.base + 0x10 + i, p.img.down[i]) || !EcW(p.base + 0x20 + i, p.img.duty[i])) return false;
    }
    return true;
  };
  if (!writeAll()) { rollback(); return Fail("failed.hw", "fan table write failed: " + lastError_); }
  // the OEM service writes the table, sets the custom-table latch and writes it again
  if (!EcRmw(kFanCurveLatch, 0x04, 0x04)) { rollback(); return Fail("failed.hw", "fan latch write failed"); }
  if (!writeAll()) { rollback(); return Fail("failed.hw", "fan table write failed (second pass): " + lastError_); }
  if (hw_.clock) hw_.clock->SleepMs(100);
  for (Part& p : parts) {
    if (!p.write) continue;
    for (int i = 0; i < 16; i++) {
      uint8_t a, b, c;
      if (!EcR(p.base + i, &a) || !EcR(p.base + 0x10 + i, &b) || !EcR(p.base + 0x20 + i, &c) || a != p.img.up[i] || b != p.img.down[i] || c != p.img.duty[i]) {
        rollback();
        return Fail("reverted", "fan table read-back differs; previous table restored");
      }
    }
  }
  return Ok();
}

Result Controller::SetFanCurve(bool isCpu, const std::string& text) {
  fan::Curve c;
  std::string why;
  if (!fan::FromText(text, &c) || !fan::Validate(c, &why)) return Fail("range", why);
  // store as the desired curve; apply to the EC only if custom mode is on (or being switched on)
  KV snap = Snapshot();
  if (snap.Str("fan.mode") != "custom") {
    cfg_.Set(isCpu ? "cfg.fan.cpu" : "cfg.fan.gpu", text);
    SaveConfig();
    return Ok("curve saved; it is written to the EC when Custom mode is selected");
  }
  Result act = EnsureEcActive();
  if (!act.ok) return act;
  if (!EcRmw(kFanCurveEnable, 0xE0, 0xE0)) return Fail("failed.hw", "cannot enable custom curve flag");
  Result w = WriteFanTable(isCpu ? &c : nullptr, isCpu ? nullptr : &c);
  if (w.ok) {
    cfg_.Set(isCpu ? "cfg.fan.cpu" : "cfg.fan.gpu", text);
    SaveConfig();
  }
  return w;
}

Result Controller::SetFanMode(int mode) {
  if (mode != 0) {
    Result act = EnsureEcActive();
    if (!act.ok) return act;
  }
  uint8_t o751 = 0, o759 = 0, o75a = 0, o7c5 = 0;
  EcR(kFanMode, &o751); EcR(kFanFixedCpu, &o759); EcR(kFanFixedGpu, &o75a); EcR(kFanCurveEnable, &o7c5);
  auto undo = [&]() {
    gec_->WriteRollback(kFanMode, o751); gec_->WriteRollback(kFanFixedCpu, o759);
    gec_->WriteRollback(kFanFixedGpu, o75a); gec_->WriteRollback(kFanCurveEnable, o7c5);
  };
  auto clearFixed = [&]() { return EcW(kFanFixedCpu, 0) && EcW(kFanFixedGpu, 0); };
  bool ok = true;
  switch (mode) {
    case 0:  // auto
      ok = clearFixed() && EcRmw(kFanMode, 0x40, 0) && EcRmw(kFanCurveEnable, 0x80, 0);
      break;
    case 1:  // boost
      ok = clearFixed() && EcRmw(kFanMode, 0x40, 0x40);
      break;
    case 2: {  // custom curves
      fan::Curve cc = fan::DefaultCpu(), gc = fan::DefaultGpu(), t;
      if (fan::FromText(cfg_.Str("cfg.fan.cpu"), &t)) cc = t;
      if (fan::FromText(cfg_.Str("cfg.fan.gpu"), &t)) gc = t;
      if (!clearFixed() || !EcRmw(kFanMode, 0x40, 0) || !EcRmw(kFanCurveEnable, 0xE0, 0xE0)) { undo(); return Fail("failed.hw", "cannot prepare custom fan mode"); }
      Result w = WriteFanTable(&cc, &gc);
      if (!w.ok) { undo(); return w; }
      break;
    }
    case 3: {  // fixed duty (bypasses EC protection; guarded by the watchdog)
      if (!cfg_.Bool("cfg.experimental", false))
        return Fail("gate.experimental", "fixed fan duty uses registers with a single source; enable Settings > Safety > Allow experimental writes");
      if (!cfg_.Bool("cfg.consent.fan_fixed", false)) {
        Result r = Fail("gate.consent", "fixed fan duty bypasses the EC's thermal logic; the BaseControl watchdog is the only protection");
        r.data.Set("consent", "fan_fixed");
        return r;
      }
      int c = static_cast<int>(cfg_.Int("cfg.fan.fixed.cpu", 60)), g = static_cast<int>(cfg_.Int("cfg.fan.fixed.gpu", 60));
      ok = EcW(kFanFixedCpu, static_cast<uint8_t>(0x80 | Clamp(c, 20, 100))) && EcW(kFanFixedGpu, static_cast<uint8_t>(0x80 | Clamp(g, 20, 100)));
      break;
    }
    default: return Fail("range", "bad fan mode");
  }
  if (!ok) { undo(); return Fail("failed.hw", "fan mode write failed: " + lastError_); }
  // verify
  if (hw_.clock) hw_.clock->SleepMs(150);
  uint8_t n751 = 0, n759 = 0, n75a = 0, n7c5 = 0, n7c6 = 0;
  EcR(kFanMode, &n751); EcR(kFanFixedCpu, &n759); EcR(kFanFixedGpu, &n75a); EcR(kFanCurveEnable, &n7c5); EcR(kFanCurveLatch, &n7c6);
  bool good = mode == 0 ? !(n751 & 0x40) && !(n759 & 0x80) && !(n75a & 0x80) && !(n7c5 & 0x80)
            : mode == 1 ? (n751 & 0x40) != 0
            : mode == 2 ? (n7c5 & 0x80) && (n7c6 & 0x04)
                        : (n759 & 0x80) && (n75a & 0x80);
  if (!good) { undo(); return Fail("reverted", "EC did not accept the fan mode; previous state restored"); }
  return Ok();
}

Result Controller::SetFixedFan(bool isCpu, int pct) {
  cfg_.SetI(isCpu ? "cfg.fan.fixed.cpu" : "cfg.fan.fixed.gpu", pct);
  SaveConfig();
  KV snap = Snapshot();
  if (snap.Str("fan.mode") != "fixed") return Ok("value saved; applied when Fixed mode is selected");
  if (!EcW(isCpu ? kFanFixedCpu : kFanFixedGpu, static_cast<uint8_t>(0x80 | pct))) return Fail("failed.hw", lastError_);
  return Ok();
}

Result Controller::SetFanRamp(int v) {
  Result act = EnsureEcActive();
  if (!act.ok) return act;
  uint8_t prev = 0;
  EcR(kFanSwitchSpeed, &prev);
  if (!EcW(kFanSwitchSpeed, static_cast<uint8_t>(0x80 | v))) return Fail("failed.hw", lastError_);
  if (!EcVerify(kFanSwitchSpeed, 0xFF, static_cast<uint8_t>(0x80 | v), 3, 100)) {
    gec_->WriteRollback(kFanSwitchSpeed, prev);
    return Fail("reverted", "EC did not keep the fan ramp value");
  }
  return Ok();
}

void Controller::FanWatchdog() {
  if (!ecConnected_) return;
  KV s = Snapshot();
  const std::string mode = s.Str("fan.mode");
  if (mode != "custom" && mode != "fixed") { fanOverTempTicks_ = 0; return; }
  int cpu = static_cast<int>(s.Int("cpu.temp.ec", s.Int("cpu.temp", 0)));
  int gpu = static_cast<int>(s.Int("gpu.temp.ec", s.Int("gpu.temp", 0)));
  if (cpu >= 97 || gpu >= 92) fanOverTempTicks_++;
  else fanOverTempTicks_ = 0;
  if (fanOverTempTicks_ >= 2) {
    L(mgp::LogLevel::Error, Fmt("fan watchdog: CPU %d C / GPU %d C in %s mode - forcing fan boost", cpu, gpu, mode.c_str()));
    EcW(kFanFixedCpu, 0);
    EcW(kFanFixedGpu, 0);
    EcRmw(kFanMode, 0x40, 0x40);
    fanOverTempTicks_ = 0;
    fanGuard_ = Fmt("CPU %d C / GPU %d C in %s mode", cpu, gpu, mode.c_str());
  }
}

// ------------------------------------------------------------------------------------------------ battery / preferences
Result Controller::SetBattMode(int m) {
  const uint8_t bits = m == 0 ? 0x08 : m == 1 ? 0x18 : 0x28;
  uint8_t prev = 0;
  EcR(kBatteryMode, &prev);
  if (!EcRmw(kBatteryMode, 0x38, bits)) return Fail("failed.hw", "battery mode write failed: " + lastError_);
  if (!EcVerify(kBatteryMode, 0x38, bits, 4, 150)) {
    gec_->WriteRollback(kBatteryMode, prev);
    return Fail("reverted", "EC did not keep the charging profile");
  }
  return Ok();
}

Result Controller::SetEcBit(uint16_t addr, uint8_t bit, bool on, const char* name) {
  const uint8_t m = static_cast<uint8_t>(1u << bit);
  uint8_t prev = 0;
  EcR(addr, &prev);
  if (!EcRmw(addr, m, on ? m : 0)) return Fail("failed.hw", std::string(name) + " write failed: " + lastError_);
  if (!EcVerify(addr, m, on ? m : 0, 4, 120)) {
    gec_->WriteRollback(addr, prev);
    return Fail("reverted", std::string("EC did not keep ") + name);
  }
  return Ok();
}

// "Powered USB while off" and "boot when AC is connected" are mutually exclusive in the OEM Control Center.
Result Controller::SetUsbVsBoot(bool usbPower, bool on) {
  uint8_t p726 = 0, p767 = 0;
  EcR(kAcAutoBoot, &p726);
  EcR(kUsbPowerShare, &p767);
  bool ok = true;
  if (on) {
    ok = usbPower ? (EcRmw(kAcAutoBoot, 0x08, 0) && EcRmw(kUsbPowerShare, 0x10, 0x10)) : (EcRmw(kUsbPowerShare, 0x10, 0) && EcRmw(kAcAutoBoot, 0x08, 0x08));
  } else {
    ok = usbPower ? EcRmw(kUsbPowerShare, 0x10, 0) : EcRmw(kAcAutoBoot, 0x08, 0);
  }
  if (!ok) {
    gec_->WriteRollback(kAcAutoBoot, p726);
    gec_->WriteRollback(kUsbPowerShare, p767);
    return Fail("failed.hw", "write failed: " + lastError_);
  }
  const uint16_t a = usbPower ? kUsbPowerShare : kAcAutoBoot;
  const uint8_t m = usbPower ? 0x10 : 0x08;
  if (!EcVerify(a, m, on ? m : 0, 4, 120)) {
    gec_->WriteRollback(kAcAutoBoot, p726);
    gec_->WriteRollback(kUsbPowerShare, p767);
    return Fail("reverted", "EC did not keep the value");
  }
  return Ok();
}

Result Controller::SetGpuDb(bool) { return Fail("unsupported", "Dynamic Boost control is not available on this model in this release"); }

}  // namespace bc
