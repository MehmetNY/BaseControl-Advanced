// Controller: the brain of Control Center Advanced. It owns the hardware access policy:
//  * probes what the machine can do (capabilities + evidence tier + plausibility checks),
//  * polls hardware into a snapshot (KV) that the UI renders (live sliders follow it),
//  * executes validated requests (typed settings only, never raw addresses) with read-back verification + rollback,
//  * keeps profiles / config, re-applies after resume, runs lighting effects and the fan safety watchdog.
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
#include "rapl.h"
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
  // ok | partial | unknown | unsupported | range | gate.experimental | gate.readonly | gate.consent | gate.thermal | gate.model |
  // gate.oem | busy | failed.hw | failed.verify | reverted | needs.activation | needs.approval | internal
  std::string code = "internal";
  std::string msg;    // technical English detail (tooltips / log)
  KV data;
};

class Controller {
 public:
  struct Options {
    std::string dataDir;        // "" = no persistence (tests)
    bool manual = false;        // no worker thread: tests call PollOnce()/ExecuteSync()
    std::string mode = "local"; // "service" | "local" | "sim" (shown in the UI)
    std::string version = "1.0.0-beta";
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
  void LoadProfiles();
  void SaveProfiles();
  void LoadDesired();
  void SaveDesired();
  Result Gate(const SettingDef& d);
  Result DoSet(const std::string& key, const std::string& raw, const KV& args);
  Result DoCmd(const std::string& name, const KV& args);
  Result DoCfg(const std::string& key, const std::string& raw);
  void Housekeeping();
  void SyncSnapshot();

  // ---- EC (ctl_ec.cpp)
  bool EcR(uint16_t a, uint8_t* v);
  bool EcW(uint16_t a, uint8_t v);
  bool EcRmw(uint16_t a, uint8_t mask, uint8_t bits);
  bool EcVerify(uint16_t a, uint8_t mask, uint8_t want, int tries = 6, uint32_t delayMs = 150);
  void ProbeEc();
  void PollEc(bool full);
  void PollEcTemps();   // only the two temperatures the fan watchdog judges
  bool EcActive();
  Result EnsureEcActive();
  Result EcBringUp();
  int Pl4Scale();
  Result SetEcPower(const std::string& key, int value, const KV& args);
  Result WriteEcTriple(int pl1, int pl2, int pl4);
  Result SetTcc(int celsius);
  Result SetFanMode(int mode);
  Result SetFanCurve(bool cpu, const std::string& text);
  Result SetFixedFan(bool cpu, int pct);
  Result SetFanRamp(int v);
  Result WriteFanTable(const fan::Curve* cpu, const fan::Curve* gpu);
  Result SetBattMode(int m);
  Result SetEcBit(uint16_t addr, uint8_t bit, bool on, const char* name);
  Result SetUsbVsBoot(bool usbPower, bool on);
  Result SetGpuDb(bool on);
  void FanWatchdog();

  // ---- GPU (ctl_gpu.cpp)
  void ProbeGpu();
  void PollGpu();
  Result SetCtgp(int watts, const KV& args);
  Result SetNvOffset(bool core, int mhz);

  // ---- CPU: MSR + Windows policy (ctl_cpu.cpp)
  void ProbeCpu();
  void PollMsr(bool full);
  Result SetMsr(const std::string& key, int value);
  Result SetOsCpu(const std::string& key, int value);
  bool MsrR(uint32_t m, uint64_t* v);

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
  Result SetLight(const std::string& key, const std::string& raw);
  void LightTick();
  void LightApply();
  std::string LightSuppressed() const;       // why the lights are forced off right now: "idle" | "sleep" | "display" | "" (not forced)
  int LightEffectiveBright(int zone) const;  // brightness level actually sent (battery limit applied)
  void LightPolicyTick();
  void LightEvent(const std::string& ev);
  Result LightIdleState(const KV& args);
  Result ApplyProfile(const std::string& name, const KV& args);
  Result SaveProfile(const std::string& name);
  Result DeleteProfile(const std::string& name);
  Result OemTakeover(bool takeover);
  void AppProfileTick();
  // Application profiles: slots cfg.app.<i>.exe / .profile / .name (i < 32, lower slot = higher priority). The commands keep them valid.
  Result AppSet(const KV& args);      // app.set: i (slot; empty = new entry), exe, profile, name (optional display name)
  Result AppDelete(const KV& args);   // app.delete: i
  Result AppMove(const KV& args);     // app.move: i, dir (-1 = up, 1 = down)
  void PowerSourceTick();   // switches the profile when the charger is plugged / unplugged (cfg.prof.auto)
  void BatteryHistoryTick();  // one charge sample a minute for the last 8 hours (published as batt.hist)
  void BatteryHistoryText();  // rebuilds battHistText_ from battHist_ (caller holds snapMu_)
  void Reapply(const char* why);
  KV CaptureProfileValues();
  std::vector<std::string> ProfileNames() const;

  Hardware hw_;
  Options opt_;
  struct RingLog;
  std::unique_ptr<RingLog> ring_;
  std::unique_ptr<GuardedEc> gec_;
  std::unique_ptr<mgp::PowerProfileEngine> engine_;

  // ---- state (touched on the worker only, except under snapMu_)
  mutable std::mutex snapMu_;
  KV sStatic_, sCaps_, sEc_, sGpu_, sMsr_, sOs_, sMisc_, sStor_, sStatus_;
  std::vector<std::string> log_;  // ring tail for diagnostics
  KV cfg_;
  std::map<std::string, KV> profiles_;
  KV desired_;                        // last confirmed/applied values worth re-applying
  std::string activeProfile_;
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
  uint64_t lastFast_ = 0, lastSlow_ = 0, lastOs_ = 0, lastHk_ = 0, lastLight_ = 0, lastStorage_ = 0, lastWatch_ = 0;
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
  bool msrOk_ = false;
  rapl::Units units_;
  uint64_t lastEnergy_[6] = {0, 0, 0, 0, 0, 0};
  uint64_t lastEnergyMs_ = 0;
  uint64_t lastPerfStat_ = 0, lastPerfMs_ = 0;
  uint64_t lastMsrFull_ = 0;
  mgp::LiveState lastLive_;
  bool engineReady_ = false;
  int fanOverTempTicks_ = 0;
  std::atomic<bool> fanWatch_{false};   // a custom or fixed fan setting is active: the watchdog needs fresh temperatures even while the window is hidden
  std::string fanGuard_;  // non-empty = the watchdog forced fan boost (cleared when the user picks a fan mode)

  // lighting state machine
  struct LightZoneState {
    bool present = false, power = true;
    uint8_t r = 255, g = 255, b = 255;
    int bright = 4, effect = 0, speed = 5;
  } light_[4];
  double lightPhase_ = 0;

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
  int acSeen_ = -1;                       // power source PowerSourceTick last reacted to (-1 = not yet looked)
  std::string profileOnAc_;               // profile that was active when the charger was unplugged
  uint64_t lastAppScan_ = 0;
  bool appRecheck_ = false;               // the mappings were edited: look again even if the same program is still the match
  uint8_t lastLightRgb_[4][3] = {};
  uint64_t lightSeq_ = 0;
  // lighting policy (see ctl_misc.cpp): temporary reasons for the lights to be off, none of them stored
  static constexpr uint64_t kLightIdleSilenceMs = 20000;   // the idle watcher repeats its report every few seconds; this long without one ends idle
  bool lightIdle_ = false, lightDisplayOff_ = false, lightSuspended_ = false;
  bool lightShownOff_[4] = {false, false, false, false};   // the zone was last sent the "off" sequence
  uint64_t lightIdleBeat_ = 0, lightReapplyAt_ = 0;
  int lightReapplyLeft_ = 0, lightAcSeen_ = -1;
  bool fpsOk_ = false;   // a frame-rate source is present (set by ProbeMisc)
  uint64_t colorReapplyAt_ = 0;   // when to push the stored colours to the panel again (0 = nothing planned)
  int colorReapplyLeft_ = 0;

  // consecutive mismatch counters for "keep" enforcement
  std::map<std::string, int> mismatch_;
};

}  // namespace bc
