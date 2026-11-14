// Models behind the Keys and In-game overlay pages: key bindings, the physical keyboard drawing, overlay configuration, hotkeys
// and the text rows of the overlay.
#include <cmath>
#include <map>
#include <set>

#include "../src/common/keymap.h"
#include "../src/common/osdcfg.h"
#include "mini_test.h"

using namespace bc;

TEST(keymap_roundtrip_and_rules) {
  auto v = ParseKeyMap("66=mic_mute, 67=osd_toggle,66=vol_mute,abc=lock,70=nonsense,0=lock,999=lock,71");
  CHECK_EQ(v.size(), static_cast<size_t>(2));          // 66 re-bound to vol_mute, 67; the rest is rejected
  CHECK_EQ(FindBind(v, 66)->action, std::string("vol_mute"));
  CHECK(FindBind(v, 67) != nullptr);
  CHECK(FindBind(v, 70) == nullptr);
  CHECK_EQ(FormatKeyMap(v), std::string("66=vol_mute,67=osd_toggle"));
  CHECK_EQ(FormatKeyMap(ParseKeyMap(FormatKeyMap(v))), FormatKeyMap(v));
  SetBind(&v, 0x153, "lock");
  CHECK_EQ(v.size(), static_cast<size_t>(3));
  SetBind(&v, 0x153, "disable");
  CHECK_EQ(v.size(), static_cast<size_t>(3));
  CHECK_EQ(FindBind(v, 0x153)->action, std::string("disable"));
  CHECK(RemoveBind(&v, 66));
  CHECK(!RemoveBind(&v, 66));
  CHECK_EQ(v.size(), static_cast<size_t>(2));
  CHECK(ParseKeyMap("").empty());
}

TEST(keymap_actions_unique_and_known) {
  std::set<std::string> ids;
  for (const KeyAction& a : KeyActions()) {
    CHECK(ids.insert(a.id).second);
    CHECK(a.label && *a.label);
    CHECK(FindKeyAction(a.id) == &a);
  }
  CHECK(FindKeyAction("mic_mute") != nullptr);
  CHECK(FindKeyAction("osd_toggle") != nullptr);
  CHECK(FindKeyAction("format_c") == nullptr);
  CHECK(FindKeyAction("vol_up")->repeat);
  CHECK(!FindKeyAction("mic_mute")->repeat);
}

TEST(keymap_keyboard_drawing_is_consistent) {
  const auto& rows = KeyboardRows();
  CHECK_EQ(rows.size(), static_cast<size_t>(6));
  std::map<int, int> seen;
  for (const auto& row : rows) {
    float w = 0;
    for (const PhysKey& k : row) {
      w += k.w;
      seen[k.id]++;
      CHECK(k.tr && *k.tr);
      CHECK(k.us && *k.us);
      CHECK(k.id >= 0 && k.id < 0x200);
    }
    CHECK_NEAR(w, 15.0, 0.001);                        // every row is exactly 15 key units wide
  }
  for (const auto& p : seen) {
    if (p.first == 0x1C) CHECK_EQ(p.second, 2);        // the ISO Enter key is drawn in two rows
    else CHECK_EQ(p.second, 1);
  }
  CHECK(seen.count(0x42) == 1);                        // F8
  CHECK_EQ(KeyLegend(0x42, true), std::string("F8"));
  CHECK_EQ(KeyLegend(0x1A, true), std::string("\xC4\x9E"));
  CHECK_EQ(KeyLegend(0x1A, false), std::string("["));
  CHECK_EQ(KeyLegend(0x9999, true), std::string(""));
  CHECK_EQ(KeyLegend(0, true), std::string(""));       // Fn is not a scan code
  for (const auto& row : rows)
    for (const PhysKey& k : row)
      if (!k.mappable) CHECK_EQ(k.id, 0);
}

TEST(osd_config_defaults_and_clamping) {
  KV p;
  OsdConfig c = OsdFromKv(p);
  CHECK(!c.enabled);
  CHECK(c.fps && c.cpu && c.gpu);
  CHECK(!c.mem && !c.fan && !c.power);
  CHECK_EQ(c.corner, 0);
  CHECK_EQ(c.mx, 20);
  CHECK_EQ(c.opacity, 78);
  CHECK_EQ(c.hotkey, std::string("ctrl+shift+o"));
  p.Set("osd.corner", "9");
  p.Set("osd.mx", "-5");
  p.Set("osd.my", "9999");
  p.Set("osd.opacity", "3");
  p.Set("osd.size", "7");
  p.Set("osd.color", "#112233");
  p.Set("osd.hotkey", "nonsense");
  p.Set("osd.games", "Cyberpunk2077.exe; C:\\Games\\cs2.exe;cs2.exe;;bad|name");
  c = OsdFromKv(p);
  CHECK_EQ(c.corner, 3);
  CHECK_EQ(c.mx, 0);
  CHECK_EQ(c.my, 200);
  CHECK_EQ(c.opacity, 10);
  CHECK_EQ(c.size, 2);
  CHECK_EQ(c.color, 0xFF112233u);
  CHECK_EQ(c.hotkey, std::string("ctrl+shift+o"));      // an unusable hotkey falls back to the default
  CHECK_EQ(c.games.size(), static_cast<size_t>(2));
  CHECK_EQ(c.games[0], std::string("cyberpunk2077.exe"));
  CHECK_EQ(c.games[1], std::string("cs2.exe"));
  CHECK_NEAR(OsdSizeFactor(0), 1.0, 0.001);
  CHECK_NEAR(OsdSizeFactor(2), 1.5, 0.001);
}

TEST(osd_exe_names_and_colors) {
  CHECK_EQ(NormalizeExeName(" Game "), std::string("game.exe"));
  CHECK_EQ(NormalizeExeName("D:\\x\\Y.EXE"), std::string("y.exe"));
  CHECK_EQ(NormalizeExeName(""), std::string(""));
  CHECK_EQ(NormalizeExeName(".exe"), std::string(""));
  CHECK_EQ(NormalizeExeName("a:b"), std::string(""));
  CHECK_EQ(NormalizeExeName(std::string(80, 'a')), std::string(""));
  uint32_t c = 0;
  CHECK(ParseColor("#AbCdEf", &c));
  CHECK_EQ(c, 0xFFABCDEFu);
  CHECK_EQ(FormatColor(c), std::string("#abcdef"));
  CHECK(!ParseColor("abcdef", &c));
  CHECK(!ParseColor("#abcde", &c));
  CHECK(!ParseColor("#abcdeg", &c));
}

TEST(osd_hotkeys) {
  unsigned m = 0, vk = 0;
  CHECK(ParseHotkey("ctrl+shift+o", &m, &vk));
  CHECK_EQ(m, 6u);
  CHECK_EQ(vk, static_cast<unsigned>('O'));
  CHECK(ParseHotkey("Alt+F10", &m, &vk));
  CHECK_EQ(m, 1u);
  CHECK_EQ(vk, 0x79u);
  CHECK(ParseHotkey("ctrl+alt+insert", &m, &vk));
  CHECK_EQ(vk, 0x2Du);
  CHECK(!ParseHotkey("o", &m, &vk));                   // needs a modifier
  CHECK(!ParseHotkey("ctrl+shift", &m, &vk));          // needs a key
  CHECK(!ParseHotkey("ctrl+a+b", &m, &vk));
  CHECK(!ParseHotkey("ctrl+f99", &m, &vk));
  CHECK_EQ(HotkeyLabel("ctrl+shift+o"), std::string("Ctrl + Shift + O"));
  CHECK_EQ(HotkeyLabel("ctrl+alt+f12"), std::string("Ctrl + Alt + F12"));
  for (const std::string& h : OsdHotkeys()) CHECK(ParseHotkey(h, nullptr, nullptr));
}

TEST(osd_lines) {
  OsdConfig c;
  OsdData d;
  auto lines = OsdLines(c, d);                          // FPS, CPU, GPU with nothing known
  CHECK_EQ(lines.size(), static_cast<size_t>(3));
  CHECK_EQ(lines[0].label, std::string("FPS"));
  CHECK_EQ(lines[0].value, std::string("\xE2\x80\x94"));
  d.fps = 143.6;
  d.cpuT = 64.2;
  d.cpuLoad = 31.8;
  d.gpuT = 58;
  d.gpuLoad = 24;
  d.memPct = 41;
  d.memUsedGb = 13.04;
  d.cpuRpm = 3100;
  d.gpuRpm = 3300;
  d.cpuW = 45.4;
  d.gpuW = 80.2;
  c.mem = c.fan = c.power = true;
  lines = OsdLines(c, d);
  CHECK_EQ(lines.size(), static_cast<size_t>(6));
  CHECK_EQ(lines[0].value, std::string("144"));
  CHECK_EQ(lines[1].value, std::string("64 \xC2\xB0""C \xC2\xB7 32 %"));
  CHECK_EQ(lines[2].value, std::string("58 \xC2\xB0""C \xC2\xB7 24 %"));
  CHECK_EQ(lines[3].label, std::string("RAM"));
  CHECK_EQ(lines[3].value, std::string("13.0 GB \xC2\xB7 41 %"));
  CHECK_EQ(lines[4].value, std::string("3100 / 3300 rpm"));
  CHECK_EQ(lines[5].value, std::string("45 W \xC2\xB7 80 W"));
  c.fps = c.cpu = c.gpu = c.mem = c.fan = c.power = false;
  CHECK(OsdLines(c, d).empty());
}

TEST(osd_data_from_snapshot) {
  KV s;
  s.SetI("cpu.temp", 71);
  s.SetF("os.cpu.pct", 12, 0);
  s.SetF("os.mem.used", 8192, 0);
  s.SetF("os.mem.pct", 25, 0);
  s.SetI("gpu.temp", 55);
  s.SetI("fan.cpu.rpm", 2500);
  OsdData d = OsdDataFrom(s);
  CHECK_NEAR(d.cpuT, 71, 0.01);
  CHECK_NEAR(d.cpuLoad, 12, 0.01);
  CHECK_NEAR(d.memUsedGb, 8.0, 0.01);
  CHECK_NEAR(d.gpuT, 55, 0.01);
  CHECK_NEAR(d.cpuRpm, 2500, 0.01);
  CHECK(d.gpuRpm < 0);                                  // not in the snapshot -> unknown
  CHECK(d.fps < 0);
  s.SetI("fps.cur", 90);
  CHECK_NEAR(OsdDataFrom(s).fps, 90, 0.01);
}
