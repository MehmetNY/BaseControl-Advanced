// MSR access through PawnIO (https://pawnio.eu, signed driver + signed "IntelMSR" module). PawnIO is NOT bundled:
// the user installs the official PawnIO setup and drops the signed module (IntelMSR.bin) next to the program data.
// Why PawnIO: the module itself enforces a MSR allow-list in the kernel (no arbitrary MSR access), and the driver
// refuses unsigned modules - both are stronger guarantees than a home-grown ring-0 driver could give.
//
// PawnIOLib.dll is loaded dynamically from trusted locations only, so the program starts (with the MSR features
// reported as "absent") on machines without PawnIO.
#pragma once
#include <string>
#include <vector>

#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class PawnIoMsr : public IMsr {
 public:
  PawnIoMsr();
  ~PawnIoMsr() override;
  PawnIoMsr(const PawnIoMsr&) = delete;
  PawnIoMsr& operator=(const PawnIoMsr&) = delete;

  bool Open(std::string* why) override;
  bool Read(uint32_t msr, uint64_t* v) override;
  bool Write(uint32_t msr, uint64_t v) override;
  std::string Name() const override { return name_; }
  int CpuCount() override { return cpus_; }
  bool ReadCpu(int cpu, uint32_t msr, uint64_t* v) override;

  // Where the module file is looked for (documentation / probe report).
  static std::vector<std::wstring> ModuleSearchPaths();

 private:
  bool Exec(const char* fn, const ULONG64* in, SIZE_T inN, ULONG64* out, SIZE_T outN);
  bool WithCpu(int cpu, bool (*fn)(PawnIoMsr*, void*), void* arg);
  void Close();

  struct Api;
  Api* api_ = nullptr;
  HMODULE lib_ = nullptr;
  HANDLE h_ = nullptr;
  bool open_ = false;
  std::string name_ = "PawnIO (not open)";
  std::string failWhy_;
  int cpus_ = 0;
  CRITICAL_SECTION cs_;
};

}  // namespace hw
}  // namespace bc
