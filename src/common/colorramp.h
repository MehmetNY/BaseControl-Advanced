// Pure maths for the software colour controls (gamma ramps). Lives in common/ so it is unit-tested on any host.
#pragma once
#include <cstdint>

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

}  // namespace bc
