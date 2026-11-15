#include "settings_reg.h"

#include <cctype>
#include <cstdlib>

#include "fan.h"
#include "util.h"

namespace bc {
namespace {

#define INT(key, mn, mx, st, unit, ev, cap) {key, Kind::Int, mn, mx, st, unit, ev, cap, ""}
#define BOOL(key, ev, cap) {key, Kind::Bool, 0, 1, 1, "", ev, cap, ""}
#define ENUM(key, count, names, ev, cap) {key, Kind::Enum, 0, (count) - 1, 1, "", ev, cap, names}
#define TEXT(key, ev, cap) {key, Kind::Text, 0, 0, 0, "", ev, cap, ""}

// The static ranges are the outer limits the program will ever send; the vendor's service states the real limits of the active profile
// (Fan/Status "...Minimum" / "...Maximum"), and those are checked as well before anything is sent.
const std::vector<SettingDef> kTable = {
    // ---- values of the Custom mode (the vendor's service; sent only while that mode is active) -------------------
    INT("perf.pl1", 5, 250, 1, "W", Ev::Service, "perf.pl"),
    INT("perf.pl2", 5, 250, 1, "W", Ev::Service, "perf.pl"),
    INT("perf.pl4", 10, 500, 1, "W", Ev::Service, "perf.pl"),
    INT("perf.tcc", 0, 63, 1, "C", Ev::Service, "perf.tcc"),
    BOOL("perf.tcc.on", Ev::Service, "perf.tcc"),
    INT("perf.gpu.core", -1000, 1000, 1, "MHz", Ev::Service, "perf.gpu"),
    INT("perf.gpu.mem", -2000, 3000, 1, "MHz", Ev::Service, "perf.gpu"),
    INT("perf.gpu.temp", 60, 100, 1, "C", Ev::Service, "perf.gpu.temp"),
    // ---- GPU power beyond the vendor's profiles (this program's own engine) ---------------------------------------
    INT("gpu.ctgp", 125, 150, 5, "W", Ev::Verified, "gpu.ctgp"),
    // ---- fans ---------------------------------------------------------------------------------------------------
    ENUM("fan.mode", 3, "auto,boost,custom", Ev::Service, "fan.ctl"),
    TEXT("fan.cpu.curve", Ev::Service, "fan.curve"),
    TEXT("fan.gpu.curve", Ev::Service, "fan.curve"),
    // ---- battery / preferences ---------------------------------------------------------------------------------
    ENUM("batt.mode", 3, "highcap,balanced,eco", Ev::Service, "batt.mode"),
    BOOL("pref.fnlock", Ev::Service, "fnlock"),
    BOOL("pref.acboot", Ev::Service, "acboot"),
    BOOL("pref.usbpower", Ev::Service, "usbpower"),
    // ---- keyboard lighting (the vendor's service; effect numbers are the vendor's own RGBKB_Effect ids) ------------
    BOOL("light.kbd.power", Ev::Service, "light.kbd"),
    ENUM("light.kbd.effect", 18, "static,breathing,wave,reactive,rainbow,ripple,raindrop,neon,marquee,stack,impact,spark,aurora,music,user,gaming,flash,mix",
         Ev::Service, "light.kbd"),
    INT("light.kbd.bright", 0, 4, 1, "", Ev::Service, "light.kbd"),
    INT("light.kbd.speed", 0, 4, 1, "", Ev::Service, "light.kbd"),
    ENUM("light.kbd.dir", 7, "none,left-right,right-left,down-up,up-down,press,sync", Ev::Service, "light.kbd"),
    TEXT("light.kbd.color", Ev::Service, "light.kbd"),     // one colour for every block
    TEXT("light.kbd.colors", Ev::Service, "light.kbd"),    // one colour per block, comma separated
    INT("light.kbd.timer", 0, 3600, 1, "s", Ev::Service, "light.kbd.timer"),   // idle timer: the light goes out after this many seconds without a key press (0 = never)
    // ---- display (Windows API) ----------------------------------------------------------------------------------
    INT("disp.bright", 0, 100, 1, "%", Ev::Os, "disp.bright"),
    INT("disp.refresh", 30, 480, 1, "Hz", Ev::Os, "disp.refresh"),
    INT("disp.c.bright", 0, 100, 1, "", Ev::Os, "disp.color"),
    INT("disp.c.contrast", 0, 100, 1, "", Ev::Os, "disp.color"),
    INT("disp.c.gamma", 50, 260, 1, "", Ev::Os, "disp.color"),
    INT("disp.c.kelvin", 3000, 9000, 50, "K", Ev::Os, "disp.color"),
    INT("disp.c.r", 50, 100, 1, "%", Ev::Os, "disp.color"),
    INT("disp.c.g", 50, 100, 1, "%", Ev::Os, "disp.color"),
    INT("disp.c.b", 50, 100, 1, "%", Ev::Os, "disp.color"),
    INT("disp.c.sat", 0, 200, 1, "%", Ev::Os, "disp.sat"),
    ENUM("disp.mode", 7, "standard,vibrant,cinema,internet,photo,video,lowblue", Ev::Os, "disp.color"),
    // ---- device switches -----------------------------------------------------------------------------------------
    BOOL("dev.wifi", Ev::Os, "dev.wifi"),
    BOOL("dev.bt", Ev::Os, "dev.bt"),
    BOOL("dev.cam", Ev::Os, "dev.cam"),
    BOOL("dev.touchpad", Ev::Os, "dev.touchpad"),
};

bool IsHexColor(const std::string& s) {
  if (s.size() != 7 || s[0] != '#') return false;
  for (size_t i = 1; i < 7; i++)
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

}  // namespace

const std::vector<SettingDef>& AllSettings() { return kTable; }

const SettingDef* FindSetting(const std::string& key) {
  for (const SettingDef& d : kTable)
    if (key == d.key) return &d;
  return nullptr;
}

int EnumCount(const SettingDef& d) {
  if (d.kind != Kind::Enum) return 0;
  return static_cast<int>(Split(d.enumNames, ',').size());
}

int EnumIndex(const SettingDef& d, const std::string& name) {
  auto v = Split(d.enumNames, ',');
  for (size_t i = 0; i < v.size(); i++)
    if (v[i] == name) return static_cast<int>(i);
  return -1;
}

std::string EnumName(const SettingDef& d, int idx) {
  auto v = Split(d.enumNames, ',');
  if (idx < 0 || idx >= static_cast<int>(v.size())) return "";
  return v[idx];
}

const char* EvName(Ev e) {
  switch (e) {
    case Ev::Verified: return "verified";
    case Ev::Service: return "service";
    case Ev::Os: return "os";
  }
  return "?";
}

Validated ValidateSetting(const SettingDef& d, const std::string& raw) {
  Validated v;
  auto no = [&](const std::string& w) { v.ok = false; v.why = w; return v; };
  switch (d.kind) {
    case Kind::Bool: {
      bool b;
      if (!ParseBool(raw, b)) return no("not a boolean: " + raw);
      v.ival = b ? 1 : 0;
      v.value = b ? "1" : "0";
      break;
    }
    case Kind::Int: {
      long long n;
      if (!ParseInt(raw, n)) return no("not an integer: " + raw);
      if (n < d.min || n > d.max) return no(Fmt("%s: %lld outside %d..%d", d.key, n, d.min, d.max));
      if (d.step > 1 && ((n - d.min) % d.step) != 0) return no(Fmt("%s: %lld is not on the %d step grid", d.key, n, d.step));
      v.ival = static_cast<int>(n);
      v.value = Fmt("%d", v.ival);
      break;
    }
    case Kind::Enum: {
      int idx = EnumIndex(d, raw);
      if (idx < 0) {
        long long n;
        if (ParseInt(raw, n) && n >= 0 && n < EnumCount(d)) idx = static_cast<int>(n);
      }
      if (idx < 0) return no(Fmt("%s: unknown choice '%s'", d.key, raw.c_str()));
      v.ival = idx;
      v.value = EnumName(d, idx);
      break;
    }
    case Kind::Text: {
      std::string k = d.key;
      if (k == "fan.cpu.curve" || k == "fan.gpu.curve") {
        fan::Curve c;
        std::string why;
        if (!fan::FromText(raw, &c)) return no("malformed fan curve");
        if (!fan::Validate(c, &why)) return no(why);
        v.value = fan::ToText(c);
      } else if (k == "light.kbd.color") {
        std::string s = Trim(raw);
        if (!IsHexColor(s)) return no("color must be #rrggbb");
        v.value = Lower(s);
      } else if (k == "light.kbd.colors") {
        const std::vector<std::string> parts = Split(raw, ',');
        if (parts.empty() || parts.size() > 16) return no("one to sixteen colours, comma separated");
        std::string out;
        for (const std::string& p : parts) {
          const std::string s = Trim(p);
          if (!IsHexColor(s)) return no("color must be #rrggbb");
          if (!out.empty()) out += ',';
          out += Lower(s);
        }
        v.value = out;
      } else {
        if (raw.size() > 4096) return no("text too long");
        v.value = raw;
      }
      v.ok = true;
      return v;
    }
  }
  v.ok = true;
  return v;
}

}  // namespace bc
