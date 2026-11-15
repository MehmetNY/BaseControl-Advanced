// Controller: the brain of Control Center Advanced. It owns the hardware access policy:
//  * probes what the machine can do (capabilities + plausibility checks),
//  * polls the embedded controller (read only), the vendor service's status, the GPU and Windows into a snapshot (KV) that the UI
//    renders (live sliders follow it, so a change made by another program shows up at once),
//  * executes validated requests (typed settings only, never raw addresses): profile values, fans, battery and keyboard lighting go to
//    the vendor's own service, and every change is confirmed by what the service and the EC report afterwards,
//  * keeps the config, the application profiles, and re-applies the extended GPU power after resume.
// All hardware access happens on one worker thread (or the calling thread in manual/test mode).
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../mgp/core/power_profile_engine.h"
#include "fan.h"
#include "hw_iface.h"
#include "kv.h"
#include "settings_reg.h"
#include "writeguard.h"

namespace bc {

struct Request {
  std::string op;     // "set" | "cmd"
  std::string key;    // setting key or command name
  std::string value;  // setting value
  KV args;            // command arguments / consent flags
};

struct Result {
  bool ok = false;
  // ok | partial | unknown | unsupported | range | gate.readonly | gate.mode | gate.consent | gate.thermal | gate.oem | busy | failed.hw |
  // failed.verify | failed.service | reverted | needs.service | needs.approval | internal
  std::string code = "internal";
  std::string msg;    // technical English detail (tooltips / log)
  KV data;
};

class Controller {
 public:
  struct Options {
    std::string dataDir;        // "" = no persistence (tests)
    bool manual = false;        // no worker thread: tests call PollOnce()/ExecuteSync()
    std::string mode = "local"; // "local" | "sim" (shown in the UI)
    std::string version = "2.0.0";
  };

  Controller(const Hardware& hw, const Options& opt);
  ~Controller();

  void Start();   // loads config, probes, starts the worker (unless manual)
  void Stop();

  KV Snapshot() const;                          // thread-safe copy of the latest state
  Result Execute(const Request& r);             // queued onto the worker, blocks until done (manual mode: runs inline)
  uint64_t Submit(const Request& r);            // async; poll with TakeResult
  bool TakeResult(uint64_t id, Result* out);
  void SetInterest(bool visible);               // UI visible -> fast polling
  void Notify(const std::string& event);        // "resume" | "suspend" | "display_on" | "display_off" | "ac_change"
  // Replaces the battery history (oldest first, same encoding as batt.hist). For the simulator's screenshots and tests only: there is no
  // request that reaches it, so no client can rewrite the history.
  void SeedBatteryHistory(const std::vector<int>& samples);

  // ---- test / manual helpers ----
  void PollOnce(bool full = true);              // one poll + housekeeping pass on the calling thread
  Result ExecuteSync(const Request& r) { return Run(r); }
  GuardedEc& ec() { return *gec_; }
  const KV& config() const { return cfg_; }

 private:
  // ---- lifecycle / plumbing (controller.cpp)
  void WorkerMain();
  Result Run(const Request& r);
  void Probe();
  void PollOs(bool full);
  void PublishStatic();
  void L(mgp::LogLevel lv, const std::string& m);
  Result Ok(const std::string& msg = "", const KV& data = KV());
  Result Fail(const std::string& code, const std::string& msg);
  bool CapOk(const std::string& cap) const;
  void SetCap(const std::string& cap, const std::string& state, const std::string& why = "");
  std::string CapState(const std::string& cap) const;
  void LoadConfig();
  void SaveConfig();
  void LoadDesired();
  void SaveDesired();
  Result Gate(const SettingDef& d);
  Result DoSet(const std::string& key, const std::string& raw, const KV& args);
  Result DoCmd(const std::string& name, const KV& args);
  Result DoCfg(const std::string& key, const std::string& raw);
  void Housekeeping();
  void SyncSnapshot();

  // ---- embedded controller, read only (ctl_ec.cpp): the live values every slider follows. Nothing here writes.
  bool EcR(uint16_t a, uint8_t* v);
  void ProbeEc();
  void PollEc(bool full);

  // ---- the vendor's service (ctl_oem.cpp)
  void ProbeOem();
  void PollOem();
  Result OemSend(const oem::Msg& m);                                   // needs.service when there is no session
  // Sends one SET_OPERATING_MODE_DETAIL, waits for the service to report the values back and checks the EC where it can. `settingKey`
  // is the registry key being set (messages and hints), `ecKey` the live snapshot key to compare ("" = nothing to read back).
  Result OemDetail(const std::vector<oem::msg::Detail>& kv, const std::string& settingKey);
  Result SetPerf(const std::string& key, int value);
  Result SwitchMode(int mode);                                         // 0 office, 1 gaming, 2 turbo, 3 custom
  Result SetFanBoost(bool on);
  Result SetFanMode(int mode);                                         // 0 auto, 1 boost, 2 custom curve (composite: boost + custom flag)
  Result SetFanCurve(bool cpu, const std::string& text);
  Result FanRestore();
  Result DetailRestore();
  Result SetBattMode(int mode);
  Result SetSwitchPref(const std::string& key, bool on);
  Result SetKbd(const std::string& key, const std::string& raw);
  Result SetKbdTimer(int seconds);                                      // light.kbd.timer: the light goes out after this many seconds idle (0 = never)
  Result KbdSave();                                                     // kb.save: "Save to BIOS" (the keyboard's own memory)
  bool WaitOem(const std::function<bool()>& done, uint32_t timeoutMs, uint32_t refreshAfterMs = 0);
  std::string OemValue(const std::string& statusField) const;           // "" when the service has not reported it
  int OemOperatingMode() const;                                         // -1 unknown

  // ---- GPU (ctl_gpu.cpp)
  void ProbeGpu();
  void PollGpu();
  Result SetCtgp(int watts, const KV& args);

  // ---- storage inventory and drive health, read-only (ctl_storage.cpp)
  void PollStorage();

  // ---- misc (ctl_misc.cpp)
  void ProbeMisc();
  void PollMisc();
  Result SetDisplay(const std::string& key, int value);
  ColorSettings StoredColor() const;
  void ColorEvent(const std::string& ev);
  void ScheduleColorReapply(uint64_t afterMs, int times);
  void ColorTick();
  Result SetDevice(const std::string& key, bool on);
  Result ApplyProfile(const std::string& name, const KV& args);
  void AppProfileTick();
  // Application profiles: slots cfg.app.<i>.exe / .profile / .name (i < 32, lower slot = higher priority). The commands keep them valid.
  Result AppSet(const KV& args);      // app.set: i (slot; empty = new entry), exe, profile, name (optional display name)
  Result AppDelete(const KV& args);   // app.delete: i
  Result AppMove(const KV& args);     // app.move: i, dir (-1 = up, 1 = down)
  void BatteryHistoryTick();  // one charge sample a minute for the last 8 hours (published as batt.hist)
  void BatteryHistoryText();  // rebuilds battHistText_ from battHist_ (caller holds snapMu_)
  void Reapply(const char* why);

  Hardware hw_;
  Options opt_;
  struct RingLog;
  std::unique_ptr<RingLog> ring_;
  std::unique_ptr<GuardedEc> gec_;
  std::unique_ptr<mgp::PowerProfileEngine> engine_;

  // ---- state (touched on the worker only, except under snapMu_)
  mutable std::mutex snapMu_;
  KV sStatic_, sCaps_, sEc_, sOem_, sGpu_, sOs_, sMisc_, sStor_, sStatus_;
  std::vector<std::string> log_;  // ring tail for diagnostics
  KV cfg_;
  KV desired_;                        // last confirmed/applied values worth re-applying (the extended GPU power only)
  std::string lastError_;

  // ---- worker plumbing
  std::thread th_;
  std::mutex qMu_;
  std::condition_variable qCv_;
  struct Job { uint64_t id; Request req; };
  std::deque<Job> queue_;
  std::map<uint64_t, Result> done_;
  std::condition_variable doneCv_;
  uint64_t nextId_ = 1;
  std::atomic<bool> stop_{false};
  std::atomic<bool> visible_{true};
  bool started_ = false;
  uint64_t lastFast_ = 0, lastSlow_ = 0, lastOs_ = 0, lastHk_ = 0, lastStorage_ = 0, lastOem_ = 0;
  uint64_t oemSeen_ = 0;                  // link version PollOem last merged
  bool storageKnown_ = false;             // PollStorage has read the drives at least once
  std::vector<std::string> pendingEvents_;
  uint64_t reapplyAt_ = 0;
  std::string reapplyWhy_;
  int reapplyTries_ = 0;
  uint64_t reapplyWindowStart_ = 0;

  // ---- probed facts
  bool ecConnected_ = false;
  std::string devClass_ = "unknown";  // verified | family | unknown
  int projectId_ = -1;
  mgp::LiveState lastLive_;
  bool engineReady_ = false;

  std::string appRunning_, profileBeforeApp_;
  bool acNow_ = true, acKnown_ = false;   // power source as last read by PollOs
  int battPctNow_ = -1;                   // charge in percent as last read by PollOs (-1 = no battery / unknown)
  // Battery history: a sample is the charge in percent, plus 200 while the charger was connected; -1 marks a minute without a reading
  // (the machine slept). The newest sample is last. Kept in memory only: it starts again when the service starts.
  static constexpr uint64_t kBattStepMs = 60000;
  static constexpr size_t kBattMax = 480;   // 8 hours
  std::deque<int> battHist_;
  uint64_t battHistAt_ = 0;                 // clock time of the newest sample (0 = none yet)
  std::string battHistText_;                // battHist_ as "a,b,c" for the snapshot (rebuilt when a sample is added)
  uint64_t lastAppScan_ = 0;
  bool appRecheck_ = false;               // the mappings were edited: look again even if the same program is still the match
  bool fpsOk_ = false;   // a frame-rate source is present (set by ProbeMisc)
  uint64_t colorReapplyAt_ = 0;   // when to push the stored colours to the panel again (0 = nothing planned)
  int colorReapplyLeft_ = 0;
};

}  // namespace bc
