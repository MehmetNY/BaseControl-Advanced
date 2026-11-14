#include "host_info.h"

#include <cpuid.h>

#include <cstring>
#include <cwchar>

#include "../common/hostdata.h"
#include "../common/util.h"
#include "../mgp/core/smbios.h"

namespace bc {
namespace hw {
namespace {

std::string CpuBrand() {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000004u) return "x86-64 CPU";
  char brand[49] = {0};
  for (unsigned i = 0; i < 3; ++i) {
    __get_cpuid(0x80000002u + i, &a, &b, &c, &d);
    std::memcpy(brand + 16 * i, &a, 4);
    std::memcpy(brand + 16 * i + 4, &b, 4);
    std::memcpy(brand + 16 * i + 8, &c, 4);
    std::memcpy(brand + 16 * i + 12, &d, 4);
  }
  std::string s = Trim(brand);
  // collapse the vendor's internal padding
  std::string out;
  bool sp = false;
  for (char ch : s) {
    if (ch == ' ') {
      if (!sp) out.push_back(' ');
      sp = true;
    } else {
      out.push_back(ch);
      sp = false;
    }
  }
  return out;
}

std::string RegString(HKEY root, const wchar_t* path, const wchar_t* name) {
  HKEY k = nullptr;
  if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return "";
  wchar_t buf[256] = {0};
  DWORD sz = sizeof buf - 2, type = 0;
  const LONG rc = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz);
  RegCloseKey(k);
  return (rc == ERROR_SUCCESS && type == REG_SZ) ? U(buf) : "";
}

std::string OsName() {
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
  OSVERSIONINFOW v = {};
  v.dwOSVersionInfoSize = sizeof v;
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
    if (RtlGetVersionFn f = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) f(&v);
  }
  const char* name = v.dwMajorVersion >= 10 && v.dwBuildNumber >= 22000 ? "Windows 11" : (v.dwMajorVersion >= 10 ? "Windows 10" : "Windows");
  std::string ver = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
  return Fmt("%s%s%s (build %lu)", name, ver.empty() ? "" : " ", ver.c_str(), static_cast<unsigned long>(v.dwBuildNumber));
}

// Physical cores and logical processors (every processor group), from the Windows topology.
void CpuTopology(int* cores, int* threads) {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;
  std::vector<BYTE> buf(len);
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) return;
  int c = 0, t = 0;
  for (DWORD off = 0; off + sizeof(DWORD) * 2 <= len;) {
    const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    if (e->Size == 0) break;
    if (e->Relationship == RelationProcessorCore) {
      c++;
      for (WORD g = 0; g < e->Processor.GroupCount && g < 8; g++)
        for (KAFFINITY m = e->Processor.GroupMask[g].Mask; m; m &= m - 1) t++;
    }
    off += e->Size;
  }
  *cores = c;
  *threads = t;
}

// Nominal frequency of the first logical processor ("~MHz" in the registry: 2304 for a Core i7-11800H @ 2.30 GHz).
int CpuBaseMhz() {
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return 0;
  DWORD mhz = 0, sz = sizeof mhz, type = 0;
  const LONG rc = RegQueryValueExW(k, L"~MHz", nullptr, &type, reinterpret_cast<BYTE*>(&mhz), &sz);
  RegCloseKey(k);
  return (rc == ERROR_SUCCESS && type == REG_DWORD && mhz > 0 && mhz < 20000) ? static_cast<int>(mhz) : 0;
}

// Display adapters Windows knows about (the class key lists every installed adapter, also one that is not driving a screen).
std::string GpuNames() {
  std::string out;
  HKEY cls = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
                    KEY_READ | KEY_WOW64_64KEY, &cls) != ERROR_SUCCESS)
    return out;
  for (DWORD i = 0; i < 16; i++) {
    wchar_t sub[16];
    swprintf(sub, 16, L"%04lu", static_cast<unsigned long>(i));
    HKEY k = nullptr;
    if (RegOpenKeyExW(cls, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
    wchar_t buf[256] = {0};
    DWORD sz = sizeof buf - 2, type = 0;
    const LONG rc = RegQueryValueExW(k, L"DriverDesc", nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ) continue;
    const std::string name = Trim(U(buf));
    const std::string low = Lower(name);
    if (name.empty() || low.find("basic display") != std::string::npos || low.find("remote") != std::string::npos ||
        low.find("virtual") != std::string::npos || low.find("hyper-v") != std::string::npos)
      continue;
    if ((";" + out + ";").find(";" + name + ";") != std::string::npos) continue;
    out += (out.empty() ? "" : ";") + name;
  }
  RegCloseKey(cls);
  return out;
}

}  // namespace

HostInfo HostInfoWin::Get() {
  if (have_) {
    cached_.elevated = IsElevated();
    return cached_;
  }
  HostInfo h;
  h.os = OsName();
  h.cpu = CpuBrand();
  h.gpus = GpuNames();
  MEMORYSTATUSEX m;
  m.dwLength = sizeof m;
  if (GlobalMemoryStatusEx(&m)) {
    const double gb = static_cast<double>(m.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    h.ram = Fmt("%d GB", static_cast<int>(gb + 0.5));
  }
  CpuTopology(&h.cpuCores, &h.cpuThreads);
  h.cpuBaseMhz = CpuBaseMhz();
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameW(name, &n)) h.machineName = U(name);
  if (smbios_) {
    std::vector<uint8_t> raw;
    std::string err;
    mgp::SmbiosInfo si;
    if (smbios_->ReadRaw(raw, err) && mgp::ParseRawSmbios(raw.data(), raw.size(), si)) {
      h.biosVendor = si.biosVendor;
      h.biosVersion = si.biosVersion;
      h.sysVendor = si.sysManufacturer;
      h.sysProduct = si.sysProduct;
      h.boardName = si.boardProduct;
      h.memory = MemModulesFromSmbios(si);
    }
  }
  h.elevated = IsElevated();
  cached_ = h;
  have_ = true;
  return h;
}

}  // namespace hw
}  // namespace bc
