#include "controller.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "../mgp/core/hardware_identity_guard.h"
#include "../mgp/core/smbios.h"
#include "fsutil.h"
#include "i18n_mark.h"
#include "hostdata.h"
#include "profile_text.h"
#include "registers.h"
#include "util.h"

namespace bc {

// ------------------------------------------------------------------------------------------------ ring log
struct Controller::RingLog : mgp::ILog {
  mgp::ILog* inner = nullptr;
  mgp::IClock* clock = nullptr;
  std::mutex mu;
  std::deque<std::string> lines;
  void Write(mgp::LogLevel lv, const std::string& m) override {
    if (inner) inner->Write(lv, m);
    const char* tag = lv == mgp::LogLevel::Error ? "ERR" : lv == mgp::LogLevel::Warn ? "WRN" : lv == mgp::LogLevel::Debug ? "DBG" : "INF";
    uint64_t t = clock ? clock->NowMs() : 0;
    std::lock_guard<std::mutex> g(mu);
    lines.push_back(Fmt("%02u:%02u:%02u.%03u %s %s", static_cast<unsigned>((t / 3600000) % 100), static_cast<unsigned>((t / 60000) % 60),
                        static_cast<unsigned>((t / 1000) % 60), static_cast<unsigned>(t % 1000), tag, m.c_str()));
    while (lines.size() > 120) lines.pop_front();
  }
  std::vector<std::string> Tail(size_t n) {
    std::lock_guard<std::mutex> g(mu);
    size_t from = lines.size() > n ? lines.size() - n : 0;
    return std::vector<std::string>(lines.begin() + from, lines.end());
  }
};

namespace {
// The only value this program keeps and puts back itself: the extended GPU power. Everything else lives in the vendor's own profiles.
bool IsProfileKey(const std::string& k) { return k == "gpu.ctgp"; }
}  // namespace

// ------------------------------------------------------------------------------------------------ lifecycle
Controller::Controller(const Hardware& hw, const Options& opt) : hw_(hw), opt_(opt), ring_(new RingLog) {
  ring_->inner = hw.log;
  ring_->clock = hw.clock;
  hw_.log = ring_.get();
  if (hw_.ec) gec_.reset(new GuardedEc(*hw_.ec, *hw_.log));
}

Controller::~Controller() { Stop(); }

void Controller::L(mgp::LogLevel lv, const std::string& m) {
  if (hw_.log) hw_.log->Write(lv, m);
}

void Controller::Start() {
  if (started_) return;
  started_ = true;
  LoadConfig();
  if (gec_) gec_->SetReadOnly(cfg_.Bool("cfg.readonly", false));
  if (gec_ && hw_.nv && hw_.smbios && hw_.clock) {
    engine_.reset(new mgp::PowerProfileEngine(*gec_, *hw_.nv, *hw_.smbios, *hw_.clock, *hw_.log));
  }
  LoadDesired();
  if (hw_.oemLink) hw_.oemLink->Start();   // the way to the vendor's service; it keeps trying until the service answers
  Probe();
  PollOnce(true);
  if (hw_.display && !StoredColor().Neutral()) ScheduleColorReapply(2500, 2);   // colours chosen earlier come back after sign-in
  if (desired_.Size() && cfg_.Bool("cfg.applyOnStart", true) && cfg_.Bool("cfg.keep", true)) {
    reapplyAt_ = (hw_.clock ? hw_.clock->NowMs() : 0) + 8000;  // let the system settle after boot / login
    reapplyWhy_ = "startup";
  }
  if (!opt_.manual) th_ = std::thread([this] { WorkerMain(); });
}

void Controller::Stop() {
  if (!started_) return;
  stop_ = true;
  qCv_.notify_all();
  if (th_.joinable()) th_.join();
  if (hw_.oemLink) hw_.oemLink->Stop();
  if (engine_) engine_->Shutdown();
  started_ = false;
}

void Controller::SetInterest(bool visible) {
  visible_ = visible;
  qCv_.notify_all();
}

void Controller::Notify(const std::string& event) {
  std::lock_guard<std::mutex> g(qMu_);
  pendingEvents_.push_back(event);
  qCv_.notify_all();
}

KV Controller::Snapshot() const {
  std::lock_guard<std::mutex> g(snapMu_);
  KV out;
  out.Merge(sStatic_); out.Merge(sCaps_); out.Merge(sEc_); out.Merge(sOem_); out.Merge(sGpu_); out.Merge(sOs_); out.Merge(sMisc_);
  out.Merge(sStor_);
  out.Merge(sStatus_);
  return out;
}

uint64_t Controller::Submit(const Request& r) {
  std::lock_guard<std::mutex> g(qMu_);
  uint64_t id = nextId_++;
  queue_.push_back(Job{id, r});
  qCv_.notify_all();
  return id;
}

bool Controller::TakeResult(uint64_t id, Result* out) {
  std::lock_guard<std::mutex> g(qMu_);
  auto it = done_.find(id);
  if (it == done_.end()) return false;
  if (out) *out = it->second;
  done_.erase(it);
  return true;
}

Result Controller::Execute(const Request& r) {
  if (opt_.manual || !started_) return Run(r);
  uint64_t id = Submit(r);
  std::unique_lock<std::mutex> lk(qMu_);
  doneCv_.wait_for(lk, std::chrono::seconds(60), [&] { return done_.count(id) != 0; });
  auto it = done_.find(id);
  if (it == done_.end()) {
    Result t;
    t.code = "busy";
    t.msg = "request timed out";
    return t;
  }
  Result res = it->second;
  done_.erase(it);
  return res;
}

void Controller::WorkerMain() {
  while (!stop_) {
    Job job;
    bool have = false;
    {
      std::unique_lock<std::mutex> lk(qMu_);
      if (queue_.empty()) qCv_.wait_for(lk, std::chrono::milliseconds(100));
      if (!queue_.empty()) {
        job = queue_.front();
        queue_.pop_front();
        have = true;
      }
    }
    if (have) {
      Result r = Run(job.req);
      std::lock_guard<std::mutex> lk(qMu_);
      done_[job.id] = r;
      doneCv_.notify_all();
      // keep the result map bounded: async callers that never collect would leak
      while (done_.size() > 256) done_.erase(done_.begin());
    }
    if (!stop_) PollOnce(false);
  }
}

void Controller::PollOnce(bool full) {
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  const bool vis = visible_;
  const uint64_t fastMs = vis ? 1000 : 5000;
  const uint64_t slowMs = vis ? 2000 : 10000;
  const uint64_t osMs = vis ? 1000 : 5000;
  bool did = false;
  // The order matters: what the EC and the graphics driver really run is read first, then merged with what the vendor's service reports.
  if (full || now - lastFast_ >= fastMs) {
    PollEc(false);
    PollGpu();
    PollOem();
    lastFast_ = now;
    did = true;
  }
  if (full || now - lastSlow_ >= slowMs) {
    PollEc(true);
    PollOem();
    lastSlow_ = now;
    did = true;
  }
  if (full || now - lastOs_ >= osMs) {
    PollOs(full);
    PollMisc();
    lastOs_ = now;
    did = true;
  }
  // Drive health: read while the window is open (every few seconds) and once at start; never while the UI is hidden, so a sleeping
  // drive is not woken for nobody.
  if (hw_.storage && (!storageKnown_ || ((full || vis) && now - lastStorage_ >= (full ? 3000u : 5000u)))) {
    PollStorage();
    lastStorage_ = now;
    did = true;
  }
  // The vendor's service also reports on its own (another program changed a value, a hotkey switched the profile): follow it between polls.
  if (!did && hw_.oemLink && hw_.oemLink->Version() != oemSeen_) {
    PollOem();
    did = true;
  }
  if (full || now - lastHk_ >= 1000) {
    Housekeeping();
    lastHk_ = now;
  }
  if (did) SyncSnapshot();
}

void Controller::SyncSnapshot() {
  KV st;
  st.Set("app.version", opt_.version);
  st.Set("svc.mode", opt_.mode);
  st.Set("ec.link", ecConnected_ ? "connected" : "absent");
  st.Set("dev.class", devClass_);
  st.Set("cfg.readonly", cfg_.Bool("cfg.readonly", false) ? "1" : "0");
  st.SetI("ec.writes", gec_ ? gec_->writes() : 0);
  st.SetI("ec.refused", gec_ ? gec_->refused() : 0);
  st.Set("app.active", appRunning_);               // the program whose profile is applied right now ("" = none)
  st.Set("app.before", profileBeforeApp_);         // the profile that comes back when it closes
  for (auto& p : cfg_.All()) st.Set(p.first, p.second);
  auto tail = ring_->Tail(30);
  st.Set("log.tail", Join(tail, "\n"));
  st.Set("last.error", lastError_);
  std::lock_guard<std::mutex> g(snapMu_);
  if (!battHistText_.empty()) {
    st.Set("batt.hist", battHistText_);
    st.SetI("batt.hist.step", static_cast<long long>(kBattStepMs / 1000));
  }
  sStatus_ = st;
}

// ------------------------------------------------------------------------------------------------ capabilities
void Controller::SetCap(const std::string& cap, const std::string& state, const std::string& why) {
  std::lock_guard<std::mutex> g(snapMu_);
  sCaps_.Set("cap." + cap, state);
  if (!why.empty()) sCaps_.Set("capwhy." + cap, why);
  else sCaps_.Erase("capwhy." + cap);
}

std::string Controller::CapState(const std::string& cap) const {
  std::lock_guard<std::mutex> g(snapMu_);
  return sCaps_.Str("cap." + cap, "absent");
}

bool Controller::CapOk(const std::string& cap) const { return cap.empty() || CapState(cap) == "ok"; }

void Controller::Probe() {
  {
    std::lock_guard<std::mutex> g(snapMu_);
    sCaps_.Clear();
  }
  PublishStatic();
  ProbeEc();
  ProbeGpu();
  ProbeOem();
  ProbeMisc();
  L(mgp::LogLevel::Info, Fmt("probe done: class=%s ec=%s", devClass_.c_str(), ecConnected_ ? "connected" : "absent"));
}

void Controller::PublishStatic() {
  KV s;
  s.Set("app.version", opt_.version);
  if (hw_.host) {
    HostInfo h = hw_.host->Get();
    s.Set("host.os", h.os);
    s.Set("host.cpu", h.cpu);
    s.Set("host.ram", h.ram);
    s.Set("host.name", h.machineName);
    s.Set("host.gpus", h.gpus);
    s.Set("host.bios", h.biosVersion);
    s.Set("host.vendor", h.sysVendor);
    s.Set("host.product", h.sysProduct);
    s.Set("host.board", h.boardName);
    s.SetB("host.elevated", h.elevated);
    if (h.cpuCores > 0) s.SetI("host.cores", h.cpuCores);
    if (h.cpuThreads > 0) s.SetI("host.threads", h.cpuThreads);
    if (h.cpuBaseMhz > 0) s.SetI("host.cpu.mhz", h.cpuBaseMhz);
    int n = 0;
    for (const MemModule& m : h.memory) {
      if (n >= 8) break;
      const std::string p = Fmt("host.mem.%d.", n++);
      s.SetI(p + "mb", m.sizeMb);
      if (!m.type.empty()) s.Set(p + "type", CleanText(m.type, 12));
      if (m.ratedMts > 0) s.SetI(p + "rated", m.ratedMts);
      if (m.runMts > 0) s.SetI(p + "run", m.runMts);
      if (!m.locator.empty()) s.Set(p + "slot", CleanText(m.locator, 40));
      if (!m.maker.empty()) s.Set(p + "maker", CleanText(m.maker, 40));
      if (!m.part.empty()) s.Set(p + "part", CleanText(m.part, 40));
    }
    s.SetI("host.mem.n", n);
  }
  std::lock_guard<std::mutex> g(snapMu_);
  sStatic_ = s;
}

// ------------------------------------------------------------------------------------------------ config / profiles
void Controller::LoadConfig() {
  cfg_.Clear();
  cfg_.Set("cfg.keep", "1");
  cfg_.Set("cfg.lang", "auto");
  cfg_.Set("cfg.accent", "e2a387");
  if (opt_.dataDir.empty()) return;
  std::string text;
  if (fs::ReadAll(fs::Join(opt_.dataDir, "config.kv"), &text, 1u << 20)) cfg_.Merge(KV::Parse(text));
}

void Controller::SaveConfig() {
  if (opt_.dataDir.empty()) return;
  fs::MakeDirs(opt_.dataDir);
  if (!fs::WriteAtomic(fs::Join(opt_.dataDir, "config.kv"), cfg_.Serialize())) L(mgp::LogLevel::Warn, "could not save config.kv");
}

void Controller::LoadDesired() {
  desired_.Clear();
  if (opt_.dataDir.empty()) return;
  std::string text;
  if (fs::ReadAll(fs::Join(opt_.dataDir, "desired.kv"), &text, 1u << 20)) {
    KV all = KV::Parse(text);
    for (auto& p : all.All()) {
      // only keys that are known profile keys and still validate are ever restored
      const SettingDef* d = FindSetting(p.first);
      if (d && IsProfileKey(p.first) && ValidateSetting(*d, p.second).ok) desired_.Set(p.first, p.second);
    }
  }
}

void Controller::SaveDesired() {
  if (opt_.dataDir.empty()) return;
  fs::MakeDirs(opt_.dataDir);
  if (!fs::WriteAtomic(fs::Join(opt_.dataDir, "desired.kv"), desired_.Serialize())) L(mgp::LogLevel::Warn, "could not save desired.kv");
}

// ------------------------------------------------------------------------------------------------ request dispatch
Result Controller::Ok(const std::string& msg, const KV& data) {
  Result r;
  r.ok = true;
  r.code = "ok";
  r.msg = msg;
  r.data = data;
  return r;
}

Result Controller::Fail(const std::string& code, const std::string& msg) {
  Result r;
  r.ok = false;
  r.code = code;
  r.msg = msg;
  if (code == "gate.consent" || code == "needs.approval") {
    // Asking first is the normal course of a risky change, not a failure: the window shows a dialog and sends the request again once the user agrees.
    L(mgp::LogLevel::Info, "waiting for the user's confirmation: " + code + ": " + msg);
    return r;
  }
  lastError_ = code + ": " + msg;
  L(mgp::LogLevel::Warn, "request failed: " + lastError_);
  return r;
}

Result Controller::Run(const Request& r) {
  Result res;
  try {
    if (r.op == "set") res = DoSet(r.key, r.value, r.args);
    else if (r.op == "cmd") res = DoCmd(r.key, r.args);
    else if (r.op == "cfg") res = DoCfg(r.key, r.value);
    else res = Fail("unknown", "unknown op " + r.op);
  } catch (const std::exception& e) {
    res = Fail("internal", std::string("exception: ") + e.what());
  } catch (...) {
    res = Fail("internal", "unknown exception");
  }
  SyncSnapshot();  // config, last error and log tail are visible to the UI right after every request
  return res;
}

Result Controller::Gate(const SettingDef& d) {
  if (d.ev != Ev::Os && cfg_.Bool("cfg.readonly", false)) return Fail("gate.readonly", N_("read-only mode (kill switch) is on"));
  Result ok;
  ok.ok = true;
  ok.code = "ok";
  return ok;
}

Result Controller::DoSet(const std::string& key, const std::string& raw, const KV& args) {
  const SettingDef* d = FindSetting(key);
  if (!d) return Fail("unknown", "unknown setting " + key);
  Validated v = ValidateSetting(*d, raw);
  if (!v.ok) return Fail("range", v.why);
  if (d->cap[0] && !CapOk(d->cap)) {
    const std::string state = CapState(d->cap);
    std::string why;
    {
      std::lock_guard<std::mutex> g(snapMu_);
      why = sCaps_.Str(std::string("capwhy.") + d->cap, "");
    }
    // the vendor's service not being reachable is something the user can fix, so it has its own code
    return Fail(state == "offline" ? "needs.service" : "unsupported",
                Fmt("capability %s is %s%s%s", d->cap, state.c_str(), why.empty() ? "" : ": ", why.c_str()));
  }
  Result g = Gate(*d);
  if (!g.ok) return g;

  // consent flags travel with the request that needs them and are remembered
  bool cfgChanged = false;
  for (auto& p : args.All())
    if (StartsWith(p.first, "consent.") && p.second == "1") {
      cfg_.Set("cfg." + p.first, "1");
      cfgChanged = true;
    }
  if (cfgChanged) SaveConfig();

  const std::string k = key;
  Result res;
  if (StartsWith(k, "perf.")) res = SetPerf(k, v.ival);
  else if (k == "gpu.ctgp") res = SetCtgp(v.ival, args);
  else if (k == "fan.mode") res = SetFanMode(v.ival);
  else if (k == "fan.cpu.curve") res = SetFanCurve(true, v.value);
  else if (k == "fan.gpu.curve") res = SetFanCurve(false, v.value);
  else if (k == "batt.mode") res = SetBattMode(v.ival);
  else if (k == "pref.fnlock" || k == "pref.acboot" || k == "pref.usbpower") res = SetSwitchPref(k, v.ival != 0);
  else if (k == "light.kbd.timer") res = SetKbdTimer(v.ival);
  else if (StartsWith(k, "light.kbd.")) res = SetKbd(k, v.value);
  else if (StartsWith(k, "disp.")) res = SetDisplay(k, v.ival);
  else if (StartsWith(k, "dev.")) res = SetDevice(k, v.ival != 0);
  else res = Fail("unsupported", "no handler for " + k);

  if (res.ok) {
    if (IsProfileKey(k)) {
      desired_.Set(k, v.value);
      SaveDesired();
    }
    L(mgp::LogLevel::Info, Fmt("set %s=%s ok", k.c_str(), v.value.c_str()));
    // refresh the affected domains right away so the UI shows the result without waiting for the next tick
    PollEc(true);
    PollGpu();
    PollOem();
    PollOs(true);
    PollMisc();
    SyncSnapshot();
  }
  return res;
}

Result Controller::DoCfg(const std::string& key, const std::string& raw) {
  if (!StartsWith(key, "cfg.") || key.size() > 64 || raw.size() > 512) return Fail("range", "bad config key/value");
  for (char c : key)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_')) return Fail("range", "bad config key");
  cfg_.Set(key, raw);
  if (key == "cfg.app.auto") {
    appRecheck_ = true;
    lastAppScan_ = 0;   // switching the matching on or off takes effect on the next tick, not 3 s later
  }
  if (key == "cfg.readonly" && gec_) gec_->SetReadOnly(raw == "1");
  SaveConfig();
  SyncSnapshot();
  return Ok();
}

Result Controller::DoCmd(const std::string& name, const KV& args) {
  if (name == "probe") {
    Probe();
    PollOnce(true);
    return Ok();
  }
  if (name == "profile.apply") {
    Result r = ApplyProfile(args.Str("name"), args);
    if (r.ok) {
      PollEc(true);   // the new profile's values are what the EC shows now
      PollGpu();
      PollOem();
      SyncSnapshot();
    }
    return r;
  }
  if (name == "app.set") return AppSet(args);
  if (name == "app.delete") return AppDelete(args);
  if (name == "app.move") return AppMove(args);
  if (name == "fan.restore" || name == "perf.restore" || StartsWith(name, "kb.")) {
    if (cfg_.Bool("cfg.readonly", false)) return Fail("gate.readonly", N_("read-only mode (kill switch) is on"));
    Result r;
    if (name == "fan.restore") r = FanRestore();
    else if (name == "perf.restore") r = DetailRestore();
    else if (name == "kb.save") r = KbdSave();
    else r = Fail("unknown", "unknown command " + name);
    if (r.ok) {
      PollEc(true);
      PollGpu();
      PollOem();
      SyncSnapshot();
    }
    return r;
  }
  if (name == "link.capture") {   // diagnostics: also record every other message on the broker (Service page)
    if (hw_.oemLink) hw_.oemLink->SetCapture(args.Bool("on", false));
    return Ok();
  }
  if (name == "oem.start") {   // Service page: start the vendor's services when they are installed but stopped
    if (cfg_.Bool("cfg.readonly", false)) return Fail("gate.readonly", N_("read-only mode (kill switch) is on"));
    if (!hw_.oemSvc) return Fail("unsupported", N_("this build cannot start the vendor's services"));
    std::string why;
    if (!hw_.oemSvc->StartServices(&why)) return Fail("failed.hw", why);
    L(mgp::LogLevel::Info, "the vendor's services were started");
    if (hw_.oemLink) hw_.oemLink->RequestAll();
    PollMisc();
    PollOem();
    SyncSnapshot();
    return Ok();
  }
  if (name == "link.reconnect") {
    if (hw_.oemLink) {
      hw_.oemLink->RequestAll();
      PollOem();
    }
    return Ok();
  }
  if (name == "consent.reset") {
    std::vector<std::string> del;
    for (auto& p : cfg_.All())
      if (StartsWith(p.first, "cfg.consent.")) del.push_back(p.first);
    for (auto& k : del) cfg_.Erase(k);
    SaveConfig();
    return Ok();
  }
  if (name == "reapply") {
    Reapply("manual");
    return Ok();
  }
  return Fail("unknown", "unknown command " + name);
}

// ------------------------------------------------------------------------------------------------ housekeeping
void Controller::Housekeeping() {
  AppProfileTick();
  BatteryHistoryTick();
  ColorTick();
  // events -> delayed re-apply (firmware resets values after resume / AC change / display wake)
  std::vector<std::string> ev;
  {
    std::lock_guard<std::mutex> g(qMu_);
    ev.swap(pendingEvents_);
  }
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  for (auto& e : ev) {
    L(mgp::LogLevel::Info, "event: " + e);
    ColorEvent(e);
    if (e == "suspend" || e == "display_off") continue;   // nothing to re-apply while the machine is going quiet
    if (cfg_.Bool("cfg.keep", true)) {
      reapplyAt_ = now + 3000;
      reapplyWhy_ = e;
    }
  }
  if (reapplyAt_ && now >= reapplyAt_) {
    reapplyAt_ = 0;
    Reapply(reapplyWhy_.c_str());
  }
}

void Controller::Reapply(const char* why) {
  if (desired_.Size() == 0) return;
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  if (now - reapplyWindowStart_ > 300000) {
    reapplyWindowStart_ = now;
    reapplyTries_ = 0;
  }
  if (++reapplyTries_ > 6) {
    L(mgp::LogLevel::Warn, "re-apply suppressed: too many attempts in 5 minutes (another program keeps changing the values?)");
    return;
  }
  L(mgp::LogLevel::Info, Fmt("re-applying %zu desired values (%s)", desired_.Size(), why));
  // Only the extended GPU power is kept by this program; the vendor's own profiles keep their values themselves.
  static const char* order[] = {"gpu.ctgp"};
  KV want = desired_;
  for (const char* k : order) {
    if (!want.Has(k)) continue;
    // skip values that already match (the live value of setting K is published under snapshot key K)
    if (Snapshot().Str(k, "") == want.Str(k)) continue;
    Result r = DoSet(k, want.Str(k), KV());
    if (!r.ok) L(mgp::LogLevel::Warn, Fmt("re-apply %s failed: %s", k, r.code.c_str()));
  }
}

}  // namespace bc
