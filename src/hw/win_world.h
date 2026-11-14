// Composition root of the Windows hardware layer: builds the hardware objects for one of three process roles and
// exposes them as a bc::Hardware bundle for the Controller.
//   Service    SYSTEM service: EC, MSR, NVML, power plan, Wi-Fi/BT/camera, OEM services, HID lighting.
//   UiSession  the user's session: only what needs the desktop (display brightness/refresh/colour, touchpad).
//   Local      one elevated process doing everything (portable mode without the service).
#pragma once
#include <memory>

#include "../common/hw_iface.h"
#include "../mgp/win/nvml_win.h"
#include "../mgp/win/oem_platform_win.h"
#include "../mgp/win/smbios_win.h"
#include "acpi_ec.h"
#include "host_info.h"
#include "ite_hid.h"
#include "nvml_extra.h"
#include "oem_services.h"
#include "os_devices.h"
#include "os_display.h"
#include "os_power.h"
#include "pawnio_msr.h"
#include "winutil.h"

namespace bc {
namespace hw {

enum class WorldKind { Service, UiSession, Local };

// Named mutex shared with the MONSTER GPU POWER app (same name) so two programs never apply cTGP at the same time.
class NamedApplyLock : public ICrossProcessLock {
 public:
  NamedApplyLock();
  ~NamedApplyLock() override;
  bool Acquire(uint32_t timeoutMs) override;
  void Release() override;

 private:
  HANDLE h_ = nullptr;
};

class WinWorld {
 public:
  WinWorld(WorldKind kind, const std::string& dataDir, const std::string& logName);
  ~WinWorld();
  WinWorld(const WinWorld&) = delete;
  WinWorld& operator=(const WinWorld&) = delete;

  const Hardware& hw() const { return hw_; }
  WorldKind kind() const { return kind_; }
  const std::string& dataDir() const { return dataDir_; }
  mgp::ILog* log() { return &log_; }

 private:
  WorldKind kind_;
  std::string dataDir_;
  FileLog log_;
  SystemClock clock_;
  std::unique_ptr<AcpiEc> ec_;
  std::unique_ptr<mgp::win::NvmlTelemetry> nv_;
  mgp::win::WinSmbiosSource smbios_;
  std::unique_ptr<mgp::win::WinOemPlatform> oemFiles_;
  std::unique_ptr<PawnIoMsr> msr_;
  std::unique_ptr<NvmlExtra> gpu_;
  std::unique_ptr<OsPower> power_;
  std::unique_ptr<OsDisplay> display_;
  std::unique_ptr<OsDevices> devices_;
  std::unique_ptr<OemServices> oem_;
  std::unique_ptr<IteHid> light_;
  std::unique_ptr<HostInfoWin> host_;
  std::unique_ptr<NamedApplyLock> lock_;
  Hardware hw_;
};

}  // namespace hw
}  // namespace bc
