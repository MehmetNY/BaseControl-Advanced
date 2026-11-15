#include "nvml_extra.h"

#include <cstring>

#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

typedef int nvmlReturn_t;  // 0 = NVML_SUCCESS
typedef void* nvmlDevice_t;
struct nvmlUtilization_t { unsigned int gpu, memory; };
struct nvmlMemory_t { unsigned long long total, free, used; };

typedef nvmlReturn_t (*F_void)();
typedef nvmlReturn_t (*F_count)(unsigned int*);
typedef nvmlReturn_t (*F_handle)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*F_name)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*F_uint)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*F_temp)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*F_clock)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*F_util)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (*F_mem)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (*F_pstate)(nvmlDevice_t, int*);
typedef nvmlReturn_t (*F_reasons)(nvmlDevice_t, unsigned long long*);
typedef nvmlReturn_t (*F_getoff)(nvmlDevice_t, int*);
typedef const char* (*F_err)(nvmlReturn_t);

template <typename T>
T Sym(HMODULE m, const char* a, const char* b = nullptr) {
  FARPROC p = GetProcAddress(m, a);
  if (!p && b) p = GetProcAddress(m, b);
  return reinterpret_cast<T>(reinterpret_cast<void*>(p));
}

HMODULE LoadNvml() {
  const std::wstring sys = SystemDirectory() + L"\\nvml.dll";
  if (FileExists(sys)) {
    if (HMODULE m = LoadLibraryExW(sys.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return m;
  }
  const std::wstring nv = ProgramFiles() + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
  if (FileExists(nv)) {
    if (HMODULE m = LoadLibraryExW(nv.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return m;
  }
  return nullptr;
}

}  // namespace

struct NvmlExtra::Fns {
  F_void init = nullptr, shutdown = nullptr;
  F_count count = nullptr;
  F_handle handle = nullptr;
  F_name name = nullptr;
  F_uint power = nullptr, limit = nullptr, fan = nullptr;
  F_temp temp = nullptr;
  F_clock clock = nullptr, maxClock = nullptr;
  F_util util = nullptr;
  F_mem mem = nullptr;
  F_pstate pstate = nullptr;
  F_reasons reasons = nullptr;
  F_getoff getGpc = nullptr, getMem = nullptr;
  F_err errStr = nullptr;
};

NvmlExtra::NvmlExtra() { InitializeCriticalSection(&cs_); }

NvmlExtra::~NvmlExtra() {
  if (inited_ && f_ && f_->shutdown) f_->shutdown();
  delete f_;
  DeleteCriticalSection(&cs_);
}

bool NvmlExtra::Init(std::string* why) {
  EnterCriticalSection(&cs_);
  struct Leave { CRITICAL_SECTION* c; ~Leave() { LeaveCriticalSection(c); } } leave{&cs_};
  if (inited_) return true;
  auto fail = [&](const std::string& m) {
    if (why) *why = m;
    return false;
  };
  if (!f_) f_ = new Fns();
  if (!mod_) {
    mod_ = LoadNvml();
    if (!mod_) return fail("nvml.dll not found (NVIDIA driver not installed)");
    f_->init = Sym<F_void>(mod_, "nvmlInit_v2", "nvmlInit");
    f_->shutdown = Sym<F_void>(mod_, "nvmlShutdown");
    f_->count = Sym<F_count>(mod_, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    f_->handle = Sym<F_handle>(mod_, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    f_->name = Sym<F_name>(mod_, "nvmlDeviceGetName");
    f_->power = Sym<F_uint>(mod_, "nvmlDeviceGetPowerUsage");
    f_->limit = Sym<F_uint>(mod_, "nvmlDeviceGetPowerManagementLimit");
    f_->fan = Sym<F_uint>(mod_, "nvmlDeviceGetFanSpeed");
    f_->temp = Sym<F_temp>(mod_, "nvmlDeviceGetTemperature");
    f_->clock = Sym<F_clock>(mod_, "nvmlDeviceGetClockInfo");
    f_->maxClock = Sym<F_clock>(mod_, "nvmlDeviceGetMaxClockInfo");
    f_->util = Sym<F_util>(mod_, "nvmlDeviceGetUtilizationRates");
    f_->mem = Sym<F_mem>(mod_, "nvmlDeviceGetMemoryInfo");
    f_->pstate = Sym<F_pstate>(mod_, "nvmlDeviceGetPerformanceState");
    f_->reasons = Sym<F_reasons>(mod_, "nvmlDeviceGetCurrentClocksEventReasons", "nvmlDeviceGetCurrentClocksThrottleReasons");
    f_->getGpc = Sym<F_getoff>(mod_, "nvmlDeviceGetGpcClkVfOffset");
    f_->getMem = Sym<F_getoff>(mod_, "nvmlDeviceGetMemClkVfOffset");
    f_->errStr = Sym<F_err>(mod_, "nvmlErrorString");
  }
  if (!f_->init || !f_->shutdown || !f_->count || !f_->handle || !f_->name) return fail("nvml.dll lacks the basic exports");
  nvmlReturn_t rc = f_->init();
  if (rc != 0) return fail(std::string("nvmlInit failed: ") + (f_->errStr ? f_->errStr(rc) : Fmt("%d", rc).c_str()));
  inited_ = true;
  unsigned cnt = 0;
  if (f_->count(&cnt) != 0 || cnt == 0) {
    f_->shutdown();
    inited_ = false;
    return fail("NVML reports no NVIDIA GPU");
  }
  nvmlDevice_t chosen = nullptr;
  for (unsigned i = 0; i < cnt; ++i) {
    nvmlDevice_t d = nullptr;
    if (f_->handle(i, &d) != 0 || !d) continue;
    char nm[96] = {0};
    f_->name(d, nm, sizeof nm);
    if (!chosen) chosen = d;
    if (Lower(nm).find("laptop") != std::string::npos) {
      chosen = d;
      break;
    }
  }
  if (!chosen) {
    f_->shutdown();
    inited_ = false;
    return fail("NVML device handle unavailable");
  }
  dev_ = chosen;
  return true;
}

bool NvmlExtra::Read(GpuInfo* o) {
  if (!o) return false;
  *o = GpuInfo();
  EnterCriticalSection(&cs_);
  struct Leave { CRITICAL_SECTION* c; ~Leave() { LeaveCriticalSection(c); } } leave{&cs_};
  if (!inited_ || !dev_) return false;
  char nm[96] = {0};
  if (f_->name(dev_, nm, sizeof nm) != 0) return false;
  o->name = nm;
  unsigned v = 0;
  if (f_->temp && f_->temp(dev_, 0, &v) == 0) o->tempC = static_cast<int>(v);
  if (f_->power && f_->power(dev_, &v) == 0) o->powerW = v / 1000.0;
  if (f_->limit && f_->limit(dev_, &v) == 0) o->limitW = v / 1000.0;
  if (f_->clock) {
    if (f_->clock(dev_, 0, &v) == 0) o->gfxMhz = static_cast<int>(v);
    if (f_->clock(dev_, 2, &v) == 0) o->memMhz = static_cast<int>(v);
  }
  if (f_->maxClock && f_->maxClock(dev_, 0, &v) == 0) o->maxGfxMhz = static_cast<int>(v);
  nvmlUtilization_t u = {0, 0};
  if (f_->util && f_->util(dev_, &u) == 0) {
    o->utilPct = static_cast<int>(u.gpu);
    o->memUtilPct = static_cast<int>(u.memory);
  }
  nvmlMemory_t m = {0, 0, 0};
  if (f_->mem && f_->mem(dev_, &m) == 0) {
    o->vramTotalMb = static_cast<double>(m.total) / (1024.0 * 1024.0);
    o->vramUsedMb = static_cast<double>(m.used) / (1024.0 * 1024.0);
  }
  if (f_->fan && f_->fan(dev_, &v) == 0) o->fanPct = static_cast<int>(v);
  int ps = 0;
  if (f_->pstate && f_->pstate(dev_, &ps) == 0) o->pstate = ps;
  unsigned long long reasons = 0;
  if (f_->reasons && f_->reasons(dev_, &reasons) == 0) o->throttleMask = reasons;
  int a = 0, b = 0;
  if (f_->getGpc && f_->getMem && f_->getGpc(dev_, &a) == 0 && f_->getMem(dev_, &b) == 0) {
    o->haveOffsets = true;
    o->coreOffsetMhz = a;
    o->memOffsetMhz = b;
  }
  o->ok = true;
  return true;
}

}  // namespace hw
}  // namespace bc
