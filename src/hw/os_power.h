// Live system status (battery, CPU load, memory, per-core clock hint) and the running-process list used by application profiles.
// Read only: the Windows power plan is left alone.
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class OsPower : public IOsPower {
 public:
  OsPower();
  SysStatus Status() override;
  std::vector<std::string> RunningExes() override;

 private:
  uint64_t lastIdle_ = 0, lastKernel_ = 0, lastUser_ = 0;
  bool haveCpuBase_ = false;
};

}  // namespace hw
}  // namespace bc
