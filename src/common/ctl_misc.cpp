// Controller: display colours and refresh rate, device switches, application profiles, battery history. Windows-side features only;
// everything that goes to the vendor's service is in ctl_oem.cpp.
#include <algorithm>
#include <cstring>
#include <cmath>

#include "colorramp.h"
#include "controller.h"
#include "i18n_mark.h"
#include "profile_text.h"
#include "registers.h"
#include "util.h"

namespace bc {

// ------------------------------------------------------------------------------------------------ probe / poll
void Controller::ProbeMisc() {
  DisplayInfo di;
  if (hw_.display) di = hw_.display->Info();
  SetCap("disp.bright", di.ok && di.brightness >= 0 ? "ok" : "absent", "");
  SetCap("disp.refresh", di.ok && di.refreshRates.size() > 1 ? "ok" : "absent", "");
  SetCap("disp.color", hw_.display ? "ok" : "absent", "");
  SetCap("disp.sat", !hw_.display ? "absent" : (hw_.display->SaturationSupported() ? "ok" : "unsupported"),
         hw_.display && !hw_.display->SaturationSupported() ? N_("Needs the Windows colour-effect API (Magnification.dll), which is not available.") : "");
  if (hw_.devices) {
    SetCap("dev.wifi", hw_.devices->Get(Dev::Wifi) >= 0 ? "ok" : "absent");
    SetCap("dev.bt", hw_.devices->Get(Dev::Bluetooth) >= 0 ? "ok" : "absent");
    SetCap("dev.cam", hw_.devices->Get(Dev::Webcam) >= 0 ? "ok" : "absent");
    SetCap("dev.touchpad", hw_.devices->Get(Dev::Touchpad) >= 0 ? "ok" : "absent");
  } else {
    for (const char* c : {"dev.wifi", "dev.bt", "dev.cam", "dev.touchpad"}) SetCap(c, "absent");
  }
  {
    std::string why;
    const bool ok = hw_.fps && hw_.fps->Available(&why);
    fpsOk_ = ok;
    SetCap("fps", ok ? "ok" : "absent", ok ? "" : (hw_.fps ? why : std::string(N_("frame counting is not available in this version"))));
  }
}

void Controller::PollMisc() {
  KV k;
  if (hw_.display) {
    DisplayInfo di = hw_.display->Info();
    if (di.ok) {
      if (di.brightness >= 0) k.SetI("disp.bright", di.brightness);
      if (di.refresh) k.SetI("disp.refresh", di.refresh);
      std::string l;
      for (size_t i = 0; i < di.refreshRates.size(); i++) l += Fmt("%s%d", i ? "," : "", di.refreshRates[i]);
      k.Set("disp.refresh.list", l);
      k.Set("disp.res", Fmt("%dx%d", di.width, di.height));
    }
    const ColorSettings c = StoredColor();
    k.SetI("disp.c.bright", c.brightness);
    k.SetI("disp.c.contrast", c.contrast);
    k.SetI("disp.c.gamma", c.gamma);
    k.SetI("disp.c.kelvin", c.kelvin);
    k.SetI("disp.c.r", c.r);
    k.SetI("disp.c.g", c.g);
    k.SetI("disp.c.b", c.b);
    k.SetI("disp.c.sat", c.saturation);
    k.Set("disp.mode", cfg_.Str("cfg.disp.mode", "standard"));
  }
  if (hw_.devices) {
    int v;
    if ((v = hw_.devices->Get(Dev::Wifi)) >= 0) k.SetB("dev.wifi", v);
    if ((v = hw_.devices->Get(Dev::Bluetooth)) >= 0) k.SetB("dev.bt", v);
    if ((v = hw_.devices->Get(Dev::Webcam)) >= 0) k.SetB("dev.cam", v);
    if ((v = hw_.devices->Get(Dev::Touchpad)) >= 0) k.SetB("dev.touchpad", v);
  }
  if (hw_.fps && fpsOk_) {
    std::string app;
    const double f = hw_.fps->Fps(&app);
    if (f >= 0) {
      k.SetI("fps.cur", static_cast<long long>(std::lround(f)));
      k.Set("fps.app", app);
    }
  }
  if (hw_.oemSvc) {
    OemSvcState o = hw_.oemSvc->Probe();
    k.SetB("oem.installed", o.installed);
    k.SetB("oem.running", o.running);
    k.Set("oem.version", o.version);
    k.Set("oem.services", Join(o.services, ","));
  }
  std::lock_guard<std::mutex> g(snapMu_);
  sMisc_ = k;
}

// ------------------------------------------------------------------------------------------------ display / devices
Result Controller::SetDisplay(const std::string& key, int value) {
  if (!hw_.display) return Fail("unsupported", "no display interface");
  std::string why;
  if (key == "disp.bright") {
    if (!hw_.display->SetBrightness(value, &why)) return Fail("failed.hw", why);
    return Ok();
  }
  if (key == "disp.refresh") {
    if (!hw_.display->SetRefresh(value, &why)) return Fail("failed.hw", why);
    ScheduleColorReapply(1200, 1);   // some drivers drop the gamma ramp on a mode change
    return Ok();
  }
  ColorSettings c = StoredColor();
  std::string mode = cfg_.Str("cfg.disp.mode", "standard");
  if (key == "disp.mode") {
    // The image profiles are colour curves of Control Center Advanced's own (the panel's factory calibration is not touched).
    const SettingDef* d = FindSetting("disp.mode");
    mode = EnumName(*d, value);
    if (const ColorSettings* p = FindColorPreset(mode)) c = *p;
  } else {
    if (key == "disp.c.bright") c.brightness = value;
    else if (key == "disp.c.contrast") c.contrast = value;
    else if (key == "disp.c.gamma") c.gamma = value;
    else if (key == "disp.c.kelvin") c.kelvin = value;
    else if (key == "disp.c.r") c.r = value;
    else if (key == "disp.c.g") c.g = value;
    else if (key == "disp.c.b") c.b = value;
    else if (key == "disp.c.sat") c.saturation = value;
    mode = "custom";
  }
  if (!hw_.display->SetColor(c, &why)) return Fail("failed.hw", why);
  cfg_.SetI("cfg.disp.c.bright", c.brightness); cfg_.SetI("cfg.disp.c.contrast", c.contrast); cfg_.SetI("cfg.disp.c.gamma", c.gamma);
  cfg_.SetI("cfg.disp.c.kelvin", c.kelvin); cfg_.SetI("cfg.disp.c.r", c.r); cfg_.SetI("cfg.disp.c.g", c.g); cfg_.SetI("cfg.disp.c.b", c.b);
  cfg_.SetI("cfg.disp.c.sat", c.saturation);
  cfg_.Set("cfg.disp.mode", mode);
  SaveConfig();
  return Ok();
}

// The colour settings the user last applied (neutral when they never touched them).
ColorSettings Controller::StoredColor() const {
  ColorSettings c;
  c.brightness = static_cast<int>(cfg_.Int("cfg.disp.c.bright", 50));
  c.contrast = static_cast<int>(cfg_.Int("cfg.disp.c.contrast", 50));
  c.gamma = static_cast<int>(cfg_.Int("cfg.disp.c.gamma", 100));
  c.kelvin = static_cast<int>(cfg_.Int("cfg.disp.c.kelvin", 6500));
  c.r = static_cast<int>(cfg_.Int("cfg.disp.c.r", 100));
  c.g = static_cast<int>(cfg_.Int("cfg.disp.c.g", 100));
  c.b = static_cast<int>(cfg_.Int("cfg.disp.c.b", 100));
  c.saturation = static_cast<int>(cfg_.Int("cfg.disp.c.sat", 100));
  return c;
}

// Windows forgets the gamma ramp and the colour effect at sign-in, after sleep and on some mode changes, so the stored colours are put
// back a moment after those events. Never while they are neutral: an untouched computer keeps whatever another tool (night light,
// f.lux) has set.
void Controller::ColorEvent(const std::string& ev) {
  if (!hw_.display || !hw_.clock) return;
  if (ev == "resume" || ev == "display_on") ScheduleColorReapply(1500, 2);
}

void Controller::ScheduleColorReapply(uint64_t afterMs, int times) {
  if (!hw_.clock) return;
  colorReapplyAt_ = hw_.clock->NowMs() + afterMs;
  colorReapplyLeft_ = times;
}

void Controller::ColorTick() {
  if (!hw_.display || !hw_.clock || colorReapplyAt_ == 0) return;
  const uint64_t now = hw_.clock->NowMs();
  if (now < colorReapplyAt_) return;
  colorReapplyAt_ = --colorReapplyLeft_ > 0 ? now + 4500 : 0;
  const ColorSettings c = StoredColor();
  if (c.Neutral()) return;
  std::string why;
  if (!hw_.display->SetColor(c, &why)) L(mgp::LogLevel::Warn, "colours not re-applied: " + why);
}

Result Controller::SetDevice(const std::string& key, bool on) {
  if (!hw_.devices) return Fail("unsupported", "no device interface");
  Dev d = key == "dev.wifi" ? Dev::Wifi : key == "dev.bt" ? Dev::Bluetooth : key == "dev.cam" ? Dev::Webcam : Dev::Touchpad;
  std::string why;
  if (!hw_.devices->Set(d, on, &why)) return Fail("failed.hw", why);
  if (hw_.clock) hw_.clock->SleepMs(300);
  if (hw_.devices->Get(d) != (on ? 1 : 0)) return Fail("reverted", N_("the device did not change state"));
  return Ok();
}

// ------------------------------------------------------------------------------------------------ profiles
// The four profiles are the vendor's operating modes (Balanced = office, Enthusiast = gaming, Overboost = turbo, Customized). Switching
// goes to the vendor's service, which applies the values the profile stores. The GPU power this program adds on top is applied again
// afterwards, because the vendor's profile may put its own value back.
Result Controller::ApplyProfile(const std::string& name, const KV&) {
  int idx = -1;
  const std::vector<std::string>& names = BuiltinProfiles();
  for (size_t i = 0; i < names.size(); i++)
    if (EqNoCase(name, names[i])) idx = static_cast<int>(i);
  if (idx < 0) return Fail("unknown", "no such profile: " + name);
  if (OemOperatingMode() == idx) return Ok("already active");
  Result r = SwitchMode(idx);
  if (r.ok && desired_.Has("gpu.ctgp") && cfg_.Bool("cfg.keep", true) && hw_.clock) {
    reapplyAt_ = hw_.clock->NowMs() + 2500;
    reapplyWhy_ = "profile";
  }
  return r;
}

// ------------------------------------------------------------------------------------------------ application profiles
namespace {
constexpr int kAppSlots = 32;

bool ValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    unsigned cp = 0;
    if (c < 0x80) { len = 1; cp = c; }
    else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
    else return false;
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; k++) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return false;   // control characters
    i += len;
  }
  return true;
}

struct AppEntry {
  std::string exe, profile, name;
  bool Used() const { return !exe.empty(); }
};

AppEntry ReadAppSlot(const KV& cfg, int i) {
  AppEntry e;
  e.exe = cfg.Str(Fmt("cfg.app.%d.exe", i));
  e.profile = cfg.Str(Fmt("cfg.app.%d.profile", i));
  e.name = cfg.Str(Fmt("cfg.app.%d.name", i));
  return e;
}

void WriteAppSlot(KV& cfg, int i, const AppEntry& e) {
  const char* parts[3] = {"exe", "profile", "name"};
  const std::string* vals[3] = {&e.exe, &e.profile, &e.name};
  for (int k = 0; k < 3; k++) {
    const std::string key = Fmt("cfg.app.%d.%s", i, parts[k]);
    if (e.Used() && !vals[k]->empty()) cfg.Set(key, *vals[k]);
    else cfg.Erase(key);
  }
}

// Image name as Windows lists it, lower case. Only plain file names: no path, no wildcard, no control character.
bool NormalizeExe(const std::string& in, std::string* out) {
  std::string e = Lower(Trim(in));
  if (e.empty() || e.size() > 64) return false;
  for (unsigned char c : e)
    if (c < 0x20 || c == 0x7F || std::strchr("\\/:*?\"<>|", c)) return false;
  if (e.find('.') == std::string::npos) e += ".exe";
  if (e.size() < 5 || e.compare(e.size() - 4, 4, ".exe") != 0 || e == ".exe") return false;
  *out = e;
  return true;
}

bool ParseSlot(const KV& args, const char* key, int* out) {
  const std::string v = Trim(args.Str(key));
  if (v.empty()) return false;
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (!end || *end || n < 0 || n >= kAppSlots) return false;
  *out = static_cast<int>(n);
  return true;
}
}  // namespace

Result Controller::AppSet(const KV& args) {
  std::string exe;
  if (!NormalizeExe(args.Str("exe"), &exe)) return Fail("range", N_("the program must be a plain file name such as game.exe"));
  std::string profile = Trim(args.Str("profile"));
  {
    bool known = false;
    for (const std::string& b : BuiltinProfiles())
      if (EqNoCase(profile, b)) {
        profile = b;   // stored with the spelling the vendor's service uses
        known = true;
      }
    if (!known) return Fail("unknown", "no such profile: " + profile);
  }
  std::string name = Trim(args.Str("name"));
  if (name.size() > 48 || !ValidUtf8(name)) return Fail("range", N_("the display name must be valid text of at most 48 bytes"));

  int slot = -1;
  const bool editing = !Trim(args.Str("i")).empty();
  if (editing && !ParseSlot(args, "i", &slot)) return Fail("range", "bad slot");
  if (editing && !ReadAppSlot(cfg_, slot).Used()) return Fail("range", N_("that slot is empty"));
  int last = -1, firstFree = -1;
  for (int i = 0; i < kAppSlots; i++) {
    const AppEntry e = ReadAppSlot(cfg_, i);
    if (e.Used()) {
      last = i;
      if (e.exe == exe && i != slot) return Fail("range", N_("that program is already in the list"));
    } else if (firstFree < 0) {
      firstFree = i;
    }
  }
  if (!editing) {
    slot = last + 1 < kAppSlots ? last + 1 : firstFree;
    if (slot < 0) return Fail("range", N_("the list is full (32 programs)"));
  }
  AppEntry e;
  e.exe = exe;
  e.profile = profile;
  e.name = name;
  WriteAppSlot(cfg_, slot, e);
  SaveConfig();
  appRecheck_ = true;
  lastAppScan_ = 0;
  SyncSnapshot();
  return Ok();
}

Result Controller::AppDelete(const KV& args) {
  int slot = 0;
  if (!ParseSlot(args, "i", &slot) || !ReadAppSlot(cfg_, slot).Used()) return Fail("range", N_("no such entry"));
  WriteAppSlot(cfg_, slot, AppEntry());
  SaveConfig();
  appRecheck_ = true;
  lastAppScan_ = 0;
  SyncSnapshot();
  return Ok();
}

Result Controller::AppMove(const KV& args) {
  int slot = 0;
  if (!ParseSlot(args, "i", &slot) || !ReadAppSlot(cfg_, slot).Used()) return Fail("range", N_("no such entry"));
  const int dir = Trim(args.Str("dir")) == "-1" ? -1 : 1;
  int other = slot + dir;
  while (other >= 0 && other < kAppSlots && !ReadAppSlot(cfg_, other).Used()) other += dir;
  if (other < 0 || other >= kAppSlots) return Fail("range", N_("already at the end of the list"));
  const AppEntry a = ReadAppSlot(cfg_, slot), b = ReadAppSlot(cfg_, other);
  WriteAppSlot(cfg_, slot, b);
  WriteAppSlot(cfg_, other, a);
  SaveConfig();
  appRecheck_ = true;
  lastAppScan_ = 0;
  SyncSnapshot();
  return Ok();
}

// Programs that switch the profile while they run. The first entry of the list that is running wins; when it closes the profile
// that was active before it comes back. Entries whose profile no longer exists are skipped. A switch the vendor's service did not accept
// (not reachable yet, say) is tried again at the next scan.
void Controller::AppProfileTick() {
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  if (now < lastAppScan_ + 3000) return;
  lastAppScan_ = now;
  if (!hw_.power) return;
  const bool recheck = appRecheck_;
  appRecheck_ = false;
  auto activeName = [&]() {
    const int m = OemOperatingMode();
    const std::vector<std::string>& n = BuiltinProfiles();
    return m >= 0 && m < static_cast<int>(n.size()) ? n[static_cast<size_t>(m)] : std::string();
  };
  auto back = [&]() {
    if (profileBeforeApp_.empty()) return true;
    Result r = ApplyProfile(profileBeforeApp_, KV());
    return r.ok;
  };
  if (!cfg_.Bool("cfg.app.auto", true)) {   // switched off: leave a running program's profile as soon as possible
    if (!appRunning_.empty()) {
      L(mgp::LogLevel::Info, "application profiles switched off");
      if (back()) {
        appRunning_.clear();
        profileBeforeApp_.clear();
      }
      SyncSnapshot();
    }
    return;
  }
  struct Map { std::string exe, profile; };
  std::vector<Map> maps;
  for (int i = 0; i < kAppSlots; i++) {
    const AppEntry e = ReadAppSlot(cfg_, i);
    if (e.Used() && !e.profile.empty() && IsBuiltinProfile(e.profile)) maps.push_back({Lower(e.exe), e.profile});
  }
  if (maps.empty() && appRunning_.empty()) return;
  std::vector<std::string> run = hw_.power->RunningExes();
  std::string hit, hitProfile;
  for (auto& m : maps)
    if (std::find(run.begin(), run.end(), m.exe) != run.end()) { hit = m.exe; hitProfile = m.profile; break; }
  if (!hit.empty() && (hit != appRunning_ || (recheck && !EqNoCase(activeName(), hitProfile)))) {
    const std::string before = appRunning_.empty() ? activeName() : profileBeforeApp_;
    L(mgp::LogLevel::Info, "application profile: " + hit + " -> " + hitProfile);
    Result r = ApplyProfile(hitProfile, KV());
    if (r.ok) {
      profileBeforeApp_ = before;
      appRunning_ = hit;
    } else {
      L(mgp::LogLevel::Warn, "application profile not applied (" + r.code + "); will try again");
      lastAppScan_ = now + 9000;   // next scan in 12 s
    }
    SyncSnapshot();
  } else if (hit.empty() && !appRunning_.empty()) {
    L(mgp::LogLevel::Info, "application profile: " + appRunning_ + " closed");
    if (back()) {
      appRunning_.clear();
      profileBeforeApp_.clear();
    } else {
      lastAppScan_ = now + 9000;
    }
    SyncSnapshot();
  }
}

// One sample a minute: the charge in percent (+200 while the charger is connected). Minutes in which nothing was sampled - the machine was
// asleep, the service was busy - become -1 entries so the time axis stays true. A machine without a battery never starts a history.
void Controller::BatteryHistoryTick() {
  if (!hw_.clock) return;
  const uint64_t now = hw_.clock->NowMs();
  std::lock_guard<std::mutex> g(snapMu_);
  if (battHistAt_ == 0 && battPctNow_ < 0) return;
  if (battHistAt_ != 0 && now - battHistAt_ < kBattStepMs) return;
  uint64_t steps = 1;
  if (battHistAt_ != 0) {
    steps = (now - battHistAt_) / kBattStepMs;   // >= 1 here
    const uint64_t missed = std::min<uint64_t>(steps - 1, kBattMax);
    for (uint64_t i = 0; i < missed; i++) battHist_.push_back(-1);
  }
  battHist_.push_back(battPctNow_ < 0 ? -1 : static_cast<int>(Clamp<long long>(battPctNow_, 0, 100)) + (acKnown_ && acNow_ ? 200 : 0));
  while (battHist_.size() > kBattMax) battHist_.pop_front();
  battHistAt_ = battHistAt_ == 0 ? now : battHistAt_ + steps * kBattStepMs;   // keeps the cadence: no drift over hours
  BatteryHistoryText();
}

// Rebuilds the snapshot text of the history. battHist_ and battHistText_ change together, under snapMu_ (the simulator's screenshot
// tool seeds the history from another thread).
void Controller::BatteryHistoryText() {
  std::string t;
  t.reserve(battHist_.size() * 4);
  for (size_t i = 0; i < battHist_.size(); i++) {
    if (i) t += ',';
    t += std::to_string(battHist_[i]);
  }
  battHistText_ = t;
}

void Controller::SeedBatteryHistory(const std::vector<int>& samples) {
  std::lock_guard<std::mutex> g(snapMu_);
  battHist_.clear();
  const size_t from = samples.size() > kBattMax ? samples.size() - kBattMax : 0;
  for (size_t i = from; i < samples.size(); i++) battHist_.push_back(samples[i] < 0 ? -1 : std::min(samples[i], 300));
  battHistAt_ = hw_.clock ? hw_.clock->NowMs() : 1;
  BatteryHistoryText();
}

}  // namespace bc
