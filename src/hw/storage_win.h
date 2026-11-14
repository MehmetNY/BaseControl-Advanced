// Read-only drive inventory and health for the Device info page (Windows). Opens "\\.\PhysicalDriveN" without write access and only
// issues query IOCTLs: device descriptor (model, bus), seek-penalty flag, drive geometry (size), the temperature property and, for NVMe,
// the SMART / health log page. Serial numbers are never read. Health queries may need administrator rights (the service has them);
// without them the inventory is still returned and the health values stay "unknown".
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class StorageWin : public IStorage {
 public:
  bool Read(std::vector<StorageDrive>* out, bool health) override;
};

}  // namespace hw
}  // namespace bc
