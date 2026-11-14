#include "sim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../common/fan.h"
#include "../common/rapl.h"
#include "../common/registers.h"
#include "../common/util.h"

namespace bc {
namespace sim {

using namespace reg;

namespace {
double Clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

// ------------------------------------------------------------------------------------------------ construction
Platform::Platform(Clock& clock) : clk(clock) {
  std::memset(ec, 0, sizeof ec);
  ec[0x740] = 26;           // project id (IDY_7Y)
  ec[0x741] = 0x01;         // activation byte: loop running (as left by the OEM service)
  ec[kAcAutoBoot] = 0x80;   // PECI loop enabled
  ec[kCustomProfile] = 0x40;
  ec[kPl1] = 120; ec[kPl2] = 120; ec[kPl4] = 200;
  ec[kTccWrite] = 0x05; ec[kCpuTempLimitRead] = 0x05;  // offset 5 = 100 C target
  ec[kGpuCtrl] = 0x05; ec[kGpuCtgpOffset] = 15; ec[kCtwa] = 15;
  ec[kFanSwitchSpeed] = 0x85;
  ec[kBatteryMode] = 0x18;
  fan::TableImage c = fan::Pack(fan::DefaultCpu()), g = fan::Pack(fan::DefaultGpu());
  for (int i = 0; i < 16; i++) {
    ec[kFanTableCpuUp + i] = c.up[i]; ec[kFanTableCpuDown + i] = c.down[i]; ec[kFanTableCpuDuty + i] = c.duty[i];
    ec[kFanTableGpuUp + i] = g.up[i]; ec[kFanTableGpuDown + i] = g.down[i]; ec[kFanTableGpuDuty + i] = g.duty[i];
  }
  ec[kSsdTemp] = 38;

  const rapl::Units u = rapl::DecodeUnits(0xA0E03);
  msr[0x606] = 0xA0E03;
  rapl::PkgLimit pk;
  pk.pl1W = 120; pk.pl2W = 120; pk.pl1En = pk.pl2En = true; pk.pl1Clamp = pk.pl2Clamp = true; pk.tau1S = 28; pk.tau2S = 0.00244;
  uint64_t v = 0;
  rapl::EncodePkgLimit(0, pk, u, &v);
  msr[0x610] = v;
  msr[0x614] = (360ULL) | (200ULL << 16) | (1600ULL << 32);    // TDP 45 W, min 25 W, max 200 W
  msr[0x601] = 1040;                                           // Icc max 130 A, unlocked
  msr[0x1A2] = 100ULL << 16;                                   // TjMax 100, TCC offset 0
  msr[0x0CE] = 23ULL << 8;                                     // base ratio 23
  msr[0x1AD] = 0x2B2B2C2C2D2D2E2EULL;                          // 46,46,45,45,44,44,43,43 (byte 0 = 1 core)
  msr[0x1A4] = 0;
  mirroredPl1_ = ec[kPl1];
  mirroredPl2_ = ec[kPl2];
  last_ = clk.NowMs();
  UpdateRegisters();
}

void Platform::SetLoad(double cpu, double gpu) {
  std::lock_guard<std::recursive_mutex> g(mu);
  cpuLoad = Clampd(cpu, 0, 1);
  gpuLoad = Clampd(gpu, 0, 1);
}

void Platform::SetEcProfile(bool oemOn, int off) {
  ec[kGpuCtrl] = oemOn ? 0x05 : 0x01;
  ec[kGpuCtgpOffset] = static_cast<uint8_t>(off);
  ec[kCtwa] = static_cast<uint8_t>(off);
}

void Platform::DisplayCycle() {
  if (ec[kGpuCtgpOffset] > 15) {
    ec[kGpuCtgpOffset] = 15;
    ec[kCtwa] = 15;
  }
}

void Platform::MakeEcCold() {
  ec[0x741] = 0x00;
  ec[kAcAutoBoot] &= static_cast<uint8_t>(~0x80);
  ec[kCustomProfile] &= static_cast<uint8_t>(~0x40);
  pendingActAt_ = 0;
}

double Platform::Pl1W() const {
  rapl::PkgLimit p = rapl::DecodePkgLimit(msr.at(0x610), rapl::DecodeUnits(msr.at(0x606)));
  return p.pl1W;
}
double Platform::Pl2W() const {
  rapl::PkgLimit p = rapl::DecodePkgLimit(msr.at(0x610), rapl::DecodeUnits(msr.at(0x606)));
  return p.pl2W;
}
double Platform::CtgpW() const { return (ec[kGpuCtrl] & 0x04) ? 125.0 + ec[kGpuCtgpOffset] : 125.0; }

// ------------------------------------------------------------------------------------------------ EC access
mgp::EcByte Platform::EcRead(uint16_t a) {
  Step();
  ++ecReads;
  mgp::EcByte b;
  if (ecReadFail) {
    b.error = "sim: EC read failed";
    return b;
  }
  b.ok = true;
  b.bytesReturned = 1;
  b.value = ec[a & 0xFFF];
  return b;
}

mgp::EcByte Platform::EcWrite(uint16_t a, uint8_t v) {
  Step();
  ++ecWrites;
  writeLog.push_back({a, v});
  mgp::EcByte b;
  if (ecFailWriteNumber == ecWrites) {
    b.error = "sim: EC write failed";
    return b;
  }
  b.ok = true;
  b.bytesReturned = 1;
  if (ecDropWrites) return b;
  a &= 0xFFF;
  const bool active = EcActive();
  switch (a) {
    case 0x741:
      ec[a] = v;
      if (v & 0x04) pendingActAt_ = clk.NowMs() + 600;  // the EC clears bit 2 once it has processed the request
      break;
    case kPl1: case kPl2: case kPl4:
      if (active) ec[a] = v;  // an inactive loop ignores power-limit writes
      break;
    case kTccWrite:
      if (active && (v & 0x80)) {
        ec[kCpuTempLimitRead] = v & 0x7F;
        ec[a] = v & 0x7F;  // commit bit consumed
      } else if (active) {
        ec[a] = v;
      }
      break;
    case kGpuCtgpOffset:
      ec[a] = v;
      ec[kCtwa] = v;
      break;
    // read-only sensor / status bytes: writes are ignored by the EC
    case kCpuTemp: case kGpuTemp: case kCpuFanRpmHi: case kCpuFanRpmLo: case kGpuFanRpmHi: case kGpuFanRpmLo: case kAcStatus:
    case kCpuFanDuty: case kGpuFanDuty: case kSsdTemp: case kBatterySoc:
      break;
    default:
      ec[a] = v;
  }
  if (a == kFanCurveLatch) ec[a] = v;  // the latch bit stays set in this model
  return b;
}

// ------------------------------------------------------------------------------------------------ MSR access
bool Platform::MsrAllowed(uint32_t m, bool write) const {
  static const uint32_t kRead[] = {0x0CE, 0x198, 0x19C, 0x1A2, 0x1A4, 0x1AD, 0x1B1, 0x150, 0x601, 0x606, 0x610, 0x611, 0x613, 0x614, 0x619, 0x639, 0x641, 0x64D};
  static const uint32_t kWrite[] = {0x1A4, 0x150, 0x601, 0x610};
  if (write) {
    for (uint32_t x : kWrite)
      if (x == m) return true;
    return false;
  }
  for (uint32_t x : kRead)
    if (x == m) return true;
  return false;
}

bool Platform::MsrRead(uint32_t m, uint64_t* v) {
  if (!msrAvailable || !MsrAllowed(m, false)) return false;
  Step();
  if (m == 0x150) {
    *v = ocResp_;
    return true;
  }
  auto it = msr.find(m);
  *v = it == msr.end() ? 0 : it->second;
  if (m == 0x610 && msrPkgLocked) *v |= (1ULL << 63);
  return true;
}

bool Platform::ocHandle(uint64_t cmd) {
  const int code = static_cast<int>((cmd >> 32) & 0xFF);
  const int plane = static_cast<int>((cmd >> 40) & 0xFF);
  uint64_t resp = cmd & ~(1ULL << 63);
  resp &= ~(0xFFULL << 32);  // response code 0 = success
  if (!ocMailboxOpen || plane > 4 || (code != 0x10 && code != 0x11)) {
    ocResp_ = (cmd & ~(1ULL << 63) & ~(0xFFULL << 32)) | (1ULL << 32);
    return true;
  }
  if (code == 0x10) {
    long long units = std::llround(ocOffsetMv_[plane] * 1.024);
    resp = (resp & ~(0x7FFULL << 21)) | ((static_cast<uint64_t>(units) & 0x7FF) << 21);
  } else {
    int units = static_cast<int>((cmd >> 21) & 0x7FF);
    if (units & 0x400) units -= 0x800;
    ocOffsetMv_[plane] = static_cast<int>(std::lround(units / 1.024));
  }
  ocResp_ = resp;
  return true;
}

bool Platform::MsrWrite(uint32_t m, uint64_t v) {
  if (!msrAvailable || !MsrAllowed(m, true)) return false;
  Step();
  switch (m) {
    case 0x150:
      return ocHandle(v);
    case 0x610:
      if (msrPkgLocked) return false;
      msr[0x610] = v & ~(1ULL << 63);
      return true;
    case 0x601:
      if (iccLocked) return false;
      msr[0x601] = v & 0xFFFFFFFFULL & ~(1ULL << 31);
      return true;
    default:
      msr[m] = v;
      return true;
  }
}

void Platform::MirrorEcToMsr(bool force) {
  const uint8_t p1 = ec[kPl1], p2 = ec[kPl2];
  if (!force && p1 == mirroredPl1_ && p2 == mirroredPl2_) return;
  mirroredPl1_ = p1;
  mirroredPl2_ = p2;
  const rapl::Units u = rapl::DecodeUnits(msr[0x606]);
  const uint64_t old = msr[0x610] & ~(1ULL << 63);
  rapl::PkgLimit pk = rapl::DecodePkgLimit(old, u);
  pk.pl1W = p1;
  pk.pl2W = p2;
  uint64_t nv = 0;
  if (rapl::EncodePkgLimit(old, pk, u, &nv)) msr[0x610] = nv | (msrPkgLocked ? (1ULL << 63) : 0);
}

// ------------------------------------------------------------------------------------------------ physics
void Platform::Step() {
  std::lock_guard<std::recursive_mutex> g(mu);
  const uint64_t now = clk.NowMs();
  if (pendingActAt_ && now >= pendingActAt_) {
    pendingActAt_ = 0;
    const bool ok = (ec[kAcAutoBoot] & 0x80) && (ec[kCustomProfile] & 0x40);
    ec[0x741] = ok ? 0x01 : 0x00;
  }
  if (now <= last_) return;
  double dt = static_cast<double>(now - last_) / 1000.0;
  last_ = now;
  if (dt > 120.0) dt = 120.0;
  int n = static_cast<int>(std::ceil(dt / 0.5));
  if (n < 1) n = 1;
  for (int i = 0; i < n; i++) Sub(dt / n);
  UpdateRegisters();
}

void Platform::Sub(double h) {
  if (msrFirmwareOwns) MirrorEcToMsr(true);
  else if (msrFollowsEc && EcActive()) MirrorEcToMsr(false);

  // ---- CPU power: PL2 burst budget (tau1 = 28 s), then PL1
  const double pl1 = std::max(1.0, Pl1W()), pl2 = std::max(pl1, Pl2W());
  const double tau1 = 28.0;
  const double cap = (pl2 - pl1) * tau1;
  if (bucketJ_ < 0.0 || bucketJ_ > cap) bucketJ_ = cap;
  const double demand = 7.0 + cpuLoad * 133.0;
  cpuDemandW = demand;
  double allowed;
  if (demand <= pl1) {
    allowed = demand;
    bucketJ_ = std::min(cap, bucketJ_ + (pl1 - demand) * h);
  } else if (bucketJ_ > 0.0) {
    allowed = std::min(demand, pl2);
    bucketJ_ = std::max(0.0, bucketJ_ - (allowed - pl1) * h);
  } else {
    allowed = pl1;
  }
  powerLimited = demand > allowed + 0.5;
  // ---- thermal throttle from the EC temperature limit (105 - offset)
  const double tcc = 105.0 - (ec[kCpuTempLimitRead] & 0x7F);
  thermalLimited = cpuT > tcc - 1.0;
  if (thermalLimited) allowed *= std::max(0.4, 1.0 - (cpuT - (tcc - 1.0)) * 0.12);
  double gdemand = 10.0 + gpuLoad * (CtgpW() - 10.0);
  double gw = gdemand;
  if (gpuT > 87.0) gw *= std::max(0.4, 1.0 - (gpuT - 87.0) * 0.1);
  // ---- shared CPU+GPU budget (observed ~200-210 W on the real unit; cause unknown)
  budgetLimited = false;
  if (sysBudgetW > 0 && allowed + gw > sysBudgetW) {
    const double f = sysBudgetW / (allowed + gw);
    allowed *= f;
    gw *= f;
    budgetLimited = true;
  }
  cpuW = allowed;
  gpuW = gw;

  // ---- fans
  auto fanTarget = [&](bool cpu, double temp) {
    const uint8_t fixed = ec[cpu ? kFanFixedCpu : kFanFixedGpu];
    if (fixed & 0x80) return static_cast<double>(std::min(100, fixed & 0x7F));
    if (ec[kFanMode] & 0x40) return 100.0;
    if ((ec[kFanCurveEnable] & 0x80) && (ec[kFanCurveLatch] & 0x04)) {
      fan::TableImage img;
      const uint16_t base = cpu ? kFanTableCpuUp : kFanTableGpuUp;
      for (int i = 0; i < 16; i++) {
        img.up[i] = ec[base + i];
        img.down[i] = ec[base + 0x10 + i];
        img.duty[i] = ec[base + 0x20 + i];
      }
      fan::Curve c;
      if (fan::Unpack(img, &c, nullptr)) return static_cast<double>(fan::DutyAt(c, static_cast<int>(std::lround(temp))));
    }
    return static_cast<double>(fan::DutyAt(cpu ? fan::DefaultCpu() : fan::DefaultGpu(), static_cast<int>(std::lround(temp))));
  };
  const double ct = fanTarget(true, cpuT), gt = fanTarget(false, gpuT);
  auto slew = [&](double cur, double tgt) { return cur + Clampd(tgt - cur, -20.0 * h, 35.0 * h); };
  cpuFanPct = slew(cpuFanPct, ct);
  gpuFanPct = slew(gpuFanPct, gt);
  cpuRpm = cpuFanPct < 1.0 ? 0 : 1500 + cpuFanPct * 41.0;
  gpuRpm = gpuFanPct < 1.0 ? 0 : 1400 + gpuFanPct * 38.0;

  // ---- temperatures (first order lag towards an equilibrium that depends on power and airflow)
  const double cEq = 28.0 + (0.80 * cpuW + 0.10 * gpuW) * (1.35 - 0.70 * cpuFanPct / 100.0);
  const double gEq = 28.0 + (0.55 * gpuW + 0.06 * cpuW) * (1.30 - 0.65 * gpuFanPct / 100.0);
  const double k = 1.0 - std::exp(-h / 6.0);
  cpuT += (cEq - cpuT) * k;
  gpuT += (gEq - gpuT) * k;
  if (cpuT >= tcc - 1.0) logThermal = true;
  if (powerLimited) logPower = true;

  // ---- energy counters / throttle time
  eCpu_ += cpuW * h;
  eCore_ += cpuW * 0.82 * h;
  eIgpu_ += (0.8 + 0.3 * cpuLoad) * h;
  eDram_ += (2.2 + 3.5 * cpuLoad) * h;
  ePsys_ += (cpuW + gpuW + 18.0) * h;
  if (powerLimited) throttleS_ += (1.0 - allowed / std::max(1.0, demand)) * h;

  // ---- battery
  const double sysW = cpuW + gpuW + 18.0;
  if (ac) battPct = std::min(100.0, battPct + 0.012 * h);
  else battPct = std::max(0.0, battPct - (sysW / 62.0) / 3600.0 * 100.0 * h);
}

void Platform::UpdateRegisters() {
  const rapl::Units u = rapl::DecodeUnits(msr[0x606]);
  auto cnt = [&](double j) { return static_cast<uint64_t>(j / u.energyJ) & 0xFFFFFFFFULL; };
  msr[0x611] = cnt(eCpu_);
  msr[0x639] = cnt(eCore_);
  msr[0x641] = cnt(eIgpu_);
  msr[0x619] = cnt(eDram_);
  msr[0x64D] = cnt(ePsys_);
  msr[0x613] = static_cast<uint64_t>(throttleS_ / u.timeS) & 0xFFFFFFFFULL;
  const double tjmax = 100.0;
  const int below = static_cast<int>(Clampd(tjmax - cpuT, 0, 127));
  uint64_t st = static_cast<uint64_t>(below) << 16;
  if (cpuT >= tjmax - 2) st |= 1;
  if (logThermal) st |= 2;
  if (powerLimited) st |= (1ULL << 10);
  if (logPower) st |= (1ULL << 11);
  msr[0x1B1] = st;
  msr[0x19C] = st & 0x7F0003ULL;
  const double ratio = 8.0 + 38.0 * Clampd(cpuW / 95.0, 0, 1);
  msr[0x198] = static_cast<uint64_t>(std::lround(ratio)) << 8;
  if (iccLocked) msr[0x601] |= (1ULL << 31);
  else msr[0x601] &= ~(1ULL << 31);

  ec[kCpuTemp] = static_cast<uint8_t>(std::lround(cpuT));
  ec[kGpuTemp] = static_cast<uint8_t>(std::lround(gpuT));
  const int cr = static_cast<int>(std::lround(cpuRpm)), gr = static_cast<int>(std::lround(gpuRpm));
  ec[kCpuFanRpmHi] = static_cast<uint8_t>(cr >> 8); ec[kCpuFanRpmLo] = static_cast<uint8_t>(cr);
  ec[kGpuFanRpmHi] = static_cast<uint8_t>(gr >> 8); ec[kGpuFanRpmLo] = static_cast<uint8_t>(gr);
  ec[kCpuFanDuty] = static_cast<uint8_t>(std::lround(cpuFanPct * 2.0));
  ec[kGpuFanDuty] = static_cast<uint8_t>(std::lround(gpuFanPct * 2.0));
  ec[kAcStatus] = ac ? 1 : 0;
  ec[kSsdTemp] = static_cast<uint8_t>(36 + std::lround(cpuLoad * 8));
  ec[kBatterySoc] = static_cast<uint8_t>(std::lround(battPct));
  auto le16 = [&](uint16_t a, int v) { ec[a] = static_cast<uint8_t>(v); ec[a + 1] = static_cast<uint8_t>(v >> 8); };
  le16(kBatCapDesignLo, 5300);
  le16(kBatCapFullLo, 4980);
  le16(kBatCycleLo, 87);
  le16(kBatVoltageLo, 15380);
  le16(kBatRemainLo, static_cast<int>(4980 * battPct / 100.0));
  le16(kBatRateLo, ac ? 0 : -static_cast<int>((cpuW + gpuW + 18.0) * 1000.0 / 15.4));
  le16(kBatTempLo, 3010);
}

// ------------------------------------------------------------------------------------------------ adapters
bool Nv::Init(std::string& err) {
  if (!p_.nvmlAvailable) {
    err = "nvml.dll not found (simulated)";
    return false;
  }
  return true;
}

bool Nv::ReadStatic(mgp::NvStatic& o, std::string& err) {
  if (!p_.nvmlAvailable) {
    err = "no device";
    return false;
  }
  o.ok = true;
  o.name = "NVIDIA GeForce RTX 3070 Laptop GPU";
  o.minW = 1;
  o.maxW = 150;
  o.defaultW = 125;
  o.pciDeviceId = 0x249D;
  o.busId = "00000000:01:00.0";
  return true;
}

bool Nv::ReadDynamic(mgp::NvDynamic& o) {
  std::lock_guard<std::recursive_mutex> g(p_.mu);
  p_.Step();
  if (!p_.nvmlAvailable) {
    o.ok = false;
    o.error = "sim: NVML unavailable";
    return false;
  }
  if (static_cast<uint32_t>(p_.ecWrites) != seenWrites_) {
    seenWrites_ = static_cast<uint32_t>(p_.ecWrites);
    countdown_ = p_.nvLagSamples;
  }
  if (countdown_ > 0) --countdown_;
  else limit_ = p_.CtgpW();
  o.ok = true;
  o.haveMgmt = o.haveEnforced = true;
  o.mgmtW = o.enforcedW = o.limitW = limit_;
  o.haveDraw = o.haveTemp = o.haveUtil = true;
  o.drawW = p_.gpuW;
  o.tempC = static_cast<int>(std::lround(p_.gpuT));
  o.utilPct = static_cast<int>(std::lround(p_.gpuLoad * 100.0));
  return true;
}

bool Msr::Open(std::string* why) {
  if (!p_.msrAvailable) {
    if (why) *why = "PawnIO driver / IntelMSR module not available (simulated)";
    return false;
  }
  return true;
}

bool GpuExtra::Init(std::string* why) {
  if (!p_.nvmlAvailable) {
    if (why) *why = "NVML unavailable (simulated)";
    return false;
  }
  return true;
}

bool GpuExtra::Read(GpuInfo* o) {
  std::lock_guard<std::recursive_mutex> g(p_.mu);
  p_.Step();
  if (!p_.nvmlAvailable) return false;
  o->ok = true;
  o->name = "NVIDIA GeForce RTX 3070 Laptop GPU";
  o->tempC = static_cast<int>(std::lround(p_.gpuT));
  o->powerW = p_.gpuW;
  o->limitW = p_.CtgpW();
  o->maxGfxMhz = 1620;
  o->gfxMhz = static_cast<int>(210 + p_.gpuLoad * (1500 - 210) + p_.gpuCoreOffset * p_.gpuLoad);
  o->memMhz = p_.gpuLoad < 0.05 ? 405 : 7001 + p_.gpuMemOffset;
  o->utilPct = static_cast<int>(std::lround(p_.gpuLoad * 100.0));
  o->memUtilPct = static_cast<int>(std::lround(p_.gpuLoad * 62.0));
  o->vramTotalMb = 8192;
  o->vramUsedMb = 700 + p_.gpuLoad * 4200;
  o->pstate = p_.gpuLoad < 0.05 ? 8 : 0;
  o->fanPct = -1;
  uint64_t t = 0;
  if (p_.gpuLoad < 0.05) t |= kNvThrottleIdle;
  if (p_.gpuLoad > 0.8 && p_.gpuW >= p_.CtgpW() - 1.5) t |= kNvThrottleSwPower;
  if (p_.gpuT > 87) t |= kNvThrottleSwThermal;
  o->throttleMask = t;
  o->haveOffsets = true;
  o->coreOffsetMhz = p_.gpuCoreOffset;
  o->memOffsetMhz = p_.gpuMemOffset;
  return true;
}

bool Power::SetCpuPolicy(const CpuPolicy& p, std::string* why) {
  if (failSet) {
    if (why) *why = "sim: access denied";
    return false;
  }
  if (p.boost >= 0) policy.boost = p.boost;
  if (p.epp >= 0) policy.epp = p.epp;
  if (p.minPct >= 0) policy.minPct = p.minPct;
  if (p.maxPct >= 0) policy.maxPct = p.maxPct;
  if (p.parkMinPct >= 0) policy.parkMinPct = p.parkMinPct;
  return true;
}

SysStatus Power::Status() {
  std::lock_guard<std::recursive_mutex> g(p_.mu);
  p_.Step();
  SysStatus s;
  s.ac = p_.ac;
  s.battPct = static_cast<int>(std::lround(p_.battPct));
  s.battSecs = p_.ac ? -1 : static_cast<int>(p_.battPct / 100.0 * 62.0 / std::max(8.0, p_.cpuW + p_.gpuW + 18.0) * 3600.0);
  s.charging = p_.ac && p_.battPct < 99.5;
  s.cpuPct = p_.cpuLoad * 100.0;
  s.memTotalMb = 32768;
  s.memPct = 38 + p_.cpuLoad * 20;
  s.memUsedMb = s.memTotalMb * s.memPct / 100.0;
  const double ratio = 8.0 + 38.0 * Clampd(p_.cpuW / 95.0, 0, 1);
  for (int i = 0; i < 16; i++) s.coreMhz.push_back(static_cast<int>(ratio * 100.0 * (0.92 + 0.08 * ((i * 7) % 5) / 4.0)));
  s.cpuMaxMhz = 4600;
  return s;
}

DisplayInfo Display::Info() {
  DisplayInfo d;
  d.ok = true;
  d.brightness = brightness;
  d.width = width;
  d.height = height;
  d.refresh = refresh;
  d.refreshRates = rates;
  return d;
}

bool Display::SetRefresh(int hz, std::string* why) {
  if (std::find(rates.begin(), rates.end(), hz) == rates.end()) {
    if (why) *why = "the panel does not support this refresh rate";
    return false;
  }
  refresh = hz;
  return true;
}

bool Display::SetColor(const ColorSettings& c, std::string* why) {
  if (failColor) {
    if (why) *why = "sim: gamma ramp rejected by the driver";
    return false;
  }
  color = c;
  return true;
}

bool Devices::Set(Dev d, bool on, std::string* why) {
  auto it = state.find(d);
  if (it == state.end()) {
    if (why) *why = "no such device";
    return false;
  }
  if (!stuck) it->second = on ? 1 : 0;
  return true;
}

OemSvcState Oem::Probe() {
  OemSvcState s;
  s.installed = installed;
  s.running = installed && running && !disabled;
  s.version = installed ? "4.9.47.11" : "";
  if (installed) s.services = {"gcuService", "GcuBridge"};
  return s;
}

bool Oem::StopAndDisable(std::string* why) {
  if (failStop) {
    if (why) *why = "access denied";
    return false;
  }
  running = false;
  disabled = true;
  return true;
}

bool Oem::EnableAndStart(std::string*) {
  disabled = false;
  running = true;
  return true;
}

std::vector<LightZone> Light::Detect(std::string* info) {
  std::vector<LightZone> v;
  if (kbd) v.push_back(LightZone::Keyboard);
  if (bar) v.push_back(LightZone::LightBar);
  if (logo) v.push_back(LightZone::Logo);
  if (info) *info = kbd ? "simulated ITE 048D:600B" : "none";
  return v;
}

HostInfo Host::Get() {
  HostInfo h;
  h.os = "Windows 11 Pro 23H2 (simulated)";
  h.cpu = "11th Gen Intel(R) Core(TM) i7-11800H @ 2.30GHz";
  h.ram = "32 GB";
  h.machineName = "SIM-TULPAR";
  h.biosVendor = "INSYDE";
  h.biosVersion = "N.1.07Mon08";
  h.sysVendor = "MONSTER";
  h.sysProduct = "TULPAR T5 V21.7";
  h.boardName = "GM5TG7Y";
  h.elevated = true;
  return h;
}

std::vector<uint8_t> BuildSmbios(const std::string& manuf, const std::string& product, const std::string& bios, const std::string& board) {
  std::vector<uint8_t> t;
  auto addStruct = [&](std::vector<uint8_t> formatted, std::vector<std::string> strs) {
    t.insert(t.end(), formatted.begin(), formatted.end());
    if (strs.empty()) {
      t.push_back(0);
      t.push_back(0);
      return;
    }
    for (auto& s : strs) {
      t.insert(t.end(), s.begin(), s.end());
      t.push_back(0);
    }
    t.push_back(0);
  };
  {
    std::vector<uint8_t> f(0x18, 0);
    f[0] = 0; f[1] = 0x18; f[4] = 1; f[5] = 2; f[8] = 3; f[0x14] = 1; f[0x15] = 7; f[0x16] = 1; f[0x17] = 7;
    addStruct(f, {"INSYDE", bios, "12/13/2021"});
  }
  {
    std::vector<uint8_t> f(0x1B, 0);
    f[0] = 1; f[1] = 0x1B; f[4] = 1; f[5] = 2; f[6] = 3; f[7] = 4; f[0x19] = 5; f[0x1A] = 6;
    addStruct(f, {manuf, product, "Not Applicable", "SERIAL-FAKE-FOR-TESTS", "Not Applicable", "Tulpar"});
  }
  {
    std::vector<uint8_t> f(0x08, 0);
    f[0] = 2; f[1] = 0x08; f[4] = 1; f[5] = 2; f[6] = 3; f[7] = 4;
    addStruct(f, {"Tongfang", board, "Not Applicable", "BOARD-SERIAL-FAKE"});
  }
  {
    std::vector<uint8_t> f(4, 0);
    f[0] = 127; f[1] = 4;
    addStruct(f, {});
  }
  std::vector<uint8_t> raw(8, 0);
  raw[1] = 3;
  raw[2] = 3;
  uint32_t n = static_cast<uint32_t>(t.size());
  raw[4] = n & 0xFF; raw[5] = (n >> 8) & 0xFF; raw[6] = (n >> 16) & 0xFF; raw[7] = (n >> 24) & 0xFF;
  raw.insert(raw.end(), t.begin(), t.end());
  return raw;
}

World::World(Model m, bool realClock)
    : clock(realClock), plat(clock), ec(plat), nv(plat), msr(plat), gpu(plat), power(plat) {
  if (m == Model::Exact) smbios.raw = BuildSmbios("MONSTER", "TULPAR T5 V21.7", "N.1.07Mon08", "GM5TG7Y");
  else if (m == Model::Family) smbios.raw = BuildSmbios("SCHENKER", "XMG NEO 15 M21", "N.1.07Mon08", "GM5TG7W");
  else {
    smbios.raw = BuildSmbios("ACME", "Generic Laptop", "1.0", "XYZ");
    plat.ec[0x740] = 40;
  }
}

Hardware World::hw() {
  Hardware h;
  h.ec = &ec;
  h.nv = &nv;
  h.smbios = &smbios;
  h.oemFiles = nullptr;
  h.clock = &clock;
  h.log = &log;
  h.msr = &msr;
  h.gpu = &gpu;
  h.power = &power;
  h.display = &display;
  h.devices = &devices;
  h.oem = &oem;
  h.light = &light;
  h.host = &host;
  h.applyLock = &lock;
  return h;
}

}  // namespace sim
}  // namespace bc
