#include "os_power.h"

#include <powrprof.h>
#include <tlhelp32.h>

#include <algorithm>

#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

// PROCESSOR_POWER_INFORMATION (winnt.h in the Windows SDK; not present in the mingw headers)
struct ProcPowerInfo {
  ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};

uint64_t FtU64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

}  // namespace

OsPower::OsPower() {}

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
