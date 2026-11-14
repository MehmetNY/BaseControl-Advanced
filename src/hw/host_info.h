// Static facts about the machine for the Device Info page: OS, CPU, RAM, firmware identity (SMBIOS).
// Serial numbers / UUIDs / asset tags are never read (the SMBIOS parser skips them by design).
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class HostInfoWin : public IHostInfo {
 public:
  explicit HostInfoWin(mgp::ISmbiosSource* smbios) : smbios_(smbios) {}
  HostInfo Get() override;

 private:
  mgp::ISmbiosSource* smbios_;
  bool have_ = false;
  HostInfo cached_;
};

}  // namespace hw
}  // namespace bc
