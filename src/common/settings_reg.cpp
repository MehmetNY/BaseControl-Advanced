#include "settings_reg.h"

#include <cstdlib>

#include "fan.h"
#include "util.h"

namespace bc {
namespace {

#define INT(key, mn, mx, st, unit, ev, cap, exp) {key, Kind::Int, mn, mx, st, unit, ev, cap, exp, ""}
#define BOOL(key, ev, cap, exp) {key, Kind::Bool, 0, 1, 1, "", ev, cap, exp, ""}
#define ENUM(key, count, names, ev, cap, exp) {key, Kind::Enum, 0, (count) - 1, 1, "", ev, cap, exp, names}
#define TEXT(key, ev, cap, exp) {key, Kind::Text, 0, 0, 0, "", ev, cap, exp, ""}

const std::vector<SettingDef> kTable = {
    // ---- CPU power through the EC (OEM "Custom Profile" semantics) ---------------------------------------------
    INT("ec.pl1", 10, 200, 1, "W", Ev::Family, "pl.ec", true),
    INT("ec.pl2", 10, 200, 1, "W", Ev::Family, "pl.ec", true),
    INT("ec.pl4", 20, 250, 1, "W", Ev::Family, "pl.ec", true),
    INT("ec.tcc", 70, 100, 1, "C", Ev::Family, "tcc", true),
    // ---- GPU ----------------------------------------------------------------------------------------------------
    INT("gpu.ctgp", 125, 150, 5, "W", Ev::Verified, "gpu.ctgp", false),
    BOOL("gpu.db", Ev::Experimental, "gpu.db", true),
    INT("nv.offset.core", -500, 500, 5, "MHz", Ev::Experimental, "gpu.offsets", true),
    INT("nv.offset.mem", -1000, 1500, 10, "MHz", Ev::Experimental, "gpu.offsets", true),
    // ---- fans ---------------------------------------------------------------------------------------------------
    ENUM("fan.mode", 4, "auto,boost,custom,fixed", Ev::Family, "fan.ctl", true),
    TEXT("fan.cpu.curve", Ev::Family, "fan.curve", true),
    TEXT("fan.gpu.curve", Ev::Family, "fan.curve", true),
    INT("fan.fixed.cpu", 20, 100, 1, "%", Ev::Experimental, "fan.fixed", true),
    INT("fan.fixed.gpu", 20, 100, 1, "%", Ev::Experimental, "fan.fixed", true),
    INT("fan.ramp", 1, 127, 1, "", Ev::Experimental, "fan.ramp", true),
    // ---- battery / preferences ---------------------------------------------------------------------------------
    ENUM("batt.mode", 3, "highcap,balanced,eco", Ev::Family, "batt.mode", true),
    BOOL("pref.fnlock", Ev::Family, "fnlock", true),
    BOOL("pref.acboot", Ev::Family, "acboot", true),
    BOOL("pref.usbpower", Ev::Family, "usbpower", true),
    // ---- Windows CPU policy (driverless) ------------------------------------------------------------------------
    ENUM("os.boost", 7, "disabled,enabled,aggressive,efficient,efficient_aggressive,aggressive_guaranteed,efficient_aggressive_guaranteed",
         Ev::Os, "os.cpu", false),
    INT("os.epp", 0, 100, 1, "%", Ev::Os, "os.cpu", false),
    INT("os.min", 0, 100, 1, "%", Ev::Os, "os.cpu", false),
    INT("os.max", 5, 100, 1, "%", Ev::Os, "os.cpu", false),
    INT("os.park", 0, 100, 1, "%", Ev::Os, "os.cpu", false),
    // ---- CPU advanced (MSR via PawnIO) -------------------------------------------------------------------------
    INT("msr.pl1", 5, 250, 1, "W", Ev::Experimental, "msr.pkg", true),
    INT("msr.pl2", 5, 250, 1, "W", Ev::Experimental, "msr.pkg", true),
    INT("msr.tau1", 1, 128, 1, "s", Ev::Experimental, "msr.pkg", true),
    INT("msr.tau2", 1, 10000, 1, "ms", Ev::Experimental, "msr.pkg", true),
    BOOL("msr.pl1.en", Ev::Experimental, "msr.pkg", true),
    BOOL("msr.pl2.en", Ev::Experimental, "msr.pkg", true),
    BOOL("msr.clamp", Ev::Experimental, "msr.pkg", true),
    INT("msr.psys.pl1", 20, 400, 1, "W", Ev::Experimental, "msr.psys", true),
    INT("msr.psys.pl2", 20, 400, 1, "W", Ev::Experimental, "msr.psys", true),
    BOOL("msr.psys.pl1.en", Ev::Experimental, "msr.psys", true),
    BOOL("msr.psys.pl2.en", Ev::Experimental, "msr.psys", true),
    BOOL("msr.bdp", Ev::Experimental, "msr.bdp", true),
    INT("msr.icc", 10, 250, 1, "A", Ev::Experimental, "msr.icc", true),
    INT("msr.volt.core", -250, 0, 1, "mV", Ev::Experimental, "msr.volt", true),
    INT("msr.volt.cache", -250, 0, 1, "mV", Ev::Experimental, "msr.volt", true),
    INT("msr.volt.sa", -250, 0, 1, "mV", Ev::Experimental, "msr.volt", true),
    BOOL("msr.pf.l2", Ev::Experimental, "msr.pf", true),
    BOOL("msr.pf.l2adj", Ev::Experimental, "msr.pf", true),
    BOOL("msr.pf.dcu", Ev::Experimental, "msr.pf", true),
    BOOL("msr.pf.dcuip", Ev::Experimental, "msr.pf", true),
    // ---- lighting (ITE HID) ------------------------------------------------------------------------------------
    BOOL("light.kbd.power", Ev::Family, "light.kbd", true),
    TEXT("light.kbd.color", Ev::Family, "light.kbd", true),
    INT("light.kbd.bright", 1, 4, 1, "", Ev::Family, "light.kbd", true),
    ENUM("light.kbd.effect", 5, "static,breathing,cycle,temp,pulse", Ev::Family, "light.kbd", true),
    INT("light.kbd.speed", 1, 10, 1, "", Ev::Family, "light.kbd", true),
    BOOL("light.bar.power", Ev::Family, "light.bar", true),
    TEXT("light.bar.color", Ev::Family, "light.bar", true),
    INT("light.bar.bright", 1, 4, 1, "", Ev::Family, "light.bar", true),
    BOOL("light.logo.power", Ev::Family, "light.logo", true),
    TEXT("light.logo.color", Ev::Family, "light.logo", true),
    INT("light.logo.bright", 1, 4, 1, "", Ev::Family, "light.logo", true),
    INT("light.idle", 0, 600, 5, "s", Ev::Os, "light.kbd", false),
    BOOL("light.battdim", Ev::Os, "light.kbd", false),
    INT("light.battlevel", 1, 4, 1, "", Ev::Os, "light.kbd", false),
    BOOL("light.sleepoff", Ev::Os, "light.kbd", false),
    // ---- display (Windows API) ----------------------------------------------------------------------------------
    INT("disp.bright", 0, 100, 1, "%", Ev::Os, "disp.bright", false),
    INT("disp.refresh", 30, 480, 1, "Hz", Ev::Os, "disp.refresh", false),
    INT("disp.c.bright", 0, 100, 1, "", Ev::Os, "disp.color", false),
    INT("disp.c.contrast", 0, 100, 1, "", Ev::Os, "disp.color", false),
    INT("disp.c.gamma", 50, 260, 1, "", Ev::Os, "disp.color", false),
    INT("disp.c.kelvin", 3000, 9000, 50, "K", Ev::Os, "disp.color", false),
    INT("disp.c.r", 50, 100, 1, "%", Ev::Os, "disp.color", false),
    INT("disp.c.g", 50, 100, 1, "%", Ev::Os, "disp.color", false),
    INT("disp.c.b", 50, 100, 1, "%", Ev::Os, "disp.color", false),
    ENUM("disp.mode", 7, "standard,vibrant,cinema,internet,photo,video,lowblue", Ev::Os, "disp.color", false),
    // ---- device switches -----------------------------------------------------------------------------------------
    BOOL("dev.wifi", Ev::Os, "dev.wifi", false),
    BOOL("dev.bt", Ev::Os, "dev.bt", false),
    BOOL("dev.cam", Ev::Os, "dev.cam", false),
    BOOL("dev.touchpad", Ev::Os, "dev.touchpad", false),
};

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
    case Ev::Family: return "family";
    case Ev::Experimental: return "experimental";
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
      } else if (EndsWith(k, ".color")) {
        std::string s = Trim(raw);
        if (s.size() != 7 || s[0] != '#') return no("color must be #rrggbb");
        for (size_t i = 1; i < 7; i++)
          if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return no("color must be #rrggbb");
        v.value = Lower(s);
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
