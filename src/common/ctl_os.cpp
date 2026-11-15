// Controller: what Windows reports (processor load and frequency, memory, power source and battery charge) plus the best temperature
// readings. Read only; no driver involved.
#include "controller.h"
#include "util.h"

namespace bc {

// ------------------------------------------------------------------------------------------------ OS polling (usage, frequency, power source)
void Controller::PollOs(bool full) {
  KV k;
  if (hw_.power) {
    SysStatus s = hw_.power->Status();
    acNow_ = s.ac;
    acKnown_ = true;
    battPctNow_ = s.battPct;
    k.SetB("power.ac", s.ac);
    if (s.battPct >= 0) k.SetI("power.batt.pct", s.battPct);
    if (s.battSecs >= 0) k.SetI("power.batt.secs", s.battSecs);
    k.SetB("power.charging", s.charging);
    k.SetF("os.cpu.pct", s.cpuPct, 0);
    k.SetF("os.mem.pct", s.memPct, 0);
    k.SetF("os.mem.used", s.memUsedMb, 0);
    k.SetF("os.mem.total", s.memTotalMb, 0);
    if (!s.coreMhz.empty()) {
      std::string t;
      long sum = 0;
      for (size_t i = 0; i < s.coreMhz.size(); i++) {
        t += Fmt("%s%d", i ? "," : "", s.coreMhz[i]);
        sum += s.coreMhz[i];
      }
      k.Set("os.cores.mhz", t);
      k.SetI("os.cpu.mhz", static_cast<long long>(sum / static_cast<long>(s.coreMhz.size())));
    }
    if (s.cpuMaxMhz) k.SetI("os.cpu.maxmhz", s.cpuMaxMhz);
  }
  // best available CPU temperature
  KV merged = Snapshot();
  long long t = merged.Int("cpu.temp.ec", 0);
  if (t > 0) k.SetI("cpu.temp", t);
  long long gt = merged.Int("gpu.temp", merged.Int("gpu.temp.ec", 0));
  if (gt > 0) k.SetI("gpu.temp.best", gt);
  (void)full;
  std::lock_guard<std::mutex> g(snapMu_);
  sOs_ = k;
}

}  // namespace bc
