#include "os_devices.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <wlanapi.h>

#include <algorithm>

#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

// Device setup classes
const GUID kClassBluetooth = {0xe0cbf06c, 0xcd8b, 0x4647, {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}};
const GUID kClassCamera = {0xca3e7ab9, 0xb4c3, 0x4ae6, {0x82, 0x51, 0x57, 0x9e, 0xf9, 0x33, 0x89, 0x0f}};
const GUID kClassImage = {0x6bdd1fc6, 0x810f, 0x11d0, {0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f}};

struct Node {
  DEVINST inst = 0;
  bool disabled = false;
  std::string id, name;
};

std::string PropString(HDEVINFO set, SP_DEVINFO_DATA* d, DWORD prop) {
  wchar_t buf[512] = {0};
  DWORD type = 0, need = 0;
  if (!SetupDiGetDeviceRegistryPropertyW(set, d, prop, &type, reinterpret_cast<PBYTE>(buf), sizeof buf - 2, &need)) return "";
  return U(buf);
}

bool Removable(HDEVINFO set, SP_DEVINFO_DATA* d) {
  DWORD caps = 0, type = 0, need = 0;
  if (!SetupDiGetDeviceRegistryPropertyW(set, d, SPDRP_CAPABILITIES, &type, reinterpret_cast<PBYTE>(&caps), sizeof caps, &need)) return true;
  return (caps & CM_DEVCAP_REMOVABLE) != 0;
}

// Collects the devices of a class that this program may switch.
std::vector<Node> Collect(Dev d) {
  std::vector<Node> out;
  const GUID* classes[2] = {nullptr, nullptr};
  if (d == Dev::Bluetooth) classes[0] = &kClassBluetooth;
  else {
    classes[0] = &kClassCamera;
    classes[1] = &kClassImage;
  }
  for (const GUID* cls : classes) {
    if (!cls) continue;
    HDEVINFO set = SetupDiGetClassDevsW(cls, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) continue;
    SP_DEVINFO_DATA did;
    did.cbSize = sizeof did;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &did); ++i) {
      wchar_t idw[MAX_DEVICE_ID_LEN] = {0};
      if (CM_Get_Device_IDW(did.DevInst, idw, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) continue;
      const std::string id = Lower(U(idw));
      std::string name = PropString(set, &did, SPDRP_FRIENDLYNAME);
      if (name.empty()) name = PropString(set, &did, SPDRP_DEVICEDESC);
      const std::string ln = Lower(name);
      if (d == Dev::Bluetooth) {
        // the radio itself sits on USB/PCI/ACPI; BTH*/BTHENUM*/BTHLE* are its child devices
        if (StartsWith(id, "bth")) continue;
        if (!(StartsWith(id, "usb\\") || StartsWith(id, "pci\\") || StartsWith(id, "acpi\\") || StartsWith(id, "uart\\"))) continue;
      } else {
        if (!StartsWith(id, "usb\\")) continue;
        if (Removable(set, &did)) continue;  // external webcams stay untouched
        if (ln.find("infrared") != std::string::npos || ln.find(" ir ") != std::string::npos || ln.find("ir camera") != std::string::npos ||
            ln.find("hello") != std::string::npos)
          continue;
      }
      ULONG status = 0, problem = 0;
      Node n;
      n.inst = did.DevInst;
      if (CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0) == CR_SUCCESS) n.disabled = (problem == CM_PROB_DISABLED);
      n.id = id;
      n.name = name;
      out.push_back(n);
    }
    SetupDiDestroyDeviceInfoList(set);
  }
  return out;
}

bool ChangeState(Dev d, DEVINST inst, bool enable, std::string* why) {
  // Re-open the device list and locate the node again: SetupAPI wants the SP_DEVINFO_DATA of an open set.
  const GUID* classes[2] = {d == Dev::Bluetooth ? &kClassBluetooth : &kClassCamera, d == Dev::Bluetooth ? nullptr : &kClassImage};
  for (const GUID* cls : classes) {
    if (!cls) continue;
    HDEVINFO set = SetupDiGetClassDevsW(cls, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) continue;
    SP_DEVINFO_DATA did;
    did.cbSize = sizeof did;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &did); ++i) {
      if (did.DevInst != inst) continue;
      SP_PROPCHANGE_PARAMS pcp = {};
      pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
      pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
      pcp.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
      pcp.Scope = DICS_FLAG_GLOBAL;
      pcp.HwProfile = 0;
      bool ok = SetupDiSetClassInstallParamsW(set, &did, &pcp.ClassInstallHeader, sizeof pcp) &&
                SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &did);
      const DWORD le = GetLastError();
      SetupDiDestroyDeviceInfoList(set);
      if (!ok && why) *why = "SetupAPI: " + Win32Text(le);
      return ok;
    }
    SetupDiDestroyDeviceInfoList(set);
  }
  if (why) *why = "device vanished";
  return false;
}

// ---- WLAN
struct WlanSession {
  HANDLE h = nullptr;
  WLAN_INTERFACE_INFO_LIST* list = nullptr;
  bool Open() {
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) {
      h = nullptr;
      return false;
    }
    if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS || !list || list->dwNumberOfItems == 0) return false;
    return true;
  }
  ~WlanSession() {
    if (list) WlanFreeMemory(list);
    if (h) WlanCloseHandle(h, nullptr);
  }
};

}  // namespace

// ------------------------------------------------------------------------------------------------ Wi-Fi
int OsDevices::WifiGet() {
  WlanSession s;
  if (!s.Open()) return -1;
  for (DWORD i = 0; i < s.list->dwNumberOfItems; ++i) {
    WLAN_RADIO_STATE* rs = nullptr;
    DWORD size = 0;
    WLAN_OPCODE_VALUE_TYPE type;
    if (WlanQueryInterface(s.h, &s.list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &size, reinterpret_cast<PVOID*>(&rs),
                           &type) != ERROR_SUCCESS || !rs)
      continue;
    int res = -1;
    if (rs->dwNumberOfPhys > 0) {
      const WLAN_PHY_RADIO_STATE& p = rs->PhyRadioState[0];
      res = (p.dot11SoftwareRadioState == dot11_radio_state_on && p.dot11HardwareRadioState == dot11_radio_state_on) ? 1 : 0;
    }
    WlanFreeMemory(rs);
    if (res >= 0) return res;
  }
  return -1;
}

bool OsDevices::WifiSet(bool on, std::string* why) {
  WlanSession s;
  if (!s.Open()) {
    if (why) *why = "no Wi-Fi interface (WLAN AutoConfig service stopped?)";
    return false;
  }
  bool any = false;
  for (DWORD i = 0; i < s.list->dwNumberOfItems; ++i) {
    WLAN_PHY_RADIO_STATE st = {};
    st.dwPhyIndex = 0;
    st.dot11SoftwareRadioState = on ? dot11_radio_state_on : dot11_radio_state_off;
    st.dot11HardwareRadioState = dot11_radio_state_unknown;
    const DWORD rc = WlanSetInterface(s.h, &s.list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_radio_state, sizeof st, &st, nullptr);
    if (rc == ERROR_SUCCESS) any = true;
    else if (why) *why = "WlanSetInterface: " + Win32Text(rc);
  }
  return any;
}

// ------------------------------------------------------------------------------------------------ Bluetooth / camera
int OsDevices::PnpGet(Dev d) {
  std::vector<Node> v = Collect(d);
  if (v.empty()) return -1;
  for (const Node& n : v)
    if (!n.disabled) return 1;  // any enabled device -> "on"
  return 0;
}

bool OsDevices::PnpSet(Dev d, bool on, std::string* why) {
  std::vector<Node> v = Collect(d);
  if (v.empty()) {
    if (why) *why = "device not found";
    return false;
  }
  bool all = true;
  for (const Node& n : v) {
    if (n.disabled == !on) continue;  // already in the wanted state
    all = ChangeState(d, n.inst, on, why) && all;
  }
  return all;
}

// ------------------------------------------------------------------------------------------------ touchpad
int OsDevices::TouchpadGet() {
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad\\Status", 0, KEY_READ, &k) != ERROR_SUCCESS) {
    // key absent: a precision touchpad that was never toggled keeps the default (enabled) - only report it when the parent exists
    HKEY p = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad", 0, KEY_READ, &p) != ERROR_SUCCESS) return -1;
    RegCloseKey(p);
    return 1;
  }
  DWORD v = 1, sz = sizeof v, type = 0;
  const LONG rc = RegQueryValueExW(k, L"Enabled", nullptr, &type, reinterpret_cast<BYTE*>(&v), &sz);
  RegCloseKey(k);
  if (rc != ERROR_SUCCESS || type != REG_DWORD) return 1;
  return v ? 1 : 0;
}

bool OsDevices::TouchpadSet(bool on, std::string* why) {
  HKEY k = nullptr;
  DWORD disp = 0;
  const LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad\\Status", 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &k, &disp);
  if (rc != ERROR_SUCCESS) {
    if (why) *why = "registry: " + Win32Text(static_cast<DWORD>(rc));
    return false;
  }
  const DWORD v = on ? 1 : 0;
  const LONG rc2 = RegSetValueExW(k, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
  RegCloseKey(k);
  if (rc2 != ERROR_SUCCESS) {
    if (why) *why = "registry: " + Win32Text(static_cast<DWORD>(rc2));
    return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------ dispatch
int OsDevices::Get(Dev d) {
  switch (d) {
    case Dev::Wifi: return scope_ == Scope::UserOnly ? -1 : WifiGet();
    case Dev::Bluetooth:
    case Dev::Webcam: return scope_ == Scope::UserOnly ? -1 : PnpGet(d);
    case Dev::Touchpad: return scope_ == Scope::ServiceOnly ? -1 : TouchpadGet();
  }
  return -1;
}

bool OsDevices::Set(Dev d, bool on, std::string* why) {
  if (Get(d) < 0) {
    if (why) *why = "device not available in this process";
    return false;
  }
  switch (d) {
    case Dev::Wifi: return WifiSet(on, why);
    case Dev::Bluetooth:
    case Dev::Webcam: return PnpSet(d, on, why);
    case Dev::Touchpad: return TouchpadSet(on, why);
  }
  return false;
}

}  // namespace hw
}  // namespace bc
