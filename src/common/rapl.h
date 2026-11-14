// Intel RAPL / MSR field codecs (pure functions, unit-tested). Layouts follow the Intel SDM Vol. 4
// (MSR_RAPL_POWER_UNIT, MSR_PKG_POWER_LIMIT, MSR_PKG_POWER_INFO, MSR_TEMPERATURE_TARGET, OC mailbox).
// Whether a given write is permitted is decided elsewhere (PawnIO's IntelMSR module has its own allowlist;
// BaseControl additionally refuses anything that is locked, out of range or not read back identically).
#pragma once
#include <cstdint>

namespace bc {
namespace rapl {

inline constexpr uint32_t kMsrPlatformInfo = 0x0CE;
inline constexpr uint32_t kMsrPerfStatus = 0x198;
inline constexpr uint32_t kMsrThermStatus = 0x19C;
inline constexpr uint32_t kMsrTempTarget = 0x1A2;
inline constexpr uint32_t kMsrPkgThermStatus = 0x1B1;
inline constexpr uint32_t kMsrTurboRatio = 0x1AD;
inline constexpr uint32_t kMsrOcMailbox = 0x150;
inline constexpr uint32_t kMsrMiscFeatureControl = 0x1A4;
inline constexpr uint32_t kMsrVrCurrentConfig = 0x601;
inline constexpr uint32_t kMsrRaplUnit = 0x606;
inline constexpr uint32_t kMsrPkgPowerLimit = 0x610;
inline constexpr uint32_t kMsrPkgEnergy = 0x611;
inline constexpr uint32_t kMsrPkgPerfStatus = 0x613;
inline constexpr uint32_t kMsrPkgPowerInfo = 0x614;
inline constexpr uint32_t kMsrPp0Energy = 0x639;

struct Units {
  double powerW = 0.125;      // 1 / 2^PU
  double energyJ = 1.0 / 16384.0;
  double timeS = 1.0 / 1024.0;
};
Units DecodeUnits(uint64_t msr606);

struct PkgLimit {
  double pl1W = 0, pl2W = 0;
  bool pl1En = false, pl1Clamp = false, pl2En = false, pl2Clamp = false;
  double tau1S = 0, tau2S = 0;
  bool lock = false;
};
PkgLimit DecodePkgLimit(uint64_t msr610, const Units& u);
// Builds a new 0x610 value from `old`, replacing only the fields in `want`. The lock bit is preserved from `old`
// (we never set it). Returns false if a value cannot be represented.
bool EncodePkgLimit(uint64_t old, const PkgLimit& want, const Units& u, uint64_t* out);

bool EncodeTimeWindow(double seconds, const Units& u, uint32_t* y, uint32_t* z);
double DecodeTimeWindow(uint32_t y, uint32_t z, const Units& u);

struct PkgInfo { double tdpW = 0, minW = 0, maxW = 0, maxTauS = 0; };
PkgInfo DecodePkgInfo(uint64_t msr614, const Units& u);

// Energy counter delta -> average watts (handles 32-bit wrap).
double EnergyToWatts(uint32_t prev, uint32_t cur, double dtSeconds, const Units& u);

// MSR_VR_CURRENT_CONFIG: bits 12:0 current limit in 1/8 A, bit 31 lock.
double DecodeIccMaxA(uint64_t msr601);
bool EncodeIccMax(uint64_t old, double amps, uint64_t* out);
inline bool IccLocked(uint64_t msr601) { return (msr601 >> 31) & 1; }

// MSR_TEMPERATURE_TARGET: bits 23:16 TjMax, bits 29:24 TCC offset.
inline int TjMax(uint64_t msr1a2) { return static_cast<int>((msr1a2 >> 16) & 0xFF); }
inline int TccOffset(uint64_t msr1a2) { return static_cast<int>((msr1a2 >> 24) & 0x3F); }
// IA32_PACKAGE_THERM_STATUS: bits 22:16 = degrees below the TCC activation temperature.
inline int ThermReadout(uint64_t msr1b1) { return static_cast<int>((msr1b1 >> 16) & 0x7F); }
// MSR_TURBO_RATIO_LIMIT byte n (0-based) = ratio limit for n+1 active cores.
inline int TurboRatio(uint64_t msr1ad, int n) { return static_cast<int>((msr1ad >> (8 * n)) & 0xFF); }
inline int PlatformBaseRatio(uint64_t msr0ce) { return static_cast<int>((msr0ce >> 8) & 0xFF); }
inline int PerfStatusRatio(uint64_t msr198) { return static_cast<int>((msr198 >> 8) & 0xFF); }

// OC mailbox (voltage offsets). Planes: 0 core, 1 gpu, 2 cache, 3 system agent, 4 analog io.
uint64_t OcReadVoltageCmd(int plane);
uint64_t OcWriteVoltageCmd(int plane, int offsetMv);
bool OcBusy(uint64_t v);
int OcDecodeOffsetMv(uint64_t response);    // signed, nearest mV
int OcResponseCode(uint64_t v);             // bits 39:32 after completion (0 = ok)

}  // namespace rapl
}  // namespace bc
