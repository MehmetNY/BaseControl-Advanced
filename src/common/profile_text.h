// Profile bookkeeping shared by the controller (which publishes it) and the UI (which shows it).
#pragma once
#include <string>
#include <vector>

#include "kv.h"

namespace bc {

// The four profiles that ship with BaseControl, in display order. They always exist and can be neither deleted nor overwritten.
const std::vector<std::string>& BuiltinProfiles();
bool IsBuiltinProfile(const std::string& name);   // not case-sensitive

// What one profile stores travels in the snapshot as "ec.pl1=60|ec.pl2=80|fan.mode=auto" (key "profile.v.<name>").
// Keys and values are escaped, so any stored text round-trips.
std::string FormatProfileValues(const KV& values);
KV ParseProfileValues(const std::string& text);

// Profile names for display, taken from the snapshot ("profile.list"): built-ins first, then the user's own.
std::vector<std::string> ProfileList(const KV& snap);

}  // namespace bc
