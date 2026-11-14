#include "lightfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bc {
namespace lightfx {

bool ParseHex(const std::string& s, Rgb* out) {
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (size_t i = 1; i < 7; i++) {
    const char c = s[i];
    const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + static_cast<unsigned>(d);
  }
  if (out) {
    out->r = static_cast<uint8_t>(v >> 16);
    out->g = static_cast<uint8_t>(v >> 8);
    out->b = static_cast<uint8_t>(v);
  }
  return true;
}

std::string ToHex(const Rgb& c) {
  char b[16];
  std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
  return b;
}

Rgb FromHsv(double h, double s, double v) {
  h = std::fmod(h, 360.0);
  if (h < 0) h += 360.0;
  const double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1)), m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  auto q = [&](double f) { return static_cast<uint8_t>(std::min(255, std::max(0, static_cast<int>(std::lround((f + m) * 255.0))))); };
  Rgb o;
  o.r = q(r);
  o.g = q(g);
  o.b = q(b);
  return o;
}

const char* EffectName(int e) {
  static const char* kNames[kEffectCount] = {"static", "breathing", "cycle", "temp", "pulse"};
  return kNames[std::min(std::max(e, 0), static_cast<int>(kEffectCount) - 1)];
}

double PeriodMs(int speed) { return 12000.0 / (std::min(std::max(speed, 1), 10) + 1.0); }

Rgb EffectColor(int effect, int speed, uint64_t nowMs, const Rgb& base, long long cpuTemp) {
  const double period = PeriodMs(speed);
  const double ph = std::fmod(static_cast<double>(nowMs), period) / period;
  Rgb out = base;
  double f = 1.0;
  switch (effect) {
    case kBreathing: f = (1.0 - std::cos(ph * 6.283185307179586)) * 0.5; break;
    case kCycle: out = FromHsv(ph * 360.0, 1.0, 1.0); break;
    case kTemp: out = FromHsv(std::min(std::max((95.0 - static_cast<double>(cpuTemp)) / 55.0, 0.0), 1.0) * 120.0, 1.0, 1.0); break;
    case kPulse: f = ph < 0.35 ? 1.0 - ph / 0.35 : 0.0; break;
    default: break;
  }
  Rgb c;
  c.r = static_cast<uint8_t>(out.r * f);
  c.g = static_cast<uint8_t>(out.g * f);
  c.b = static_cast<uint8_t>(out.b * f);
  return c;
}

double LevelFraction(int level) { return std::min(std::max(level, 1), 4) / 4.0; }

}  // namespace lightfx
}  // namespace bc
