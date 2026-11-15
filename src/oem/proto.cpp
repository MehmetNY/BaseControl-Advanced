#include "proto.h"

#include <cstring>

#include "../common/util.h"
#include "json.h"

namespace bc {
namespace oem {

// ------------------------------------------------------------------------------------------------ identity
std::string SelectUiPrefix(int bridgeType, int customizeTarget) {
  if (bridgeType == 1) {
    if (customizeTarget == 16 || customizeTarget == 25) return "UWPIntelClient";
    if (customizeTarget == 29) return "UWPADATAClient";
    return "UWPSTDClient";
  }
  return "UWPClient";
}

std::vector<std::string> UiPrefixCandidates(int bridgeType, int customizeTarget) {
  std::vector<std::string> v;
  v.push_back(SelectUiPrefix(bridgeType, customizeTarget));
  for (const char* p : {"UWPClient", "UWPIntelClient", "UWPSTDClient", "UWPADATAClient"}) {
    bool have = false;
    for (const std::string& s : v) have = have || s == p;
    if (!have) v.push_back(p);
  }
  return v;
}

Identity MakeIdentity(const std::string& prefix, int slot) {
  // The password is the vendor client's fixed pattern; it only identifies "a program of the same family on this machine" to the
  // loopback-only broker and is not a secret of the user.
  static const char kTag[] = "888881772688";
  Identity id;
  id.prefix = prefix;
  id.slot = slot;
  id.clientId = Fmt("%s_%d", prefix.c_str(), slot);
  id.user = Fmt("%s_User_%d", prefix.c_str(), slot);
  id.pass = Fmt("%s_Pwd%s_%d", prefix.c_str(), kTag, slot);
  return id;
}

// ------------------------------------------------------------------------------------------------ topics
const std::vector<std::string>& StatusFilters() {
  static const std::vector<std::string> v = {topic::kFanStatus,   topic::kFanTable,       topic::kKeyboardStatus,
                                             topic::kSettingStatus, topic::kSupportInfo,  topic::kCustomizeInfo,
                                             topic::kBatteryStatus};
  return v;
}

std::string StateStem(const std::string& t) {
  if (t == topic::kFanStatus) return "oem.fan";
  if (t == topic::kFanTable) return "oem.table";
  if (t == topic::kKeyboardStatus) return "oem.kb";
  if (t == topic::kSettingStatus) return "oem.set";
  if (t == topic::kSupportInfo) return "oem.sup";
  if (t == topic::kCustomizeInfo) return "oem.info";
  if (t == topic::kBatteryStatus) return "oem.batt";
  return "";
}

// ------------------------------------------------------------------------------------------------ messages
namespace msg {
namespace {
Msg Make(const char* topic, const std::string& payload) {
  Msg m;
  m.topic = topic;
  m.payload = payload;
  return m;
}
Msg FanAction(const char* action) {
  JsonOut j;
  j.BeginObj().Str("Action", action).EndObj();
  return Make(topic::kFanControl, j.Take());
}
}  // namespace

Msg FanGetStatus() { return FanAction("GETSTATUS"); }
Msg FanGetTable() { return FanAction("GET_FAN_SPEED_CURVE_SETTING"); }

Msg FanMode(Mode m, int profileIndex) {
  const char* a = m == Mode::Office ? "OPERATING_OFFICE_MODE" : m == Mode::Gaming ? "OPERATING_GAMING_MODE" : m == Mode::Turbo ? "OPERATING_TURBO_MODE" : "OPERATING_CUSTOM_MODE";
  JsonOut j;
  j.BeginObj().Str("Action", a);
  if (profileIndex >= 0) j.Int("ProfileIndex", profileIndex);
  j.EndObj();
  return Make(topic::kFanControl, j.Take());
}

Msg FanBoost(bool on) { return FanAction(on ? "FAN_BOOST_ON" : "FAN_BOOST_OFF"); }

bool FanDetailAllowed(const std::string& key) {
  // Everything the Performance page edits and nothing else. Deliberately absent: CPUPerformanceAndOverClockMenuSwitch and
  // MemoryOverClockSwitch (the service stores them in the firmware's variable store), the CpuOffsetCoreVoltage pair (applied through a
  // firmware setting that needs reboots), OverClockingSwitch / GpuConfigurableTGP* / GpuDynamicBoost* / CustomTGPinGCUforGN21_* (they move
  // the GPU power state that the verified GPU power engine owns, so two writers would undo each other), FanSwitchSpeed* (the unit of the
  // value is not known) and the AMD-only keys, which this Intel machine does not have.
  static const char* const kAllowed[] = {"PL1", "PL2", "PL4", "CpuTccOffsetSwitch", "CpuTccOffset", "GpuCoreClockOffset", "GpuCoreClockOffsetOC",
                                         "GpuMemoryClockOffset", "GpuMemoryClockOffsetOC", "GpuTargetTemperature"};
  for (const char* k : kAllowed)
    if (key == k) return true;
  return false;
}

bool FanDetail(const std::vector<Detail>& kv, Msg* out) {
  if (kv.empty()) return false;
  JsonOut j;
  j.BeginObj().Str("Action", "SET_OPERATING_MODE_DETAIL");
  for (const Detail& d : kv) {
    if (!d.key || !FanDetailAllowed(d.key)) return false;
    j.Int(d.key, d.value);
  }
  j.EndObj();
  *out = Make(topic::kFanControl, j.Take());
  return true;
}

Msg FanRestoreDetail() { return FanAction("RESTORE_OPERATING_MODE_DETAIL"); }
Msg FanTableRestore() { return FanAction("RESTORE_FAN_SPEED_CURVE_SETTING"); }

Msg FanCustomControl(const std::string& tableName, bool on) {
  JsonOut j;
  j.BeginObj().Str("Action", "SET_FAN_CONTROL_RESPECTIVE").Str("Name", tableName).Bool("FanControlRespective", on).EndObj();
  return Make(topic::kFanControl, j.Take());
}

static void TableObj(JsonOut& j, const char* key, const fan::TableImage& t) {
  j.BeginObj(key);
  for (int part = 0; part < 3; part++) {
    const char* names[3] = {"Duty", "UpT", "DownT"};
    const uint8_t* arr = part == 0 ? t.duty : part == 1 ? t.up : t.down;
    j.BeginObj(names[part]);
    for (int i = 0; i < fan::kSlots; i++) {
      const std::string k = Fmt("T%d", i);
      j.Int(k.c_str(), arr[i]);
    }
    j.EndObj();
  }
  j.EndObj();
}

Msg FanTableSet(const std::string& name, const std::string& type, const fan::TableImage* cpu, const fan::TableImage* gpu) {
  JsonOut j;
  j.BeginObj().Str("Action", "SET_FAN_SPEED_CURVE_SETTING").Str("Name", name).Str("Type", type);
  if (cpu) TableObj(j, "CPU", *cpu);
  if (gpu) TableObj(j, "GPU", *gpu);
  j.EndObj();
  return Make(topic::kFanControl, j.Take());
}

// ---- keyboard: the handler reads both "function" and "Action" from every message and answers a status request first, then runs the
// function; so a command carries a harmless Action and a status request carries a function that does nothing.
static Msg Kb(const char* function, const char* action, JsonOut* extra) {
  JsonOut j;
  j.BeginObj().Str("function", function).Str("Action", action);
  if (extra) {
    // splice the extra members in (they were built as a complete object)
    std::string e = extra->Take();
    j.EndObj();
    std::string head = j.Take();
    head.pop_back();                                   // drop the closing brace
    if (e.size() > 2) head += "," + e.substr(1, e.size() - 2);
    return Make(topic::kKeyboardCtrl, head + "}");
  }
  j.EndObj();
  return Make(topic::kKeyboardCtrl, j.Take());
}

Msg KbGetStatus() { return Kb("None", "GETSTATUS", nullptr); }
Msg KbInit() { return Kb("Init", "SET", nullptr); }

Msg KbPower(bool on) {
  JsonOut e;
  e.BeginObj().Int("powerstatus", on ? 1 : 0).EndObj();
  return Kb("SetPower", "SET", &e);
}

Msg KbEffectSet(const KbEffect& ef) {
  JsonOut e;
  e.BeginObj()
      .Int("mode", ef.mode)
      .Int("effect", ef.effect)
      .Int("light", ef.light < 0 ? 0 : ef.light > 4 ? 4 : ef.light)
      .Int("speed", ef.speed < 0 ? 0 : ef.speed > 4 ? 4 : ef.speed)
      .Int("direction", ef.direction)
      .Bool("nv_save", ef.save)
      .BeginObj("color")
      .Bool("isCircular", ef.circular);
  const size_t n = ef.colors.empty() ? 1 : (ef.colors.size() > 16 ? 16 : ef.colors.size());
  e.Int("ColorBlocks", static_cast<long long>(n)).BeginArr("ColorBuffer");
  for (size_t i = 0; i < n; i++) {
    const KbColor c = ef.colors.empty() ? KbColor() : ef.colors[i];
    auto cl = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
    e.BeginObj().Int("ID", static_cast<long long>(i)).Int("R", cl(c.r)).Int("G", cl(c.g)).Int("B", cl(c.b)).EndObj();
  }
  e.EndArr().EndObj().Int("backgroundcolor", ef.background).Str("alphabet", "").EndObj();
  return Kb("SetEffectALL", "SET", &e);
}

// ---- Setting/Control
bool SettingActionAllowed(const std::string& a) {
  static const char* const kAllowed[] = {
      "WIFI_ON", "WIFI_OFF", "BT_ON", "BT_OFF", "WEBCAM_ON", "WEBCAM_OFF", "TOUCHPAD_ON", "TOUCHPAD_OFF", "AUTOBRIGHT_ON", "AUTOBRIGHT_OFF",
      "WINKEY_LOCK", "WINKEY_UNLOCK", "FNKEY_LOCK", "FNKEY_UNLOCK", "NUMPAD_LOCK", "NUMPAD_UNLOCK", "OSD_HIDDEN_ON", "OSD_HIDDEN_OFF",
      "TOUCHPAD_TOGGLE_ON", "TOUCHPAD_TOGGLE_OFF", "USB_CHARGER_ON", "USB_CHARGER_OFF", "ACRECOVERY_TOGGLE_ON", "ACRECOVERY_TOGGLE_OFF",
      "FN_WITH1_HOTKEY_TOGGLE_ON", "FN_WITH1_HOTKEY_TOGGLE_OFF", "DGPU_DIRECT_CONNECT_TOGGLE_ON", "DGPU_DIRECT_CONNECT_TOGGLE_OFF",
      "KEYBOARD_LIGHTBAR_TIMER_ON", "KEYBOARD_LIGHTBAR_TIMER_OFF", "GETSTATUS"};
  for (const char* k : kAllowed)
    if (a == k) return true;
  return false;
}

Msg SettingGetStatus() { return SettingAction("GETSTATUS"); }

Msg SettingAction(const std::string& action) {
  JsonOut j;
  j.BeginObj().Str("Action", action).EndObj();
  return Make(topic::kSettingControl, j.Take());
}

Msg SettingBrightness(int pct) {
  JsonOut j;
  j.BeginObj().Str("Action", "SETSCREENBRIGHTNESS").Int("brightness", pct < 0 ? 0 : pct > 100 ? 100 : pct).EndObj();
  return Make(topic::kSettingControl, j.Take());
}

Msg SettingCloseTimer(int minutes, int seconds) {
  JsonOut j;
  j.BeginObj().Str("Action", "SetKeyboardCloseTime").Int("Mins", minutes < 0 ? 0 : minutes).Int("Seconds", seconds < 0 ? 0 : seconds).EndObj();
  return Make(topic::kSettingControl, j.Take());
}

Msg BatteryProfile(int mode) {
  const char* a = mode == 0 ? "PERFORMANCEDMODE" : mode == 1 ? "BALANCEDMODE" : "HEALTHYMODE";
  JsonOut j;
  j.BeginObj().Str("Action", a).EndObj();
  return Make(topic::kBatteryControl, j.Take());
}

Msg BatteryReport() {
  JsonOut j;
  j.BeginObj().Str("Action", "Report").EndObj();
  return Make(topic::kBatteryControl, j.Take());
}

Msg CustomizeGetSupport() {
  JsonOut j;
  j.BeginObj().Str("Action", "GETSUPPORT").EndObj();
  return Make(topic::kCustomizeControl, j.Take());
}
}  // namespace msg

const char* KbEffectName(int e) {
  switch (e) {
    case 0: return "Static";
    case 1: return "Breathing";
    case 2: return "Wave";
    case 3: return "Reactive";
    case 4: return "Rainbow";
    case 5: return "Ripple";
    case 6: return "Raindrop";
    case 7: return "Neon";
    case 8: return "Marquee";
    case 9: return "Stack";
    case 10: return "Impact";
    case 11: return "Spark";
    case 12: return "Aurora";
    case 13: return "Music";
    case 14: return "User mode";
    case 15: return "Gaming";
    case 16: return "Flash";
    case 17: return "Mix";
    default: return "";
  }
}

}  // namespace oem
}  // namespace bc
