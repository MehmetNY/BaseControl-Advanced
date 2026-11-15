// Hardware / OS abstraction layer. The Controller (pure logic, no <windows.h>) only talks to these interfaces,
// so the whole control stack runs against simulated hardware on any host (unit tests, screenshots, --sim).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../mgp/core/interfaces.h"
#include "../mgp/core/oem_platform.h"
#include "../oem/link.h"

namespace bc {

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

// Extra readings from the graphics driver (NVML): clocks, load, throttle reasons and the clock offsets that are in force.
class IGpuExtra {
 public:
  virtual ~IGpuExtra() = default;
  virtual bool Init(std::string* why) = 0;
  virtual bool Read(GpuInfo* out) = 0;   // read only: the clock offsets are the vendor service's to change
};

// ------------------------------------------------------------------------------------------------ OS: power status / running programs
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

// ------------------------------------------------------------------------------------------------ OEM Control Center (read-only look)
// Control Center Advanced does not stop, disable or replace anything of the vendor's: its services are the way to the EC and the BIOS.
// This tells the Service page whether they are installed and running and can start them again when they are not.
struct OemSvcState {
  bool installed = false;
  bool running = false;
  std::string version;
  std::vector<std::string> services;  // names of OEM services that are installed
};
class IOemProbe {
 public:
  virtual ~IOemProbe() = default;
  virtual OemSvcState Probe() = 0;
  // Starts the vendor's services that are installed but not running (an earlier version of this program, or the user, may have stopped
  // them). Never installs, stops, disables or replaces anything.
  virtual bool StartServices(std::string* why) { if (why) *why = "not supported here"; return false; }
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
// One populated memory slot (or soldered package) as the firmware describes it (SMBIOS type 17). Serial numbers are never read.
struct MemModule {
  int sizeMb = 0;
  std::string type;          // "DDR4", "LPDDR5" ... ("" when the firmware code is unknown)
  int ratedMts = 0;          // speed the module is rated for (0 = not reported)
  int runMts = 0;            // speed it runs at now (0 = not reported)
  std::string locator, maker, part;
};

struct HostInfo {
  std::string os, cpu, ram, machineName;
  std::string gpus;   // display adapters installed in Windows, ';' separated (integrated first when the driver lists it first)
  std::string biosVendor, biosVersion, sysVendor, sysProduct, boardName;
  int cpuCores = 0, cpuThreads = 0, cpuBaseMhz = 0;   // 0 = not reported
  std::vector<MemModule> memory;                      // populated slots only
  bool elevated = true;
};
class IHostInfo {
 public:
  virtual ~IHostInfo() = default;
  virtual HostInfo Get() = 0;
};

// ------------------------------------------------------------------------------------------------ storage (read-only inventory + health)
struct StorageDrive {
  std::string model;           // as the drive reports it, whitespace collapsed
  std::string bus;             // "NVMe" | "SATA" | "SAS" | "SCSI" | "RAID" | "ATA"
  bool ssd = false;
  uint64_t sizeBytes = 0;
  // Health. -1 = not reported (the drive, the driver or this process's rights did not provide it).
  int tempC = -1;
  long long powerCycles = -1;
  long long powerOnHours = -1;
  int wearPct = -1;            // NVMe "percentage used" (100 = rated endurance reached; may exceed 100)
  int sparePct = -1;           // NVMe "available spare"
  int critWarn = -1;           // NVMe critical-warning bit field (0 = none)
  long long writtenGb = -1;    // total host data written, decimal GB
};

class IStorage {
 public:
  virtual ~IStorage() = default;
  // Lists the internal drives. With health=false only the inventory is read (no SMART/log-page queries).
  virtual bool Read(std::vector<StorageDrive>* out, bool health) = 0;
};

// Everything the controller needs, owned by the composition root (service / UI host / simulator).
struct Hardware {
  mgp::IEcDriver* ec = nullptr;
  mgp::INvidiaTelemetry* nv = nullptr;
  mgp::ISmbiosSource* smbios = nullptr;
  mgp::oem::IOemPlatform* oemFiles = nullptr;  // OEM profile JSON access (optional; only for cTGP persistence sync)
  mgp::IClock* clock = nullptr;
  mgp::ILog* log = nullptr;
  IGpuExtra* gpu = nullptr;
  IOsPower* power = nullptr;
  IOsDisplay* display = nullptr;
  IOsDevices* devices = nullptr;
  IOemProbe* oemSvc = nullptr;             // optional: are the vendor's services installed / running (read-only)
  oem::IOemLink* oemLink = nullptr;        // the way to the vendor's service (profile values, fans, keyboard lighting, battery, switches)
  IHostInfo* host = nullptr;
  IStorage* storage = nullptr;             // optional (Device info page)
  IFpsSource* fps = nullptr;               // optional
  ICrossProcessLock* applyLock = nullptr;  // optional
};

}  // namespace bc
