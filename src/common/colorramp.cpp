#include "colorramp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

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

void SaturationMatrix(int saturationPercent, float m[5][5]) {
  const double s = Cl(saturationPercent, 0, 200) / 100.0;
  const double lr = 0.2126, lg = 0.7152, lb = 0.0722;
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 5; ++j) m[i][j] = i == j ? 1.0f : 0.0f;
  const double wr[3] = {lr, lg, lb};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m[i][j] = static_cast<float>((1.0 - s) * wr[i] + (i == j ? s : 0.0));
}

void TransformRgb(const ColorSettings& c, const uint16_t ramp[3][256], int r, int g, int b, int* ro, int* go, int* bo) {
  float m[5][5];
  SaturationMatrix(c.saturation, m);
  const double in[3] = {Cl(r, 0, 255), Cl(g, 0, 255), Cl(b, 0, 255)};
  int out[3];
  for (int ch = 0; ch < 3; ++ch) {
    const double v = Cl(in[0] * m[0][ch] + in[1] * m[1][ch] + in[2] * m[2][ch], 0, 255);
    out[ch] = static_cast<int>(std::lround(ramp[ch][static_cast<int>(std::lround(v))] / 257.0));
  }
  *ro = out[0];
  *go = out[1];
  *bo = out[2];
}

PreviewMap::PreviewMap(const ColorSettings& want, const ColorSettings* live) {
  float m[5][5];
  SaturationMatrix(want.saturation, m);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) sat_[i][j] = m[i][j];
  uint16_t w16[3][256];
  BuildGammaRamp(want, w16);
  for (int ch = 0; ch < 3; ++ch)
    for (int v = 0; v < 256; ++v) ramp_[ch][v] = static_cast<uint8_t>(std::lround(w16[ch][v] / 257.0));
  std::memset(inv_, 0, sizeof(inv_));
  if (!live || live->Neutral()) return;
  undo_ = true;
  uint16_t l16[3][256];
  BuildGammaRamp(*live, l16);
  for (int ch = 0; ch < 3; ++ch) {
    auto shown = [&](int d) { return static_cast<int>(std::lround(l16[ch][d] / 257.0)); };   // what the panel shows for a drawn value d
    for (int t = 0; t < 256; ++t) {
      // the ramp never decreases, so the first d that reaches t is found by bisection; the nearer of d and d-1 wins
      int lo = 0, hi = 255;
      if (shown(255) < t) lo = hi = 255;
      while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (shown(mid) >= t) hi = mid; else lo = mid + 1;
      }
      if (lo > 0 && std::abs(shown(lo - 1) - t) < std::abs(shown(lo) - t)) lo--;
      inv_[ch][t] = static_cast<uint8_t>(lo);
    }
  }
  if (live->saturation != 100 && live->saturation > 0) undoSat_ = 100.0f / static_cast<float>(live->saturation);
}

void PreviewMap::Map(int r, int g, int b, int* ro, int* go, int* bo) const {
  const double in[3] = {Cl(r, 0, 255), Cl(g, 0, 255), Cl(b, 0, 255)};
  double t[3];
  for (int ch = 0; ch < 3; ++ch) {
    const double v = Cl(in[0] * sat_[0][ch] + in[1] * sat_[1][ch] + in[2] * sat_[2][ch], 0, 255);
    t[ch] = ramp_[ch][static_cast<int>(std::lround(v))];
  }
  if (undo_) {
    for (int ch = 0; ch < 3; ++ch) t[ch] = inv_[ch][static_cast<int>(t[ch])];
    if (undoSat_ != 1.0f) {   // the colour effect came first on the way to the panel, so it is undone last
      const double lum = 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
      for (int ch = 0; ch < 3; ++ch) t[ch] = Cl(lum + (t[ch] - lum) * undoSat_, 0, 255);
    }
  }
  *ro = static_cast<int>(std::lround(t[0]));
  *go = static_cast<int>(std::lround(t[1]));
  *bo = static_cast<int>(std::lround(t[2]));
}

const std::vector<ColorPreset>& ColorPresets() {
  static const auto make = [](int br, int co, int ga, int k, int r, int g, int b, int sat) {
    ColorSettings c;
    c.brightness = br; c.contrast = co; c.gamma = ga; c.kelvin = k; c.r = r; c.g = g; c.b = b; c.saturation = sat;
    return c;
  };
  static const std::vector<ColorPreset> kPresets = {
      {"standard", make(50, 50, 100, 6500, 100, 100, 100, 100)}, {"vibrant", make(52, 60, 95, 6500, 100, 100, 100, 135)},
      {"cinema", make(46, 56, 110, 6200, 100, 98, 96, 95)},       {"internet", make(50, 50, 100, 6500, 100, 100, 100, 105)},
      {"photo", make(50, 52, 100, 6500, 100, 100, 100, 110)},     {"video", make(52, 54, 105, 6500, 100, 100, 100, 115)},
      {"lowblue", make(50, 50, 100, 4500, 100, 100, 100, 100)}};
  return kPresets;
}

const ColorSettings* FindColorPreset(const std::string& name) {
  for (const ColorPreset& p : ColorPresets())
    if (name == p.name) return &p.c;
  return nullptr;
}

}  // namespace bc
