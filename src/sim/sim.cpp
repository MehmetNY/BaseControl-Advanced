#include "sim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../common/fan.h"
#include "../common/hostdata.h"
#include "../common/registers.h"
#include "../common/util.h"
#include "fake_oem.h"

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
  ec[kCpuTempLimitRead] = 0x05;  // offset 5 = 100 C target
  ec[kGpuCtrl] = 0x05; ec[kGpuCtgpOffset] = 15; ec[kCtwa] = 15;
  ec[kFanSwitchSpeed] = 0x85;
  ec[kBatteryMode] = 0x18;
  fan::TableImage c = fan::Pack(fan::DefaultCpu()), g = fan::Pack(fan::DefaultGpu());
  for (int i = 0; i < 16; i++) {
    ec[kFanTableCpuUp + i] = c.up[i]; ec[kFanTableCpuDown + i] = c.down[i]; ec[kFanTableCpuDuty + i] = c.duty[i];
    ec[kFanTableGpuUp + i] = g.up[i]; ec[kFanTableGpuDown + i] = g.down[i]; ec[kFanTableGpuDuty + i] = g.duty[i];
  }
  ec[kSsdTemp] = 38;

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
}

// While the EC's power-management loop runs the CPU obeys the EC registers; without it the firmware's own defaults apply (assumed here).
double Platform::Pl1W() const { return EcActive() ? ec[kPl1] : 45.0; }
double Platform::Pl2W() const { return EcActive() ? ec[kPl2] : 90.0; }
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
  switch (a) {
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
  return b;
}

// ------------------------------------------------------------------------------------------------ physics
void Platform::Step() {
  std::lock_guard<std::recursive_mutex> g(mu);
  const uint64_t now = clk.NowMs();
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

  // ---- battery
  const double sysW = cpuW + gpuW + 18.0;
  if (ac) battPct = std::min(100.0, battPct + 0.012 * h);
  else battPct = std::max(0.0, battPct - (sysW / 62.0) / 3600.0 * 100.0 * h);
}

void Platform::UpdateRegisters() {
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
  le16(kBatRateLo, ac ? (battPct < 99.5 ? 2150 : 0) : -static_cast<int>((cpuW + gpuW + 18.0) * 1000.0 / 15.4));
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

double FpsSim::Fps(std::string* app) {
  std::lock_guard<std::recursive_mutex> g(p_.mu);
  p_.Step();
  if (p_.gpuLoad < 0.20) return -1;   // nothing heavy is drawing: no "game"
  if (app) *app = "sim-game.exe";
  return std::floor(244.0 - 150.0 * p_.gpuLoad);
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
  if (c.saturation != 100 && !saturationOk) {
    if (why) *why = "sim: the colour-effect API is not available";
    return false;
  }
  color = c;
  colorApplied++;
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

OemSvcState OemProbeSim::Probe() {
  OemSvcState s;
  s.installed = installed;
  s.running = installed && running;
  s.version = installed ? "4.9.47.11" : "";
  if (installed) s.services = {"gcuService", "GcuBridge"};
  return s;
}

Storage::Storage() {
  StorageDrive a;
  a.model = "WD Green SN3000 2TB";
  a.bus = "NVMe";
  a.ssd = true;
  a.sizeBytes = 2000398934016ull;
  a.tempC = 33;
  a.powerCycles = 927;
  a.powerOnHours = 4 * 24 + 20;
  a.wearPct = 1;
  a.sparePct = 100;
  a.critWarn = 0;
  a.writtenGb = 3870;
  StorageDrive b;
  b.model = "SAMSUNG MZVL21T0HCLR-00B00";
  b.bus = "NVMe";
  b.ssd = true;
  b.sizeBytes = 1024209543168ull;
  b.tempC = 35;
  b.powerCycles = 3109;
  b.powerOnHours = 559 * 24 + 23;
  b.wearPct = 7;
  b.sparePct = 100;
  b.critWarn = 0;
  b.writtenGb = 41300;
  drives = {a, b};
}

bool Storage::Read(std::vector<StorageDrive>* out, bool health) {
  std::lock_guard<std::mutex> lk(mu);
  ++reads;
  if (failRead) return false;
  if (health) ++healthReads;
  out->clear();
  for (StorageDrive d : drives) {
    if (!health || healthDenied) {
      d.tempC = -1;
      d.powerCycles = d.powerOnHours = d.writtenGb = -1;
      d.wearPct = d.sparePct = d.critWarn = -1;
    }
    out->push_back(d);
  }
  return true;
}

HostInfo Host::Get() {
  HostInfo h;
  h.os = "Windows 11 Pro 23H2 (simulated)";
  h.cpu = "11th Gen Intel(R) Core(TM) i7-11800H @ 2.30GHz";
  h.ram = "32 GB";
  h.machineName = "SIM-TULPAR";
  h.gpus = "Intel(R) UHD Graphics;NVIDIA GeForce RTX 3070 Laptop GPU";
  h.biosVendor = "INSYDE";
  h.biosVersion = "N.1.07Mon08";
  h.sysVendor = "MONSTER";
  h.sysProduct = "TULPAR T5 V21.7";
  h.boardName = "GM5TG7Y";
  h.cpuCores = 8;
  h.cpuThreads = 16;
  h.cpuBaseMhz = 2304;
  if (smbios) {
    std::vector<uint8_t> raw;
    std::string err;
    mgp::SmbiosInfo si;
    if (smbios->ReadRaw(raw, err) && mgp::ParseRawSmbios(raw.data(), raw.size(), si)) h.memory = MemModulesFromSmbios(si);
  }
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
  // Memory devices (type 17): two 16 GB DDR4-3200 SODIMMs whose maker is the unresolved JEDEC id, plus one empty slot. Serial numbers are fake.
  auto memDev = [&](int sizeMb, const char* loc, const char* maker, const char* part) {
    std::vector<uint8_t> f(0x22, 0);
    f[0] = 17; f[1] = 0x22; f[2] = 0x10;
    f[8] = 64; f[0x0A] = 64;
    f[0x0C] = static_cast<uint8_t>(sizeMb & 0xFF); f[0x0D] = static_cast<uint8_t>((sizeMb >> 8) & 0xFF);
    f[0x0E] = 0x0D; f[0x10] = 1; f[0x11] = 2; f[0x12] = 0x1A; f[0x13] = 0x80;
    f[0x15] = 3200 & 0xFF; f[0x16] = 3200 >> 8;
    f[0x17] = 3; f[0x18] = 4; f[0x19] = 5; f[0x1A] = 6; f[0x1B] = 2;
    f[0x20] = 3200 & 0xFF; f[0x21] = 3200 >> 8;
    addStruct(f, {loc, "BANK 0", maker, "SERIAL-FAKE-DIMM", "Asset-FAKE", part});
  };
  memDev(16384, "ChannelA-DIMM0", "80CE", "M471A2G43AB2-CWE");
  memDev(16384, "ChannelB-DIMM0", "80CE", "M471A2G43AB2-CWE");
  {
    std::vector<uint8_t> f(0x15, 0);
    f[0] = 17; f[1] = 0x15; f[2] = 0x12; f[0x0E] = 0x0D; f[0x10] = 1;
    addStruct(f, {"ChannelC-DIMM0"});
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

World::World(Model m, bool realClock) : clock(realClock), plat(clock), ec(plat), nv(plat), gpu(plat), power(plat), fps(plat) {
  host.smbios = &smbios;
  if (m == Model::Exact) smbios.raw = BuildSmbios("MONSTER", "TULPAR T5 V21.7", "N.1.07Mon08", "GM5TG7Y");
  else if (m == Model::Family) smbios.raw = BuildSmbios("SCHENKER", "XMG NEO 15 M21", "N.1.07Mon08", "GM5TG7W");
  else {
    smbios.raw = BuildSmbios("ACME", "Generic Laptop", "1.0", "XYZ");
    plat.ec[0x740] = 40;
  }
  oemWorld.reset(new FakeOemWorld(&plat));
  oemProbe.onStart = [this] { if (oemWorld) oemWorld->SetServiceUp(true); };
  oemLink.reset(new oem::MqttOemLink(oemWorld->LinkConfigFor()));
}

World::~World() {
  if (oemLink) oemLink->Stop();   // the link talks to the fake service: it has to end first
}

Hardware World::hw() {
  Hardware h;
  h.ec = &ec;
  h.nv = &nv;
  h.smbios = &smbios;
  h.oemFiles = nullptr;
  h.clock = &clock;
  h.log = &log;
  h.gpu = &gpu;
  h.power = &power;
  h.display = &display;
  h.devices = &devices;
  h.oemSvc = &oemProbe;
  h.oemLink = oemLink.get();
  h.host = &host;
  h.storage = &storage;
  h.fps = &fps;
  h.applyLock = &lock;
  return h;
}

}  // namespace sim
}  // namespace bc
