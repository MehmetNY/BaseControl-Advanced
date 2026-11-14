// Hardware / OS abstraction layer. The Controller (pure logic, no <windows.h>) only talks to these interfaces,
// so the whole control stack runs against simulated hardware on any host (unit tests, screenshots, --sim).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../mgp/core/interfaces.h"
#include "../mgp/core/oem_platform.h"

namespace bc {

// ------------------------------------------------------------------------------------------------ MSR (PawnIO)
class IMsr {
 public:
  virtual ~IMsr() = default;
  virtual bool Open(std::string* why) = 0;  // true when a driver + module is usable
  virtual bool Read(uint32_t msr, uint64_t* v) = 0;
  virtual bool Write(uint32_t msr, uint64_t v) = 0;
  virtual std::string Name() const = 0;
  // Per-logical-processor access (APERF/MPERF effective clocks, per-core prefetcher state). Optional.
  virtual int CpuCount() { return 0; }
  virtual bool ReadCpu(int /*cpu*/, uint32_t /*msr*/, uint64_t* /*v*/) { return false; }
};

// ------------------------------------------------------------------------------------------------ GPU (NVML extras)
struct GpuInfo {
  bool ok = false;
  std::string name;
  int tempC = 0;
  double powerW = 0, limitW = 0;
  int gfxMhz = 0, memMhz = 0, maxGfxMhz = 0;
  int utilPct = 0, memUtilPct = 0;
  double vramUsedMb = 0, vramTotalMb = 0;
  int fanPct = -1;
  int pstate = -1;
  uint64_t throttleMask = 0;  // NVML clocksThrottleReasons
  bool haveOffsets = false;
  int coreOffsetMhz = 0, memOffsetMhz = 0;
};
inline constexpr uint64_t kNvThrottleIdle = 0x1, kNvThrottleAppClocks = 0x2, kNvThrottleSwPower = 0x4, kNvThrottleHwSlow = 0x8,
                          kNvThrottleSyncBoost = 0x10, kNvThrottleSwThermal = 0x20, kNvThrottleHwThermal = 0x40,
                          kNvThrottleHwPowerBrake = 0x80, kNvThrottleDisplayClk = 0x100;

class IGpuExtra {
 public:
  virtual ~IGpuExtra() = default;
  virtual bool Init(std::string* why) = 0;
  virtual bool Read(GpuInfo* out) = 0;
  virtual bool OffsetRange(int* coreMin, int* coreMax, int* memMin, int* memMax) = 0;
  virtual bool SetCoreOffset(int mhz, std::string* why) = 0;
  virtual bool SetMemOffset(int mhz, std::string* why) = 0;
};

// ------------------------------------------------------------------------------------------------ OS: power / CPU policy / usage
struct CpuPolicy {
  int boost = -1;     // Windows "processor performance boost mode": 0 disabled 1 enabled 2 aggressive 3 efficient enabled 4 efficient aggressive 5 aggressive at guaranteed 6 efficient aggressive at guaranteed
  int epp = -1;       // 0..100 energy performance preference (0 = performance)
  int minPct = -1;    // 0..100 minimum processor state
  int maxPct = -1;    // 1..100 maximum processor state
  int parkMinPct = -1;  // 0..100 minimum parked cores
};

struct SysStatus {
  bool ac = true;
  int battPct = -1;
  int battSecs = -1;
  bool charging = false;
  double cpuPct = 0;
  double memPct = 0, memUsedMb = 0, memTotalMb = 0;
  std::vector<int> coreMhz;  // current frequency per logical processor (CallNtPowerInformation)
  int cpuMaxMhz = 0;
};

class IOsPower {
 public:
  virtual ~IOsPower() = default;
  virtual bool GetCpuPolicy(CpuPolicy* out) = 0;
  virtual bool SetCpuPolicy(const CpuPolicy& p, std::string* why) = 0;
  virtual SysStatus Status() = 0;
  virtual std::vector<std::string> RunningExes() = 0;  // lower-case image names of running processes
};

// ------------------------------------------------------------------------------------------------ OS: display
struct DisplayInfo {
  bool ok = false;
  int brightness = -1;               // 0..100, -1 unknown
  int width = 0, height = 0, refresh = 0;
  std::vector<int> refreshRates;     // supported at the current resolution
};
struct ColorSettings {  // software colour control: a gamma ramp (everything but saturation) and a colour matrix (saturation)
  int brightness = 50;  // 0..100 (50 = neutral)
  int contrast = 50;
  int gamma = 100;      // 50..260 (x100)
  int kelvin = 6500;    // 3000..9000
  int r = 100, g = 100, b = 100;  // channel gain percent
  int saturation = 100;           // 0..200 percent (100 = neutral); needs the Windows colour-effect API, see IOsDisplay::SaturationSupported
  bool operator==(const ColorSettings& o) const {
    return brightness == o.brightness && contrast == o.contrast && gamma == o.gamma && kelvin == o.kelvin && r == o.r && g == o.g && b == o.b &&
           saturation == o.saturation;
  }
  bool Neutral() const { return *this == ColorSettings(); }
};
class IOsDisplay {
 public:
  virtual ~IOsDisplay() = default;
  virtual DisplayInfo Info() = 0;
  virtual bool SetBrightness(int pct, std::string* why) = 0;
  virtual bool SetRefresh(int hz, std::string* why) = 0;
  virtual bool SetColor(const ColorSettings& c, std::string* why) = 0;
  // True when the saturation slider can work here (it uses the Windows full-screen colour effect, not the gamma ramp). Must not
  // initialise anything: it is called while probing, from another thread than the one that applies colours.
  virtual bool SaturationSupported() { return false; }
};

// ------------------------------------------------------------------------------------------------ OS: device switches
enum class Dev { Wifi, Bluetooth, Webcam, Touchpad };
class IOsDevices {
 public:
  virtual ~IOsDevices() = default;
  virtual int Get(Dev d) = 0;  // -1 unknown / absent, 0 off, 1 on
  virtual bool Set(Dev d, bool on, std::string* why) = 0;
};

// ------------------------------------------------------------------------------------------------ frame rate of the program in front
// Optional. A real implementation needs an event-tracing session (administrator), i.e. it lives in the service.
class IFpsSource {
 public:
  virtual ~IFpsSource() = default;
  virtual bool Available(std::string* why) = 0;
  // Frames presented per second over the last second by the busiest graphics process; -1 when nothing was presented lately.
  // `app` receives that process' image name.
  virtual double Fps(std::string* app) = 0;
};

// ------------------------------------------------------------------------------------------------ OEM Control Center coexistence
struct OemSvcState {
  bool installed = false;
  bool running = false;
  std::string version;
  std::vector<std::string> services;  // names of OEM services that are installed
};
class IOemControl {
 public:
  virtual ~IOemControl() = default;
  virtual OemSvcState Probe() = 0;
  virtual bool StopAndDisable(std::string* why) = 0;   // stop + start type Disabled (reversible)
  virtual bool EnableAndStart(std::string* why) = 0;
};

// ------------------------------------------------------------------------------------------------ RGB lighting (ITE HID)
enum class LightZone { Keyboard = 0, LightBar = 2, Logo = 3 };
class ILightHid {
 public:
  virtual ~ILightHid() = default;
  // Returns the zones for which a controller was found. `info` is a human readable list (for the probe report).
  virtual std::vector<LightZone> Detect(std::string* info) = 0;
  // 9-byte HID feature report ([0]=report id 0). false = not delivered.
  virtual bool Send(LightZone z, const uint8_t report[9]) = 0;
};

// ------------------------------------------------------------------------------------------------ cross-process apply lock
// Serialises "apply" sequences with other tools that write the same registers (the MONSTER GPU POWER app / its logon task).
class ICrossProcessLock {
 public:
  virtual ~ICrossProcessLock() = default;
  virtual bool Acquire(uint32_t timeoutMs) = 0;  // false = timed out
  virtual void Release() = 0;
};

// ------------------------------------------------------------------------------------------------ identity / misc
struct HostInfo {
  std::string os, cpu, ram, machineName;
  std::string gpus;   // display adapters installed in Windows, ';' separated (integrated first when the driver lists it first)
  std::string biosVendor, biosVersion, sysVendor, sysProduct, boardName;
  bool elevated = true;
};
class IHostInfo {
 public:
  virtual ~IHostInfo() = default;
  virtual HostInfo Get() = 0;
};

// Everything the controller needs, owned by the composition root (service / UI host / simulator).
struct Hardware {
  mgp::IEcDriver* ec = nullptr;
  mgp::INvidiaTelemetry* nv = nullptr;
  mgp::ISmbiosSource* smbios = nullptr;
  mgp::oem::IOemPlatform* oemFiles = nullptr;  // OEM profile JSON access (optional; only for cTGP persistence sync)
  mgp::IClock* clock = nullptr;
  mgp::ILog* log = nullptr;
  IMsr* msr = nullptr;
  IGpuExtra* gpu = nullptr;
  IOsPower* power = nullptr;
  IOsDisplay* display = nullptr;
  IOsDevices* devices = nullptr;
  IOemControl* oem = nullptr;
  ILightHid* light = nullptr;
  IHostInfo* host = nullptr;
  IFpsSource* fps = nullptr;               // optional
  ICrossProcessLock* applyLock = nullptr;  // optional
};

}  // namespace bc
