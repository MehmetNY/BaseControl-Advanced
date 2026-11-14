// Controller: display, device switches, RGB lighting (ITE HID), profiles, application profiles, OEM coexistence.
#include <algorithm>
#include <cmath>

#include "controller.h"
#include "registers.h"
#include "util.h"

namespace bc {

namespace {
struct Rgb { uint8_t r, g, b; };

bool ParseColor(const std::string& s, Rgb* out) {
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (size_t i = 1; i < 7; i++) {
    char c = s[i];
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + static_cast<unsigned>(d);
  }
  out->r = static_cast<uint8_t>(v >> 16);
  out->g = static_cast<uint8_t>(v >> 8);
  out->b = static_cast<uint8_t>(v);
  return true;
}

std::string ColorStr(const Rgb& c) { return Fmt("#%02x%02x%02x", c.r, c.g, c.b); }

Rgb Hsv(double h, double s, double v) {
  h = std::fmod(h, 360.0);
  if (h < 0) h += 360.0;
  double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1)), m = v - c, r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  auto q = [&](double f) { return static_cast<uint8_t>(Clamp(static_cast<int>(std::lround((f + m) * 255.0)), 0, 255)); };
  return Rgb{q(r), q(g), q(b)};
}

int ZoneIndex(const std::string& key, std::string* prop) {
  // light.kbd.color -> zone 0, prop "color"; light.bar.* -> 2; light.logo.* -> 3
  auto parts = Split(key, '.');
  if (parts.size() != 3) return -1;
  *prop = parts[2];
  return parts[1] == "kbd" ? 0 : parts[1] == "bar" ? 2 : parts[1] == "logo" ? 3 : -1;
}

const char* ZoneName(int z) { return z == 0 ? "kbd" : z == 2 ? "bar" : "logo"; }
LightZone ZoneEnum(int z) { return z == 0 ? LightZone::Keyboard : z == 2 ? LightZone::LightBar : LightZone::Logo; }
uint8_t ZoneCmd(int z) { return z == 0 ? 0x02 : static_cast<uint8_t>(0x20 + z); }
uint8_t BrightHid(int z, int level) {
  level = Clamp(level, 1, 4);
  return z == 0 ? static_cast<uint8_t>(8 + (level - 1) * 14) : static_cast<uint8_t>(level * 25 == 100 ? 0x64 : level * 25 == 25 ? 0x19 : level * 25 == 50 ? 0x32 : 0x4B);
}
}  // namespace

// ------------------------------------------------------------------------------------------------ probe / poll
void Controller::ProbeMisc() {
  DisplayInfo di;
  if (hw_.display) di = hw_.display->Info();
  SetCap("disp.bright", di.ok && di.brightness >= 0 ? "ok" : "absent", "");
  SetCap("disp.refresh", di.ok && di.refreshRates.size() > 1 ? "ok" : "absent", "");
  SetCap("disp.color", hw_.display ? "ok" : "absent", "");
  if (hw_.devices) {
    SetCap("dev.wifi", hw_.devices->Get(Dev::Wifi) >= 0 ? "ok" : "absent");
    SetCap("dev.bt", hw_.devices->Get(Dev::Bluetooth) >= 0 ? "ok" : "absent");
    SetCap("dev.cam", hw_.devices->Get(Dev::Webcam) >= 0 ? "ok" : "absent");
    SetCap("dev.touchpad", hw_.devices->Get(Dev::Touchpad) >= 0 ? "ok" : "absent");
  } else {
    for (const char* c : {"dev.wifi", "dev.bt", "dev.cam", "dev.touchpad"}) SetCap(c, "absent");
  }
  std::string info;
  std::vector<LightZone> zones;
  if (hw_.light) zones = hw_.light->Detect(&info);
  bool z[4] = {false, false, false, false};
  for (LightZone l : zones) z[static_cast<int>(l)] = true;
  SetCap("light.kbd", z[0] ? "ok" : "absent", z[0] ? "" : "no ITE 048D keyboard controller found");
  SetCap("light.bar", z[2] ? "ok" : "absent", z[2] ? "" : "no light bar controller (this model has none)");
  SetCap("light.logo", z[3] ? "ok" : "absent", z[3] ? "" : "no logo controller found");
  for (int i : {0, 2, 3}) {
    light_[i].present = z[i];
    std::string p = std::string("cfg.light.") + ZoneName(i) + ".";
    Rgb c{255, 255, 255};
    if (ParseColor(cfg_.Str(p + "color"), &c)) { light_[i].r = c.r; light_[i].g = c.g; light_[i].b = c.b; }
    light_[i].power = cfg_.Bool(p + "power", true);
    light_[i].bright = static_cast<int>(cfg_.Int(p + "bright", 4));
    light_[i].effect = static_cast<int>(cfg_.Int(p + "effect", 0));
    light_[i].speed = static_cast<int>(cfg_.Int(p + "speed", 5));
  }
  {
    std::lock_guard<std::mutex> g(snapMu_);
    sCaps_.Set("light.info", info);
  }
  for (int i : {0, 2, 3})
    if (light_[i].present) {
      for (int tries = 0; tries < 1; tries++) {}
      lightSeq_++;
    }
  if (z[0] || z[2] || z[3]) LightApply();
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
    k.SetI("disp.c.bright", cfg_.Int("cfg.disp.c.bright", 50));
    k.SetI("disp.c.contrast", cfg_.Int("cfg.disp.c.contrast", 50));
    k.SetI("disp.c.gamma", cfg_.Int("cfg.disp.c.gamma", 100));
    k.SetI("disp.c.kelvin", cfg_.Int("cfg.disp.c.kelvin", 6500));
    k.SetI("disp.c.r", cfg_.Int("cfg.disp.c.r", 100));
    k.SetI("disp.c.g", cfg_.Int("cfg.disp.c.g", 100));
    k.SetI("disp.c.b", cfg_.Int("cfg.disp.c.b", 100));
    k.Set("disp.mode", cfg_.Str("cfg.disp.mode", "standard"));
  }
  if (hw_.devices) {
    int v;
    if ((v = hw_.devices->Get(Dev::Wifi)) >= 0) k.SetB("dev.wifi", v);
    if ((v = hw_.devices->Get(Dev::Bluetooth)) >= 0) k.SetB("dev.bt", v);
    if ((v = hw_.devices->Get(Dev::Webcam)) >= 0) k.SetB("dev.cam", v);
    if ((v = hw_.devices->Get(Dev::Touchpad)) >= 0) k.SetB("dev.touchpad", v);
  }
  if (hw_.oem) {
    OemSvcState o = hw_.oem->Probe();
    k.SetB("oem.installed", o.installed);
    k.SetB("oem.running", o.running);
    k.Set("oem.version", o.version);
    k.Set("oem.services", Join(o.services, ","));
  }
  for (int i : {0, 2, 3}) {
    if (!light_[i].present) continue;
    std::string p = std::string("light.") + ZoneName(i) + ".";
    k.SetB(p + "power", light_[i].power);
    k.Set(p + "color", ColorStr(Rgb{light_[i].r, light_[i].g, light_[i].b}));
    k.SetI(p + "bright", light_[i].bright);
    if (i == 0) {
      static const char* ef[5] = {"static", "breathing", "cycle", "temp", "pulse"};
      k.Set(p + "effect", ef[Clamp(light_[i].effect, 0, 4)]);
      k.SetI(p + "speed", light_[i].speed);
    }
  }
  k.SetI("light.idle", cfg_.Int("cfg.light.idle", 60));
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
    return Ok();
  }
  ColorSettings c;
  c.brightness = static_cast<int>(cfg_.Int("cfg.disp.c.bright", 50));
  c.contrast = static_cast<int>(cfg_.Int("cfg.disp.c.contrast", 50));
  c.gamma = static_cast<int>(cfg_.Int("cfg.disp.c.gamma", 100));
  c.kelvin = static_cast<int>(cfg_.Int("cfg.disp.c.kelvin", 6500));
  c.r = static_cast<int>(cfg_.Int("cfg.disp.c.r", 100));
  c.g = static_cast<int>(cfg_.Int("cfg.disp.c.g", 100));
  c.b = static_cast<int>(cfg_.Int("cfg.disp.c.b", 100));
  std::string mode = cfg_.Str("cfg.disp.mode", "standard");
  if (key == "disp.mode") {
    static const struct { const char* n; int br, co, ga, k, r, g, b; } kPre[] = {
        {"standard", 50, 50, 100, 6500, 100, 100, 100}, {"vibrant", 52, 60, 95, 6500, 100, 100, 100}, {"cinema", 46, 56, 110, 6200, 100, 98, 96},
        {"internet", 50, 50, 100, 6500, 100, 100, 100},  {"photo", 50, 52, 100, 6500, 100, 100, 100},   {"video", 52, 54, 105, 6500, 100, 100, 100},
        {"lowblue", 50, 50, 100, 4500, 100, 100, 100}};
    const SettingDef* d = FindSetting("disp.mode");
    mode = EnumName(*d, value);
    for (auto& p : kPre)
      if (mode == p.n) { c.brightness = p.br; c.contrast = p.co; c.gamma = p.ga; c.kelvin = p.k; c.r = p.r; c.g = p.g; c.b = p.b; }
  } else {
    if (key == "disp.c.bright") c.brightness = value;
    else if (key == "disp.c.contrast") c.contrast = value;
    else if (key == "disp.c.gamma") c.gamma = value;
    else if (key == "disp.c.kelvin") c.kelvin = value;
    else if (key == "disp.c.r") c.r = value;
    else if (key == "disp.c.g") c.g = value;
    else if (key == "disp.c.b") c.b = value;
    mode = "custom";
  }
  if (!hw_.display->SetColor(c, &why)) return Fail("failed.hw", why);
  cfg_.SetI("cfg.disp.c.bright", c.brightness); cfg_.SetI("cfg.disp.c.contrast", c.contrast); cfg_.SetI("cfg.disp.c.gamma", c.gamma);
  cfg_.SetI("cfg.disp.c.kelvin", c.kelvin); cfg_.SetI("cfg.disp.c.r", c.r); cfg_.SetI("cfg.disp.c.g", c.g); cfg_.SetI("cfg.disp.c.b", c.b);
  cfg_.Set("cfg.disp.mode", mode);
  SaveConfig();
  return Ok();
}

Result Controller::SetDevice(const std::string& key, bool on) {
  if (!hw_.devices) return Fail("unsupported", "no device interface");
  Dev d = key == "dev.wifi" ? Dev::Wifi : key == "dev.bt" ? Dev::Bluetooth : key == "dev.cam" ? Dev::Webcam : Dev::Touchpad;
  std::string why;
  if (!hw_.devices->Set(d, on, &why)) return Fail("failed.hw", why);
  if (hw_.clock) hw_.clock->SleepMs(300);
  if (hw_.devices->Get(d) != (on ? 1 : 0)) return Fail("reverted", "the device did not change state");
  return Ok();
}

// ------------------------------------------------------------------------------------------------ lighting
void Controller::LightApply() {
  if (!hw_.light) return;
  for (int z : {0, 2, 3}) {
    LightZoneState& s = light_[z];
    if (!s.present) continue;
    uint8_t rep[9];
    auto send = [&](std::initializer_list<uint8_t> b) {
      std::fill(rep, rep + 9, 0);
      size_t i = 0;
      for (uint8_t x : b) rep[i++] = x;
      hw_.light->Send(ZoneEnum(z), rep);
    };
    const uint8_t zn = static_cast<uint8_t>(z), zc = ZoneCmd(z);
    if (!s.power) {
      if (z == 0) send({0x00, 0x09, 0x02, 0, 0, 0, 0, 0, 0});
      send({0x00, 0x12, 0x00, 0x03, 0, 0, 0, 0, 0});
      send({0x00, 0x08, 0x05, 0, 0, 0, 0, 0, 0});
      send({0x00, 0x08, 0x01, zn, 0, 0, 0, 0, 0});
      send({0x00, 0x1A, zn, 0x00, 0x00, 0, 0, 0, 0x01});
      continue;
    }
    send({0x00, 0x1A, zn, 0x01, 0x04, 0, 0, 0, 0x01});
    send({0x00, 0x14, zn, 0x01, s.r, s.g, s.b, 0, 0x00});
    send({0x00, 0x08, zc, 0x01, static_cast<uint8_t>(z == 0 ? 0x07 : 0x01), BrightHid(z, s.bright), static_cast<uint8_t>(z == 0 ? 0x08 : 0x01), 0, 0x00});
    lastLightRgb_[z][0] = s.r; lastLightRgb_[z][1] = s.g; lastLightRgb_[z][2] = s.b;
  }
}

void Controller::LightTick() {
  if (!hw_.light) return;
  LightZoneState& s = light_[0];
  if (!s.present || !s.power || s.effect == 0) return;
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  const double period = 12000.0 / (s.speed * 1.0 + 1.0);  // ms for one cycle: speed 1 ~6 s, speed 10 ~1.1 s
  double ph = std::fmod(static_cast<double>(now), period) / period;
  Rgb base{s.r, s.g, s.b}, out = base;
  double f = 1.0;
  switch (s.effect) {
    case 1: f = (1.0 - std::cos(ph * 6.283185307)) * 0.5; break;                 // breathing
    case 2: out = Hsv(ph * 360.0, 1.0, 1.0); break;                              // colour cycle
    case 3: {                                                                     // CPU temperature gradient
      long long t = cfg_.Int("cfg.light.tempnow", 0);
      KV snap = Snapshot();
      t = snap.Int("cpu.temp", t);
      out = Hsv(Clamp((95.0 - static_cast<double>(t)) / 55.0, 0.0, 1.0) * 120.0, 1.0, 1.0);
      break;
    }
    case 4: f = ph < 0.35 ? 1.0 - ph / 0.35 : 0.0; break;                         // pulse
    default: break;
  }
  Rgb c{static_cast<uint8_t>(out.r * f), static_cast<uint8_t>(out.g * f), static_cast<uint8_t>(out.b * f)};
  if (std::abs(c.r - lastLightRgb_[0][0]) + std::abs(c.g - lastLightRgb_[0][1]) + std::abs(c.b - lastLightRgb_[0][2]) < 6) return;
  uint8_t rep[9] = {0x00, 0x14, 0x00, 0x01, c.r, c.g, c.b, 0x00, 0x00};
  hw_.light->Send(LightZone::Keyboard, rep);
  lastLightRgb_[0][0] = c.r; lastLightRgb_[0][1] = c.g; lastLightRgb_[0][2] = c.b;
}

Result Controller::SetLight(const std::string& key, const std::string& raw) {
  if (key == "light.idle") {
    cfg_.Set("cfg.light.idle", raw);
    SaveConfig();
    return Ok();
  }
  std::string prop;
  int z = ZoneIndex(key, &prop);
  if (z < 0 || !light_[z].present || !hw_.light) return Fail("unsupported", "lighting zone not present");
  LightZoneState& s = light_[z];
  const std::string pfx = std::string("cfg.light.") + ZoneName(z) + ".";
  if (prop == "power") s.power = raw == "1";
  else if (prop == "color") {
    Rgb c;
    if (!ParseColor(raw, &c)) return Fail("range", "bad colour");
    s.r = c.r; s.g = c.g; s.b = c.b;
  } else if (prop == "bright") s.bright = std::atoi(raw.c_str());
  else if (prop == "effect") {
    const SettingDef* d = FindSetting(key);
    s.effect = EnumIndex(*d, raw);
  } else if (prop == "speed") s.speed = std::atoi(raw.c_str());
  cfg_.Set(pfx + prop, prop == "effect" ? Fmt("%d", s.effect) : raw);
  SaveConfig();
  LightApply();
  return Ok();
}

// ------------------------------------------------------------------------------------------------ profiles
KV Controller::CaptureProfileValues() {
  KV snap = Snapshot();
  KV out;
  static const char* keys[] = {"ec.pl1", "ec.pl2", "ec.pl4", "ec.tcc", "gpu.ctgp", "fan.mode", "os.boost", "os.epp", "os.min", "os.max", "os.park", "batt.mode"};
  for (const char* k : keys)
    if (snap.Has(k) && snap.Str(k) != "") out.Set(k, snap.Str(k));
  if (cfg_.Has("cfg.fan.cpu")) out.Set("fan.cpu.curve", cfg_.Str("cfg.fan.cpu"));
  if (cfg_.Has("cfg.fan.gpu")) out.Set("fan.gpu.curve", cfg_.Str("cfg.fan.gpu"));
  return out;
}

Result Controller::SaveProfile(const std::string& name) {
  std::string n = Trim(name);
  if (n.empty() || n.size() > 32) return Fail("range", "profile name must be 1..32 characters");
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_')) return Fail("range", "profile names may contain letters, digits, space, - and _");
  profiles_[n] = CaptureProfileValues();
  activeProfile_ = n;
  SaveProfiles();
  SyncSnapshot();
  return Ok();
}

Result Controller::DeleteProfile(const std::string& name) {
  for (const char* b : {"Quiet", "Balanced", "Enthusiast", "Overboost"})
    if (name == b) return Fail("range", "built-in profiles cannot be deleted (edit and save them under another name)");
  if (!profiles_.erase(name)) return Fail("unknown", "no such profile");
  if (activeProfile_ == name) activeProfile_.clear();
  SaveProfiles();
  SyncSnapshot();
  return Ok();
}

Result Controller::ApplyProfile(const std::string& name, const KV& args) {
  auto it = profiles_.find(name);
  if (it == profiles_.end()) return Fail("unknown", "no such profile: " + name);
  KV prof = it->second;
  Result agg;
  agg.ok = true;
  agg.code = "ok";
  int applied = 0, failed = 0, skipped = 0;
  std::string firstFail;
  auto note = [&](const std::string& k, const Result& r) {
    if (r.ok) { applied++; return; }
    if (r.code == "unsupported" || r.code == "gate.experimental" || r.code == "gate.model") { skipped++; agg.data.Set("skipped." + k, r.code); return; }
    failed++;
    agg.data.Set("failed." + k, r.code);
    if (firstFail.empty()) firstFail = r.code;
    if (r.code == "gate.consent") agg.data.Set("consent", r.data.Str("consent"));
  };
  // CPU power as one group so the PL1 <= PL2 <= PL4 ordering can never be violated half-way
  if (prof.Has("ec.pl1") || prof.Has("ec.pl2") || prof.Has("ec.pl4")) {
    KV snap = Snapshot();
    int p1 = static_cast<int>(prof.Int("ec.pl1", snap.Int("ec.pl1", 0)));
    int p2 = static_cast<int>(prof.Int("ec.pl2", snap.Int("ec.pl2", 0)));
    int p4 = static_cast<int>(prof.Int("ec.pl4", snap.Int("ec.pl4", 0)));
    const SettingDef* d = FindSetting("ec.pl1");
    Result gate = CapOk("pl.ec") ? Gate(*d) : Fail("unsupported", "pl.ec");
    Result r = gate.ok ? WriteEcTriple(p1, p2, p4) : gate;
    if (r.ok) { desired_.SetI("ec.pl1", p1); desired_.SetI("ec.pl2", p2); desired_.SetI("ec.pl4", p4); }
    note("ec.power", r);
  }
  static const char* order[] = {"ec.tcc", "gpu.ctgp", "fan.cpu.curve", "fan.gpu.curve", "fan.mode", "batt.mode", "os.boost", "os.epp", "os.min", "os.max", "os.park"};
  for (const char* k : order) {
    if (!prof.Has(k)) continue;
    Result r = DoSet(k, prof.Str(k), args);
    note(k, r);
  }
  activeProfile_ = name;
  SaveProfiles();
  SaveDesired();
  SyncSnapshot();
  if (failed == 0) {
    agg.msg = Fmt("%d applied, %d skipped", applied, skipped);
    return agg;
  }
  agg.ok = false;
  agg.code = applied ? "partial" : firstFail;
  agg.msg = Fmt("%d applied, %d failed, %d skipped", applied, failed, skipped);
  lastError_ = agg.code + ": " + agg.msg;
  return agg;
}

// ------------------------------------------------------------------------------------------------ application profiles
void Controller::AppProfileTick() {
  const uint64_t now = hw_.clock ? hw_.clock->NowMs() : 0;
  if (now - lastAppScan_ < 3000) return;
  lastAppScan_ = now;
  if (!hw_.power) return;
  struct Map { std::string exe, profile; };
  std::vector<Map> maps;
  for (int i = 0; i < 32; i++) {
    std::string e = cfg_.Str(Fmt("cfg.app.%d.exe", i)), p = cfg_.Str(Fmt("cfg.app.%d.profile", i));
    if (!e.empty() && !p.empty()) maps.push_back({Lower(e), p});
  }
  if (maps.empty() && appRunning_.empty()) return;
  std::vector<std::string> run = hw_.power->RunningExes();
  std::string hit, hitProfile;
  for (auto& m : maps)
    if (std::find(run.begin(), run.end(), m.exe) != run.end()) { hit = m.exe; hitProfile = m.profile; break; }
  if (!hit.empty() && hit != appRunning_) {
    if (appRunning_.empty()) profileBeforeApp_ = activeProfile_;
    appRunning_ = hit;
    L(mgp::LogLevel::Info, "application profile: " + hit + " -> " + hitProfile);
    ApplyProfile(hitProfile, KV());
  } else if (hit.empty() && !appRunning_.empty()) {
    L(mgp::LogLevel::Info, "application profile: " + appRunning_ + " closed");
    appRunning_.clear();
    if (!profileBeforeApp_.empty()) ApplyProfile(profileBeforeApp_, KV());
    profileBeforeApp_.clear();
  }
}

// ------------------------------------------------------------------------------------------------ OEM coexistence
Result Controller::OemTakeover(bool takeover) {
  if (!hw_.oem) return Fail("unsupported", "no service interface");
  std::string why;
  if (takeover) {
    if (!hw_.oem->StopAndDisable(&why)) return Fail("failed.hw", "could not stop the OEM service: " + why);
    cfg_.Set("cfg.takeover", "1");
    L(mgp::LogLevel::Info, "OEM services stopped and disabled (reversible)");
  } else {
    if (!hw_.oem->EnableAndStart(&why)) return Fail("failed.hw", "could not restore the OEM service: " + why);
    cfg_.Set("cfg.takeover", "0");
    L(mgp::LogLevel::Info, "OEM services restored");
  }
  SaveConfig();
  Probe();
  PollOnce(true);
  return Ok();
}

}  // namespace bc
