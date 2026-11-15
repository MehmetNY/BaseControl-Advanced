// Simulated machine: a small physical model (power, temperatures, fans, battery) plus an EC memory image, exposed through the same
// interfaces the real Windows backends implement, and a stand-in for the vendor's Control Center service (fake_oem.h) behind a real
// MQTT client. Used by the unit tests, by `ControlCenterAdvanced.exe --sim` and for the README screenshots (which are always labelled
// SIMULATION).
//
// This is NOT evidence about real hardware: every behaviour here is an assumption written down so the controller logic can be
// exercised (verify/rollback paths, firmware resets, a service that does not answer ...).
#pragma once
#include <array>
#include <memory>
#include <chrono>
#include <functional>
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
  void MakeEcCold();                               // vendor service not running: power-management loop inactive
  void ClearThrottleLogs() { logThermal = logPower = false; }

  // ---- derived physical state (read by adapters) -----------------------------------------------------
  double cpuW = 8, gpuW = 10, cpuT = 41, gpuT = 38;
  double cpuFanPct = 0, gpuFanPct = 0, cpuRpm = 0, gpuRpm = 0;
  double cpuDemandW = 8;
  bool powerLimited = false, thermalLimited = false, budgetLimited = false;
  bool logThermal = false, logPower = false;
  int gpuCoreOffset = 0, gpuMemOffset = 0;
  double Pl1W() const;   // limits the CPU actually obeys (from the EC while its loop runs, otherwise an assumed firmware default)
  double Pl2W() const;
  double CtgpW() const;  // GPU power limit derived from EC 0x743/0x744
  void Step();           // advance the model to clock.NowMs()

  Clock& clk;

 private:
  void Sub(double h);
  void UpdateRegisters();
  double bucketJ_ = 0;
  uint64_t last_ = 0;
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

struct GpuExtra : IGpuExtra {
  explicit GpuExtra(Platform& p) : p_(p) {}
  bool Init(std::string* why) override;
  bool Read(GpuInfo* out) override;
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
  std::vector<std::string> exes;  // running processes (lower case)
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
  bool saturationOk = true;   // false: the colour-effect API is missing (the saturation slider is unavailable)
  int colorApplied = 0;       // how many times colours were pushed to the panel (tests: re-apply after resume)
  DisplayInfo Info() override;
  bool SetBrightness(int pct, std::string*) override { brightness = pct; return true; }
  bool SetRefresh(int hz, std::string* why) override;
  bool SetColor(const ColorSettings& c, std::string* why) override;
  bool SaturationSupported() override { return saturationOk; }
};

struct Devices : IOsDevices {
  std::map<Dev, int> state{{Dev::Wifi, 1}, {Dev::Bluetooth, 1}, {Dev::Webcam, 1}, {Dev::Touchpad, 1}};
  bool stuck = false;  // Set() succeeds but the device does not change
  int Get(Dev d) override { auto it = state.find(d); return it == state.end() ? -1 : it->second; }
  bool Set(Dev d, bool on, std::string* why) override;
};

// Whether the vendor's services look installed and running (read-only; the program never stops them).
struct OemProbeSim : IOemProbe {
  bool installed = true, running = true;
  int started = 0;   // how many times "start the vendor's services" was asked for
  std::function<void()> onStart;   // the simulated world brings its fake service up when the services are "started"
  OemSvcState Probe() override;
  bool StartServices(std::string* why) override {
    if (!installed) {
      if (why) *why = "no OEM service is installed";
      return false;
    }
    ++started;
    running = true;
    if (onStart) onStart();
    return true;
  }
};

struct Host : IHostInfo {
  mgp::ISmbiosSource* smbios = nullptr;   // memory modules come from the same (synthetic) table the real implementation parses
  HostInfo Get() override;
};

// Two internal NVMe drives with the readings of the reference machine's Components page (simulation values, not evidence).
struct Storage : IStorage {
  std::mutex mu;               // the interactive simulator changes drives from the script thread while the controller reads them
  std::vector<StorageDrive> drives;
  bool failRead = false;       // Read() fails altogether
  bool healthDenied = false;   // the inventory works but the health queries are refused (no rights)
  int reads = 0, healthReads = 0;
  Storage();
  bool Read(std::vector<StorageDrive>* out, bool health) override;
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

class FakeOemWorld;   // fake_oem.h

struct World {
  explicit World(Model m = Model::Exact, bool realClock = false);
  ~World();
  Clock clock;
  MemLog log;
  Platform plat;
  Ec ec;
  Nv nv;
  GpuExtra gpu;
  Power power;
  Display display;
  Devices devices;
  OemProbeSim oemProbe;
  Host host;
  Storage storage;
  FpsSim fps;
  Smbios smbios;
  Lock lock;
  // The vendor's service (in memory) and the client the controller uses to reach it; the link is destroyed first.
  std::unique_ptr<FakeOemWorld> oemWorld;
  std::unique_ptr<oem::IOemLink> oemLink;
  Hardware hw();
};

}  // namespace sim
}  // namespace bc
