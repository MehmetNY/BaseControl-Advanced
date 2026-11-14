// Controller: CPU. Two independent layers:
//   * MSR (via PawnIO "IntelMSR" allow-listed module): package power limit, Icc max, voltage offsets (OC mailbox),
//     prefetchers, plus read-only diagnostics (RAPL energy, throttling status). Every write is verified and undone on mismatch.
//   * Windows power-plan CPU policy (boost mode, EPP, min/max state, core parking): no driver needed.
#include <cmath>

#include "controller.h"
#include "i18n_mark.h"
#include "util.h"

namespace bc {

namespace {
int PlaneFor(const std::string& key) { return key == "msr.volt.core" ? 0 : key == "msr.volt.cache" ? 2 : 3; }

// OC mailbox: write the command, then poll until the run/busy bit clears.
bool OcCall(IMsr* msr, uint64_t cmd, uint64_t* resp) {
  if (!msr->Write(rapl::kMsrOcMailbox, cmd)) return false;
  for (int i = 0; i < 20; i++) {
    uint64_t v;
    if (!msr->Read(rapl::kMsrOcMailbox, &v)) return false;
    if (!rapl::OcBusy(v)) {
      if (resp) *resp = v;
      return rapl::OcResponseCode(v) == 0;
    }
  }
  return false;
}
}  // namespace

bool Controller::MsrR(uint32_t m, uint64_t* v) { return hw_.msr && msrOk_ && hw_.msr->Read(m, v); }

// ------------------------------------------------------------------------------------------------ probe
void Controller::ProbeCpu() {
  msrOk_ = false;
  std::string why;
  if (hw_.msr && hw_.msr->Open(&why)) {
    uint64_t u;
    if (hw_.msr->Read(rapl::kMsrRaplUnit, &u)) {
      units_ = rapl::DecodeUnits(u);
      msrOk_ = true;
    } else {
      why = N_("MSR read failed (is the IntelMSR module loaded for this CPU?)");
    }
  }
  const char* kMsrCaps[] = {"msr.pkg", "msr.psys", "msr.bdp", "msr.icc", "msr.volt", "msr.pf"};
  if (!msrOk_) {
    SetCap("msr", "absent", why.empty() ? N_("PawnIO driver / IntelMSR module not available") : why);
    for (const char* c : kMsrCaps) SetCap(c, "absent", N_("needs the PawnIO MSR driver"));
  } else {
    SetCap("msr", "ok", hw_.msr->Name());
    uint64_t v;
    if (MsrR(rapl::kMsrPkgPowerLimit, &v)) {
      if (rapl::DecodePkgLimit(v, units_).lock) SetCap("msr.pkg", "locked", N_("MSR 0x610 is locked by the firmware until reboot"));
      else SetCap("msr.pkg", "ok");
    } else SetCap("msr.pkg", "absent");
    if (MsrR(rapl::kMsrPlatformPowerLimit, &v)) {
      if (rapl::DecodePkgLimit(v, units_).lock) SetCap("msr.psys", "locked", N_("MSR 0x65C (platform power limit) is locked by the firmware until reboot"));
      else SetCap("msr.psys", "ok");
    } else SetCap("msr.psys", "absent", N_("this CPU does not expose the platform (PSys) power limit"));
    if (MsrR(rapl::kMsrPowerCtl, &v)) SetCap("msr.bdp", "ok");
    else SetCap("msr.bdp", "absent", N_("MSR 0x1FC (POWER_CTL) cannot be read"));
    if (MsrR(rapl::kMsrVrCurrentConfig, &v)) {
      if (rapl::IccLocked(v)) SetCap("msr.icc", "locked", N_("MSR 0x601 is locked by the firmware"));
      else SetCap("msr.icc", "ok");
    } else SetCap("msr.icc", "absent");
    uint64_t resp = 0;
    if (OcCall(hw_.msr, rapl::OcReadVoltageCmd(0), &resp)) SetCap("msr.volt", "ok");
    else SetCap("msr.volt", "locked", N_("the voltage mailbox is closed by the BIOS (typical for 11th-gen undervolt protection)"));
    if (MsrR(0x1A4, &v)) SetCap("msr.pf", "ok");
    else SetCap("msr.pf", "absent");
  }
  CpuPolicy p;
  SetCap("os.cpu", hw_.power && hw_.power->GetCpuPolicy(&p) ? "ok" : "absent", "");
}

// ------------------------------------------------------------------------------------------------ polling
void Controller::PollMsr(bool full) {
  if (!msrOk_ || !hw_.msr) return;
  KV k;
  uint64_t v;
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  if (MsrR(rapl::kMsrPkgPowerLimit, &v)) {
    rapl::PkgLimit p = rapl::DecodePkgLimit(v, units_);
    k.SetF("msr.pl1", p.pl1W, 0);
    k.SetF("msr.pl2", p.pl2W, 0);
    k.SetF("msr.tau1", p.tau1S, 1);
    k.SetF("msr.tau2", p.tau2S * 1000.0, 1);
    k.SetB("msr.pl1.en", p.pl1En);
    k.SetB("msr.pl2.en", p.pl2En);
    k.SetB("msr.clamp", p.pl1Clamp && p.pl2Clamp);
    k.SetB("msr.lock", p.lock);
  }
  if (MsrR(rapl::kMsrPlatformPowerLimit, &v)) {
    rapl::PkgLimit p = rapl::DecodePkgLimit(v, units_);
    k.SetF("msr.psys.pl1", p.pl1W, 0);
    k.SetF("msr.psys.pl2", p.pl2W, 0);
    k.SetB("msr.psys.pl1.en", p.pl1En);
    k.SetB("msr.psys.pl2.en", p.pl2En);
    k.SetB("msr.psys.lock", p.lock);
  }
  if (MsrR(rapl::kMsrPowerCtl, &v)) k.SetB("msr.bdp", (v >> rapl::kPowerCtlBdProchotBit) & 1);
  // energy counters -> watts
  const uint32_t e[6] = {rapl::kMsrPkgEnergy, rapl::kMsrPp0Energy, 0x641, 0x619, 0x64D, 0};
  static const char* kNames[5] = {"cpu.pkg.w", "cpu.core.w", "cpu.igpu.w", "cpu.dram.w", "cpu.psys.w"};
  const double dt = lastEnergyMs_ ? (now - lastEnergyMs_) / 1000.0 : 0;
  for (int i = 0; i < 5; i++) {
    if (!MsrR(e[i], &v)) continue;
    uint32_t cur = static_cast<uint32_t>(v & 0xFFFFFFFF);
    if (dt > 0.15 && lastEnergy_[i] != 0xFFFFFFFFFFFFFFFFULL && lastEnergyMs_)
      k.SetF(kNames[i], rapl::EnergyToWatts(static_cast<uint32_t>(lastEnergy_[i]), cur, dt, units_), 1);
    lastEnergy_[i] = cur;
  }
  if (MsrR(rapl::kMsrPkgPerfStatus, &v)) {
    uint32_t cur = static_cast<uint32_t>(v & 0xFFFFFFFF);
    if (lastPerfMs_ && now > lastPerfMs_) {
      uint32_t d = cur - static_cast<uint32_t>(lastPerfStat_);
      double frac = d * units_.timeS / ((now - lastPerfMs_) / 1000.0);
      k.SetF("cpu.throttle.pct", Clamp(frac * 100.0, 0.0, 100.0), 1);
    }
    lastPerfStat_ = cur;
    lastPerfMs_ = now;
  }
  lastEnergyMs_ = now;
  if (MsrR(rapl::kMsrPkgThermStatus, &v)) {
    k.SetB("cpu.st.thermal", v & (1u << 0));
    k.SetB("cpu.st.prochot", v & (1u << 2));
    k.SetB("cpu.st.critical", v & (1u << 4));
    k.SetB("cpu.st.power", v & (1u << 10));
    k.SetB("cpu.st.current", v & (1u << 12));
    k.SetB("cpu.st.cross", v & (1u << 14));
    k.SetB("cpu.log.thermal", v & (1u << 1));
    k.SetB("cpu.log.prochot", v & (1u << 3));
    k.SetB("cpu.log.power", v & (1u << 11));
    uint64_t tt;
    if (MsrR(rapl::kMsrTempTarget, &tt)) {
      int tj = rapl::TjMax(tt), off = rapl::TccOffset(tt);
      k.SetI("msr.tjmax", tj);
      k.SetI("msr.tcc", off);
      int temp = tj - off - rapl::ThermReadout(v);
      if (temp > 0 && temp < 130) k.SetI("cpu.temp.msr", temp);
    }
  }
  if (MsrR(rapl::kMsrPerfStatus, &v)) k.SetI("cpu.ratio", rapl::PerfStatusRatio(v));
  if (MsrR(rapl::kMsrPlatformInfo, &v)) k.SetI("cpu.base.ratio", rapl::PlatformBaseRatio(v));
  if (MsrR(rapl::kMsrVrCurrentConfig, &v)) {
    k.SetF("msr.icc", rapl::DecodeIccMaxA(v), 0);
    k.SetB("msr.icc.lock", rapl::IccLocked(v));
  }
  if (MsrR(rapl::kMsrMiscFeatureControl, &v)) {
    k.SetB("msr.pf.l2", !(v & 1));
    k.SetB("msr.pf.l2adj", !(v & 2));
    k.SetB("msr.pf.dcu", !(v & 4));
    k.SetB("msr.pf.dcuip", !(v & 8));
  }
  if (full) {
    if (MsrR(rapl::kMsrPkgPowerInfo, &v)) {
      rapl::PkgInfo i = rapl::DecodePkgInfo(v, units_);
      k.SetF("msr.tdp", i.tdpW, 0);
      k.SetF("msr.minw", i.minW, 0);
      k.SetF("msr.maxw", i.maxW, 0);
    }
    if (MsrR(rapl::kMsrTurboRatio, &v)) {
      std::string t;
      for (int n = 0; n < 8; n++) t += Fmt("%s%d", n ? "," : "", rapl::TurboRatio(v, n));
      k.Set("msr.turbo", t);
    }
    if (CapState("msr.volt") == "ok") {
      uint64_t resp;
      static const char* names[3] = {"msr.volt.core", "msr.volt.cache", "msr.volt.sa"};
      static const int planes[3] = {0, 2, 3};
      for (int i = 0; i < 3; i++)
        if (OcCall(hw_.msr, rapl::OcReadVoltageCmd(planes[i]), &resp)) k.SetI(names[i], rapl::OcDecodeOffsetMv(resp));
    }
  }
  std::lock_guard<std::mutex> g(snapMu_);
  if (full) sMsr_ = k;
  else
    for (auto& p : k.All()) sMsr_.Set(p.first, p.second);
}

// ------------------------------------------------------------------------------------------------ MSR writes
Result Controller::SetMsr(const std::string& key, int value) {
  if (!msrOk_ || !hw_.msr) return Fail("unsupported", "MSR backend unavailable");
  if (!cfg_.Bool("cfg.consent.msr_write", false)) {
    Result r = Fail("gate.consent", N_("direct CPU register writes can destabilise the system; confirmation required"));
    r.data.Set("consent", "msr_write");
    return r;
  }
  const bool raising = (key == "msr.pl1" || key == "msr.pl2" || key == "msr.psys.pl1" || key == "msr.psys.pl2");
  KV snap = Snapshot();
  if (raising && snap.Int("cpu.temp", snap.Int("cpu.temp.msr", 0)) >= 92 && value > snap.Int(key, 0))
    return Fail("gate.thermal", N_("CPU is hotter than 92 C; refusing to raise a power limit"));

  if (key == "msr.pl1" || key == "msr.pl2" || key == "msr.tau1" || key == "msr.tau2" || key == "msr.pl1.en" || key == "msr.pl2.en" || key == "msr.clamp") {
    uint64_t old;
    if (!MsrR(rapl::kMsrPkgPowerLimit, &old)) return Fail("failed.hw", "cannot read MSR 0x610");
    rapl::PkgLimit p = rapl::DecodePkgLimit(old, units_);
    if (p.lock) return Fail("unsupported", N_("MSR 0x610 is locked until the next reboot"));
    if (key == "msr.pl1") p.pl1W = value;
    else if (key == "msr.pl2") p.pl2W = value;
    else if (key == "msr.tau1") p.tau1S = value;
    else if (key == "msr.tau2") p.tau2S = value / 1000.0;
    else if (key == "msr.pl1.en") p.pl1En = value != 0;
    else if (key == "msr.pl2.en") p.pl2En = value != 0;
    else p.pl1Clamp = p.pl2Clamp = value != 0;
    if (p.pl2W < p.pl1W) return Fail("range", N_("PL2 must be >= PL1"));
    uint64_t nv;
    if (!rapl::EncodePkgLimit(old, p, units_, &nv)) return Fail("range", N_("value cannot be represented in the RAPL fields"));
    if (!hw_.msr->Write(rapl::kMsrPkgPowerLimit, nv)) return Fail("failed.hw", N_("MSR write refused by the driver"));
    if (hw_.clock) hw_.clock->SleepMs(120);
    uint64_t back = 0;
    if (!MsrR(rapl::kMsrPkgPowerLimit, &back) || ((back ^ nv) & 0x7FFFFFFFFFFFFFFFULL) != 0) {
      hw_.msr->Write(rapl::kMsrPkgPowerLimit, old);
      return Fail("reverted", N_("the value did not stick (the EC/firmware rewrote MSR 0x610); previous value restored"));
    }
    return Ok();
  }
  if (key == "msr.psys.pl1" || key == "msr.psys.pl2" || key == "msr.psys.pl1.en" || key == "msr.psys.pl2.en") {
    uint64_t old;
    if (!MsrR(rapl::kMsrPlatformPowerLimit, &old)) return Fail("failed.hw", "cannot read MSR 0x65C");
    rapl::PkgLimit p = rapl::DecodePkgLimit(old, units_);
    if (p.lock) return Fail("unsupported", N_("MSR 0x65C is locked until the next reboot"));
    if (key == "msr.psys.pl1") p.pl1W = value;
    else if (key == "msr.psys.pl2") p.pl2W = value;
    else if (key == "msr.psys.pl1.en") p.pl1En = value != 0;
    else p.pl2En = value != 0;
    if (p.pl2W < p.pl1W) return Fail("range", N_("platform PL2 must be >= PL1"));
    uint64_t nv;
    if (!rapl::EncodePkgLimit(old, p, units_, &nv)) return Fail("range", N_("value cannot be represented in the RAPL fields"));
    if (!hw_.msr->Write(rapl::kMsrPlatformPowerLimit, nv)) return Fail("failed.hw", N_("MSR write refused by the driver"));
    if (hw_.clock) hw_.clock->SleepMs(120);
    uint64_t back = 0;
    if (!MsrR(rapl::kMsrPlatformPowerLimit, &back) || ((back ^ nv) & 0x7FFFFFFFFFFFFFFFULL) != 0) {
      hw_.msr->Write(rapl::kMsrPlatformPowerLimit, old);
      return Fail("reverted", N_("the value did not stick (the firmware rewrote MSR 0x65C); previous value restored"));
    }
    return Ok();
  }
  if (key == "msr.bdp") {
    uint64_t old;
    if (!MsrR(rapl::kMsrPowerCtl, &old)) return Fail("failed.hw", "cannot read MSR 0x1FC");
    const uint64_t bit = 1ULL << rapl::kPowerCtlBdProchotBit;
    const uint64_t nv = value ? (old | bit) : (old & ~bit);
    if (nv == old) return Ok();
    if (!hw_.msr->Write(rapl::kMsrPowerCtl, nv)) return Fail("failed.hw", N_("MSR write refused by the driver"));
    uint64_t back = 0;
    if (!MsrR(rapl::kMsrPowerCtl, &back) || ((back ^ nv) & bit) != 0) {
      hw_.msr->Write(rapl::kMsrPowerCtl, old);
      return Fail("reverted", "the setting did not stick; previous value restored");
    }
    return Ok();
  }
  if (key == "msr.icc") {
    uint64_t old, nv;
    if (!MsrR(rapl::kMsrVrCurrentConfig, &old)) return Fail("failed.hw", "cannot read MSR 0x601");
    if (!rapl::EncodeIccMax(old, value, &nv)) return Fail("unsupported", "Icc max is locked or out of range");
    if (!hw_.msr->Write(rapl::kMsrVrCurrentConfig, nv)) return Fail("failed.hw", N_("MSR write refused by the driver"));
    if (hw_.clock) hw_.clock->SleepMs(120);
    uint64_t back = 0;
    if (!MsrR(rapl::kMsrVrCurrentConfig, &back) || (back & 0x1FFF) != (nv & 0x1FFF)) {
      hw_.msr->Write(rapl::kMsrVrCurrentConfig, old);
      return Fail("reverted", "the value did not stick; previous value restored");
    }
    return Ok();
  }
  if (StartsWith(key, "msr.volt.")) {
    if (value > 0) return Fail("range", N_("only undervolt (negative) offsets are accepted"));
    const int plane = PlaneFor(key);
    uint64_t resp = 0;
    int prev = 0;
    if (!OcCall(hw_.msr, rapl::OcReadVoltageCmd(plane), &resp)) return Fail("unsupported", N_("voltage mailbox is closed by the BIOS"));
    prev = rapl::OcDecodeOffsetMv(resp);
    if (!OcCall(hw_.msr, rapl::OcWriteVoltageCmd(plane, value), nullptr)) return Fail("failed.hw", "mailbox write was rejected");
    if (hw_.clock) hw_.clock->SleepMs(100);
    if (!OcCall(hw_.msr, rapl::OcReadVoltageCmd(plane), &resp) || std::abs(rapl::OcDecodeOffsetMv(resp) - value) > 3) {
      OcCall(hw_.msr, rapl::OcWriteVoltageCmd(plane, prev), nullptr);
      return Fail("reverted", N_("the BIOS ignored the voltage offset; previous value restored"));
    }
    return Ok();
  }
  if (StartsWith(key, "msr.pf.")) {
    int bit = key == "msr.pf.l2" ? 0 : key == "msr.pf.l2adj" ? 1 : key == "msr.pf.dcu" ? 2 : 3;
    uint64_t old;
    if (!MsrR(0x1A4, &old)) return Fail("failed.hw", "cannot read MSR 0x1A4");
    uint64_t nv = value ? (old & ~(1ULL << bit)) : (old | (1ULL << bit));  // bit set = prefetcher DISABLED
    if (!hw_.msr->Write(0x1A4, nv)) return Fail("failed.hw", N_("MSR write refused by the driver"));
    uint64_t back = 0;
    if (!MsrR(0x1A4, &back) || back != nv) {
      hw_.msr->Write(0x1A4, old);
      return Fail("reverted", "the value did not stick");
    }
    return Ok();
  }
  return Fail("unsupported", "no MSR handler for " + key);
}

// ------------------------------------------------------------------------------------------------ Windows CPU policy
Result Controller::SetOsCpu(const std::string& key, int value) {
  if (!hw_.power) return Fail("unsupported", "no power interface");
  CpuPolicy cur;
  hw_.power->GetCpuPolicy(&cur);
  CpuPolicy p;
  if (key == "os.boost") p.boost = value;
  else if (key == "os.epp") p.epp = value;
  else if (key == "os.min") {
    if (cur.maxPct >= 0 && value > cur.maxPct) return Fail("range", N_("minimum processor state cannot exceed the maximum"));
    p.minPct = value;
  } else if (key == "os.max") {
    if (cur.minPct >= 0 && value < cur.minPct) return Fail("range", N_("maximum processor state cannot be below the minimum"));
    p.maxPct = value;
  } else p.parkMinPct = value;
  std::string why;
  if (!hw_.power->SetCpuPolicy(p, &why)) return Fail("failed.hw", "Windows refused the power setting: " + why);
  CpuPolicy back;
  hw_.power->GetCpuPolicy(&back);
  int got = key == "os.boost" ? back.boost : key == "os.epp" ? back.epp : key == "os.min" ? back.minPct : key == "os.max" ? back.maxPct : back.parkMinPct;
  if (got != value) return Fail("reverted", N_("Windows did not keep the setting"));
  return Ok();
}

// ------------------------------------------------------------------------------------------------ OS polling (usage, policy, power)
void Controller::PollOs(bool full) {
  KV k;
  if (hw_.power) {
    SysStatus s = hw_.power->Status();
    acNow_ = s.ac;
    acKnown_ = true;
    battPctNow_ = s.battPct;
    k.SetB("power.ac", s.ac);
    if (s.battPct >= 0) k.SetI("power.batt.pct", s.battPct);
    if (s.battSecs >= 0) k.SetI("power.batt.secs", s.battSecs);
    k.SetB("power.charging", s.charging);
    k.SetF("os.cpu.pct", s.cpuPct, 0);
    k.SetF("os.mem.pct", s.memPct, 0);
    k.SetF("os.mem.used", s.memUsedMb, 0);
    k.SetF("os.mem.total", s.memTotalMb, 0);
    if (!s.coreMhz.empty()) {
      std::string t;
      long sum = 0;
      for (size_t i = 0; i < s.coreMhz.size(); i++) {
        t += Fmt("%s%d", i ? "," : "", s.coreMhz[i]);
        sum += s.coreMhz[i];
      }
      k.Set("os.cores.mhz", t);
      k.SetI("os.cpu.mhz", static_cast<long long>(sum / static_cast<long>(s.coreMhz.size())));
    }
    if (s.cpuMaxMhz) k.SetI("os.cpu.maxmhz", s.cpuMaxMhz);
    CpuPolicy p;
    if (hw_.power->GetCpuPolicy(&p)) {
      static const char* names[7] = {"disabled", "enabled", "aggressive", "efficient", "efficient_aggressive", "aggressive_guaranteed",
                                     "efficient_aggressive_guaranteed"};
      if (p.boost >= 0 && p.boost < 7) k.Set("os.boost", names[p.boost]);
      if (p.epp >= 0) k.SetI("os.epp", p.epp);
      if (p.minPct >= 0) k.SetI("os.min", p.minPct);
      if (p.maxPct >= 0) k.SetI("os.max", p.maxPct);
      if (p.parkMinPct >= 0) k.SetI("os.park", p.parkMinPct);
    }
  }
  // best available CPU temperature
  KV merged = Snapshot();
  long long t = merged.Int("cpu.temp.msr", merged.Int("cpu.temp.ec", 0));
  if (t > 0) k.SetI("cpu.temp", t);
  long long gt = merged.Int("gpu.temp", merged.Int("gpu.temp.ec", 0));
  if (gt > 0) k.SetI("gpu.temp.best", gt);
  (void)full;
  std::lock_guard<std::mutex> g(snapMu_);
  sOs_ = k;
}

}  // namespace bc
