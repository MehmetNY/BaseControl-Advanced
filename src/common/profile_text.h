// Profile bookkeeping shared by the controller (which publishes it) and the UI (which shows it).
#pragma once
#include <string>
#include <vector>

#include "kv.h"

namespace bc {

// The four profiles of the vendor's Control Center, in display order (the service's OperatingMode 0..3): Balanced (office), Enthusiast
// (gaming), Overboost (turbo) and Customized. The program does not add profiles of its own.
const std::vector<std::string>& BuiltinProfiles();
bool IsBuiltinProfile(const std::string& name);   // not case-sensitive

// Profile names for display, taken from the snapshot ("profile.list", comma separated): duplicates and blanks dropped.
std::vector<std::string> ProfileList(const KV& snap);

}  // namespace bc
