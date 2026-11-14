#include "os_power.h"

#include <powrprof.h>
#include <tlhelp32.h>

#include <algorithm>

#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

// Windows power setting GUIDs (documented in the power policy SDK; the subgroup is GUID_PROCESSOR_SETTINGS_SUBGROUP).
const GUID kSubProcessor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
const GUID kBoostMode = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
const GUID kEpp = {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}};
const GUID kMinState = {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};
const GUID kMaxState = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
const GUID kParkMin = {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}};

// PROCESSOR_POWER_INFORMATION (winnt.h in the Windows SDK; not present in the mingw headers)
struct ProcPowerInfo {
  ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};

uint64_t FtU64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

}  // namespace

OsPower::OsPower() {}

bool OsPower::OnAc() {
  SYSTEM_POWER_STATUS s;
  if (!GetSystemPowerStatus(&s)) return true;
  return s.ACLineStatus != 0;  // 0 offline, 1 online, 255 unknown -> treat unknown as AC (desktop-like)
}

bool OsPower::ReadValue(const GUID& setting, bool ac, DWORD* v) {
  GUID* scheme = nullptr;
  if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
  DWORD val = 0;
  const DWORD rc = ac ? PowerReadACValueIndex(nullptr, scheme, &kSubProcessor, &setting, &val)
                      : PowerReadDCValueIndex(nullptr, scheme, &kSubProcessor, &setting, &val);
  LocalFree(scheme);
  if (rc != ERROR_SUCCESS) return false;
  *v = val;
  return true;
}

bool OsPower::WriteValue(const GUID& setting, bool ac, DWORD v, std::string* why) {
  GUID* scheme = nullptr;
  if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) {
    if (why) *why = "no active power scheme";
    return false;
  }
  const DWORD rc = ac ? PowerWriteACValueIndex(nullptr, scheme, &kSubProcessor, &setting, v)
                      : PowerWriteDCValueIndex(nullptr, scheme, &kSubProcessor, &setting, v);
  LocalFree(scheme);
  if (rc != ERROR_SUCCESS) {
    if (why) *why = "PowerWrite*ValueIndex: " + Win32Text(rc);
    return false;
  }
  return true;
}

bool OsPower::GetCpuPolicy(CpuPolicy* out) {
  if (!out) return false;
  const bool ac = OnAc();
  CpuPolicy p;
  DWORD v = 0;
  bool any = false;
  if (ReadValue(kBoostMode, ac, &v)) { p.boost = static_cast<int>(v); any = true; }
  if (ReadValue(kEpp, ac, &v)) { p.epp = static_cast<int>(v); any = true; }
  if (ReadValue(kMinState, ac, &v)) { p.minPct = static_cast<int>(v); any = true; }
  if (ReadValue(kMaxState, ac, &v)) { p.maxPct = static_cast<int>(v); any = true; }
  if (ReadValue(kParkMin, ac, &v)) { p.parkMinPct = static_cast<int>(v); any = true; }
  if (any) *out = p;
  return any;
}

bool OsPower::SetCpuPolicy(const CpuPolicy& p, std::string* why) {
  const bool ac = OnAc();
  bool wrote = false;
  auto put = [&](const GUID& g, int val) {
    if (val < 0) return true;  // -1 = leave unchanged
    if (!WriteValue(g, ac, static_cast<DWORD>(val), why)) return false;
    wrote = true;
    return true;
  };
  if (!put(kBoostMode, p.boost) || !put(kEpp, p.epp) || !put(kMinState, p.minPct) || !put(kMaxState, p.maxPct) || !put(kParkMin, p.parkMinPct)) return false;
  if (wrote) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
      const DWORD rc = PowerSetActiveScheme(nullptr, scheme);  // makes Windows re-read the scheme
      LocalFree(scheme);
      if (rc != ERROR_SUCCESS) {
        if (why) *why = "PowerSetActiveScheme: " + Win32Text(rc);
        return false;
      }
    }
  }
  return true;
}

SysStatus OsPower::Status() {
  SysStatus s;
  SYSTEM_POWER_STATUS ps;
  if (GetSystemPowerStatus(&ps)) {
    s.ac = ps.ACLineStatus != 0;
    s.battPct = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : -1;
    s.battSecs = ps.BatteryLifeTime != 0xFFFFFFFFu ? static_cast<int>(ps.BatteryLifeTime) : -1;
    s.charging = (ps.BatteryFlag & 8) != 0;
  }
  FILETIME idle, kern, user;
  if (GetSystemTimes(&idle, &kern, &user)) {
    const uint64_t i = FtU64(idle), k = FtU64(kern), u = FtU64(user);  // kernel time includes idle time
    if (haveCpuBase_) {
      const uint64_t total = (k - lastKernel_) + (u - lastUser_);
      const uint64_t busy = total > (i - lastIdle_) ? total - (i - lastIdle_) : 0;
      s.cpuPct = total ? 100.0 * static_cast<double>(busy) / static_cast<double>(total) : 0.0;
    }
    lastIdle_ = i;
    lastKernel_ = k;
    lastUser_ = u;
    haveCpuBase_ = true;
  }
  MEMORYSTATUSEX m;
  m.dwLength = sizeof m;
  if (GlobalMemoryStatusEx(&m)) {
    s.memTotalMb = static_cast<double>(m.ullTotalPhys) / (1024.0 * 1024.0);
    s.memUsedMb = static_cast<double>(m.ullTotalPhys - m.ullAvailPhys) / (1024.0 * 1024.0);
    s.memPct = static_cast<double>(m.dwMemoryLoad);
  }
  const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (n > 0 && n <= 256) {
    std::vector<ProcPowerInfo> info(n);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(), static_cast<ULONG>(n * sizeof(ProcPowerInfo))) == 0) {
      for (DWORD i = 0; i < n; ++i) {
        s.coreMhz.push_back(static_cast<int>(info[i].CurrentMhz));
        s.cpuMaxMhz = std::max(s.cpuMaxMhz, static_cast<int>(info[i].MaxMhz));
      }
    }
  }
  return s;
}

std::vector<std::string> OsPower::RunningExes() {
  std::vector<std::string> v;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return v;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) {
    do {
      v.push_back(Lower(U(pe.szExeFile)));
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

}  // namespace hw
}  // namespace bc
