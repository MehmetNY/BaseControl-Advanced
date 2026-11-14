// NVIDIA NVML extras: clocks, memory, throttle reasons and the core/memory clock offsets (VF offset).
// nvml.dll is loaded dynamically from the system folder / NVIDIA's own folder (never from PATH or the working
// directory). Everything is optional: missing exports simply turn the matching feature off.
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class NvmlExtra : public IGpuExtra {
 public:
  NvmlExtra();
  ~NvmlExtra() override;
  NvmlExtra(const NvmlExtra&) = delete;
  NvmlExtra& operator=(const NvmlExtra&) = delete;

  bool Init(std::string* why) override;
  bool Read(GpuInfo* out) override;
  bool OffsetRange(int* coreMin, int* coreMax, int* memMin, int* memMax) override;
  bool SetCoreOffset(int mhz, std::string* why) override;
  bool SetMemOffset(int mhz, std::string* why) override;

 private:
  struct Fns;
  Fns* f_ = nullptr;
  HMODULE mod_ = nullptr;
  void* dev_ = nullptr;
  bool inited_ = false;
  CRITICAL_SECTION cs_;
};

}  // namespace hw
}  // namespace bc
