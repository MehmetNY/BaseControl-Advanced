// Pure maths for the software colour controls (gamma ramps). Lives in common/ so it is unit-tested on any host.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "hw_iface.h"

namespace bc {

struct KelvinGain {
  double r, g, b;  // 0..1, the strongest channel is exactly 1
};

// Colour temperature (3000..9000 K) as per-channel attenuation; 6500 K is neutral (1,1,1).
KelvinGain KelvinToGain(int kelvin);

// Builds three 256-entry 16-bit ramps (R,G,B). The result is monotonic non-decreasing and never deviates more than
// +-127 steps (x256) from the identity ramp: Windows rejects ramps beyond that range, and it guarantees that a bad
// slider position can never make the panel unreadable.
void BuildGammaRamp(const ColorSettings& c, uint16_t out[3][256]);

// The 5x5 colour matrix for a saturation (0..200 %, 100 = identity) in the row-vector convention of the Windows MAGCOLOREFFECT:
// out = [r g b a 1] x M. Luminance weights are Rec. 709, so grey stays grey and 0 % is a plain luminance image.
void SaturationMatrix(int saturationPercent, float m[5][5]);

// What the settings do to one sRGB colour on screen: saturation first (the desktop compositor), then the gamma ramp (the display output),
// the same order Windows applies them in. Used by the UI to preview a draft before anything is sent.
void TransformRgb(const ColorSettings& c, const uint16_t ramp[3][256], int r, int g, int b, int* ro, int* go, int* bo);

// Draws a colour as the panel will show it under `want` while the panel already applies `live` (its gamma ramp and its colour effect are
// on every pixel, the UI's own included). That is how the UI previews a draft without the live colours piling up on top of it: the
// colour is taken through `want`, then through the inverse of `live`. Where the live ramp has pulled a channel down so far that the panel
// cannot reach the wanted value, the result is clipped to the nearest reachable one. `live` null = nothing is applied (the simulator).
// Built once per change of settings; Map() is a few multiplications and table reads per pixel.
class PreviewMap {
 public:
  PreviewMap(const ColorSettings& want, const ColorSettings* live);
  void Map(int r, int g, int b, int* ro, int* go, int* bo) const;

 private:
  float sat_[3][3];
  uint8_t ramp_[3][256];
  bool undo_ = false;
  uint8_t inv_[3][256];
  float undoSat_ = 1.0f;   // 100 / the live saturation; 1 = nothing to undo
};

// The image profiles on offer. They are colour curves of Control Center Advanced's own (a gamma ramp plus a saturation matrix); the panel's factory
// calibration is never touched. The first entry, "standard", is neutral. Names match the values of the registry enum "disp.mode".
struct ColorPreset {
  const char* name;
  ColorSettings c;
};
const std::vector<ColorPreset>& ColorPresets();
const ColorSettings* FindColorPreset(const std::string& name);   // nullptr for an unknown name

}  // namespace bc
