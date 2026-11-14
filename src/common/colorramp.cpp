#include "colorramp.h"

#include <algorithm>
#include <cmath>

namespace bc {
namespace {
double Cl(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Tanner Helland's black-body approximation (public domain algorithm), result in 0..255.
void Raw(int kelvin, double* r, double* g, double* b) {
  const double t = Cl(kelvin, 1000.0, 40000.0) / 100.0;
  *r = t <= 66 ? 255.0 : Cl(329.698727446 * std::pow(t - 60.0, -0.1332047592), 0, 255);
  *g = t <= 66 ? Cl(99.4708025861 * std::log(t) - 161.1195681661, 0, 255) : Cl(288.1221695283 * std::pow(t - 60.0, -0.0755148492), 0, 255);
  *b = t >= 66 ? 255.0 : (t <= 19 ? 0.0 : Cl(138.5177312231 * std::log(t - 10.0) - 305.0447927307, 0, 255));
}
}  // namespace

KelvinGain KelvinToGain(int kelvin) {
  double r, g, b, rn, gn, bn;
  Raw(kelvin, &r, &g, &b);
  Raw(6500, &rn, &gn, &bn);
  r /= rn;
  g /= gn;
  b /= bn;
  const double m = std::max(r, std::max(g, b));
  if (m <= 0) return {1, 1, 1};
  return {r / m, g / m, b / m};
}

void BuildGammaRamp(const ColorSettings& c, uint16_t out[3][256]) {
  const KelvinGain k = KelvinToGain(c.kelvin);
  const double gain[3] = {k.r * Cl(c.r, 50, 100) / 100.0, k.g * Cl(c.g, 50, 100) / 100.0, k.b * Cl(c.b, 50, 100) / 100.0};
  const double contrast = 0.5 + Cl(c.contrast, 0, 100) / 100.0;       // 0.5 .. 1.5, 1.0 = neutral (slider 50)
  const double bright = (Cl(c.brightness, 0, 100) - 50.0) / 100.0 * 0.5;  // -0.25 .. +0.25
  const double inv = 1.0 / (Cl(c.gamma, 50, 260) / 100.0);
  for (int ch = 0; ch < 3; ++ch) {
    double prev = 0;
    for (int i = 0; i < 256; ++i) {
      double y = i / 255.0;
      y = (y - 0.5) * contrast + 0.5 + bright;
      y = std::pow(Cl(y, 0, 1), inv);
      y *= gain[ch];
      double v = Cl(y, 0, 1) * 65535.0;
      // stay inside the range Windows accepts and keep the curve monotonic
      v = Cl(v, std::max(0.0, (i - 127) * 256.0), std::min(65535.0, (i + 127) * 256.0));
      v = std::max(v, prev);
      prev = v;
      out[ch][i] = static_cast<uint16_t>(std::lround(v));
    }
  }
}

}  // namespace bc
