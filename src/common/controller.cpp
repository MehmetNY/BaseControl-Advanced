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
const char* kProfileKeys[] = {"ec.pl1", "ec.pl2", "ec.pl4", "ec.tcc", "gpu.ctgp", "fan.mode", "fan.cpu.curve", "fan.gpu.curve",
                              "os.boost", "os.epp", "os.min", "os.max", "os.park", "batt.mode"};

bool IsProfileKey(const std::string& k) {
  for (const char* p : kProfileKeys)
    if (k == p) return true;
  return false;
}

// Built-in presets. Values are Control Center Advanced defaults, not OEM profile equivalents.
KV Preset(const std::string& name) {
  KV k;
  if (name == "Quiet") {
    k.Set("ec.pl1", "35"); k.Set("ec.pl2", "45"); k.Set("ec.pl4", "90"); k.Set("ec.tcc", "90"); k.Set("fan.mode", "auto");
  } else if (name == "Balanced") {
    k.Set("ec.pl1", "60"); k.Set("ec.pl2", "80"); k.Set("ec.pl4", "120"); k.Set("ec.tcc", "95"); k.Set("fan.mode", "auto");
  } else if (name == "Enthusiast") {
    k.Set("ec.pl1", "80"); k.Set("ec.pl2", "100"); k.Set("ec.pl4", "150"); k.Set("ec.tcc", "95"); k.Set("fan.mode", "auto");
  } else if (name == "Overboost") {
    k.Set("ec.pl1", "120"); k.Set("ec.pl2", "120"); k.Set("ec.pl4", "200"); k.Set("ec.tcc", "100"); k.Set("fan.mode", "boost");
  }
  return k;
}
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
  LoadProfiles();
  if (gec_) gec_->SetReadOnly(cfg_.Bool("cfg.readonly", false));
  if (gec_ && hw_.nv && hw_.smbios && hw_.clock) {
    engine_.reset(new mgp::PowerProfileEngine(*gec_, *hw_.nv, *hw_.smbios, *hw_.clock, *hw_.log));
  }
  LoadDesired();
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
  // Safety: a fixed fan duty has no thermal feedback of its own; this process's watchdog is its only protection.
  // Hand the fans back to the EC on the way out (the stored choice is re-applied the next time the controller starts).
  if (ecConnected_ && Snapshot().Str("fan.mode") == "fixed") {
    L(mgp::LogLevel::Info, "stopping: returning the fans to automatic mode");
    Result r = SetFanMode(0);
    if (!r.ok) L(mgp::LogLevel::Warn, "could not return the fans to automatic mode: " + r.msg);
  }
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
  out.Merge(sStatic_); out.Merge(sCaps_); out.Merge(sEc_); out.Merge(sGpu_); out.Merge(sMsr_); out.Merge(sOs_); out.Merge(sMisc_);
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
  if (full || now - lastFast_ >= fastMs) {
    PollEc(false);
    PollMsr(false);
    PollGpu();
    lastFast_ = now;
    did = true;
  }
  if (full || now - lastSlow_ >= slowMs) {
    PollEc(true);
    PollMsr(true);
    lastSlow_ = now;
    did = true;
  }
  // The fan watchdog judges the temperatures of the last poll. While a custom or fixed fan setting is active that must not depend on
  // whether the window is open (the full poll then runs every 5 s): two register reads a second, not the whole table.
  if (!vis && fanWatch_ && now - lastFast_ >= 1000 && now - lastWatch_ >= 1000) {
    PollEcTemps();
    lastWatch_ = now;
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
  if (full || now - lastHk_ >= 1000) {
    Housekeeping();
    lastHk_ = now;
  }
  if (now - lastLight_ >= 70) {
    LightTick();
    lastLight_ = now;
  }
  if (did) SyncSnapshot();
}

void Controller::SyncSnapshot() {
  KV st;
  st.Set("app.version", opt_.version);
  st.Set("svc.mode", opt_.mode);
  st.Set("ec.link", ecConnected_ ? "connected" : "absent");
  st.Set("dev.class", devClass_);
  st.Set("cfg.readonly", gec_ && gec_->ReadOnly() ? "1" : "0");
  st.SetI("ec.writes", gec_ ? gec_->writes() : 0);
  st.SetI("ec.refused", gec_ ? gec_->refused() : 0);
  st.Set("profile.active", activeProfile_);
  st.Set("app.active", appRunning_);               // the program whose profile is applied right now ("" = none)
  st.Set("app.before", profileBeforeApp_);         // the profile that comes back when it closes
  st.Set("profile.list", Join(ProfileNames(), ","));
  // what each profile stores, for the Profiles page: "ec.pl1=60|ec.pl2=80|..."
  for (auto& p : profiles_) st.Set("profile.v." + p.first, FormatProfileValues(p.second));
  for (auto& p : cfg_.All()) st.Set(p.first, p.second);
  auto tail = ring_->Tail(30);
  st.Set("log.tail", Join(tail, "\n"));
  st.Set("last.error", lastError_);
  if (light_[0].present || light_[2].present || light_[3].present) {
    st.Set("light.off.why", LightSuppressed());             // "" while the lights show the user's setting
    st.SetI("light.bright.eff", LightEffectiveBright(0));   // what the keyboard really gets (battery limit applied)
  }
  st.Set("fan.guard", fanGuard_.empty() ? "ok" : "tripped");
  st.Set("fan.guard.why", fanGuard_);
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
  ProbeCpu();
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
  cfg_.Set("cfg.experimental", "0");
  cfg_.Set("cfg.keep", "1");
  cfg_.Set("cfg.takeover", "0");
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

void Controller::LoadProfiles() {
  profiles_.clear();
  for (const std::string& n : BuiltinProfiles()) profiles_[n] = Preset(n);
  if (opt_.dataDir.empty()) return;
  std::string text;
  if (fs::ReadAll(fs::Join(opt_.dataDir, "profiles.kv"), &text, 4u << 20)) {
    KV all = KV::Parse(text);
    for (auto& p : all.All()) {
      if (!StartsWith(p.first, "p.")) continue;
      profiles_[p.first.substr(2)] = KV::Parse(Unescape(p.second));
    }
    activeProfile_ = all.Str("active", "");
  }
}

void Controller::SaveProfiles() {
  if (opt_.dataDir.empty()) return;
  KV all;
  for (auto& p : profiles_) all.Set("p." + p.first, Escape(p.second.Serialize()));
  all.Set("active", activeProfile_);
  fs::MakeDirs(opt_.dataDir);
  if (!fs::WriteAtomic(fs::Join(opt_.dataDir, "profiles.kv"), all.Serialize())) L(mgp::LogLevel::Warn, "could not save profiles.kv");
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

std::vector<std::string> Controller::ProfileNames() const {
  std::vector<std::string> v = BuiltinProfiles();
  for (auto& p : profiles_)
    if (!IsBuiltinProfile(p.first)) v.push_back(p.first);
  return v;
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
  if (d.ev != Ev::Os) {
    if (cfg_.Bool("cfg.readonly", false)) return Fail("gate.readonly", N_("read-only mode (kill switch) is on"));
  }
  // Family-tier mechanisms (same EC family, independently documented, plausibility-probed at start-up) are enabled by default
  // only on the exactly identified unit; on other models, and for anything Experimental, the explicit toggle is required.
  bool needExp = d.needsExperimental;
  if (needExp && d.ev == Ev::Family && devClass_ == "verified") needExp = false;
  if (needExp && !cfg_.Bool("cfg.experimental", false))
    return Fail("gate.experimental", std::string(d.key) + " is " + EvName(d.ev) + "; enable Safety and service > Allow experimental writes");
  const std::string k = d.key;
  const bool hwWrite = StartsWith(k, "ec.") || StartsWith(k, "fan.") || StartsWith(k, "batt.") || StartsWith(k, "pref.") ||
                       StartsWith(k, "gpu.") || StartsWith(k, "msr.") || StartsWith(k, "nv.");
  if (hwWrite && devClass_ == "unknown" && !cfg_.Bool("cfg.unknownModel", false))
    return Fail("gate.model", N_("unrecognised hardware model; hardware writes are disabled (see Safety and service)"));
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
    std::string why;
    {
      std::lock_guard<std::mutex> g(snapMu_);
      why = sCaps_.Str(std::string("capwhy.") + d->cap, "");
    }
    return Fail("unsupported", Fmt("capability %s is %s%s%s", d->cap, CapState(d->cap).c_str(), why.empty() ? "" : ": ", why.c_str()));
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
  if (k == "ec.pl1" || k == "ec.pl2" || k == "ec.pl4") res = SetEcPower(k, v.ival, args);
  else if (k == "ec.tcc") res = SetTcc(v.ival);
  else if (k == "gpu.ctgp") res = SetCtgp(v.ival, args);
  else if (k == "gpu.db") res = SetGpuDb(v.ival != 0);
  else if (k == "nv.offset.core") res = SetNvOffset(true, v.ival);
  else if (k == "nv.offset.mem") res = SetNvOffset(false, v.ival);
  else if (k == "fan.mode") {
    res = SetFanMode(v.ival);
    if (res.ok) fanGuard_.clear();
  }
  else if (k == "fan.cpu.curve") res = SetFanCurve(true, v.value);
  else if (k == "fan.gpu.curve") res = SetFanCurve(false, v.value);
  else if (k == "fan.fixed.cpu") res = SetFixedFan(true, v.ival);
  else if (k == "fan.fixed.gpu") res = SetFixedFan(false, v.ival);
  else if (k == "fan.ramp") res = SetFanRamp(v.ival);
  else if (k == "batt.mode") res = SetBattMode(v.ival);
  else if (k == "pref.fnlock") res = SetEcBit(reg::kFnLock, 4, v.ival != 0, "FnLock");
  else if (k == "pref.acboot") res = SetUsbVsBoot(false, v.ival != 0);
  else if (k == "pref.usbpower") res = SetUsbVsBoot(true, v.ival != 0);
  else if (StartsWith(k, "os.")) res = SetOsCpu(k, v.ival);
  else if (StartsWith(k, "msr.")) res = SetMsr(k, v.ival);
  else if (StartsWith(k, "light.")) res = SetLight(k, v.value);
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
    PollMsr(true);
    PollGpu();
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
  if (name == "profile.apply") return ApplyProfile(args.Str("name"), args);
  if (name == "profile.save") return SaveProfile(args.Str("name"));
  if (name == "profile.delete") return DeleteProfile(args.Str("name"));
  if (name == "light.idle.state") return LightIdleState(args);
  if (name == "app.set") return AppSet(args);
  if (name == "app.delete") return AppDelete(args);
  if (name == "app.move") return AppMove(args);
  if (name == "oem.takeover") return OemTakeover(true);
  if (name == "oem.restore") return OemTakeover(false);
  if (name == "ec.bringup") {
    Result g;
    if (!cfg_.Bool("cfg.experimental", false)) return Fail("gate.experimental", "EC bring-up needs experimental writes");
    return EcBringUp();
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
  FanWatchdog();
  AppProfileTick();
  PowerSourceTick();
  BatteryHistoryTick();
  LightPolicyTick();
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
    LightEvent(e);
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
  // dependency order: EC power group first, then the rest
  static const char* order[] = {"ec.pl1", "ec.pl2", "ec.pl4", "ec.tcc", "gpu.ctgp", "fan.mode", "fan.cpu.curve", "fan.gpu.curve", "batt.mode",
                                "os.boost", "os.epp", "os.min", "os.max", "os.park"};
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
