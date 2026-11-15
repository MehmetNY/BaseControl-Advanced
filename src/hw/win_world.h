// Composition root of the Windows hardware layer: builds the hardware objects of this one elevated process and exposes them as a
// bc::Hardware bundle for the Controller. There is no service of our own: the EC, the BIOS settings and the keyboard lighting are the
// vendor's service's business and are reached through the link (src/oem); what this process touches itself is read-only (EC registers,
// NVML, SMBIOS, storage health) except the one verified GPU power register pair and the Windows-level settings (display, devices).
#pragma once
#include <memory>

#include "../common/hw_iface.h"
#include "../mgp/win/nvml_win.h"
#include "../mgp/win/oem_platform_win.h"
#include "../mgp/win/smbios_win.h"
#include "acpi_ec.h"
#include "host_info.h"
#include "nvml_extra.h"
#include "oem_link_win.h"
#include "oem_services.h"
#include "os_devices.h"
#include "os_display.h"
#include "os_power.h"
#include "storage_win.h"
#include "winutil.h"

namespace bc {
namespace hw {

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
  WinWorld(const std::string& dataDir, const std::string& logName);
  ~WinWorld();
  WinWorld(const WinWorld&) = delete;
  WinWorld& operator=(const WinWorld&) = delete;

  const Hardware& hw() const { return hw_; }
  const std::string& dataDir() const { return dataDir_; }
  mgp::ILog* log() { return &log_; }

 private:
  std::string dataDir_;
  FileLog log_;
  SystemClock clock_;
  std::unique_ptr<AcpiEc> ec_;
  std::unique_ptr<mgp::win::NvmlTelemetry> nv_;
  mgp::win::WinSmbiosSource smbios_;
  std::unique_ptr<mgp::win::WinOemPlatform> oemFiles_;
  std::unique_ptr<NvmlExtra> gpu_;
  std::unique_ptr<OsPower> power_;
  std::unique_ptr<OsDisplay> display_;
  std::unique_ptr<OsDevices> devices_;
  std::unique_ptr<OemProbeWin> oemProbe_;
  std::unique_ptr<oem::IOemLink> link_;   // destroyed before the objects it may report on
  std::unique_ptr<HostInfoWin> host_;
  std::unique_ptr<StorageWin> storage_;
  std::unique_ptr<NamedApplyLock> lock_;
  Hardware hw_;
};

}  // namespace hw
}  // namespace bc
