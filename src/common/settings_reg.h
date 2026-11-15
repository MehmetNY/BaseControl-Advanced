// Setting registry: every value that can be changed from the UI (or a profile / the CLI) is declared here once,
// with its type, range, evidence tier and the capability that must be present. The controller refuses everything
// that is not in this table (no raw addresses, no free-form writes).
#pragma once
#include <string>
#include <vector>

namespace bc {

enum class Kind { Int, Bool, Enum, Text };

// Where a value goes (shown in the UI as a badge):
//  Verified - the program's own GPU power engine, measured on a GM5TG7Y by the device owner
//  Service  - the vendor's own Control Center service does it, exactly as the vendor's window would; every change is confirmed by what
//             the service and the embedded controller report afterwards
//  Os       - plain Windows API; no firmware involved
enum class Ev { Verified, Service, Os };

struct SettingDef {
  const char* key;
  Kind kind;
  int min, max, step;        // Int: range + step (value must be min + k*step). Enum: min=0,max=count-1
  const char* unit;          // "W", "C", "%", "mV", "ms", ""
  Ev ev;
  const char* cap;           // capability key that must read "ok" ("" = always available)
  const char* enumNames;     // comma separated, for Kind::Enum
};

const SettingDef* FindSetting(const std::string& key);
const std::vector<SettingDef>& AllSettings();

struct Validated {
  bool ok = false;
  std::string why;     // technical English reason when !ok
  std::string value;   // normalised string form
  int ival = 0;        // Int/Bool/Enum value
};
Validated ValidateSetting(const SettingDef& d, const std::string& raw);

// Enum helpers
int EnumIndex(const SettingDef& d, const std::string& name);  // -1 if unknown
std::string EnumName(const SettingDef& d, int index);
int EnumCount(const SettingDef& d);

const char* EvName(Ev e);

}  // namespace bc
