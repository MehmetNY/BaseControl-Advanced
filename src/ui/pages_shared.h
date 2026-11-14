// Small pieces shared by several page files.
#pragma once
#include <vector>

#include "page.h"

namespace bc {
namespace ui {

// Refresh rates the internal panel offers (from the snapshot key "disp.refresh.list"), ascending.
std::vector<int> RefreshRates(App& a);

// Name of a charge profile ("highcap" | "balanced" | "eco") as the user reads it.
std::string BattModeLabel(const std::string& mode);

// Name of a profile as the user reads it: the four built-in ones are translated, saved ones show as typed.
std::string ProfileLabel(const std::string& name);
// Icon and accent colour that go with a profile (built-in ones have their own, saved ones share a neutral one).
IconId ProfileIconOf(const std::string& name);
uint32_t ProfileColor(const Theme& th, const std::string& name);

}  // namespace ui
}  // namespace bc
