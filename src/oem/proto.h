// What the OEM Control Center's service speaks. The vendor's UI and its service talk MQTT 3.1.1 over the loopback interface; the broker
// (GCUBridge) only lets a client in when its id / user name / password follow the pattern below. This file holds the topic names,
// the way a client identifies itself, and builders for every message Control Center Advanced sends. The structure was read from the
// vendor's own program files on the owner's machine (see docs/PROTOCOL.md); nothing here is a guess about the wire format except where a
// comment says so.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../common/fan.h"

namespace bc {
namespace oem {

inline constexpr int kBrokerPort = 13688;
inline constexpr int kOurSlot = 18;     // the vendor's own UI and tray use other slots; a second client with the same id would knock one of them off

struct Identity {
  std::string prefix, clientId, user, pass;
  int slot = 0;
};

// The client class the broker accepts for UI programs is decided by two registry values under HKLM\SOFTWARE\OEM\GamingCenter2.
std::string SelectUiPrefix(int bridgeType, int customizeTarget);
// The selected prefix first, then the other UI classes: used when the broker refuses the first one.
std::vector<std::string> UiPrefixCandidates(int bridgeType, int customizeTarget);
Identity MakeIdentity(const std::string& prefix, int slot);

// ---- topics: what we send to and what we listen to
namespace topic {
inline constexpr const char* kFanControl = "Fan/Control";
inline constexpr const char* kKeyboardCtrl = "Keyboard/Ctrl";
inline constexpr const char* kSettingControl = "Setting/Control";
inline constexpr const char* kBatteryControl = "BatteryProtection/Control";
inline constexpr const char* kCustomizeControl = "Customize/Control";
inline constexpr const char* kFanStatus = "Fan/Status";
inline constexpr const char* kFanTable = "Fan/Table";
inline constexpr const char* kKeyboardStatus = "Keyboard/Status";
inline constexpr const char* kSettingStatus = "Setting/Status";
inline constexpr const char* kSupportInfo = "Customize/SupportInfo";
inline constexpr const char* kCustomizeInfo = "Customize/Info";
inline constexpr const char* kBatteryStatus = "System/BatteryProtection";
}  // namespace topic

const std::vector<std::string>& StatusFilters();            // what we subscribe to by default
inline constexpr const char* kCaptureFilter = "#";          // diagnostics only (Service page): every message on the broker

// Short snapshot-key stem for a status topic ("Fan/Status" -> "oem.fan"); empty for topics we do not keep.
std::string StateStem(const std::string& topic);

// ---- outgoing messages
struct Msg {
  std::string topic, payload;
};

namespace msg {
Msg FanGetStatus();
Msg FanGetTable();
enum class Mode { Office, Gaming, Turbo, Custom };
Msg FanMode(Mode m, int profileIndex);                       // profileIndex < 0: keep the one the service has
Msg FanBoost(bool on);
struct Detail {
  const char* key;
  long long value;
};
// Changes values of the profile that is active right now. Only keys from the allow-list in proto.cpp are ever written; anything else
// (BIOS-backed switches such as the memory overclocking menu, the core-voltage offsets, the nvidia-smi helper, and the GPU power keys that
// the verified GPU power engine owns) is refused here.
bool FanDetailAllowed(const std::string& key);
bool FanDetail(const std::vector<Detail>& kv, Msg* out);
Msg FanRestoreDetail();
// Fan curves: the service names its tables itself (Fan/Status "FAN_TableName"); we send back the name and type it gave us.
Msg FanTableSet(const std::string& name, const std::string& type, const fan::TableImage* cpu, const fan::TableImage* gpu);
Msg FanCustomControl(const std::string& tableName, bool on);   // "Custom Fan Control": use the table above instead of the profile's own
Msg FanTableRestore();

Msg KbGetStatus();
Msg KbInit();
Msg KbPower(bool on);
struct KbColor {
  int r = 255, g = 255, b = 255;
};
struct KbEffect {
  int mode = 1;          // RGBKB_Mode: 1 = lighting, 2 = welcome effect at start-up
  int effect = 0;        // RGBKB_Effect (0 = single colour)
  int light = 3;         // brightness level 0..4 (the service translates it to the controller's scale)
  int speed = 2;         // 0..4
  int direction = -1;    // RGBKB_Direction: -1 none, 0 left-right, 1 right-left, 2 down-up, 3 up-down, 4 on key press, 5 sync
  bool circular = false;
  std::vector<KbColor> colors;   // one entry per colour block (1..16)
  int background = 0;
  bool save = false;             // nv_save: also keep the setting in the keyboard's own memory ("Save to BIOS" in the vendor's window)
};
Msg KbEffectSet(const KbEffect& e);

Msg SettingGetStatus();
Msg SettingAction(const std::string& action);                // "WIFI_ON", "FNKEY_LOCK" ... checked against the allow-list
bool SettingActionAllowed(const std::string& action);
Msg SettingBrightness(int pct);
Msg SettingCloseTimer(int minutes, int seconds);

Msg BatteryProfile(int mode);                                 // 0 high capacity, 1 balanced, 2 eco
Msg BatteryReport();
Msg CustomizeGetSupport();
}  // namespace msg

// ---- keyboard enums as numbers (RGBKB_Effect / Direction), kept in one place for the UI
const char* KbEffectName(int effect);                         // English name, "" when unknown

}  // namespace oem
}  // namespace bc
