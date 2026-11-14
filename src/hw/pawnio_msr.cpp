#include "pawnio_msr.h"

#include "../common/fsutil.h"
#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

typedef HRESULT(STDAPICALLTYPE* PFN_open)(PHANDLE);
typedef HRESULT(STDAPICALLTYPE* PFN_close)(HANDLE);
typedef HRESULT(STDAPICALLTYPE* PFN_load)(HANDLE, const UCHAR*, SIZE_T);
typedef HRESULT(STDAPICALLTYPE* PFN_exec)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT(STDAPICALLTYPE* PFN_ver)(PULONG);

constexpr uint32_t kMsrRaplUnit = 0x606;

// MSRs that are per logical processor / core: a write must reach every CPU, a read is taken from CPU 0.
bool IsPerCoreMsr(uint32_t msr) { return msr == 0x1A4 || msr == 0x1FC; }

// Our own write allow-list (the module has a broader one): only what the controller really needs.
bool WriteAllowed(uint32_t msr) { return msr == 0x610 || msr == 0x65C || msr == 0x601 || msr == 0x150 || msr == 0x1A4 || msr == 0x1FC; }

bool Trusted(const std::wstring& dir) {
  std::wstring d = dir;
  for (auto& c : d) c = static_cast<wchar_t>(towlower(c));
  auto starts = [&](const std::wstring& root) {
    std::wstring r = root;
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return !r.empty() && d.compare(0, r.size(), r) == 0;
  };
  wchar_t env[MAX_PATH];
  if (GetEnvironmentVariableW(L"ProgramFiles", env, MAX_PATH) && starts(env)) return true;
  if (GetEnvironmentVariableW(L"ProgramFiles(x86)", env, MAX_PATH) && starts(env)) return true;
  if (GetEnvironmentVariableW(L"ProgramW6432", env, MAX_PATH) && starts(env)) return true;
  if (GetEnvironmentVariableW(L"ProgramData", env, MAX_PATH) && starts(std::wstring(env) + L"\\BaseControl")) return true;
  return false;
}

std::wstring PawnIoInstallDirFromRegistry() {
  // The setup program registers an uninstall entry; read its InstallLocation (only used when under Program Files).
  const wchar_t* roots[] = {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"};
  for (const wchar_t* root : roots) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
    for (DWORD i = 0;; ++i) {
      wchar_t name[256];
      DWORD n = 256;
      if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      HKEY sub = nullptr;
      if (RegOpenKeyExW(k, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
      wchar_t disp[256] = {0}, loc[MAX_PATH] = {0};
      DWORD sz = sizeof disp, type = 0;
      bool hit = RegQueryValueExW(sub, L"DisplayName", nullptr, &type, reinterpret_cast<BYTE*>(disp), &sz) == ERROR_SUCCESS && type == REG_SZ &&
                 _wcsicmp(disp, L"PawnIO") == 0;
      if (hit) {
        sz = sizeof loc;
        if (RegQueryValueExW(sub, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(loc), &sz) != ERROR_SUCCESS || type != REG_SZ) loc[0] = 0;
      }
      RegCloseKey(sub);
      if (hit && loc[0]) {
        RegCloseKey(k);
        std::wstring s = loc;
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
      }
    }
    RegCloseKey(k);
  }
  return L"";
}

std::string HrText(HRESULT hr) {
  if ((hr & 0xFFFF0000) == 0x80070000) return Win32Text(static_cast<DWORD>(hr & 0xFFFF));
  return Fmt("HRESULT 0x%08lX", static_cast<unsigned long>(hr));
}

struct CpuCall {
  uint32_t msr;
  uint64_t* v;
  bool write;
  bool ok;
};

}  // namespace

struct PawnIoMsr::Api {
  PFN_open open = nullptr;
  PFN_close close = nullptr;
  PFN_load load = nullptr;
  PFN_exec exec = nullptr;
  PFN_ver version = nullptr;
};

PawnIoMsr::PawnIoMsr() { InitializeCriticalSection(&cs_); }

PawnIoMsr::~PawnIoMsr() {
  Close();
  delete api_;
  DeleteCriticalSection(&cs_);
}

void PawnIoMsr::Close() {
  if (h_ && api_ && api_->close) api_->close(h_);
  h_ = nullptr;
  open_ = false;
  if (lib_) {
    FreeLibrary(lib_);
    lib_ = nullptr;
  }
}

std::vector<std::wstring> PawnIoMsr::ModuleSearchPaths() {
  std::vector<std::wstring> v;
  v.push_back(W(ProgramDataDir()) + L"\\modules\\IntelMSR.bin");
  const std::wstring exe = ExeDir();
  if (Trusted(exe)) {
    v.push_back(exe + L"\\modules\\IntelMSR.bin");
    v.push_back(exe + L"\\IntelMSR.bin");
  }
  const std::wstring pio = PawnIoInstallDirFromRegistry();
  const std::wstring pf = ProgramFiles();
  for (const std::wstring& base : {pio, pf + L"\\PawnIO"}) {
    if (base.empty() || !Trusted(base)) continue;
    v.push_back(base + L"\\modules\\IntelMSR.bin");
    v.push_back(base + L"\\IntelMSR.bin");
  }
  return v;
}

bool PawnIoMsr::Open(std::string* why) {
  if (open_) return true;
  auto fail = [&](const std::string& m) {
    failWhy_ = m;
    if (why) *why = m;
    name_ = "PawnIO (unavailable)";
    return false;
  };
  EnterCriticalSection(&cs_);
  struct Leave { CRITICAL_SECTION* c; ~Leave() { LeaveCriticalSection(c); } } leave{&cs_};

  if (!lib_) {
    std::vector<std::wstring> libs;
    const std::wstring pio = PawnIoInstallDirFromRegistry();
    if (!pio.empty() && Trusted(pio)) libs.push_back(pio + L"\\PawnIOLib.dll");
    libs.push_back(ProgramFiles() + L"\\PawnIO\\PawnIOLib.dll");
    const std::wstring exe = ExeDir();
    if (Trusted(exe)) libs.push_back(exe + L"\\PawnIOLib.dll");
    for (const std::wstring& p : libs) {
      if (!FileExists(p)) continue;
      lib_ = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (lib_) break;
    }
    if (!lib_) return fail("PawnIO is not installed (PawnIOLib.dll not found). Install it from pawnio.eu.");
  }
  if (!api_) api_ = new Api();
  api_->open = reinterpret_cast<PFN_open>(reinterpret_cast<void*>(GetProcAddress(lib_, "pawnio_open")));
  api_->close = reinterpret_cast<PFN_close>(reinterpret_cast<void*>(GetProcAddress(lib_, "pawnio_close")));
  api_->load = reinterpret_cast<PFN_load>(reinterpret_cast<void*>(GetProcAddress(lib_, "pawnio_load")));
  api_->exec = reinterpret_cast<PFN_exec>(reinterpret_cast<void*>(GetProcAddress(lib_, "pawnio_execute")));
  api_->version = reinterpret_cast<PFN_ver>(reinterpret_cast<void*>(GetProcAddress(lib_, "pawnio_version")));
  if (!api_->open || !api_->close || !api_->load || !api_->exec) {
    Close();
    return fail("PawnIOLib.dll is missing required exports");
  }
  HANDLE h = nullptr;
  HRESULT hr = api_->open(&h);
  if (FAILED(hr) || !h) {
    Close();
    return fail("cannot open the PawnIO driver (" + HrText(hr) + "). Is the PawnIO service installed and are we running as administrator/SYSTEM?");
  }
  h_ = h;

  // signed module blob
  std::string blob;
  std::wstring used;
  for (const std::wstring& p : ModuleSearchPaths()) {
    std::string b;
    if (fs::ReadAll(U(p), &b, 4u << 20) && !b.empty()) {
      blob = std::move(b);
      used = p;
      break;
    }
  }
  if (blob.empty()) {
    Close();
    return fail("IntelMSR.bin (signed PawnIO module) not found. Put it into " + ProgramDataDir() + "\\modules\\ (see docs/SETUP.md).");
  }
  hr = api_->load(h_, reinterpret_cast<const UCHAR*>(blob.data()), blob.size());
  if (FAILED(hr)) {
    Close();
    return fail("PawnIO refused the IntelMSR module (" + HrText(hr) + "): unsigned/corrupt blob or unsupported CPU");
  }
  // sanity: RAPL_POWER_UNIT must be readable and non-zero on every supported Intel CPU
  uint64_t unit = 0;
  open_ = true;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  cpus_ = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  if (cpus_ <= 0) cpus_ = static_cast<int>(si.dwNumberOfProcessors);
  if (!Read(kMsrRaplUnit, &unit) || unit == 0) {
    open_ = false;
    Close();
    return fail("the PawnIO module loaded but MSR 0x606 is not readable (not an Intel CPU?)");
  }
  ULONG ver = 0;
  if (api_->version) api_->version(&ver);
  name_ = Fmt("PawnIO %lu.%lu.%lu / IntelMSR", (ver >> 16) & 0xFF, (ver >> 8) & 0xFF, ver & 0xFF);
  return true;
}

bool PawnIoMsr::Exec(const char* fn, const ULONG64* in, SIZE_T inN, ULONG64* out, SIZE_T outN) {
  if (!open_ || !api_ || !h_) return false;
  SIZE_T ret = 0;
  const HRESULT hr = api_->exec(h_, fn, in, inN, out, outN, &ret);
  return SUCCEEDED(hr);
}

// Runs fn on a specific logical processor by pinning the calling thread (restores the previous affinity).
bool PawnIoMsr::WithCpu(int cpu, bool (*fn)(PawnIoMsr*, void*), void* arg) {
  const WORD groups = GetActiveProcessorGroupCount();
  int base = 0;
  for (WORD g = 0; g < groups; ++g) {
    const int n = static_cast<int>(GetActiveProcessorCount(g));
    if (cpu < base + n) {
      GROUP_AFFINITY ga = {};
      ga.Group = g;
      ga.Mask = static_cast<KAFFINITY>(1) << (cpu - base);
      GROUP_AFFINITY prev = {};
      if (!SetThreadGroupAffinity(GetCurrentThread(), &ga, &prev)) return false;
      const bool ok = fn(this, arg);
      SetThreadGroupAffinity(GetCurrentThread(), &prev, nullptr);
      return ok;
    }
    base += n;
  }
  return false;
}

bool PawnIoMsr::Read(uint32_t msr, uint64_t* v) {
  if (!open_ || !v) return false;
  EnterCriticalSection(&cs_);
  CpuCall c{msr, v, false, false};
  auto fn = [](PawnIoMsr* self, void* a) {
    CpuCall* cc = static_cast<CpuCall*>(a);
    const ULONG64 in = cc->msr;
    ULONG64 out = 0;
    cc->ok = self->Exec("ioctl_read_msr", &in, 1, &out, 1);
    if (cc->ok) *cc->v = out;
    return cc->ok;
  };
  // package-scope registers are identical on every CPU; read on CPU 0 for a stable view
  const bool ok = WithCpu(0, fn, &c);
  LeaveCriticalSection(&cs_);
  return ok && c.ok;
}

bool PawnIoMsr::ReadCpu(int cpu, uint32_t msr, uint64_t* v) {
  if (!open_ || !v || cpu < 0 || cpu >= cpus_) return false;
  EnterCriticalSection(&cs_);
  CpuCall c{msr, v, false, false};
  auto fn = [](PawnIoMsr* self, void* a) {
    CpuCall* cc = static_cast<CpuCall*>(a);
    const ULONG64 in = cc->msr;
    ULONG64 out = 0;
    cc->ok = self->Exec("ioctl_read_msr", &in, 1, &out, 1);
    if (cc->ok) *cc->v = out;
    return cc->ok;
  };
  const bool ok = WithCpu(cpu, fn, &c);
  LeaveCriticalSection(&cs_);
  return ok && c.ok;
}

bool PawnIoMsr::Write(uint32_t msr, uint64_t v) {
  if (!open_ || !WriteAllowed(msr)) return false;
  EnterCriticalSection(&cs_);
  bool allOk = true;
  struct W2 { uint32_t msr; uint64_t v; } w{msr, v};
  auto fn = [](PawnIoMsr* self, void* a) {
    W2* ww = static_cast<W2*>(a);
    const ULONG64 in[2] = {ww->msr, ww->v};
    return self->Exec("ioctl_write_msr", in, 2, nullptr, 0);
  };
  if (IsPerCoreMsr(msr)) {
    for (int cpu = 0; cpu < cpus_; ++cpu) allOk = WithCpu(cpu, fn, &w) && allOk;
  } else {
    allOk = WithCpu(0, fn, &w);
  }
  LeaveCriticalSection(&cs_);
  return allOk;
}

}  // namespace hw
}  // namespace bc
