// The vendor's Control Center (read-only look). Evidence (device inventory): the Windows service "GCUBridge" (Auto start,
// C:\Program Files\OEM\Control Center\UniwillService\GCUBridge.exe) is the vendor's front end; its worker process is
// MyControlCenter\GCUService.exe, which owns the EC and the BIOS settings. Control Center Advanced does not replace, stop or disable any
// of it - it talks to it (src/oem) - so this class only reports whether it is installed and running and, on request, starts services that
// are installed but stopped (an earlier build of this program could stop and disable them; the start types it recorded are restored).
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class OemProbeWin : public IOemProbe {
 public:
  explicit OemProbeWin(const std::string& stateDir);
  OemSvcState Probe() override;
  bool StartServices(std::string* why) override;

 private:
  struct Svc {
    std::string name;
    std::wstring wname;
    DWORD startType = SERVICE_DEMAND_START;
    DWORD state = SERVICE_STOPPED;
  };
  std::vector<Svc> Discover(bool full);
  bool ProcessRunning();
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
