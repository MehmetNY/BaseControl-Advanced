// Coexistence with the OEM Control Center. Evidence (device inventory): the Windows service "GCUBridge" (Auto start,
// C:\Program Files\OEM\Control Center\UniwillService\GCUBridge.exe) is the OEM front; its worker process is
// MyControlCenter\GCUService.exe, which owns the EC/profile files while it runs. BaseControl never edits those files:
// instead the user can "take over", which stops + disables the OEM services (and only services whose binary lives in
// %ProgramFiles%\OEM\Control Center\) and can be reverted at any time. The original start types are remembered.
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class OemServices : public IOemControl {
 public:
  explicit OemServices(const std::string& stateDir);
  OemSvcState Probe() override;
  bool StopAndDisable(std::string* why) override;
  bool EnableAndStart(std::string* why) override;

 private:
  struct Svc {
    std::string name;
    std::wstring wname;
    DWORD startType = SERVICE_DEMAND_START;
    DWORD state = SERVICE_STOPPED;
  };
  std::vector<Svc> Discover(bool full);
  bool ProcessRunning(std::vector<DWORD>* pids);
  std::wstring OemRoot() const;
  bool IsOemImage(const std::wstring& path) const;
  std::wstring ServiceImagePath(SC_HANDLE svc) const;
  std::wstring FindGcuServiceExe() const;

  std::string stateFile_;
  std::vector<std::string> knownNames_;
  bool enumerated_ = false;
  ULONGLONG cacheAt_ = 0;
  OemSvcState cache_;
};

}  // namespace hw
}  // namespace bc
