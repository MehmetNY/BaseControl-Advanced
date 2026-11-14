// Controller: drive inventory and health for the Device info page. Read-only: nothing here can write to a drive or the EC.
#include "controller.h"
#include "hostdata.h"
#include "util.h"

namespace bc {

void Controller::PollStorage() {
  std::vector<StorageDrive> drives;
  if (!hw_.storage || !hw_.storage->Read(&drives, true)) {
    // Keep what was read before (a failed query must not blank the page); the very first failure publishes nothing at all, which the page
    // shows as "not available".
    storageKnown_ = true;
    return;
  }
  KV k;
  int n = 0;
  for (const StorageDrive& d : drives) {
    if (n >= 8) break;
    const std::string p = Fmt("storage.%d.", n++);
    k.Set(p + "model", CleanText(d.model, 96));
    if (!d.bus.empty()) k.Set(p + "bus", CleanText(d.bus, 12));
    k.SetB(p + "ssd", d.ssd);
    if (d.sizeBytes) k.Set(p + "size", std::to_string(d.sizeBytes));
    if (d.tempC >= 0) k.SetI(p + "temp", d.tempC);
    if (d.powerCycles >= 0) k.SetI(p + "cycles", d.powerCycles);
    if (d.powerOnHours >= 0) k.SetI(p + "hours", d.powerOnHours);
    if (d.wearPct >= 0) k.SetI(p + "wear", d.wearPct);
    if (d.sparePct >= 0) k.SetI(p + "spare", d.sparePct);
    if (d.critWarn >= 0) k.SetI(p + "warn", d.critWarn);
    if (d.writtenGb >= 0) k.SetI(p + "written", d.writtenGb);
  }
  k.SetI("storage.n", n);
  storageKnown_ = true;
  std::lock_guard<std::mutex> g(snapMu_);
  sStor_ = k;
}

}  // namespace bc
