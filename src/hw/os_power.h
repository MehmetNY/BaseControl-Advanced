// Windows power plan settings for the CPU (boost mode, EPP, min/max state, core parking), live system status
// (battery, CPU load, memory, per-core clock hint) and the running-process list used by application profiles.
// Writes go to the ACTIVE power scheme for the CURRENT power source (AC or battery); the controller re-applies its
// desired values after an AC/battery change.
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class OsPower : public IOsPower {
 public:
  OsPower();
  bool GetCpuPolicy(CpuPolicy* out) override;
  bool SetCpuPolicy(const CpuPolicy& p, std::string* why) override;
  SysStatus Status() override;
  std::vector<std::string> RunningExes() override;

 private:
  bool ReadValue(const GUID& setting, bool ac, DWORD* v);
  bool WriteValue(const GUID& setting, bool ac, DWORD v, std::string* why);
  bool OnAc();

  uint64_t lastIdle_ = 0, lastKernel_ = 0, lastUser_ = 0;
  bool haveCpuBase_ = false;
};

}  // namespace hw
}  // namespace bc
