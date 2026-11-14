// In-game overlay (OSD) model: the stored configuration, the numbers it shows and the text lines it is made of. Pure C++ so the
// layout rules are unit-tested on any host; the drawing lives in src/ui/osd_paint.cpp and the overlay window in osd_window.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kv.h"

namespace bc {

struct OsdConfig {
  bool enabled = false;       // overlay master switch (also toggled by the hotkey)
  bool fps = true, cpu = true, gpu = true, mem = false, fan = false, power = false;
  int corner = 0;             // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
  int mx = 20, my = 20;       // distance from the screen edge, px
  int size = 0;               // 0 small, 1 medium, 2 large
  uint32_t color = 0xFFD9F0C4;
  int opacity = 78;           // background, percent
  bool gamesOnly = false;     // show only while a game from the list is the foreground window
  std::vector<std::string> games;   // lower-case image names
  std::string hotkey = "ctrl+shift+o";
};

// Reads the "osd.*" keys of the UI preference store; every value is range-checked.
OsdConfig OsdFromKv(const KV& prefs);
float OsdSizeFactor(int size);   // 1.0 / 1.25 / 1.5

std::vector<std::string> ParseGameList(const std::string& text);   // ';' separated; trimmed, lower-case, unique
std::string FormatGameList(const std::vector<std::string>& v);
// Accepts "game.exe" or a path; returns "" when it is not a plausible image name.
std::string NormalizeExeName(const std::string& text);

// Hotkeys: "ctrl+shift+o". mods use the Win32 MOD_* values (alt 1, ctrl 2, shift 4); vk is a Win32 virtual-key code.
bool ParseHotkey(const std::string& text, unsigned* mods, unsigned* vk);
std::string HotkeyLabel(const std::string& text);   // "Ctrl + Shift + O"
const std::vector<std::string>& OsdHotkeys();       // the presets offered in the UI

// "#rrggbb" <-> 0xFFrrggbb
bool ParseColor(const std::string& text, uint32_t* argb);
std::string FormatColor(uint32_t argb);

struct OsdData {
  double fps = -1;
  double cpuT = -1, cpuLoad = -1, cpuW = -1;
  double gpuT = -1, gpuLoad = -1, gpuW = -1;
  double memPct = -1, memUsedGb = -1;
  double cpuRpm = -1, gpuRpm = -1;
};
OsdData OsdDataFrom(const KV& snap);   // from the controller snapshot (negative = unknown)

struct OsdLine {
  std::string label, value;
};
// The rows the overlay shows for this configuration, in a fixed order (FPS, CPU, GPU, RAM, FAN, PWR).
std::vector<OsdLine> OsdLines(const OsdConfig& c, const OsdData& d);

}  // namespace bc
