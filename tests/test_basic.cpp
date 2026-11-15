// Pure-logic tests: helpers, KV store, fan curves, setting validation, WriteGuard (the cTGP-only EC write allow-list), colour ramps, host data.
#include <cstring>

#include <cstdlib>

#include "../src/common/colorramp.h"
#include "../src/common/profile_text.h"
#include "../src/common/fan.h"
#include "../src/common/controller.h"
#include "../src/common/fsutil.h"
#include "../src/common/hostdata.h"
#include "../src/common/kv.h"
#include "../src/common/registers.h"
#include "../src/common/settings_reg.h"
#include "../src/common/util.h"
#include "../src/common/writeguard.h"
#include "../src/mgp/core/smbios.h"
#include "../src/sim/sim.h"
#include "mini_test.h"

using namespace bc;

// ------------------------------------------------------------------------------------------------ util / KV
TEST(util_split_join_trim) {
  CHECK_EQ(Split("a,b,,c", ',').size(), static_cast<size_t>(3));
  CHECK_EQ(Split("a,b,,c", ',', true).size(), static_cast<size_t>(4));
  CHECK_EQ(Join({"x", "y", "z"}, "-"), std::string("x-y-z"));
  CHECK_EQ(Trim("  hi \t\n"), std::string("hi"));
  CHECK(StartsWith("ec.pl1", "ec."));
  CHECK(!StartsWith("ec", "ec.pl1"));
  CHECK(EndsWith("light.kbd.color", ".color"));
  CHECK(EqNoCase("MONSTER", "monster"));
}

TEST(util_parse) {
  long long n = 0;
  CHECK(ParseInt("42", n)); CHECK_EQ(n, 42LL);
  CHECK(ParseInt("-7", n)); CHECK_EQ(n, -7LL);
  CHECK(ParseInt("0x783", n)); CHECK_EQ(n, 0x783LL);
  CHECK(!ParseInt("12abc", n));
  CHECK(!ParseInt("", n));
  CHECK(!ParseInt("0x", n));
  CHECK(!ParseInt("99999999999999999999", n));
  double d = 0;
  CHECK(ParseDouble("3.5", d)); CHECK_NEAR(d, 3.5, 1e-9);
  CHECK(!ParseDouble("nan", d));
  CHECK(!ParseDouble("1e999", d));
  bool b = false;
  CHECK(ParseBool("ON", b)); CHECK(b);
  CHECK(ParseBool("0", b)); CHECK(!b);
  CHECK(!ParseBool("maybe", b));
}

TEST(util_escape_roundtrip) {
  const std::string nasty = "a=b|c\\d\ne\rf\tg=\\n";
  CHECK_EQ(Unescape(Escape(nasty)), nasty);
  CHECK(Escape(nasty).find('\n') == std::string::npos);
  CHECK(Escape(nasty).find('=') == std::string::npos);
  CHECK(Escape(nasty).find('|') == std::string::npos);
}

TEST(util_utf_roundtrip) {
  const std::string s = u8"Güç Sınırı – 🔥 Performans";
  CHECK_EQ(Utf16ToUtf8(Utf8ToUtf16(s)), s);
  CHECK_EQ(Utf8ToUtf16("A").size(), static_cast<size_t>(1));
  CHECK_EQ(Utf8ToUtf16(u8"🔥").size(), static_cast<size_t>(2));  // surrogate pair
}

TEST(util_crc_clamp_round) {
  const char* t = "123456789";
  CHECK_EQ(Crc32(t, 9), 0xCBF43926u);  // the standard check value
  CHECK_EQ(Clamp(5, 0, 3), 3);
  CHECK_EQ(Clamp(-5, 0, 3), 0);
  CHECK_EQ(RoundToStep(123, 5), 125);
  CHECK_EQ(RoundToStep(122, 5), 120);
  CHECK_EQ(RoundToStep(-123, 5), -125);
  CHECK_EQ(RoundToStep(137, 5, 125), 135);
}

TEST(kv_basic_and_serialize) {
  KV k;
  k.Set("a", "1"); k.SetI("n", -5); k.SetF("f", 3.14159, 2); k.SetB("b", true);
  k.Set("t", "line1\nline2=x|y");
  CHECK_EQ(k.Int("n"), -5LL);
  CHECK_EQ(k.Str("f"), std::string("3.14"));
  CHECK(k.Bool("b"));
  CHECK_EQ(k.Int("missing", 9), 9LL);
  KV p = KV::Parse(k.Serialize());
  CHECK_EQ(p.Str("t"), std::string("line1\nline2=x|y"));
  CHECK_EQ(p.Size(), k.Size());
  CHECK_NEAR(p.Num("f"), 3.14, 1e-9);
  KV q = KV::Parse("# comment\n\nk=v\r\nbad line\n=novalue\nz=1\n");
  CHECK_EQ(q.Size(), static_cast<size_t>(2));
  CHECK_EQ(q.Str("k"), std::string("v"));
  KV m;
  m.Set("x", "1");
  m.Merge(k);
  CHECK(m.Has("x") && m.Has("a"));
}

// ------------------------------------------------------------------------------------------------ fan curves
TEST(fan_defaults_are_valid_and_floored) {
  for (const fan::Curve& c : {fan::DefaultCpu(), fan::DefaultGpu(), fan::QuietCpu(), fan::TurboCpu(), fan::QuietGpu(), fan::TurboGpu()}) {
    std::string why;
    CHECK(fan::Validate(c, &why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK(fan::Monotonic(c));
    CHECK_EQ(c.back().pct, 100);
  }
}

TEST(fan_validate_rejects) {
  std::string why;
  CHECK(!fan::Validate({{40, 0}}, &why));                                   // too short
  CHECK(!fan::Validate({{40, 5}, {50, 30}}, &why));                         // row 0 must be 0
  CHECK(!fan::Validate({{40, 0}, {42, 30}}, &why));                         // spacing < 4
  CHECK(!fan::Validate({{40, 0}, {50, 0}}, &why));                          // duty 0 in a later row
  CHECK(!fan::Validate({{40, 0}, {110, 100}}, &why));                       // temp out of range
  CHECK(!fan::Validate({{40, 0}, {60, 20}, {100, 20}}, &why));              // safety floor: 100 C needs 100%
  fan::Curve many;
  for (int i = 0; i < 11; i++) many.push_back({20 + i * 6, i == 0 ? 0 : 100});
  CHECK(!fan::Validate(many, &why));                                        // more than 10 rows
}

TEST(fan_safety_floor_table) {
  CHECK_EQ(fan::SafetyFloor(60), 0);
  CHECK_EQ(fan::SafetyFloor(70), 30);
  CHECK_EQ(fan::SafetyFloor(80), 50);
  CHECK_EQ(fan::SafetyFloor(88), 70);
  CHECK_EQ(fan::SafetyFloor(95), 90);
  CHECK_EQ(fan::SafetyFloor(100), 100);
}

TEST(fan_pack_unpack_roundtrip) {
  for (const fan::Curve& c : {fan::DefaultCpu(), fan::DefaultGpu(), fan::QuietCpu(), fan::TurboCpu(), fan::QuietGpu(), fan::TurboGpu()}) {
    fan::TableImage img = fan::Pack(c);
    fan::Curve back;
    std::string why;
    CHECK(fan::Unpack(img, &back, &why));
    CHECK_EQ(fan::ToText(back), fan::ToText(c));
    CHECK_EQ(img.up[c.size()], static_cast<uint8_t>(0xFF));  // padding marker
    CHECK_EQ(img.duty[0], static_cast<uint8_t>(0));
  }
}

TEST(fan_unpack_rejects_garbage) {
  fan::TableImage img;
  std::memset(&img, 0xFF, sizeof img);
  fan::Curve c;
  CHECK(!fan::Unpack(img, &c, nullptr));  // no active rows
  fan::TableImage bad = fan::Pack(fan::DefaultCpu());
  bad.up[3] = bad.up[2];                  // not increasing
  CHECK(!fan::Unpack(bad, &c, nullptr));
  bad = fan::Pack(fan::DefaultCpu());
  bad.duty[2] = 250;                      // implausible duty
  CHECK(!fan::Unpack(bad, &c, nullptr));
}

TEST(fan_dutyat_semantics) {
  fan::Curve c = fan::DefaultCpu();  // {40,0},{50,25},{58,32},...
  CHECK_EQ(fan::DutyAt(c, 20), 0);
  CHECK_EQ(fan::DutyAt(c, 40), 0);
  CHECK_EQ(fan::DutyAt(c, 41), 25);
  CHECK_EQ(fan::DutyAt(c, 50), 25);
  CHECK_EQ(fan::DutyAt(c, 51), 32);
  CHECK_EQ(fan::DutyAt(c, 120), 100);
}

TEST(fan_text_roundtrip_and_sanitize) {
  fan::Curve c;
  CHECK(fan::FromText("35:0,48:30,60:45,72:60,84:80,96:100", &c));
  CHECK_EQ(fan::ToText(c), std::string("35:0,48:30,60:45,72:60,84:80,96:100"));
  CHECK(!fan::FromText("35:0,36:30", &c));       // spacing
  CHECK(!fan::FromText("abc", &c));
  fan::Curve messy = {{50, 10}, {52, 5}, {53, 2}, {54, 1}};
  fan::Curve fixed = fan::Sanitize(messy);
  CHECK(fan::Validate(fixed, nullptr));
  CHECK(fan::Monotonic(fixed));
  CHECK_EQ(fixed.front().pct, 0);
  CHECK_EQ(fixed.back().pct, 100);
  for (size_t i = 1; i < fixed.size(); i++) CHECK(fixed[i].temp - fixed[i - 1].temp >= fan::kMinSpacing);
}

TEST(fan_editor_moves_keep_every_curve_valid) {
  for (const fan::Curve& base : {fan::DefaultCpu(), fan::DefaultGpu(), fan::QuietCpu(), fan::TurboCpu(), fan::QuietGpu(), fan::TurboGpu()}) {
    for (int i = 0; i < static_cast<int>(base.size()); i++) {
      for (int t = -10; t <= 130; t += 4) {
        for (int p = -5; p <= 110; p += 9) {
          fan::Curve m = fan::MovePoint(base, i, t, p);
          std::string why;
          if (!fan::Validate(m, &why)) std::printf("    row %d -> %d C / %d %%: %s\n", i, t, p, why.c_str());
          CHECK(fan::Validate(m, nullptr));
          CHECK(fan::Monotonic(m));
          CHECK_EQ(m.size(), base.size());
          CHECK_EQ(m.front().pct, 0);
          CHECK_EQ(m.back().pct, 100);
        }
      }
    }
  }
  // a point cannot cross its neighbours
  fan::Curve c = fan::DefaultCpu();
  fan::Curve m = fan::MovePoint(c, 3, 40, 42);   // wants to jump far left of row 2
  CHECK(m[3].temp - m[2].temp >= fan::kMinSpacing);
  // a point cannot go below the safety floor of its own temperature
  m = fan::MovePoint(c, 4, 82, 1);
  CHECK(m[4].pct >= fan::SafetyFloor(m[4].temp));
}

TEST(fan_editor_add_and_remove_points) {
  fan::Curve c = fan::DefaultCpu();                   // 9 rows
  fan::Curve a = fan::AddPoint(c, 54);
  CHECK_EQ(a.size(), c.size() + 1);
  CHECK(fan::Validate(a, nullptr));
  for (int t = 30; t <= 110; t++) CHECK_EQ(fan::DutyAt(a, t), fan::DutyAt(c, t));   // splitting a step does not change the behaviour
  CHECK_EQ(fan::AddPoint(a, 70).size(), a.size() == 10 ? a.size() : a.size() + 1);  // the table holds at most 10 rows
  CHECK_EQ(fan::AddPoint(c, 51).size(), c.size());    // 1 C from row 1: too close
  CHECK_EQ(fan::AddPoint(c, 10).size(), c.size());    // below the first row
  CHECK_EQ(fan::AddPoint(c, 120).size(), c.size());   // above the last row
  fan::Curve r = fan::RemovePoint(a, 2);
  CHECK_EQ(r.size(), a.size() - 1);
  CHECK(fan::Validate(r, nullptr));
  CHECK_EQ(fan::RemovePoint(c, 0).size(), c.size());  // row 0 stays
  fan::Curve two = {{40, 0}, {60, 100}};
  CHECK_EQ(fan::RemovePoint(two, 1).size(), static_cast<size_t>(2));   // never below two rows
  // removing a step only ever adds cooling
  fan::Curve r2 = fan::RemovePoint(c, 4);
  for (int t = 30; t <= 110; t++) CHECK(fan::DutyAt(r2, t) >= fan::DutyAt(c, t));
}

// ------------------------------------------------------------------------------------------------ settings registry
TEST(settings_registry_consistency) {
  const auto& all = AllSettings();
  CHECK_EQ(all.size(), static_cast<size_t>(39));   // the vendor's own settings plus the GPU power; docs/SETTINGS.md is generated from this table
  for (const SettingDef& d : all) {
    CHECK(FindSetting(d.key) == &d);  // unique keys
    if (d.kind == Kind::Enum) CHECK_EQ(EnumCount(d), d.max + 1);
    if (d.kind == Kind::Int) CHECK(d.min <= d.max);
  }
  CHECK(FindSetting("nope") == nullptr);
}

TEST(settings_validation) {
  const SettingDef* pl1 = FindSetting("perf.pl1");
  CHECK(pl1 != nullptr);
  CHECK(ValidateSetting(*pl1, "120").ok);
  CHECK(!ValidateSetting(*pl1, "4").ok);
  CHECK(!ValidateSetting(*pl1, "251").ok);
  CHECK(!ValidateSetting(*pl1, "abc").ok);
  const SettingDef* gc = FindSetting("perf.gpu.core");
  CHECK(ValidateSetting(*gc, "-150").ok);                  // offsets may be negative
  CHECK(!ValidateSetting(*gc, "1001").ok);
  const SettingDef* tcc = FindSetting("perf.tcc.on");
  CHECK(ValidateSetting(*tcc, "on").ival == 1);
  CHECK(ValidateSetting(*tcc, "0").ival == 0);
  const SettingDef* ct = FindSetting("gpu.ctgp");
  CHECK(ValidateSetting(*ct, "140").ok);
  CHECK(!ValidateSetting(*ct, "141").ok);                  // step grid
  CHECK(!ValidateSetting(*ct, "155").ok);
  CHECK(!ValidateSetting(*ct, "120").ok);
  const SettingDef* fm = FindSetting("fan.mode");
  CHECK_EQ(ValidateSetting(*fm, "custom").ival, 2);
  CHECK_EQ(ValidateSetting(*fm, "1").value, std::string("boost"));
  CHECK(!ValidateSetting(*fm, "fixed").ok);                // no fixed-speed mode any more: the vendor's service has none
  CHECK(!ValidateSetting(*fm, "turbo").ok);
  const SettingDef* bm = FindSetting("batt.mode");
  CHECK_EQ(ValidateSetting(*bm, "eco").ival, 2);
  const SettingDef* col = FindSetting("light.kbd.color");
  CHECK_EQ(ValidateSetting(*col, "#FFAA00").value, std::string("#ffaa00"));
  CHECK(!ValidateSetting(*col, "red").ok);
  CHECK(!ValidateSetting(*col, "#ggg000").ok);
  const SettingDef* cols = FindSetting("light.kbd.colors");
  CHECK_EQ(ValidateSetting(*cols, "#FF0000, #00ff00").value, std::string("#ff0000,#00ff00"));
  CHECK(!ValidateSetting(*cols, "#ff0000,nope").ok);
  CHECK(!ValidateSetting(*cols, "").ok);
  const SettingDef* br = FindSetting("light.kbd.bright");
  CHECK(ValidateSetting(*br, "4").ok);
  CHECK(!ValidateSetting(*br, "5").ok);
  const SettingDef* ef = FindSetting("light.kbd.effect");
  CHECK_EQ(ValidateSetting(*ef, "wave").ival, 2);
  const SettingDef* cv = FindSetting("fan.cpu.curve");
  CHECK(ValidateSetting(*cv, "35:0,48:30,60:45,72:60,84:80,96:100").ok);
  CHECK(!ValidateSetting(*cv, "35:0,48:30,60:45").ok);     // safety floor not reached at 100 C
}

TEST(settings_registry_has_no_removed_features) {
  // the removed extras (MSR / PawnIO, RAPL, undervolt, direct GPU offsets, fixed fan speed, CPU policy) must not come back as settings
  for (const char* k : {"ec.pl1", "ec.pl2", "ec.pl4", "ec.tcc", "msr.pl1", "msr.pl2", "msr.icc", "msr.volt.core", "nv.offset.core", "nv.offset.mem",
                        "fan.fixed.cpu", "fan.fixed.gpu", "gpu.db", "os.boost", "os.minstate", "os.maxstate", "light.idle"})
    CHECK(FindSetting(k) == nullptr);
}

TEST(settings_evidence_flags) {
  // every service-backed value says so; only the GPU power engine is "verified"; plain Windows values are "os"
  for (const char* k : {"perf.pl1", "perf.pl2", "perf.pl4", "perf.tcc", "perf.gpu.core", "perf.gpu.mem", "perf.gpu.temp", "fan.mode", "fan.cpu.curve",
                        "batt.mode", "pref.fnlock", "pref.acboot", "pref.usbpower", "light.kbd.effect", "light.kbd.colors"}) {
    const SettingDef* d = FindSetting(k);
    CHECK(d != nullptr);
    if (d) CHECK(d->ev == Ev::Service);
  }
  CHECK(FindSetting("gpu.ctgp")->ev == Ev::Verified);
  CHECK(FindSetting("disp.refresh")->ev == Ev::Os);
  CHECK(FindSetting("dev.wifi")->ev == Ev::Os);
  CHECK_EQ(std::string(EvName(Ev::Service)), std::string("service"));
}

// ------------------------------------------------------------------------------------------------ WriteGuard
// The EC is written for exactly one feature (the extended GPU power); everything else goes through the vendor's service.
TEST(guard_never_write_wins) {
  for (uint16_t a : {0x0780, 0x0781, 0x077E, 0x07E8, 0x0DFF, 0x0459}) {
    GuardVerdict g = WriteGuard::Check(a, 0x00, 0, true);
    CHECK(!g.ok);
    CHECK(!WriteGuard::IsAllowlisted(a));
  }
}

TEST(guard_only_the_ctgp_registers_are_writable) {
  // every address of the EC window is refused except the two cTGP registers
  int allowed = 0;
  for (uint32_t a = 0; a < 0x1000; a++)
    if (WriteGuard::IsAllowlisted(static_cast<uint16_t>(a))) {
      allowed++;
      CHECK(a == reg::kGpuCtrl || a == reg::kGpuCtgpOffset);
    }
  CHECK_EQ(allowed, 2);
  // the registers the old versions wrote (power limits, fan tables, fixed fan speed, battery mode, switches) are plain reads now
  for (uint16_t a : {reg::kPl1, reg::kPl2, reg::kPl4, reg::kTccWrite, reg::kFnLock, reg::kAcAutoBoot, reg::kFanMode, reg::kFanCurveLatch,
                     reg::kFanFixedCpu, reg::kFanFixedGpu, reg::kBatteryMode, reg::kEcActive})
    CHECK(!WriteGuard::Check(a, 0x01, 0x00, true).ok);
  CHECK(!WriteGuard::Check(reg::kFanTableCpuUp + 3, 70, 0, false).ok);
  CHECK(!WriteGuard::Check(0x0100, 1, 0, true).ok);
  CHECK(!WriteGuard::Check(0x043E, 50, 0, true).ok);   // a sensor
}

TEST(guard_ctgp_registers) {
  CHECK(WriteGuard::Check(reg::kGpuCtrl, 0x05, 0x01, true).ok);
  CHECK(WriteGuard::Check(reg::kGpuCtrl, 0x01, 0x05, true).ok);
  CHECK(!WriteGuard::Check(reg::kGpuCtrl, 0x03, 0x05, true).ok);     // 0x03 is not allowed
  CHECK(!WriteGuard::Check(reg::kGpuCtrl, 0x00, 0x05, true).ok);
  CHECK(!WriteGuard::Check(reg::kGpuCtrl, 0x05, 0x01, false).ok);    // masked register needs a fresh read
  CHECK(!WriteGuard::Check(reg::kGpuCtrl, 0x05, 0xF1, true).ok);     // bits outside the 0x07 mask would change (0xF1 -> 0x05)
  CHECK(!WriteGuard::Check(reg::kGpuCtrl, 0xF5, 0xF1, true).ok);     // only the two plain values the engine knows are ever written
  for (int v : {0, 5, 10, 15, 20, 25}) CHECK(WriteGuard::Check(reg::kGpuCtgpOffset, static_cast<uint8_t>(v), 0, false).ok);
  for (int v : {1, 4, 12, 26, 30, 255}) CHECK(!WriteGuard::Check(reg::kGpuCtgpOffset, static_cast<uint8_t>(v), 0, false).ok);
}

TEST(guard_decorator_behaviour) {
  sim::World w;
  GuardedEc g(w.ec, w.log);
  CHECK(!g.Write(0x0100, 1).ok);                       // not allowlisted
  CHECK(!g.Write(0x0780, 0).ok);                       // never-write
  CHECK(!g.Write(reg::kPl1, 100).ok);                  // the power limits are the vendor service's business
  w.plat.ec[reg::kGpuCtgpOffset] = 0;
  CHECK(g.Write(reg::kGpuCtgpOffset, 15).ok);
  CHECK_EQ(w.plat.ec[reg::kGpuCtgpOffset], static_cast<uint8_t>(15));
  CHECK_EQ(g.writes(), 1u);
  CHECK_EQ(g.refused(), 3u);
  CHECK(!g.Write(reg::kGpuCtgpOffset, 12).ok);         // off the 5 W grid
  g.SetReadOnly(true);
  CHECK(!g.Write(reg::kGpuCtgpOffset, 5).ok);          // kill switch
  CHECK_EQ(w.plat.ec[reg::kGpuCtgpOffset], static_cast<uint8_t>(15));
  CHECK(g.Read(reg::kPl1).ok);                         // reads always work
  CHECK(g.ReadOnly());
  g.SetReadOnly(false);
  // masked register: the guard reads the current byte itself and keeps the bits outside the mask
  w.plat.ec[reg::kGpuCtrl] = 0x01;
  CHECK(g.Write(reg::kGpuCtrl, 0x05).ok);
  CHECK_EQ(w.plat.ec[reg::kGpuCtrl], static_cast<uint8_t>(0x05));
  CHECK(!g.Write(reg::kGpuCtrl, 0x03).ok);             // 0x03 is not a permitted value
  w.plat.ec[reg::kGpuCtrl] = 0xA1;
  CHECK(!g.Write(reg::kGpuCtrl, 0x05).ok);             // would clear bits 5 and 7: an unknown state is left alone
  CHECK_EQ(w.plat.ec[reg::kGpuCtrl], static_cast<uint8_t>(0xA1));
}

// ------------------------------------------------------------------------------------------------ fsutil
TEST(fsutil_atomic_write_replace_and_limits) {
  char tmpl[] = "/tmp/bc_fs_XXXXXX";
  char* d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  const std::string dir = d;
  const std::string deep = fs::Join(dir, "a/b/c");
  CHECK(fs::MakeDirs(deep));
  CHECK(fs::IsDir(deep));
  CHECK(fs::MakeDirs(deep));                              // idempotent
  const std::string f = fs::Join(deep, "x.kv");
  CHECK(!fs::Exists(f));
  CHECK(fs::WriteAtomic(f, "one"));
  std::string back;
  CHECK(fs::ReadAll(f, &back));
  CHECK_EQ(back, std::string("one"));
  CHECK(fs::WriteAtomic(f, "second version"));            // replaces an existing file
  CHECK(fs::ReadAll(f, &back));
  CHECK_EQ(back, std::string("second version"));
  CHECK(!fs::Exists(f + ".tmp"));                         // no temp file left behind
  CHECK(!fs::ReadAll(f, &back, 4));                       // larger than the cap -> refused
  CHECK(fs::Append(f, "+tail"));
  CHECK(fs::ReadAll(f, &back));
  CHECK_EQ(back, std::string("second version+tail"));
  CHECK_EQ(fs::FileSize(f), static_cast<uint64_t>(19));
  CHECK(fs::Remove(f));
  CHECK(fs::Remove(f));                                   // already gone is fine
  CHECK(!fs::ReadAll(f, &back));
  CHECK_EQ(fs::Join("a/", "b"), std::string("a/b"));
  CHECK_EQ(fs::Join("a", "b"), std::string("a/b"));
  CHECK_EQ(fs::Join("", "b"), std::string("b"));
}

// ------------------------------------------------------------------------------------------------ colour ramp
TEST(colorramp_identity_and_limits) {
  ColorSettings def;
  uint16_t r[3][256];
  BuildGammaRamp(def, r);
  for (int ch = 0; ch < 3; ch++) {
    CHECK(r[ch][0] <= 256);                      // black stays (almost) black
    CHECK(r[ch][255] >= 65535 - 256);            // white stays (almost) white
    for (int i = 1; i < 256; i++) CHECK(r[ch][i] >= r[ch][i - 1]);
    CHECK_NEAR(r[ch][128] / 65535.0, 128 / 255.0, 0.02);
  }
  // 6500 K is neutral, 3000 K removes blue first
  KelvinGain n = KelvinToGain(6500), w = KelvinToGain(3000), c = KelvinToGain(9000);
  CHECK_NEAR(n.r, 1.0, 0.01);
  CHECK_NEAR(n.g, 1.0, 0.01);
  CHECK_NEAR(n.b, 1.0, 0.01);
  CHECK(w.r > w.g && w.g > w.b);
  CHECK(w.b < 0.6);
  CHECK(c.b >= c.g && c.g >= c.r);
}

TEST(colorramp_extremes_stay_inside_windows_limits) {
  ColorSettings x;
  x.brightness = 0; x.contrast = 0; x.gamma = 260; x.kelvin = 3000; x.r = 50; x.g = 50; x.b = 50;
  uint16_t r[3][256];
  BuildGammaRamp(x, r);
  for (int ch = 0; ch < 3; ch++)
    for (int i = 0; i < 256; i++) {
      CHECK(r[ch][i] >= (i - 127) * 256 || i < 127);
      CHECK(r[ch][i] <= (i + 127) * 256);
      if (i) CHECK(r[ch][i] >= r[ch][i - 1]);
    }
  ColorSettings y;
  y.brightness = 100; y.contrast = 100; y.gamma = 50;
  BuildGammaRamp(y, r);
  CHECK(r[0][255] == 65535);                     // full white is reachable
  CHECK(r[1][128] <= (128 + 127) * 256);
}

TEST(colorramp_saturation_matrix_and_preview_transform) {
  float m[5][5];
  SaturationMatrix(100, m);
  for (int i = 0; i < 5; i++)
    for (int j = 0; j < 5; j++) CHECK_NEAR(m[i][j], i == j ? 1.0 : 0.0, 1e-6);   // 100 % changes nothing
  SaturationMatrix(0, m);
  for (int j = 0; j < 3; j++) CHECK_NEAR(m[0][j] + m[1][j] + m[2][j], 1.0, 1e-5);   // every column keeps the luminance weights
  CHECK_NEAR(m[0][0], 0.2126, 1e-5);
  CHECK_NEAR(m[1][2], 0.7152, 1e-5);
  CHECK_NEAR(m[3][3], 1.0, 1e-6);
  CHECK_NEAR(m[4][4], 1.0, 1e-6);
  uint16_t ramp[3][256];
  ColorSettings c;
  BuildGammaRamp(c, ramp);
  int r, g, b;
  TransformRgb(c, ramp, 200, 120, 40, &r, &g, &b);
  CHECK(std::abs(r - 200) <= 2 && std::abs(g - 120) <= 2 && std::abs(b - 40) <= 2);   // neutral settings leave a colour alone
  c.saturation = 0;
  TransformRgb(c, ramp, 200, 120, 40, &r, &g, &b);
  CHECK(std::abs(r - g) <= 2 && std::abs(g - b) <= 2);                                  // 0 % saturation is grey
  c.saturation = 200;
  TransformRgb(c, ramp, 200, 120, 40, &r, &g, &b);
  CHECK(r > 200 && b < 40);                                                              // 200 % pushes the colour further out
  TransformRgb(c, ramp, 128, 128, 128, &r, &g, &b);
  CHECK(std::abs(r - 128) <= 2 && std::abs(g - 128) <= 2 && std::abs(b - 128) <= 2);   // greys never change
}

TEST(colorramp_preview_map_matches_the_forward_transform_and_undoes_live) {
  uint16_t ramp[3][256];
  ColorSettings neutral;
  BuildGammaRamp(neutral, ramp);
  {
    // nothing live: the preview is the plain forward transform, for any draft
    ColorSettings draft;
    draft.kelvin = 5200;
    draft.saturation = 140;
    draft.gamma = 120;
    uint16_t dr[3][256];
    BuildGammaRamp(draft, dr);
    PreviewMap pm(draft, nullptr);
    PreviewMap pn(draft, &neutral);   // a neutral live is the same thing
    for (int r = 0; r <= 255; r += 51)
      for (int g = 0; g <= 255; g += 51)
        for (int b = 0; b <= 255; b += 51) {
          int a1, a2, a3, b1, b2, b3, c1, c2, c3;
          TransformRgb(draft, dr, r, g, b, &a1, &a2, &a3);
          pm.Map(r, g, b, &b1, &b2, &b3);
          pn.Map(r, g, b, &c1, &c2, &c3);
          CHECK(std::abs(a1 - b1) <= 1 && std::abs(a2 - b2) <= 1 && std::abs(a3 - b3) <= 1);
          CHECK(b1 == c1 && b2 == c2 && b3 == c3);
        }
  }
  // A live set of colours (warm, more saturation, a touch more gamma): the colour drawn for the preview, pushed through what the panel
  // applies, comes out as the colour the draft calls for.
  ColorSettings live;
  live.kelvin = 5600;
  live.saturation = 125;
  live.gamma = 108;
  live.contrast = 52;
  uint16_t lr[3][256];
  BuildGammaRamp(live, lr);
  ColorSettings draft;
  draft.kelvin = 6000;
  draft.saturation = 110;
  uint16_t dr[3][256];
  BuildGammaRamp(draft, dr);
  PreviewMap pm(draft, &live);
  int worst = 0;
  for (int r = 40; r <= 200; r += 40)
    for (int g = 40; g <= 200; g += 40)
      for (int b = 40; b <= 200; b += 40) {
        int wr, wg, wb, pr, pg, pb, sr, sg, sb;
        TransformRgb(draft, dr, r, g, b, &wr, &wg, &wb);    // what the draft should look like on a clean panel
        pm.Map(r, g, b, &pr, &pg, &pb);                       // what is drawn
        TransformRgb(live, lr, pr, pg, pb, &sr, &sg, &sb);    // what the panel then makes of it
        worst = std::max(worst, std::max(std::abs(sr - wr), std::max(std::abs(sg - wg), std::abs(sb - wb))));
      }
  CHECK(worst <= 4);
  // previewing the live settings themselves draws the scene untouched (the panel applies them anyway)
  PreviewMap same(live, &live);
  for (int v : {40, 90, 128, 170, 200}) {   // (colours the live saturation would push past black or white cannot be undone, so they are left out)
    int r, g, b;
    same.Map(v, 255 - v, v / 2 + 20, &r, &g, &b);
    CHECK(std::abs(r - v) <= 3 && std::abs(g - (255 - v)) <= 3 && std::abs(b - (v / 2 + 20)) <= 3);
  }
  // A grey drawn for a warm live ramp keeps the channels apart in the right direction (blue is pulled down, so blue is drawn higher).
  PreviewMap plain(neutral, &live);
  int r, g, b;
  plain.Map(128, 128, 128, &r, &g, &b);
  CHECK(b > r);
  // The panel cannot show more than its ramp allows: a wanted channel above the ramp's top is clipped, never wrapped or negative.
  ColorSettings cold = live;
  cold.kelvin = 3200;
  PreviewMap pc(neutral, &cold);
  pc.Map(255, 255, 255, &r, &g, &b);
  CHECK(r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255);
  CHECK_EQ(b, 255);
  // A fully grey live saturation cannot be undone (the information is gone): the colour passes through instead of dividing by zero.
  ColorSettings grey;
  grey.saturation = 0;
  PreviewMap pg(neutral, &grey);
  pg.Map(90, 120, 150, &r, &g, &b);
  CHECK(std::abs(r - 90) <= 1 && std::abs(g - 120) <= 1 && std::abs(b - 150) <= 1);
}

TEST(colorramp_image_profiles_match_the_registry) {
  const SettingDef* d = FindSetting("disp.mode");
  CHECK(d != nullptr);
  CHECK(!ColorPresets().empty());
  CHECK_EQ(std::string(ColorPresets()[0].name), std::string("standard"));
  CHECK(ColorPresets()[0].c.Neutral());                               // the default profile changes nothing
  CHECK_EQ(static_cast<int>(ColorPresets().size()), d->max + 1);      // one registry entry per profile, in the same order
  for (size_t i = 0; i < ColorPresets().size(); i++) {
    const ColorPreset& p = ColorPresets()[i];
    CHECK_EQ(EnumName(*d, static_cast<int>(i)), std::string(p.name));
    CHECK(FindColorPreset(p.name) == &p.c);
    // every profile stays inside the ranges the individual sliders allow
    CHECK(p.c.kelvin >= 3000 && p.c.kelvin <= 9000);
    CHECK(p.c.gamma >= 50 && p.c.gamma <= 260);
    CHECK(p.c.saturation >= 0 && p.c.saturation <= 200);
    CHECK(p.c.r >= 50 && p.c.g >= 50 && p.c.b >= 50 && p.c.r <= 100 && p.c.g <= 100 && p.c.b <= 100);
    CHECK(p.c.brightness >= 0 && p.c.brightness <= 100 && p.c.contrast >= 0 && p.c.contrast <= 100);
    uint16_t ramp[3][256];
    BuildGammaRamp(p.c, ramp);                                          // and the ramp builder accepts it (monotonic, Windows-safe)
    for (int ch = 0; ch < 3; ch++)
      for (int k = 1; k < 256; k++) CHECK(ramp[ch][k] >= ramp[ch][k - 1]);
  }
  CHECK(FindColorPreset("nonsense") == nullptr);
  CHECK(FindColorPreset("custom") == nullptr);                         // "custom" is what a hand-made setting is called, not a profile
}

TEST(profile_text_builtins_and_list) {
  using namespace bc;
  // the four profiles of the vendor's Control Center (Balanced / Enthusiast / Overboost / Customized)
  CHECK(IsBuiltinProfile("Balanced"));
  CHECK(IsBuiltinProfile("overboost"));
  CHECK(IsBuiltinProfile("CUSTOMIZED"));
  CHECK(!IsBuiltinProfile("Quiet"));
  CHECK(!IsBuiltinProfile("My game"));
  CHECK_EQ(BuiltinProfiles().size(), static_cast<size_t>(4));
  CHECK_EQ(BuiltinProfiles()[0], std::string("Balanced"));
  CHECK_EQ(BuiltinProfiles()[3], std::string("Customized"));
  KV snap;
  snap.Set("profile.list", "Balanced,Enthusiast, Overboost ,,Overboost,Customized");
  std::vector<std::string> l = ProfileList(snap);
  CHECK_EQ(l.size(), static_cast<size_t>(4));
  CHECK_EQ(l[2], std::string("Overboost"));
  CHECK_EQ(ProfileList(KV()).size(), static_cast<size_t>(0));
}

// ------------------------------------------------------------------------------------------------ Device info data: SMBIOS memory, drive health
namespace {
// One SMBIOS type 17 structure with the given formatted length; strings are appended after it.
std::vector<uint8_t> MemStruct(size_t len, unsigned rawSize, int typeCode, int speed, int configured, const std::vector<std::string>& strs,
                               uint32_t extSize = 0, uint32_t extSpeed = 0) {
  std::vector<uint8_t> f(len, 0);
  f[0] = 17;
  f[1] = static_cast<uint8_t>(len);
  if (len > 0x0D) { f[0x0C] = rawSize & 0xFF; f[0x0D] = (rawSize >> 8) & 0xFF; }
  if (len > 0x12) { f[0x0E] = 0x0D; f[0x10] = 1; f[0x11] = 2; f[0x12] = static_cast<uint8_t>(typeCode); }
  if (len > 0x16) { f[0x15] = speed & 0xFF; f[0x16] = (speed >> 8) & 0xFF; }
  if (len > 0x1A) { f[0x17] = 3; f[0x18] = 4; f[0x19] = 5; f[0x1A] = 6; }
  if (len > 0x1F) for (int i = 0; i < 4; i++) f[0x1C + i] = (extSize >> (8 * i)) & 0xFF;
  if (len > 0x21) { f[0x20] = configured & 0xFF; f[0x21] = (configured >> 8) & 0xFF; }
  if (len > 0x57) for (int i = 0; i < 4; i++) f[0x54 + i] = (extSpeed >> (8 * i)) & 0xFF;
  for (const std::string& t : strs) { f.insert(f.end(), t.begin(), t.end()); f.push_back(0); }
  if (strs.empty()) f.push_back(0);
  f.push_back(0);
  return f;
}
std::vector<uint8_t> SystemStruct() {
  std::vector<uint8_t> f(0x08, 0);
  f[0] = 1; f[1] = 0x08; f[4] = 1; f[5] = 2;
  for (const char* t : {"ACME", "Laptop"}) { f.insert(f.end(), t, t + std::strlen(t)); f.push_back(0); }
  f.push_back(0);
  return f;
}
}  // namespace

TEST(smbios_type17_memory_devices_are_parsed_without_serials) {
  mgp::SmbiosInfo si;
  CHECK(mgp::ParseRawSmbios(sim::BuildSmbios("MONSTER", "TULPAR T5 V21.7", "N.1.07Mon08", "GM5TG7Y").data(),
                            sim::BuildSmbios("MONSTER", "TULPAR T5 V21.7", "N.1.07Mon08", "GM5TG7Y").size(), si));
  CHECK_EQ(si.memory.size(), static_cast<size_t>(3));
  CHECK_EQ(si.memory[0].sizeMb, 16384);
  CHECK_EQ(si.memory[0].typeCode, 0x1A);
  CHECK_EQ(si.memory[0].formFactor, 0x0D);
  CHECK_EQ(si.memory[0].ratedMts, 3200);
  CHECK_EQ(si.memory[0].configuredMts, 3200);
  CHECK_EQ(si.memory[0].locator, std::string("ChannelA-DIMM0"));
  CHECK_EQ(si.memory[0].manufacturer, std::string("80CE"));
  CHECK_EQ(si.memory[0].part, std::string("M471A2G43AB2-CWE"));
  CHECK_EQ(si.memory[2].sizeMb, 0);                      // the empty slot is listed, with no size
  // the parser has nowhere to put a serial number or an asset tag, and the fake ones from the table must not leak into a string field
  for (const mgp::SmbiosMemory& m : si.memory)
    for (const std::string& f : {m.locator, m.bank, m.manufacturer, m.part}) {
      CHECK(f.find("FAKE") == std::string::npos);
    }
}

TEST(smbios_type17_size_and_speed_encodings) {
  auto parse = [](const std::vector<uint8_t>& mem) {
    std::vector<uint8_t> t = SystemStruct();
    t.insert(t.end(), mem.begin(), mem.end());
    t.insert(t.end(), {127, 4, 0, 0, 0, 0});
    mgp::SmbiosInfo si;
    CHECK(mgp::ParseSmbiosStructures(t.data(), t.size(), si));
    return si;
  };
  // 32 GB and larger: raw size 0x7FFF, real size in the extended field (MB)
  mgp::SmbiosInfo a = parse(MemStruct(0x22, 0x7FFF, 0x22, 5600, 5200, {"SLOT", "BANK", "SK hynix", "SER", "ASSET", "HMCG88AGBSA095N"}, 65536));
  CHECK_EQ(a.memory.size(), static_cast<size_t>(1));
  CHECK_EQ(a.memory[0].sizeMb, 65536);
  CHECK_EQ(a.memory[0].typeCode, 0x22);
  CHECK_EQ(a.memory[0].ratedMts, 5600);
  CHECK_EQ(a.memory[0].configuredMts, 5200);
  // bit 15 set: the size is in KB
  CHECK_EQ(parse(MemStruct(0x22, 0x8000 | 2048, 0x1A, 2400, 2400, {"A", "B", "M", "S", "T", "P"})).memory[0].sizeMb, 2);
  // unknown (0xFFFF) and empty (0) are both "no usable size"
  CHECK_EQ(parse(MemStruct(0x22, 0xFFFF, 0x1A, 2400, 2400, {"A"})).memory[0].sizeMb, 0);
  CHECK_EQ(parse(MemStruct(0x22, 0, 0x1A, 2400, 2400, {"A"})).memory[0].sizeMb, 0);
  // speed 0xFFFF: the extended speed fields of SMBIOS 3.3 carry the value
  mgp::SmbiosInfo e = parse(MemStruct(0x5C, 8192, 0x22, 0xFFFF, 0xFFFF, {"A", "B", "M", "S", "T", "P"}, 0, 7200));
  CHECK_EQ(e.memory[0].ratedMts, 7200);
  // an old-format structure (no speed, no part number) is still understood, a structure cut off in the middle is skipped
  mgp::SmbiosInfo old = parse(MemStruct(0x15, 4096, 0x13, 0, 0, {"DIMM1"}));
  CHECK_EQ(old.memory.size(), static_cast<size_t>(1));
  CHECK_EQ(old.memory[0].sizeMb, 4096);
  CHECK_EQ(old.memory[0].ratedMts, 0);
  CHECK(old.memory[0].part.empty());
  CHECK_EQ(parse(MemStruct(0x10, 4096, 0x13, 0, 0, {})).memory.size(), static_cast<size_t>(0));
}

TEST(hostdata_memory_names_and_modules) {
  CHECK_EQ(MemTypeName(0x1A), std::string("DDR4"));
  CHECK_EQ(MemTypeName(0x22), std::string("DDR5"));
  CHECK_EQ(MemTypeName(0x23), std::string("LPDDR5"));
  CHECK_EQ(MemTypeName(0x01), std::string(""));
  CHECK_EQ(MemMakerName("80CE"), std::string("Samsung"));
  CHECK_EQ(MemMakerName("80ce"), std::string("Samsung"));
  CHECK_EQ(MemMakerName("859B"), std::string("Crucial"));
  CHECK_EQ(MemMakerName("Kingston"), std::string("Kingston"));
  CHECK_EQ(MemMakerName("Unknown"), std::string(""));
  CHECK_EQ(MemMakerName("0000"), std::string(""));
  CHECK_EQ(MemMakerName("  Team  Group; Inc "), std::string("Team Group Inc"));
  mgp::SmbiosInfo si;
  const std::vector<uint8_t> raw = sim::BuildSmbios("A", "B", "C", "D");
  CHECK(mgp::ParseRawSmbios(raw.data(), raw.size(), si));
  const std::vector<MemModule> mods = MemModulesFromSmbios(si);
  CHECK_EQ(mods.size(), static_cast<size_t>(2));       // the empty slot is left out
  CHECK_EQ(mods[1].sizeMb, 16384);
  CHECK_EQ(mods[1].type, std::string("DDR4"));
  CHECK_EQ(mods[1].ratedMts, 3200);
  CHECK_EQ(mods[1].runMts, 3200);
  CHECK_EQ(mods[1].maker, std::string("Samsung"));
  CHECK_EQ(mods[1].locator, std::string("ChannelB-DIMM0"));
}

TEST(hostdata_text_cleanup_and_drive_titles) {
  CHECK_EQ(CleanText("  A|b;c\td  "), std::string("A b c d"));
  CHECK_EQ(CleanText("x\x01y"), std::string("x y"));
  CHECK_EQ(CleanText("abcdef", 4), std::string("abcd"));
  CHECK_EQ(CleanText("\xC3\xA7\xC3\xA7\xC3\xA7", 3), std::string("\xC3\xA7"));   // never cut inside a UTF-8 character
  CHECK_EQ(StorageTitle("WD Green SN3000 2TB"), std::string("WD Green SN3000"));
  CHECK_EQ(StorageTitle("Crucial  CT1000P3SSD8   1TB"), std::string("Crucial CT1000P3SSD8"));
  CHECK_EQ(StorageTitle("Foo 1.5TB"), std::string("Foo"));
  CHECK_EQ(StorageTitle("Foo 500GB"), std::string("Foo"));
  CHECK_EQ(StorageTitle("SAMSUNG MZVL21T0HCLR-00B00"), std::string("SAMSUNG MZVL21T0HCLR-00B00"));
  CHECK_EQ(StorageTitle("2TB"), std::string("2TB"));                         // nothing left to show: unchanged
  CHECK_EQ(StorageTitle("Foo TB"), std::string("Foo TB"));
  CHECK_EQ(StorageTitle("Foo 5XB"), std::string("Foo 5XB"));
  CHECK_EQ(DriveModelName("NVMe    ", "SAMSUNG MZVL21T0HCLR-00B00 "), std::string("SAMSUNG MZVL21T0HCLR-00B00"));
  CHECK_EQ(DriveModelName("ATA     ", "Samsung SSD 860 EVO 1TB"), std::string("Samsung SSD 860 EVO 1TB"));
  CHECK_EQ(DriveModelName("WDC", "WDC PC SN730 SDBQNTY-512G"), std::string("WDC PC SN730 SDBQNTY-512G"));
  CHECK_EQ(DriveModelName("Intel", "SSDPEKNW512G8"), std::string("Intel SSDPEKNW512G8"));
  CHECK_EQ(DriveModelName("", "Some Drive"), std::string("Some Drive"));
  CHECK_EQ(DriveModelName("NVMe", ""), std::string("NVMe"));                 // nothing better to show than the vendor
  CHECK_EQ(StorageBusName(0x11), std::string("NVMe"));
  CHECK_EQ(StorageBusName(0x0B), std::string("SATA"));
  CHECK_EQ(StorageBusName(0x07), std::string(""));                           // USB sticks are not internal storage
  CHECK_EQ(StorageBusName(0x0E), std::string(""));                           // virtual disks neither
}

TEST(hostdata_nvme_health_log_and_storage_temperature) {
  std::vector<uint8_t> log(512, 0);
  log[0] = 0;
  log[1] = 0x32; log[2] = 0x01;                  // 306 K = 33 C
  log[3] = 100;                                  // available spare
  log[5] = 7;                                    // percentage used
  const uint64_t units = 10000000;               // data units written (1 unit = 512000 bytes) = 5120 GB
  const uint64_t cycles = 3109, hours = 13439;
  for (int i = 0; i < 8; i++) {
    log[48 + i] = (units >> (8 * i)) & 0xFF;
    log[112 + i] = (cycles >> (8 * i)) & 0xFF;
    log[128 + i] = (hours >> (8 * i)) & 0xFF;
  }
  StorageDrive d;
  CHECK(ParseNvmeHealth(log.data(), log.size(), &d));
  CHECK_EQ(d.tempC, 33);
  CHECK_EQ(d.sparePct, 100);
  CHECK_EQ(d.wearPct, 7);
  CHECK_EQ(d.critWarn, 0);
  CHECK_EQ(d.powerCycles, 3109LL);
  CHECK_EQ(d.powerOnHours, 13439LL);
  CHECK_EQ(d.writtenGb, 5120LL);
  // implausible values are "not reported", never shown as numbers
  std::vector<uint8_t> bad(512, 0);
  bad[3] = 200;                                  // spare > 100
  bad[1] = 0; bad[2] = 0;                        // 0 K
  bad[120] = 1;                                  // power cycles: high half of the 128-bit counter set = beyond 63 bits
  StorageDrive e;
  CHECK(ParseNvmeHealth(bad.data(), bad.size(), &e));
  CHECK_EQ(e.tempC, -1);
  CHECK_EQ(e.sparePct, -1);
  CHECK_EQ(e.powerCycles, -1LL);
  CHECK_EQ(e.powerOnHours, 0LL);
  StorageDrive f;
  CHECK(!ParseNvmeHealth(log.data(), 100, &f));  // too short
  CHECK(!ParseNvmeHealth(nullptr, 512, &f));
  // STORAGE_TEMPERATURE_DATA_DESCRIPTOR: the composite sensor (index 0) wins over other sensors, whatever the order
  std::vector<uint8_t> t(24 + 2 * 16, 0);
  t[12] = 2;
  t[24] = 1; t[26] = 40;                         // sensor 1: 40 C
  t[40] = 0; t[42] = 33;                         // composite: 33 C
  CHECK_EQ(ParseStorageTemperature(t.data(), t.size()), 33);
  t[40] = 3;                                     // no composite entry: the first valid one
  CHECK_EQ(ParseStorageTemperature(t.data(), t.size()), 40);
  t[26] = 0xFF; t[27] = 0x7F;                    // 32767 C: out of range, skipped
  t[42] = 0xD8; t[43] = 0xFF;                    // -40 C is still a possible reading
  CHECK_EQ(ParseStorageTemperature(t.data(), t.size()), -40);
  CHECK_EQ(ParseStorageTemperature(t.data(), 20), -1);
  std::vector<uint8_t> none(40, 0);
  CHECK_EQ(ParseStorageTemperature(none.data(), none.size()), -1);   // zero sensors
}

TEST(hostdata_drive_sizes_and_device_summary) {
  CHECK_EQ(FormatDriveSize(2000398934016ull), std::string("2 TB"));
  CHECK_EQ(FormatDriveSize(1024209543168ull), std::string("1 TB"));
  CHECK_EQ(FormatDriveSize(1920383410176ull), std::string("1.9 TB"));
  CHECK_EQ(FormatDriveSize(4000787030016ull), std::string("4 TB"));
  CHECK_EQ(FormatDriveSize(512110190592ull), std::string("512 GB"));
  CHECK_EQ(FormatDriveSize(500107862016ull), std::string("500 GB"));
  CHECK_EQ(FormatDriveSize(256060514304ull), std::string("256 GB"));
  CHECK_EQ(FormatDriveSize(0), std::string(""));
  // the report describes the machine but names nobody: no computer name, no serial numbers
  sim::World w;
  Controller c(w.hw(), [] { Controller::Options o; o.manual = true; o.mode = "sim"; return o; }());
  c.Start();
  const std::string text = DeviceSummary(c.Snapshot());
  CHECK(text.find("Model: MONSTER TULPAR T5 V21.7 (GM5TG7Y)") != std::string::npos);
  CHECK(text.find("CPU: 11th Gen Intel(R) Core(TM) i7-11800H @ 2.30GHz (8 cores / 16 threads)") != std::string::npos);
  CHECK(text.find("2 x") == std::string::npos);
  CHECK(text.find("16 GB DDR4-3200 Samsung M471A2G43AB2-CWE; 16 GB DDR4-3200 Samsung M471A2G43AB2-CWE") != std::string::npos);
  CHECK(text.find("WD Green SN3000 2TB (NVMe, 2 TB); SAMSUNG MZVL21T0HCLR-00B00 (NVMe, 1 TB)") != std::string::npos);
  CHECK(text.find("SIM-TULPAR") == std::string::npos);
  CHECK(text.find("FAKE") == std::string::npos);
  CHECK(DeviceSummary(KV()).empty());
}
