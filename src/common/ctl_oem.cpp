// Controller: the vendor's Control Center service. Everything that changes a profile value, a fan setting, the charge mode, a hardware
// switch or the keyboard lighting is a message to that service (src/oem/proto.cpp). This file turns the UI's typed settings into those
// messages, waits for the service to report the result and, wherever the embedded controller shows it, checks the EC as well, so a value
// is only called "applied" when the machine really holds it.
//
// What this file never does: write to the EC itself, touch the firmware's variable store, or send a key that is not on the allow-list in
// proto.cpp. The only EC writes of this program are the GPU power engine's two cTGP registers (ctl_gpu.cpp).
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

#include "controller.h"
#include "hostdata.h"
#include "i18n_mark.h"
#include "registers.h"
#include "util.h"

namespace bc {

namespace {

uint64_t RealMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ------------------------------------------------------------------------------------------------ status field access
const std::string kFan = "oem.fan.";

bool StatInt(const KV& st, const std::string& field, long long* out) {
  const std::string k = kFan + field;
  if (!st.Has(k)) return false;
  const std::string v = st.Str(k);
  if (v.empty()) return false;
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (!end || *end) return false;
  *out = static_cast<long long>(std::llround(d));
  return true;
}

// ------------------------------------------------------------------------------------------------ the typed profile values
struct PerfDef {
  const char* key;       // registry key; also the snapshot key of the live value
  const char* detail;    // SET_OPERATING_MODE_DETAIL key
  const char* detailOc;  // used instead while the profile's overclocking switch is on (nullptr = same key)
  const char* field;     // Fan/Status field that holds the value
  const char* fieldOc;
  const char* fmin;      // Fan/Status fields with the limits the service allows ("" = none stated)
  const char* fmax;
  const char* fminOc;
  const char* fmaxOc;
};

const PerfDef kPerf[] = {
    {"perf.pl1", "PL1", nullptr, "CPU_PL1", nullptr, "CPU_PL1Minimum", "CPU_PL1Maximum", nullptr, nullptr},
    {"perf.pl2", "PL2", nullptr, "CPU_PL2", nullptr, "CPU_PL2Minimum", "CPU_PL2Maximum", nullptr, nullptr},
    {"perf.pl4", "PL4", nullptr, "CPU_PL4", nullptr, "CPU_PL4Minimum", "CPU_PL4Maximum", nullptr, nullptr},
    {"perf.tcc", "CpuTccOffset", nullptr, "CPU_TccOffset", nullptr, "", "CPU_TccOffsetMaximum", nullptr, nullptr},
    {"perf.gpu.core", "GpuCoreClockOffset", "GpuCoreClockOffsetOC", "GPU_CoreClockOffset", "GPU_CoreClockOffsetOC", "GPU_CoreClockOffsetMinimum",
     "GPU_CoreClockOffsetMaximum", "GPU_CoreClockOffsetMinimum", "GPU_CoreClockOffsetMaximumHWOC"},
    {"perf.gpu.mem", "GpuMemoryClockOffset", "GpuMemoryClockOffsetOC", "GPU_MemoryClockOffset", "GPU_MemoryClockOffsetOC",
     "GPU_MemoryClockOffsetMinimum", "GPU_MemoryClockOffsetMaximum", "GPU_MemoryClockOffsetMinimumHWOC", "GPU_MemoryClockOffsetMaximumHWOC"},
    {"perf.gpu.temp", "GpuTargetTemperature", nullptr, "GPU_TargetTemperature", nullptr, "GPU_TargetTemperatureMinimum", "GPU_TargetTemperatureMaximum",
     nullptr, nullptr},
};

const PerfDef* FindPerf(const std::string& key) {
  for (const PerfDef& p : kPerf)
    if (key == p.key) return &p;
  return nullptr;
}

bool OcOn(const KV& st) {
  long long v = 0;
  return StatInt(st, "OverClockingSwitch", &v) && v == 1;
}

// detail key -> the Fan/Status field that shows it, for the echo check
const char* FieldOfDetail(const std::string& key) {
  static const struct { const char* detail; const char* field; } kMap[] = {
      {"PL1", "CPU_PL1"}, {"PL2", "CPU_PL2"}, {"PL4", "CPU_PL4"}, {"CpuTccOffsetSwitch", "CPU_TccOffsetSwitch"}, {"CpuTccOffset", "CPU_TccOffset"},
      {"GpuCoreClockOffset", "GPU_CoreClockOffset"}, {"GpuCoreClockOffsetOC", "GPU_CoreClockOffsetOC"},
      {"GpuMemoryClockOffset", "GPU_MemoryClockOffset"}, {"GpuMemoryClockOffsetOC", "GPU_MemoryClockOffsetOC"},
      {"GpuTargetTemperature", "GPU_TargetTemperature"}};
  for (const auto& m : kMap)
    if (key == m.detail) return m.field;
  return nullptr;
}

const char* const kModeNames[4] = {"Balanced", "Enthusiast", "Overboost", "Customized"};

// ------------------------------------------------------------------------------------------------ keyboard state
struct KbNow {
  bool ok = false;
  bool power = true;
  int effect = 0, light = 3, speed = 2, direction = -1, blocks = 0, background = 0;
  bool circular = false;
  std::vector<oem::msg::KbColor> colors;
};

KbNow ReadKb(const KV& st) {
  KbNow k;
  k.ok = st.Has("oem.kb.effect") || st.Has("oem.kb.color.ColorBlocks") || st.Has("oem.kb.color.ColorBuffer.#");
  if (!k.ok) return k;
  k.effect = static_cast<int>(st.Int("oem.kb.effect", 0));
  k.light = static_cast<int>(st.Has("oem.kb.light") ? st.Int("oem.kb.light", 3) : st.Int("oem.kb.brightNess", 3));
  k.speed = static_cast<int>(st.Int("oem.kb.speed", 2));
  k.direction = static_cast<int>(st.Int("oem.kb.direction", -1));
  k.background = static_cast<int>(st.Int("oem.kb.backgroundcolor", 0));
  k.circular = st.Bool("oem.kb.color.isCircular", false);
  // RGBKB_PowerStatus: even values are "off", odd values "on"
  if (st.Has("oem.kb.powerStatus")) k.power = (st.Int("oem.kb.powerStatus", 1) & 1) != 0;
  else if (st.Has("oem.kb.powerstatus")) k.power = (st.Int("oem.kb.powerstatus", 1) & 1) != 0;
  const int n = static_cast<int>(st.Int("oem.kb.color.ColorBuffer.#", 0));
  for (int i = 0; i < n && i < 16; i++) {
    oem::msg::KbColor c;
    const std::string p = Fmt("oem.kb.color.ColorBuffer.%d.", i);
    c.r = static_cast<int>(st.Int(p + "R", 255));
    c.g = static_cast<int>(st.Int(p + "G", 255));
    c.b = static_cast<int>(st.Int(p + "B", 255));
    k.colors.push_back(c);
  }
  k.blocks = static_cast<int>(st.Int("oem.kb.color.ColorBlocks", static_cast<long long>(k.colors.size())));
  if (k.blocks <= 0) k.blocks = static_cast<int>(k.colors.size());
  if (k.blocks <= 0) k.blocks = 1;
  if (k.blocks > 16) k.blocks = 16;
  return k;
}

std::string Hex6(const oem::msg::KbColor& c) {
  return Fmt("#%02x%02x%02x", std::max(0, std::min(255, c.r)), std::max(0, std::min(255, c.g)), std::max(0, std::min(255, c.b)));
}

bool ParseHex6(const std::string& in, oem::msg::KbColor* out) {
  const std::string s = Trim(in);
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (size_t i = 1; i < 7; i++) {
    const char ch = s[i];
    int d;
    if (ch >= '0' && ch <= '9') d = ch - '0';
    else if (ch >= 'a' && ch <= 'f') d = 10 + ch - 'a';
    else if (ch >= 'A' && ch <= 'F') d = 10 + ch - 'A';
    else return false;
    v = (v << 4) | static_cast<unsigned>(d);
  }
  out->r = static_cast<int>((v >> 16) & 0xFF);
  out->g = static_cast<int>((v >> 8) & 0xFF);
  out->b = static_cast<int>(v & 0xFF);
  return true;
}

// the fan table the service holds, as the packed image the EC uses; false when the service has not sent it
bool TableFromState(const KV& st, const char* part, fan::TableImage* out) {
  for (int i = 0; i < fan::kSlots; i++) {
    const std::string t = Fmt("T%d", i);
    const std::string b = std::string("oem.table.") + part + ".";
    if (!st.Has(b + "Duty." + t) || !st.Has(b + "UpT." + t) || !st.Has(b + "DownT." + t)) return false;
    out->duty[i] = static_cast<uint8_t>(std::max<long long>(0, std::min<long long>(255, st.Int(b + "Duty." + t))));
    out->up[i] = static_cast<uint8_t>(std::max<long long>(0, std::min<long long>(255, st.Int(b + "UpT." + t))));
    out->down[i] = static_cast<uint8_t>(std::max<long long>(0, std::min<long long>(255, st.Int(b + "DownT." + t))));
  }
  return true;
}

bool SameTable(const fan::TableImage& a, const fan::TableImage& b) {
  return std::memcmp(a.up, b.up, sizeof a.up) == 0 && std::memcmp(a.down, b.down, sizeof a.down) == 0 && std::memcmp(a.duty, b.duty, sizeof a.duty) == 0;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ plumbing
std::string Controller::OemValue(const std::string& field) const {
  if (!hw_.oemLink) return "";
  return hw_.oemLink->State().Str(kFan + field);
}

int Controller::OemOperatingMode() const {
  if (!hw_.oemLink) return -1;
  long long v = -1;
  return StatInt(hw_.oemLink->State(), "OperatingMode", &v) ? static_cast<int>(v) : -1;
}

bool Controller::WaitOem(const std::function<bool()>& done, uint32_t timeoutMs, uint32_t refreshAfterMs) {
  if (done()) return true;
  const uint64_t t0 = RealMs();
  bool refreshed = refreshAfterMs == 0 || !hw_.oemLink;
  for (;;) {
    const uint64_t el = RealMs() - t0;
    if (el >= timeoutMs) return done();
    if (!refreshed && el >= refreshAfterMs) {
      hw_.oemLink->RequestAll();   // the service may not republish on its own: ask once
      refreshed = true;
    }
    const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(100, timeoutMs - el));
    if (hw_.oemLink) hw_.oemLink->WaitChange(hw_.oemLink->Version(), slice);
    else if (hw_.clock) hw_.clock->SleepMs(slice);
    if (done()) return true;
  }
}

Result Controller::OemSend(const oem::Msg& m) {
  // every message to the service leaves from here, so the kill switch is enforced once, for every caller (settings, profile switches,
  // application profiles, restore commands)
  if (cfg_.Bool("cfg.readonly", false)) return Fail("gate.readonly", N_("read-only mode (kill switch) is on"));
  if (!hw_.oemLink) return Fail("needs.service", N_("the link to the vendor's Control Center service is not available in this build"));
  if (!hw_.oemLink->Connected()) {
    return Fail("needs.service", N_("the vendor's Control Center service is not reachable (see Service)"));
  }
  if (!hw_.oemLink->Publish(m)) return Fail("needs.service", N_("the vendor's Control Center service is not reachable (see Service)"));
  return Ok();
}

// ------------------------------------------------------------------------------------------------ probe / poll
void Controller::ProbeOem() {
  if (!hw_.oemLink) {
    SetCap("oem", "absent", N_("no link to the vendor's service"));
    return;
  }
  hw_.oemLink->RequestAll();
}

void Controller::PollOem() {
  KV k;
  k.Set("profile.list", "Balanced,Enthusiast,Overboost,Customized");
  if (!hw_.oemLink) {
    k.Set("oem.link.state", "none");
    k.SetB("oem.up", false);
    std::lock_guard<std::mutex> g(snapMu_);
    sOem_ = k;
    return;
  }
  const KV st = hw_.oemLink->State();
  const KV info = hw_.oemLink->Info();
  oemSeen_ = hw_.oemLink->Version();
  const bool up = hw_.oemLink->Connected();
  for (const auto& p : info.All()) k.Set(p.first, p.second);
  for (const auto& p : st.All())
    if (!StartsWith(p.first, "oem.table.")) k.Set(p.first, p.second);   // the table is read straight from the link when it is needed
  k.SetB("oem.up", up);

  KV ec;
  {
    std::lock_guard<std::mutex> g(snapMu_);
    ec = sEc_;
  }

  // ---- the profile in use
  long long mode = -1;
  const bool haveMode = StatInt(st, "OperatingMode", &mode) && mode >= 0 && mode <= 3;
  k.SetI("perf.mode", haveMode ? mode : -1);
  if (haveMode) k.Set("profile.active", kModeNames[mode]);
  else k.Set("profile.active", "");
  long long boost = 0;
  if (StatInt(st, "FanBoostEnable", &boost)) k.SetB("fan.boost", boost != 0);
  std::string pname = st.Str(kFan + "ProfileName");
  if (!pname.empty()) k.Set("perf.profile.name", CleanText(pname, 32));
  k.SetB("perf.oc", OcOn(st));

  // ---- the values of that profile: live from the EC where it can be read, otherwise what the service reports
  bool havePl = false, haveTcc = false, haveGpu = false, haveGpuTemp = false;
  const bool oc = OcOn(st);
  for (const PerfDef& p : kPerf) {
    const char* f = oc && p.fieldOc ? p.fieldOc : p.field;
    const char* lo = oc && p.fminOc ? p.fminOc : p.fmin;
    const char* hi = oc && p.fmaxOc ? p.fmaxOc : p.fmax;
    long long v = 0, x = 0;
    const bool have = StatInt(st, f, &v);
    std::string key = p.key;
    bool shown = false;
    if (key == "perf.pl1" && ec.Has("ec.pl1")) { k.SetI(key, ec.Int("ec.pl1")); shown = true; }
    else if (key == "perf.pl2" && ec.Has("ec.pl2")) { k.SetI(key, ec.Int("ec.pl2")); shown = true; }
    else if (key == "perf.pl4" && ec.Has("ec.pl4")) { k.SetI(key, ec.Int("ec.pl4")); shown = true; }
    if (!shown && have) k.SetI(key, v);
    if (have) k.SetI(key + ".profile", v);   // what the profile stores (can differ from what runs when another program changed the EC)
    if (lo[0] && StatInt(st, lo, &x)) k.SetI(key + ".min", x);
    if (hi[0] && StatInt(st, hi, &x)) k.SetI(key + ".max", x);
    if (key == "perf.pl1") havePl = have || shown;
    if (key == "perf.tcc") haveTcc = have;
    if (key == "perf.gpu.core") haveGpu = have;
    if (key == "perf.gpu.temp") haveGpuTemp = have;
  }
  long long sw = 0;
  if (StatInt(st, "CPU_TccOffsetSwitch", &sw)) k.SetB("perf.tcc.on", sw != 0);
  {
    // the graphics driver reports the offsets that really run; they win over the stored values
    std::lock_guard<std::mutex> g(snapMu_);
    if (sGpu_.Has("nv.offset.core")) k.Set("perf.gpu.core", sGpu_.Str("nv.offset.core"));
    if (sGpu_.Has("nv.offset.mem")) k.Set("perf.gpu.mem", sGpu_.Str("nv.offset.mem"));
  }

  // ---- keyboard lighting
  const KbNow kb = ReadKb(st);
  if (kb.ok) {
    static const SettingDef* effDef = FindSetting("light.kbd.effect");
    static const SettingDef* dirDef = FindSetting("light.kbd.dir");
    k.SetB("light.kbd.power", kb.power);
    k.SetI("light.kbd.effect.id", kb.effect);
    k.Set("light.kbd.effect", effDef ? EnumName(*effDef, kb.effect) : "");
    k.SetI("light.kbd.bright", kb.light);
    k.SetI("light.kbd.speed", kb.speed);
    k.Set("light.kbd.dir", dirDef ? EnumName(*dirDef, kb.direction + 1) : "");
    k.SetI("light.kbd.blocks", kb.blocks);
    std::vector<std::string> hex;
    for (int i = 0; i < kb.blocks; i++) hex.push_back(Hex6(i < static_cast<int>(kb.colors.size()) ? kb.colors[static_cast<size_t>(i)] : oem::msg::KbColor()));
    k.Set("light.kbd.colors", Join(hex, ","));
    k.Set("light.kbd.color", hex.empty() ? "#ffffff" : hex[0]);
    k.Set("light.kbd.type", CleanText(st.Str("oem.kb.type"), 32));
  }
  // idle timer of the keyboard light: seconds, 0 = never (reported in Setting/Status)
  const bool haveTimer = st.Has("oem.set.CloseTimer");
  if (haveTimer) k.SetI("light.kbd.timer", st.Int("oem.set.CloseTimer", 0));

  // ---- capabilities that depend on the link. "offline": the service cannot be reached; "pending": reached, nothing reported yet.
  const bool anyStatus = st.Has("oem.fan.OperatingMode") || st.Has("oem.kb.effect");
  auto cap = [&](const char* name, bool ok, const char* absentWhy) {
    const char* want = !up ? "offline" : ok ? "ok" : anyStatus ? "absent" : "pending";   // whatever was reported earlier, nothing can be changed while the link is down
    if (CapState(name) == want) return;
    const char* why = !up ? N_("the vendor's Control Center service is not reachable (see Service)")
                          : ok ? "" : anyStatus ? absentWhy : N_("waiting for the vendor's Control Center service to report");
    SetCap(name, want, why);
  };
  cap("oem", up && anyStatus, "");
  cap("perf.pl", havePl && st.Has("oem.fan.CPU_PL1"), N_("the service does not report power limits for this profile"));
  cap("perf.tcc", haveTcc, N_("the service does not report a temperature offset"));
  cap("perf.gpu", haveGpu && st.Int("oem.fan.IsNvGpu", 1) != 0, N_("the service does not report GPU clock offsets"));
  cap("perf.gpu.temp", haveGpuTemp, N_("the service does not report a GPU temperature target"));
  cap("fan.ctl", st.Has("oem.fan.FanBoostEnable") || haveMode, N_("the service does not report fan control"));
  cap("fan.curve", st.Has("oem.table.Name") || st.Has("oem.table.CPU.Duty.T0"), N_("the service has not reported its fan table"));
  cap("batt.mode", st.Has("oem.batt.HealthProtectionStatus") || CapOk("ec.batt"), N_("battery protection is not reported"));
  cap("fnlock", CapOk("ec.fnlock"), N_("the EC does not show this switch, so a change cannot be confirmed"));
  cap("acboot", CapOk("ec.acboot"), N_("the EC does not show this switch, so a change cannot be confirmed"));
  cap("usbpower", CapOk("ec.usbpower"), N_("the EC does not show this switch, so a change cannot be confirmed"));
  cap("light.kbd", kb.ok, N_("the service does not report a keyboard backlight"));
  cap("light.kbd.timer", haveTimer, N_("the service does not report the keyboard idle timer"));
  // battery protection falls back to what the service says when the EC cannot be read
  if (!ec.Has("batt.mode") && st.Has("oem.batt.HealthProtectionStatus")) {
    const long long m = st.Int("oem.batt.HealthProtectionStatus", -1);
    if (m >= 0 && m <= 2) k.Set("batt.mode", m == 0 ? "highcap" : m == 1 ? "balanced" : "eco");
  }

  std::lock_guard<std::mutex> g(snapMu_);
  sOem_ = k;
}

// ------------------------------------------------------------------------------------------------ profile values
Result Controller::OemDetail(const std::vector<oem::msg::Detail>& kv, const std::string& settingKey) {
  oem::Msg m;
  if (!oem::msg::FanDetail(kv, &m)) return Fail("internal", "not an allowed profile value: " + settingKey);
  if (!hw_.oemLink || !hw_.oemLink->Connected()) return OemSend(m);   // reports needs.service
  const KV before = hw_.oemLink->State();
  struct Watch {
    std::string field;
    long long want;
  };
  std::vector<Watch> watch;
  for (const oem::msg::Detail& d : kv) {
    const char* f = FieldOfDetail(d.key);
    if (f && before.Has(kFan + f)) watch.push_back({f, d.value});
  }
  const long long mode = before.Has("oem.fan.OperatingMode") ? before.Int("oem.fan.OperatingMode", -1) : -1;
  // The vendor's ready-made modes (Office, Gaming, Turbo) keep their own stored values, and the original window only edits the Custom mode. Writing a
  // value into one of them could change that stored profile for good, so nothing is sent unless the Custom mode is the active one.
  // Fails closed: while the service has not said which mode is active, nothing is sent either.
  if (mode < 0) return Fail("failed.service", N_("the vendor service has not reported which performance mode is active"));
  if (mode != 3) return Fail("gate.mode", N_("the vendor's ready-made modes keep their own values; switch to the Custom mode first"));

  // what the EC should show afterwards (only the power limits are readable there)
  struct EcWant {
    uint16_t addr;
    long long want;
    bool pl4;
    std::string name;
  };
  std::vector<EcWant> ecw;
  for (const oem::msg::Detail& d : kv) {
    if (!std::strcmp(d.key, "PL1")) ecw.push_back({reg::kPl1, d.value, false, "PL1"});
    else if (!std::strcmp(d.key, "PL2")) ecw.push_back({reg::kPl2, d.value, false, "PL2"});
    else if (!std::strcmp(d.key, "PL4")) ecw.push_back({reg::kPl4, d.value, true, "PL4"});
  }
  const bool ecReadable = !ecw.empty() && ecConnected_ && CapOk("ec.pl");

  Result sent = OemSend(m);
  if (!sent.ok) return sent;

  if (!watch.empty()) {
    auto echoed = [&]() {
      const KV now = hw_.oemLink->State();
      for (const Watch& w : watch) {
        long long v = 0;
        if (!StatInt(now, w.field, &v) || v != w.want) return false;
      }
      return true;
    };
    if (!WaitOem(echoed, 3500, 1200)) return Fail("failed.service", N_("the vendor service did not report the new value"));
  }

  if (ecReadable) {
    int scale = 1;
    uint8_t b727 = 0;
    if (EcR(reg::kCustomProfile, &b727) && (b727 & 0x80)) scale = 2;
    std::string mismatch;
    auto ecOk = [&]() {
      mismatch.clear();
      for (const EcWant& e : ecw) {
        uint8_t v = 0;
        if (!EcR(e.addr, &v)) return true;   // cannot read any more: the service's report has to do
        const long long have = e.pl4 ? static_cast<long long>(v) * scale : v;
        if (std::llabs(have - e.want) > (e.pl4 ? scale - 1 : 0)) {
          mismatch = Fmt("%s: asked %lld W, the EC holds %lld W", e.name.c_str(), e.want, have);
          return false;
        }
      }
      return true;
    };
    if (!WaitOem(ecOk, 3500)) {
      return Fail("failed.verify", Fmt(N_("the service accepted the value but the EC still holds a different one (%s)"), mismatch.c_str()));
    }
    return Ok(N_("confirmed by the vendor service and the EC"));
  }
  return Ok(watch.empty() ? N_("sent to the vendor service") : N_("confirmed by the vendor service"));
}

Result Controller::SetPerf(const std::string& key, int value) {
  if (key == "perf.tcc.on") return OemDetail({{"CpuTccOffsetSwitch", value ? 1 : 0}}, key);
  const PerfDef* p = FindPerf(key);
  if (!p) return Fail("unsupported", "no handler for " + key);
  KV st;
  if (hw_.oemLink) st = hw_.oemLink->State();
  const bool oc = OcOn(st) && p->detailOc;
  const char* dk = oc ? p->detailOc : p->detail;
  // limits the service states for this profile
  long long lo = 0, hi = 0;
  const char* lof = oc && p->fminOc ? p->fminOc : p->fmin;
  const char* hif = oc && p->fmaxOc ? p->fmaxOc : p->fmax;
  if (lof[0] && StatInt(st, lof, &lo) && value < lo) return Fail("range", Fmt(N_("%s: %d is below the lowest value the service allows (%lld)"), key.c_str(), value, lo));
  if (hif[0] && StatInt(st, hif, &hi) && value > hi) return Fail("range", Fmt(N_("%s: %d is above the highest value the service allows (%lld)"), key.c_str(), value, hi));

  if (key == "perf.pl1" || key == "perf.pl2" || key == "perf.pl4") {
    // PL1 <= PL2 <= PL4, judged against what the machine runs now (a UI that changes several raises the upper one first)
    KV ec;
    {
      std::lock_guard<std::mutex> g(snapMu_);
      ec = sEc_;
    }
    auto cur = [&](const char* ecKey, const char* field) {
      long long v = 0;
      if (ec.Has(ecKey)) return ec.Int(ecKey);
      return StatInt(st, field, &v) ? v : -1LL;
    };
    const long long c1 = cur("ec.pl1", "CPU_PL1"), c2 = cur("ec.pl2", "CPU_PL2"), c4 = cur("ec.pl4", "CPU_PL4");
    if (key == "perf.pl1" && c2 >= 0 && value > c2) return Fail("range", Fmt(N_("PL1 (%d W) cannot be above PL2 (%lld W)"), value, c2));
    if (key == "perf.pl2" && ((c1 >= 0 && value < c1) || (c4 >= 0 && value > c4)))
      return Fail("range", Fmt(N_("PL2 (%d W) has to stay between PL1 (%lld W) and PL4 (%lld W)"), value, c1, c4));
    if (key == "perf.pl4" && c2 >= 0 && value < c2) return Fail("range", Fmt(N_("PL4 (%d W) cannot be below PL2 (%lld W)"), value, c2));
  }
  return OemDetail({{dk, value}}, key);
}

Result Controller::DetailRestore() {
  // Asks the service to reset the profile that is active, so the same rule as for every other profile value applies: only the Custom mode,
  // and not while the service has not said which mode that is.
  if (hw_.oemLink && hw_.oemLink->Connected()) {
    const KV st = hw_.oemLink->State();
    const long long mode = st.Has("oem.fan.OperatingMode") ? st.Int("oem.fan.OperatingMode", -1) : -1;
    if (mode < 0) return Fail("failed.service", N_("the vendor service has not reported which performance mode is active"));
    if (mode != 3) return Fail("gate.mode", N_("the vendor's ready-made modes keep their own values; switch to the Custom mode first"));
  }
  Result s = OemSend(oem::msg::FanRestoreDetail());
  if (!s.ok) return s;
  const uint64_t v0 = hw_.oemLink->Version();
  hw_.oemLink->WaitChange(v0, 2500);
  return Ok(N_("the vendor's default values were asked for"));
}

Result Controller::SwitchMode(int mode) {
  if (mode < 0 || mode > 3) return Fail("range", "unknown profile");
  const oem::msg::Mode mm = mode == 0 ? oem::msg::Mode::Office : mode == 1 ? oem::msg::Mode::Gaming : mode == 2 ? oem::msg::Mode::Turbo : oem::msg::Mode::Custom;
  Result s = OemSend(oem::msg::FanMode(mm, -1));
  if (!s.ok) return s;
  if (!WaitOem([&] { return OemOperatingMode() == mode; }, 4000, 1500)) return Fail("failed.service", N_("the vendor service did not switch the profile"));
  return Ok();
}

// ------------------------------------------------------------------------------------------------ fans
namespace {
// What the EC says about the fans (same rule as the live display): a fixed duty > boost bit > custom-table bits > auto.
bool EcFanMode(const std::function<bool(uint16_t, uint8_t*)>& rd, std::string* mode) {
  uint8_t m751 = 0, f1 = 0, f2 = 0, c5 = 0, c6 = 0;
  if (!rd(reg::kFanMode, &m751) || !rd(reg::kFanFixedCpu, &f1) || !rd(reg::kFanFixedGpu, &f2) || !rd(reg::kFanCurveEnable, &c5) ||
      !rd(reg::kFanCurveLatch, &c6))
    return false;
  *mode = (f1 & 0x80) || (f2 & 0x80) ? "fixed" : (m751 & 0x40) ? "boost" : ((c5 & 0x80) && (c6 & 0x04)) ? "custom" : "auto";
  return true;
}
}  // namespace

Result Controller::SetFanBoost(bool on) {
  Result s = OemSend(oem::msg::FanBoost(on));
  if (!s.ok) return s;
  auto done = [&]() {
    std::string m;
    if (ecConnected_ && CapOk("ec.fan") && EcFanMode([&](uint16_t a, uint8_t* v) { return EcR(a, v); }, &m)) return (m == "boost") == on;
    long long v = 0;
    return StatInt(hw_.oemLink->State(), "FanBoostEnable", &v) && (v != 0) == on;
  };
  if (!WaitOem(done, 4000, 1500)) return Fail("failed.verify", N_("fan boost did not change"));
  return Ok();
}

Result Controller::SetFanMode(int mode) {   // 0 auto, 1 boost, 2 custom curve
  if (!hw_.oemLink) return Fail("needs.service", N_("the link to the vendor's Control Center service is not available in this build"));
  const KV st = hw_.oemLink->State();
  auto ecMode = [&](std::string* m) {
    return ecConnected_ && CapOk("ec.fan") && EcFanMode([&](uint16_t a, uint8_t* v) { return EcR(a, v); }, m);
  };
  std::string now;
  const bool ecKnown = ecMode(&now);
  long long boostSvc = 0;
  StatInt(st, "FanBoostEnable", &boostSvc);
  const bool boostNow = ecKnown ? now == "boost" : boostSvc != 0;
  const bool customNow = ecKnown ? now == "custom" : st.Bool("oem.table.FanControlRespective", false);
  std::string table = st.Str("oem.fan.FAN_TableName");
  if (table.empty()) table = st.Str("oem.table.Name");
  if (mode == 2 && table.empty()) return Fail("failed.service", N_("the vendor service has not told which fan table is active"));
  if (!hw_.oemLink->Connected()) return OemSend(oem::msg::FanBoost(false));   // reports needs.service

  if (boostNow != (mode == 1)) {
    Result r = SetFanBoost(mode == 1);
    if (!r.ok) return r;
  }
  if (mode != 1) {
    // boost has priority over the custom table in the EC, so the custom flag is judged again once boost is off
    std::string after;
    const bool customNow2 = ecMode(&after) ? after == "custom" : customNow;
    if (customNow2 != (mode == 2)) {
      Result s = OemSend(oem::msg::FanCustomControl(table, mode == 2));
      if (!s.ok) return s;
    }
  }
  const char* want = mode == 0 ? "auto" : mode == 1 ? "boost" : "custom";
  std::string last;
  auto done = [&]() {
    if (ecMode(&last)) return last == want;
    const KV cur = hw_.oemLink->State();
    long long b = 0;
    StatInt(cur, "FanBoostEnable", &b);
    const bool cust = cur.Bool("oem.table.FanControlRespective", false);
    return mode == 1 ? b != 0 : (b == 0 && cust == (mode == 2));
  };
  if (!WaitOem(done, 4000, 1500)) {
    if (last == "fixed") return Fail("failed.verify", N_("another program holds the fans at a fixed speed; the fan mode did not change"));
    return Fail("failed.verify", N_("the fan mode did not change"));
  }
  return Ok();
}

Result Controller::SetFanCurve(bool cpu, const std::string& text) {
  if (!hw_.oemLink) return Fail("needs.service", N_("the link to the vendor's Control Center service is not available in this build"));
  fan::Curve c;
  std::string why;
  if (!fan::FromText(text, &c) || !fan::Validate(c, &why)) return Fail("range", why.empty() ? "malformed fan curve" : why);
  const fan::TableImage img = fan::Pack(c);
  KV st = hw_.oemLink->State();
  // The table that is sent back must be the one the profile uses right now: the name in the status says which, the table message
  // carries the content. When they disagree the table is asked for again once; a curve is never written into a table we have not seen.
  std::string name = st.Str("oem.fan.FAN_TableName");
  if (name.empty()) name = st.Str("oem.table.Name");
  if (name.empty()) return Fail("failed.service", N_("the vendor service has not told which fan table is active"));
  if (st.Str("oem.table.Name") != name) {
    hw_.oemLink->RequestAll();
    WaitOem([&] { return hw_.oemLink->State().Str("oem.table.Name") == name; }, 2500);
    st = hw_.oemLink->State();
    if (st.Str("oem.table.Name") != name) return Fail("failed.service", N_("the vendor service's fan table does not match the active one; try again"));
  }
  const std::string type = st.Str("oem.table.Type");
  // the other fan keeps the table the service has now; both go in one message
  fan::TableImage cpuImg, gpuImg;
  fan::TableImage* mine = cpu ? &cpuImg : &gpuImg;
  fan::TableImage* other = cpu ? &gpuImg : &cpuImg;
  *mine = img;
  bool haveOther = TableFromState(st, cpu ? "GPU" : "CPU", other);
  if (!haveOther) {
    KV snap;
    {
      std::lock_guard<std::mutex> g(snapMu_);
      snap = sEc_;
    }
    fan::Curve oc;
    if (fan::FromText(snap.Str(cpu ? "fan.gpu.curve" : "fan.cpu.curve"), &oc) && fan::Validate(oc, nullptr)) {
      *other = fan::Pack(oc);
      haveOther = true;
    }
  }
  Result s = OemSend(oem::msg::FanTableSet(name, type, cpu || haveOther ? &cpuImg : nullptr, !cpu || haveOther ? &gpuImg : nullptr));
  if (!s.ok) return s;
  auto echoed = [&]() {
    fan::TableImage got;
    return TableFromState(hw_.oemLink->State(), cpu ? "CPU" : "GPU", &got) && SameTable(got, img);
  };
  if (!WaitOem(echoed, 3500, 1200)) return Fail("failed.service", N_("the vendor service did not report the new fan table"));
  // the EC holds the table the fan really follows
  if (ecConnected_ && CapOk("ec.fan")) {
    const uint16_t base = cpu ? reg::kFanTableCpuUp : reg::kFanTableGpuUp;
    auto same = [&]() {
      for (int i = 0; i < fan::kSlots; i++) {
        uint8_t a = 0, b = 0, d = 0;
        if (!EcR(static_cast<uint16_t>(base + i), &a) || !EcR(static_cast<uint16_t>(base + 0x10 + i), &b) || !EcR(static_cast<uint16_t>(base + 0x20 + i), &d)) return true;
        if (a != img.up[i] || b != img.down[i] || d != img.duty[i]) return false;
      }
      return true;
    };
    if (!WaitOem(same, 3500)) return Fail("failed.verify", N_("the service reported the table but the EC still holds a different one"));
  }
  return Ok();
}

Result Controller::FanRestore() {
  Result s = OemSend(oem::msg::FanTableRestore());
  if (!s.ok) return s;
  hw_.oemLink->WaitChange(hw_.oemLink->Version(), 2500);
  return Ok(N_("the vendor's default fan curves were asked for"));
}

// ------------------------------------------------------------------------------------------------ battery and switches
Result Controller::SetBattMode(int mode) {
  Result s = OemSend(oem::msg::BatteryProfile(mode));
  if (!s.ok) return s;
  auto done = [&]() {
    uint8_t v = 0;
    if (ecConnected_ && CapOk("ec.batt") && EcR(reg::kBatteryMode, &v)) {
      const uint8_t m = v & 0x38;
      return m == (mode == 0 ? 0x08 : mode == 1 ? 0x18 : 0x28);
    }
    return hw_.oemLink->State().Int("oem.batt.HealthProtectionStatus", -1) == mode;
  };
  if (!WaitOem(done, 4000, 1500)) return Fail("failed.verify", N_("the charge profile did not change"));
  return Ok();
}

Result Controller::SetSwitchPref(const std::string& key, bool on) {
  struct Sw {
    const char* key;
    const char* onAction;
    const char* offAction;
    uint16_t addr;
    uint8_t mask;
    const char* name;
  };
  static const Sw kSw[] = {{"pref.fnlock", "FNKEY_LOCK", "FNKEY_UNLOCK", reg::kFnLock, 0x10, "Fn lock"},
                           {"pref.acboot", "ACRECOVERY_TOGGLE_ON", "ACRECOVERY_TOGGLE_OFF", reg::kAcAutoBoot, 0x08, "power on with AC"},
                           {"pref.usbpower", "USB_CHARGER_ON", "USB_CHARGER_OFF", reg::kUsbPowerShare, 0x10, "USB charging when off"}};
  const Sw* sw = nullptr;
  for (const Sw& s : kSw)
    if (key == s.key) sw = &s;
  if (!sw) return Fail("unsupported", "no handler for " + key);
  auto state = [&]() {   // -1 unreadable, 0 off, 1 on
    uint8_t v = 0;
    if (!EcR(sw->addr, &v)) return -1;
    return (v & sw->mask) ? 1 : 0;
  };
  if (state() < 0) return Fail("unsupported", N_("the EC does not show this switch, so a change cannot be confirmed"));
  // The vendor's action names do not say which way round they act, and that was not verified on this model: the first guess is the
  // natural one, and when the EC does not move the other action is tried once. What the EC then shows decides; the answer is remembered.
  const std::string polKey = std::string("cfg.pol.") + (key.c_str() + 5);
  bool inverted = cfg_.Bool(polKey, false);
  auto attempt = [&](bool inv) {
    const char* act = (on != inv) ? sw->onAction : sw->offAction;
    if (!oem::msg::SettingActionAllowed(act)) return false;
    Result s = OemSend(oem::msg::SettingAction(act));
    return s.ok;
  };
  if (state() == (on ? 1 : 0)) return Ok(N_("already so"));   // nothing is sent: with the polarity unknown a repeat could flip it
  if (!attempt(inverted)) return Fail("needs.service", N_("the vendor's Control Center service is not reachable (see Service)"));
  if (WaitOem([&] { return state() == (on ? 1 : 0); }, 3000)) return Ok();
  if (!attempt(!inverted)) return Fail("needs.service", N_("the vendor's Control Center service is not reachable (see Service)"));
  if (WaitOem([&] { return state() == (on ? 1 : 0); }, 3500)) {
    cfg_.Set(polKey, inverted ? "0" : "1");
    SaveConfig();
    return Ok(N_("done (the other action name did it; remembered)"));
  }
  return Fail("failed.verify", N_("the switch did not change in the EC"));
}

// ------------------------------------------------------------------------------------------------ keyboard lighting
Result Controller::SetKbd(const std::string& key, const std::string& raw) {
  if (!hw_.oemLink) return Fail("needs.service", N_("the link to the vendor's Control Center service is not available in this build"));
  const KV st = hw_.oemLink->State();
  KbNow cur = ReadKb(st);
  if (!cur.ok) return Fail("failed.service", N_("the vendor service has not reported the keyboard backlight"));
  if (key == "light.kbd.power") {
    const bool on = raw == "1";
    Result s = OemSend(oem::msg::KbPower(on));
    if (!s.ok) return s;
    if (!WaitOem([&] { return ReadKb(hw_.oemLink->State()).power == on; }, 3500, 1200)) return Fail("failed.service", N_("the vendor service did not confirm the keyboard backlight change"));
    return Ok();
  }
  KbNow want = cur;
  if (key == "light.kbd.effect") {
    const SettingDef* d = FindSetting(key);
    want.effect = d ? EnumIndex(*d, raw) : -1;
    if (want.effect < 0) return Fail("range", "unknown effect");
  } else if (key == "light.kbd.bright") {
    want.light = std::atoi(raw.c_str());
  } else if (key == "light.kbd.speed") {
    want.speed = std::atoi(raw.c_str());
  } else if (key == "light.kbd.dir") {
    const SettingDef* d = FindSetting(key);
    const int idx = d ? EnumIndex(*d, raw) : -1;
    if (idx < 0) return Fail("range", "unknown direction");
    want.direction = idx - 1;
  } else if (key == "light.kbd.color") {
    oem::msg::KbColor c;
    if (!ParseHex6(raw, &c)) return Fail("range", "color must be #rrggbb");
    want.colors.assign(static_cast<size_t>(want.blocks), c);
  } else if (key == "light.kbd.colors") {
    want.colors.resize(static_cast<size_t>(want.blocks));
    const std::vector<std::string> parts = Split(raw, ',');
    for (size_t i = 0; i < parts.size() && i < want.colors.size(); i++) {
      oem::msg::KbColor c;
      if (!ParseHex6(parts[i], &c)) return Fail("range", "color must be #rrggbb");
      want.colors[i] = c;
    }
  } else {
    return Fail("unsupported", "no handler for " + key);
  }
  if (want.colors.size() < static_cast<size_t>(want.blocks)) want.colors.resize(static_cast<size_t>(want.blocks));
  oem::msg::KbEffect e;
  e.mode = 1;
  e.effect = want.effect;
  e.light = want.light;
  e.speed = want.speed;
  e.direction = want.direction;
  e.circular = want.circular;
  e.background = want.background;
  e.colors = want.colors;
  Result s = OemSend(oem::msg::KbEffectSet(e));
  if (!s.ok) return s;
  auto echoed = [&]() {
    const KbNow now = ReadKb(hw_.oemLink->State());
    if (!now.ok || now.effect != want.effect || now.light != want.light || now.speed != want.speed || now.direction != want.direction) return false;
    for (size_t i = 0; i < want.colors.size() && i < now.colors.size(); i++)
      if (now.colors[i].r != want.colors[i].r || now.colors[i].g != want.colors[i].g || now.colors[i].b != want.colors[i].b) return false;
    return true;
  };
  if (!WaitOem(echoed, 3500, 1200)) return Fail("failed.service", N_("the vendor service did not confirm the keyboard backlight change"));
  return Ok();
}

// Idle timer: the light goes out after this many seconds without a key press (0 = never). The service keeps the value and runs the timer;
// the only way to know it took is what the service reports back, so that is what is waited for.
Result Controller::SetKbdTimer(int seconds) {
  if (!hw_.oemLink) return Fail("needs.service", N_("the link to the vendor's Control Center service is not available in this build"));
  if (!hw_.oemLink->State().Has("oem.set.CloseTimer")) return Fail("failed.service", N_("the vendor service has not reported the keyboard idle timer"));
  Result s = OemSend(oem::msg::SettingCloseTimer(seconds / 60, seconds % 60));
  if (!s.ok) return s;
  if (!WaitOem([&] { return hw_.oemLink->State().Int("oem.set.CloseTimer", -1) == seconds; }, 3500, 1200))
    return Fail("failed.service", N_("the vendor service did not confirm the idle timer"));
  return Ok();
}

// "Save to BIOS": the current lighting sent again with the flag that also keeps it in the keyboard's own memory.
Result Controller::KbdSave() {
  if (!hw_.oemLink) return Fail("needs.service", N_("the link to the vendor's Control Center service is not available in this build"));
  const KbNow cur = ReadKb(hw_.oemLink->State());
  if (!cur.ok) return Fail("failed.service", N_("the vendor service has not reported the keyboard backlight"));
  oem::msg::KbEffect e;
  e.mode = 1;
  e.effect = cur.effect;
  e.light = cur.light;
  e.speed = cur.speed;
  e.direction = cur.direction;
  e.circular = cur.circular;
  e.background = cur.background;
  e.colors = cur.colors;
  e.colors.resize(static_cast<size_t>(cur.blocks));
  e.save = true;
  return OemSend(oem::msg::KbEffectSet(e));
}

}  // namespace bc
