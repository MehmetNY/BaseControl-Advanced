// Keyboard backlight effects as pure functions of time. The controller sends exactly these colours to the keyboard and the UI draws
// the same ones in its preview, so what the preview shows is what the keyboard does. No hardware, no globals: easy to test.
#pragma once
#include <cstdint>
#include <string>

namespace bc {
namespace lightfx {

struct Rgb {
  uint8_t r = 255, g = 255, b = 255;
};

bool ParseHex(const std::string& text, Rgb* out);   // "#rrggbb" (case-insensitive); anything else is rejected
std::string ToHex(const Rgb& c);                    // "#rrggbb", lower case
Rgb FromHsv(double hueDeg, double sat, double val);

enum Effect { kStatic = 0, kBreathing = 1, kCycle = 2, kTemp = 3, kPulse = 4, kEffectCount = 5 };
const char* EffectName(int effect);                 // "static", "breathing", "cycle", "temp", "pulse"

// Length of one cycle in ms for speed 1..10 (1 = about 6 s, 10 = about 1.1 s).
double PeriodMs(int speed);

// Colour the keyboard shows at time `nowMs` for the chosen effect. `cpuTemp` (deg C) only matters for the temperature effect:
// green when cool, red near 95 deg C.
Rgb EffectColor(int effect, int speed, uint64_t nowMs, const Rgb& base, long long cpuTemp);

// How bright the light looks at brightness level 1..4 (0.25 .. 1.0), for drawing previews.
double LevelFraction(int level);

}  // namespace lightfx
}  // namespace bc
