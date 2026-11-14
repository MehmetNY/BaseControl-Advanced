// Simulated machine: a small physical model (power, temperatures, fans, battery) plus an EC memory image and a MSR file,
// exposed through the same interfaces the real Windows backends implement. Used by the unit tests, by `BaseControl.exe --sim`
// and for the README screenshots (which are always labelled SIMULATION).
//
// This is NOT evidence about real hardware: every behaviour here is an assumption written down so the controller logic can be
// exercised (activation delay, verify/rollback paths, firmware resets, locked registers ...).
#pragma once
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/hw_iface.h"

namespace bc {
namespace sim {

// Virtual clock (tests: instant, deterministic) or wall clock (interactive simulator).
class Clock : public mgp::IClock {
 public:
  explicit Clock(bool real = false) : real_(real), t0_(std::chrono::steady_clock::now()) {}
  uint64_t NowMs() override {
    if (real_) return 1000 + static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0_).count());
    return virt_;
  }
  void SleepMs(uint32_t ms) override {
    if (real_) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    else virt_ += ms;
  }
  void Advance(uint64_t ms) { if (!real_) virt_ += ms; }
  bool real() const { return real_; }

 private:
  bool real_;
  uint64_t virt_ = 1000;
  std::chrono::steady_clock::time_point t0_;
};

struct MemLog : mgp::ILog {
  std::mutex mu;
  std::vector<std::string> lines;
  void Write(mgp::LogLevel, const std::string& m) override {
    std::lock_guard<std::mutex> g(mu);
    lines.push_back(m);
    if (lines.size() > 4000) lines.erase(lines.begin(), lines.begin() + 1000);
  }
  bool Has(const std::string& needle) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& l : lines)
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

// ------------------------------------------------------------------------------------------------ the machine
class Platform {
 public:
  explicit Platform(Clock& clk);

  std::recursive_mutex mu;

  // ---- scenario inputs ----------------------------------------------------------------------------
  double cpuLoad = 0.12, gpuLoad = 0.04;  // 0..1
  bool ac = true;
  double battPct = 76;
  double sysBudgetW = 210;                // CPU+GPU combined budget enforced by "something" (0 = none); matches what was measured
  void SetLoad(double cpu, double gpu);

  // ---- fault injection (tests) --------------------------------------------------------------------
  bool ecReadFail = false;       // every EC read fails
  bool ecDropWrites = false;     // writes "succeed" but change nothing
  int ecFailWriteNumber = -1;    // the N-th write (1-based, counted from ecWrites == 0) fails
  bool msrAvailable = true;      // PawnIO + IntelMSR usable
  bool msrFollowsEc = true;      // EC PL1/PL2 changes are mirrored into MSR 0x610 (unknown on the real unit)
  bool msrFirmwareOwns = false;  // firmware rewrites MSR 0x610 from the EC values continuously (writes do not stick)
  bool msrPkgLocked = false;     // MSR 0x610 lock bit
  bool iccLocked = false;        // MSR 0x601 lock bit
  bool msrPsysLocked = false;    // MSR 0x65C (platform power limit) lock bit
  bool ocMailboxOpen = true;     // voltage mailbox answers (false = BIOS closed it)
  bool nvmlAvailable = true;
  int nvLagSamples = 0;          // NVML needs N reads before it shows a new cTGP

  // ---- EC ------------------------------------------------------------------------------------------
  uint8_t ec[0x1000];
  int ecReads = 0, ecWrites = 0;
  std::vector<std::pair<uint16_t, uint8_t>> writeLog;
  mgp::EcByte EcRead(uint16_t addr);
  mgp::EcByte EcWrite(uint16_t addr, uint8_t value);
  void SetEcProfile(bool oemOn, int ctgpOffset);   // cTGP state like the OEM service leaves it
  void DisplayCycle();                             // display off/on: firmware drops cTGP offsets above 15
  bool EcActive() const { return (ec[0x741] & 0x05) == 0x01; }
  void MakeEcCold();                               // OEM service not running: loop inactive

  // ---- MSR -----------------------------------------------------------------------------------------
  std::map<uint32_t, uint64_t> msr;
  bool MsrAllowed(uint32_t m, bool write) const;
  bool MsrRead(uint32_t m, uint64_t* v);
  bool MsrWrite(uint32_t m, uint64_t v);
  void ClearThrottleLogs() { logThermal = logPower = false; }

  // ---- derived physical state (read by adapters) -----------------------------------------------------
  double cpuW = 8, gpuW = 10, cpuT = 41, gpuT = 38;
  double cpuFanPct = 0, gpuFanPct = 0, cpuRpm = 0, gpuRpm = 0;
  double cpuDemandW = 8;
  bool powerLimited = false, thermalLimited = false, budgetLimited = false;
  bool logThermal = false, logPower = false;
  int gpuCoreOffset = 0, gpuMemOffset = 0;
  double Pl1W() const;   // limits the CPU actually obeys (from MSR 0x610)
  double Pl2W() const;
  double CtgpW() const;  // GPU power limit derived from EC 0x743/0x744
  void Step();           // advance the model to clock.NowMs()

  Clock& clk;

 private:
  void Sub(double h);
  void MirrorEcToMsr(bool force);
  void UpdateRegisters();
  double bucketJ_ = 0;
  uint64_t last_ = 0, pendingActAt_ = 0;
  uint8_t mirroredPl1_ = 0, mirroredPl2_ = 0;
  double eCpu_ = 0, eCore_ = 0, eIgpu_ = 0, eDram_ = 0, ePsys_ = 0, throttleS_ = 0;
  uint64_t ocResp_ = 0;
  int ocOffsetMv_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  bool ocHandle(uint64_t cmd);
};

// ------------------------------------------------------------------------------------------------ adapters
struct Ec : mgp::IEcDriver {
  explicit Ec(Platform& p) : p_(p) {}
  mgp::DriverState state = mgp::DriverState::Connected;
  uint32_t winErr = 0;
  mgp::DriverState Probe(uint32_t* w) override { if (w) *w = winErr; return state; }
  mgp::EcByte Read(uint16_t a) override { std::lock_guard<std::recursive_mutex> g(p_.mu); return p_.EcRead(a); }
  mgp::EcByte Write(uint16_t a, uint8_t v) override { std::lock_guard<std::recursive_mutex> g(p_.mu); return p_.EcWrite(a, v); }
 private:
  Platform& p_;
};

struct Nv : mgp::INvidiaTelemetry {
  explicit Nv(Platform& p) : p_(p) {}
  bool Init(std::string& err) override;
  bool ReadStatic(mgp::NvStatic& o, std::string& err) override;
  bool ReadDynamic(mgp::NvDynamic& o) override;
  void Shutdown() override {}
 private:
  Platform& p_;
  uint32_t seenWrites_ = 0;
  int countdown_ = 0;
  double limit_ = 125;
};

struct Msr : IMsr {
  explicit Msr(Platform& p) : p_(p) {}
  bool Open(std::string* why) override;
  bool Read(uint32_t m, uint64_t* v) override { std::lock_guard<std::recursive_mutex> g(p_.mu); return p_.MsrRead(m, v); }
  bool Write(uint32_t m, uint64_t v) override { std::lock_guard<std::recursive_mutex> g(p_.mu); return p_.MsrWrite(m, v); }
  std::string Name() const override { return "simulated MSR (no hardware)"; }
 private:
  Platform& p_;
};

struct GpuExtra : IGpuExtra {
  explicit GpuExtra(Platform& p) : p_(p) {}
  bool Init(std::string* why) override;
  bool Read(GpuInfo* out) override;
  bool OffsetRange(int* cmin, int* cmax, int* mmin, int* mmax) override { *cmin = -500; *cmax = 500; *mmin = -1000; *mmax = 1500; return true; }
  bool SetCoreOffset(int mhz, std::string*) override { std::lock_guard<std::recursive_mutex> g(p_.mu); p_.gpuCoreOffset = mhz; return true; }
  bool SetMemOffset(int mhz, std::string*) override { std::lock_guard<std::recursive_mutex> g(p_.mu); p_.gpuMemOffset = mhz; return true; }
 private:
  Platform& p_;
};

// Frame rate that follows the simulated GPU load: lighter load = more frames. Only for the simulator and the README pictures.
struct FpsSim : IFpsSource {
  explicit FpsSim(Platform& p) : p_(p) {}
  bool Available(std::string*) override { return true; }
  double Fps(std::string* app) override;
 private:
  Platform& p_;
};

struct Power : IOsPower {
  explicit Power(Platform& p) : p_(p) {}
  CpuPolicy policy{2, 33, 5, 100, 100};
  bool failSet = false;
  std::vector<std::string> exes;  // running processes (lower case)
  bool GetCpuPolicy(CpuPolicy* out) override { *out = policy; return true; }
  bool SetCpuPolicy(const CpuPolicy& p, std::string* why) override;
  SysStatus Status() override;
  std::vector<std::string> RunningExes() override { return exes; }
 private:
  Platform& p_;
};

struct Display : IOsDisplay {
  int brightness = 70, width = 1920, height = 1080, refresh = 144;
  std::vector<int> rates{60, 144};
  ColorSettings color;
  bool failColor = false;
  DisplayInfo Info() override;
  bool SetBrightness(int pct, std::string*) override { brightness = pct; return true; }
  bool SetRefresh(int hz, std::string* why) override;
  bool SetColor(const ColorSettings& c, std::string* why) override;
};

struct Devices : IOsDevices {
  std::map<Dev, int> state{{Dev::Wifi, 1}, {Dev::Bluetooth, 1}, {Dev::Webcam, 1}, {Dev::Touchpad, 1}};
  bool stuck = false;  // Set() succeeds but the device does not change
  int Get(Dev d) override { auto it = state.find(d); return it == state.end() ? -1 : it->second; }
  bool Set(Dev d, bool on, std::string* why) override;
};

struct Oem : IOemControl {
  bool installed = true, running = true, disabled = false, failStop = false;
  OemSvcState Probe() override;
  bool StopAndDisable(std::string* why) override;
  bool EnableAndStart(std::string* why) override;
};

struct Light : ILightHid {
  bool kbd = true, bar = false, logo = false;
  std::vector<std::pair<LightZone, std::array<uint8_t, 9>>> sent;
  std::vector<LightZone> Detect(std::string* info) override;
  bool Send(LightZone z, const uint8_t r[9]) override {
    std::array<uint8_t, 9> a;
    for (int i = 0; i < 9; i++) a[i] = r[i];
    sent.push_back({z, a});
    if (sent.size() > 20000) sent.erase(sent.begin(), sent.begin() + 5000);
    return true;
  }
};

struct Host : IHostInfo {
  HostInfo Get() override;
};

struct Smbios : mgp::ISmbiosSource {
  std::vector<uint8_t> raw;
  bool ok = true;
  bool ReadRaw(std::vector<uint8_t>& out, std::string& err) override {
    if (!ok) { err = "sim smbios failure"; return false; }
    out = raw;
    return true;
  }
};
// Synthetic SMBIOS table (types 0, 1, 2, 127). Serial numbers are deliberately fake.
std::vector<uint8_t> BuildSmbios(const std::string& manuf, const std::string& product, const std::string& bios, const std::string& board);

// ------------------------------------------------------------------------------------------------ everything together
enum class Model { Exact, Family, Unknown };  // exact = Monster Tulpar T5 V21.7 / N.1.07Mon08; family = same EC project id, other SMBIOS; unknown

// Counts acquire/release pairs; can be told to time out.
class Lock : public ICrossProcessLock {
 public:
  bool Acquire(uint32_t) override {
    if (failNext) return false;
    ++acquired;
    ++depth;
    return true;
  }
  void Release() override {
    ++released;
    --depth;
  }
  int acquired = 0, released = 0, depth = 0;
  bool failNext = false;
};

struct World {
  explicit World(Model m = Model::Exact, bool realClock = false);
  Clock clock;
  MemLog log;
  Platform plat;
  Ec ec;
  Nv nv;
  Msr msr;
  GpuExtra gpu;
  Power power;
  Display display;
  Devices devices;
  Oem oem;
  Light light;
  Host host;
  FpsSim fps;
  Smbios smbios;
  Lock lock;
  Hardware hw();
};

}  // namespace sim
}  // namespace bc
