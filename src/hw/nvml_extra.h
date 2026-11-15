// NVIDIA NVML extras (read only): clocks, memory, throttle reasons and the core/memory clock offsets that are in force. The offsets are
// changed through the vendor's service, never from here.
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
