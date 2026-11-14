// Pure-logic tests: helpers, KV store, fan curves, RAPL/MSR codecs, setting validation, WriteGuard.
#include <cstring>

#include <cstdlib>

#include "../src/common/colorramp.h"
#include "../src/common/profile_text.h"
#include "../src/common/fan.h"
#include "../src/common/fsutil.h"
#include "../src/common/kv.h"
#include "../src/common/rapl.h"
#include "../src/common/registers.h"
#include "../src/common/settings_reg.h"
#include "../src/common/util.h"
#include "../src/common/writeguard.h"
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

// ------------------------------------------------------------------------------------------------ RAPL / MSR codecs
TEST(rapl_units) {
  rapl::Units u = rapl::DecodeUnits(0xA0E03);
  CHECK_NEAR(u.powerW, 0.125, 1e-12);
  CHECK_NEAR(u.energyJ, 1.0 / 16384.0, 1e-12);
  CHECK_NEAR(u.timeS, 1.0 / 1024.0, 1e-12);
}

TEST(rapl_pkg_limit_roundtrip) {
  rapl::Units u = rapl::DecodeUnits(0xA0E03);
  rapl::PkgLimit p;
  p.pl1W = 120; p.pl2W = 150; p.pl1En = p.pl2En = true; p.pl1Clamp = p.pl2Clamp = true; p.tau1S = 28; p.tau2S = 0.00244;
  uint64_t v = 0;
  CHECK(rapl::EncodePkgLimit(0, p, u, &v));
  rapl::PkgLimit q = rapl::DecodePkgLimit(v, u);
  CHECK_NEAR(q.pl1W, 120, 1e-9);
  CHECK_NEAR(q.pl2W, 150, 1e-9);
  CHECK(q.pl1En && q.pl2En && q.pl1Clamp && q.pl2Clamp);
  CHECK_NEAR(q.tau1S, 28, 1.0);
  CHECK(!q.lock);
  // changing only PL1 keeps the rest
  q.pl1W = 90;
  uint64_t v2 = 0;
  CHECK(rapl::EncodePkgLimit(v, q, u, &v2));
  rapl::PkgLimit r = rapl::DecodePkgLimit(v2, u);
  CHECK_NEAR(r.pl1W, 90, 1e-9);
  CHECK_NEAR(r.pl2W, 150, 1e-9);
}

TEST(rapl_lock_and_range) {
  rapl::Units u = rapl::DecodeUnits(0xA0E03);
  rapl::PkgLimit p;
  p.pl1W = 100; p.pl2W = 100; p.tau1S = 28;
  uint64_t out = 0;
  CHECK(!rapl::EncodePkgLimit(1ULL << 63, p, u, &out));  // locked: refuse
  p.pl1W = 5000;                                         // 15-bit field overflow
  CHECK(!rapl::EncodePkgLimit(0, p, u, &out));
  p.pl1W = 100; p.tau1S = 1e9;                           // time window not representable (max ~3.7e6 s)
  CHECK(!rapl::EncodePkgLimit(0, p, u, &out));
  p.tau1S = 28;
  CHECK(rapl::EncodePkgLimit(0, p, u, &out));
  CHECK((out >> 63) == 0);                               // never sets the lock bit
}

TEST(rapl_time_window) {
  rapl::Units u = rapl::DecodeUnits(0xA0E03);
  uint32_t y, z;
  CHECK(rapl::EncodeTimeWindow(28, u, &y, &z));
  CHECK_NEAR(rapl::DecodeTimeWindow(y, z, u), 28, 0.5);
  CHECK(!rapl::EncodeTimeWindow(0, u, &y, &z));
}

TEST(rapl_energy_wrap) {
  rapl::Units u = rapl::DecodeUnits(0xA0E03);
  // 16384 counts = 1 J; 1 s window -> 1 W
  CHECK_NEAR(rapl::EnergyToWatts(0, 16384, 1.0, u), 1.0, 1e-9);
  CHECK_NEAR(rapl::EnergyToWatts(0xFFFFFF00u, 0x00003F00u + 0x100u - 0x100u, 1.0, u) > 0 ? 1.0 : 0.0, 1.0, 1e-9);
  // wrap: prev near the top, cur small
  double w = rapl::EnergyToWatts(0xFFFFC000u, 0x00004000u, 2.0, u);  // delta = 0x8000 counts = 2 J over 2 s = 1 W
  CHECK_NEAR(w, 1.0, 1e-9);
  CHECK_NEAR(rapl::EnergyToWatts(10, 20, 0, u), 0.0, 1e-12);
}

TEST(rapl_icc_and_temp_fields) {
  CHECK_NEAR(rapl::DecodeIccMaxA(1040), 130.0, 1e-9);
  uint64_t out = 0;
  CHECK(rapl::EncodeIccMax(1040, 140, &out));
  CHECK_NEAR(rapl::DecodeIccMaxA(out), 140.0, 1e-9);
  CHECK(!rapl::EncodeIccMax(1040 | (1ULL << 31), 140, &out));  // locked
  CHECK(!rapl::EncodeIccMax(1040, 0.5, &out));
  CHECK_EQ(rapl::TjMax(100ULL << 16), 100);
  CHECK_EQ(rapl::TccOffset(5ULL << 24), 5);
  CHECK_EQ(rapl::ThermReadout(37ULL << 16), 37);
  CHECK_EQ(rapl::PlatformBaseRatio(23ULL << 8), 23);
  CHECK_EQ(rapl::TurboRatio(0x2B2B2C2C2D2D2E2EULL, 0), 0x2E);
}

TEST(rapl_oc_mailbox_codec) {
  uint64_t rd = rapl::OcReadVoltageCmd(0);
  CHECK((rd >> 63) == 1);
  CHECK_EQ(static_cast<int>((rd >> 32) & 0xFF), 0x10);
  uint64_t wr = rapl::OcWriteVoltageCmd(2, -80);
  CHECK_EQ(static_cast<int>((wr >> 32) & 0xFF), 0x11);
  CHECK_EQ(static_cast<int>((wr >> 40) & 0xFF), 2);
  CHECK_EQ(rapl::OcDecodeOffsetMv(wr & ~(1ULL << 63)), -80);
  CHECK_EQ(rapl::OcDecodeOffsetMv(rapl::OcWriteVoltageCmd(0, 0)), 0);
  CHECK(rapl::OcBusy(wr));
  CHECK(!rapl::OcBusy(wr & ~(1ULL << 63)));
}

// ------------------------------------------------------------------------------------------------ settings registry
TEST(settings_registry_consistency) {
  const auto& all = AllSettings();
  CHECK(all.size() > 50);
  for (const SettingDef& d : all) {
    CHECK(FindSetting(d.key) == &d);  // unique keys
    if (d.kind == Kind::Enum) CHECK_EQ(EnumCount(d), d.max + 1);
    if (d.kind == Kind::Int) CHECK(d.min <= d.max);
  }
  CHECK(FindSetting("nope") == nullptr);
}

TEST(settings_validation) {
  const SettingDef* pl1 = FindSetting("ec.pl1");
  CHECK(pl1 != nullptr);
  CHECK(ValidateSetting(*pl1, "120").ok);
  CHECK(!ValidateSetting(*pl1, "9").ok);
  CHECK(!ValidateSetting(*pl1, "201").ok);
  CHECK(!ValidateSetting(*pl1, "abc").ok);
  const SettingDef* ct = FindSetting("gpu.ctgp");
  CHECK(ValidateSetting(*ct, "140").ok);
  CHECK(!ValidateSetting(*ct, "141").ok);   // step grid
  CHECK(!ValidateSetting(*ct, "155").ok);
  const SettingDef* fm = FindSetting("fan.mode");
  CHECK_EQ(ValidateSetting(*fm, "custom").ival, 2);
  CHECK_EQ(ValidateSetting(*fm, "3").value, std::string("fixed"));
  CHECK(!ValidateSetting(*fm, "turbo").ok);
  const SettingDef* col = FindSetting("light.kbd.color");
  CHECK_EQ(ValidateSetting(*col, "#FFAA00").value, std::string("#ffaa00"));
  CHECK(!ValidateSetting(*col, "red").ok);
  CHECK(!ValidateSetting(*col, "#ggg000").ok);
  const SettingDef* cv = FindSetting("fan.cpu.curve");
  CHECK(ValidateSetting(*cv, "35:0,48:30,60:45,72:60,84:80,96:100").ok);
  CHECK(!ValidateSetting(*cv, "35:0,48:30,60:45").ok);  // safety floor not reached at 100 C
  const SettingDef* volt = FindSetting("msr.volt.core");
  CHECK(ValidateSetting(*volt, "-100").ok);
  CHECK(!ValidateSetting(*volt, "10").ok);              // overvolt never accepted
}

TEST(settings_evidence_flags) {
  // nothing that bypasses firmware protection may be "verified" or ungated
  for (const char* k : {"msr.pl1", "msr.pl2", "msr.icc", "msr.volt.core", "nv.offset.core", "fan.fixed.cpu", "gpu.db"}) {
    const SettingDef* d = FindSetting(k);
    CHECK(d != nullptr);
    CHECK(d->ev == Ev::Experimental);
    CHECK(d->needsExperimental);
  }
  CHECK(FindSetting("gpu.ctgp")->ev == Ev::Verified);
  CHECK(FindSetting("os.boost")->ev == Ev::Os);
}

// ------------------------------------------------------------------------------------------------ WriteGuard
TEST(guard_never_write_wins) {
  for (uint16_t a : {0x0780, 0x0781, 0x077E, 0x07E8, 0x0DFF, 0x0459}) {
    GuardVerdict g = WriteGuard::Check(a, 0x00, 0, true, true);
    CHECK(!g.ok);
    CHECK(!WriteGuard::IsAllowlisted(a));
  }
}

TEST(guard_not_on_allowlist) {
  CHECK(!WriteGuard::Check(0x0100, 1, 0, true).ok);
  CHECK(!WriteGuard::Check(0x043E, 50, 0, true).ok);  // sensor
  CHECK(!WriteGuard::Check(0x0728, 1, 0, true).ok);
}

TEST(guard_power_limits) {
  CHECK(WriteGuard::Check(reg::kPl1, 120, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kPl1, 9, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kPl1, 211, 0, false).ok);
  CHECK(WriteGuard::Check(reg::kPl4, 250, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kPl4, 4, 0, false).ok);
  CHECK(WriteGuard::Check(reg::kTccWrite, 0x85, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kTccWrite, 0x05, 0, false).ok);   // commit bit missing
  CHECK(!WriteGuard::Check(reg::kTccWrite, 0x80 | 36, 0, false).ok);
}

TEST(guard_masked_registers_need_rmw) {
  // 0x074E: only bit 4 may change
  CHECK(!WriteGuard::Check(reg::kFnLock, 0x10, 0, false).ok);              // no fresh read
  CHECK(WriteGuard::Check(reg::kFnLock, 0x10, 0x00, true).ok);             // flips bit 4 only
  CHECK(!WriteGuard::Check(reg::kFnLock, 0x11, 0x00, true).ok);            // would change bit 0
  // 0x0726: bits 3 and 7 only; bits 0..2 (battery derating commands) never
  CHECK(WriteGuard::Check(reg::kAcAutoBoot, 0x88, 0x00, true).ok);
  CHECK(!WriteGuard::Check(reg::kAcAutoBoot, 0x81, 0x00, true).ok);
  // 0x0751: only bit 6
  CHECK(WriteGuard::Check(reg::kFanMode, 0x40, 0x00, true).ok);
  CHECK(!WriteGuard::Check(reg::kFanMode, 0x50, 0x00, true).ok);
  // 0x07C6: only bit 2 (bit 6 must never be touched)
  CHECK(WriteGuard::Check(reg::kFanCurveLatch, 0x04, 0x00, true).ok);
  CHECK(!WriteGuard::Check(reg::kFanCurveLatch, 0x44, 0x00, true).ok);
}

TEST(guard_gpu_registers) {
  CHECK(WriteGuard::Check(reg::kGpuCtrl, 0x05, 0x01, true).ok);
  CHECK(WriteGuard::Check(reg::kGpuCtrl, 0x01, 0x05, true).ok);
  CHECK(!WriteGuard::Check(reg::kGpuCtrl, 0x03, 0x05, true).ok);   // 0x03 not allowed
  CHECK(WriteGuard::Check(reg::kGpuCtgpOffset, 25, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kGpuCtgpOffset, 12, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kGpuCtgpOffset, 30, 0, false).ok);
}

TEST(guard_fan_registers) {
  CHECK(WriteGuard::Check(reg::kFanFixedCpu, 0x80 | 60, 0, false).ok);
  CHECK(WriteGuard::Check(reg::kFanFixedCpu, 0, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kFanFixedCpu, 60, 0, false).ok);            // missing 0x80
  CHECK(!WriteGuard::Check(reg::kFanFixedGpu, 0x80 | 101, 0, false).ok);
  CHECK(WriteGuard::Check(reg::kFanTableCpuUp + 3, 70, 0, false).ok);
  CHECK(WriteGuard::Check(reg::kFanTableCpuUp + 12, 0xFF, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kFanTableCpuUp + 3, 120, 0, false).ok);
  CHECK(WriteGuard::Check(reg::kFanTableCpuDuty + 2, 200, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kFanTableCpuDuty + 2, 201, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kFanTableEnd + 1, 0, 0, false).ok);         // one past the table
}

TEST(guard_battery_mode_values) {
  CHECK(WriteGuard::Check(reg::kBatteryMode, 0x18, 0x08, true).ok);
  CHECK(!WriteGuard::Check(reg::kBatteryMode, 0x38, 0x08, true).ok);
  CHECK(!WriteGuard::Check(reg::kBatteryMode, 0x1A, 0x08, true).ok);        // bit outside mask changes
}

TEST(guard_bringup_registers_need_scope) {
  CHECK(!WriteGuard::Check(reg::kBiosCtrl, 0x41, 0, false, false).ok);
  CHECK(WriteGuard::Check(reg::kBiosCtrl, 0x41, 0, false, true).ok);
  CHECK(!WriteGuard::Check(reg::kBiosCtrl, 0x40, 0, false, true).ok);       // wrong value even in scope
  CHECK(WriteGuard::Check(reg::kEcHandshake1, 0x05, 0, false, true).ok);
  CHECK(!WriteGuard::Check(reg::kEcHandshake2, 0x05, 0, false, true).ok);
  CHECK(WriteGuard::Check(reg::kFeatureEnable, 0x81, 0, false, true).ok);
  CHECK(WriteGuard::Check(reg::kOcpl, 0x0C, 0, false, true).ok);
  CHECK(!WriteGuard::Check(reg::kOcpl, 0x0D, 0, false, true).ok);
  CHECK(WriteGuard::Check(reg::kEcActive, 0x05, 0, false).ok);
  CHECK(!WriteGuard::Check(reg::kEcActive, 0x01, 0, false).ok);
}

TEST(guard_decorator_behaviour) {
  sim::World w;
  GuardedEc g(w.ec, w.log);
  CHECK(!g.Write(0x0100, 1).ok);                       // not allowlisted
  CHECK(!g.Write(0x0780, 0).ok);                       // never-write
  CHECK(g.Write(reg::kPl1, 100).ok);
  CHECK_EQ(w.plat.ec[reg::kPl1], static_cast<uint8_t>(100));
  CHECK_EQ(g.writes(), 1u);
  CHECK_EQ(g.refused(), 2u);
  g.SetReadOnly(true);
  CHECK(!g.Write(reg::kPl1, 90).ok);                   // kill switch
  CHECK_EQ(w.plat.ec[reg::kPl1], static_cast<uint8_t>(100));
  CHECK(g.Read(reg::kPl1).ok);                         // reads still work
  g.SetReadOnly(false);
  // masked register: the guard reads the current byte itself
  w.plat.ec[reg::kFnLock] = 0x01;
  CHECK(!g.Write(reg::kFnLock, 0x10).ok);              // would clear bit 0 -> refused
  CHECK(g.Write(reg::kFnLock, 0x11).ok);
  // rollback: address still allowlisted, but value not range checked
  w.plat.ec[reg::kPl2] = 150;
  CHECK(g.WriteRollback(reg::kPl2, 5).ok);
  CHECK(!g.WriteRollback(0x0100, 5).ok);
  w.plat.ec[reg::kFnLock] = 0x41;
  CHECK(!g.WriteRollback(reg::kFnLock, 0x00).ok);      // bit 6 differs and is outside the mask
  // bring-up scope
  CHECK(!g.Write(reg::kBiosCtrl, 0x41).ok);
  {
    GuardedEc::InitScope s(g);
    CHECK(g.Write(reg::kBiosCtrl, 0x41).ok);
  }
  CHECK(!g.Write(reg::kBiosCtrl, 0x41).ok);
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

TEST(profile_text_roundtrip_and_builtins) {
  using namespace bc;
  CHECK(IsBuiltinProfile("Quiet"));
  CHECK(IsBuiltinProfile("overboost"));
  CHECK(!IsBuiltinProfile("My game"));
  CHECK_EQ(BuiltinProfiles().size(), static_cast<size_t>(4));
  KV v;
  v.Set("ec.pl1", "60");
  v.Set("fan.cpu.curve", "30:25,40:30,100:100");
  v.Set("weird", "a=b|c\\d");
  const std::string t = FormatProfileValues(v);
  CHECK(t.find("ec.pl1=60") != std::string::npos);
  // exactly one '|' between the three entries, none inside a value
  size_t bars = 0;
  for (char ch : t) bars += ch == '|';
  CHECK_EQ(bars, static_cast<size_t>(2));
  KV back = ParseProfileValues(t);
  CHECK_EQ(back.Size(), static_cast<size_t>(3));
  CHECK_EQ(back.Str("ec.pl1"), std::string("60"));
  CHECK_EQ(back.Str("fan.cpu.curve"), std::string("30:25,40:30,100:100"));
  CHECK_EQ(back.Str("weird"), std::string("a=b|c\\d"));
  CHECK_EQ(ParseProfileValues("").Size(), static_cast<size_t>(0));
  CHECK_EQ(ParseProfileValues("garbage|=x|k=").Size(), static_cast<size_t>(1));
  KV snap;
  snap.Set("profile.list", "Quiet,Balanced, My game ,,My game,Enthusiast");
  std::vector<std::string> l = ProfileList(snap);
  CHECK_EQ(l.size(), static_cast<size_t>(4));
  CHECK_EQ(l[2], std::string("My game"));
}
