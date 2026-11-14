#include "os_display.h"

#include <powrprof.h>
#include <winioctl.h>

#include <algorithm>
#include <set>

#include "../common/colorramp.h"
#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

constexpr DWORD kIoctlQuerySupported = 0x230494;  // CTL_CODE(FILE_DEVICE_VIDEO, 0x125, ...) IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS
constexpr DWORD kIoctlQueryBrightness = 0x230498; // IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS
constexpr DWORD kIoctlSetBrightness = 0x23049C;   // IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS
struct DisplayBrightness {
  UCHAR policy;  // 1 AC, 2 DC, 3 both
  UCHAR ac;
  UCHAR dc;
};

const GUID kSubVideo = {0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
const GUID kVideoBrightness = {0xaded5e82, 0xb909, 0x4619, {0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb}};

bool OnAc() {
  SYSTEM_POWER_STATUS s;
  return !GetSystemPowerStatus(&s) || s.ACLineStatus != 0;
}

HANDLE OpenLcd() {
  return CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

const wchar_t* DevName(const std::wstring& n) { return n.empty() ? nullptr : n.c_str(); }

bool IsInternalTech(int tech) {
  // INTERNAL (0x80000000), DISPLAYPORT_EMBEDDED (11), UDI_EMBEDDED (13)
  return static_cast<unsigned>(tech) == 0x80000000u || tech == 11 || tech == 13;
}

}  // namespace

std::wstring OsDisplay::InternalPanelName() {
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return L"";
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm ? nm : 1);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return L"";
  for (UINT32 i = 0; i < np; ++i) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME t = {};
    t.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    t.header.size = sizeof t;
    t.header.adapterId = paths[i].targetInfo.adapterId;
    t.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&t.header) != ERROR_SUCCESS) continue;
    if (!IsInternalTech(static_cast<int>(t.outputTechnology))) continue;
    DISPLAYCONFIG_SOURCE_DEVICE_NAME s = {};
    s.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    s.header.size = sizeof s;
    s.header.adapterId = paths[i].sourceInfo.adapterId;
    s.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&s.header) == ERROR_SUCCESS && s.viewGdiDeviceName[0]) return s.viewGdiDeviceName;
  }
  return L"";
}

DisplayInfo OsDisplay::Info() {
  DisplayInfo info;
  const std::wstring name = InternalPanelName();
  DEVMODEW cur = {};
  cur.dmSize = sizeof cur;
  if (!EnumDisplaySettingsW(DevName(name), ENUM_CURRENT_SETTINGS, &cur)) return info;
  info.ok = true;
  info.width = static_cast<int>(cur.dmPelsWidth);
  info.height = static_cast<int>(cur.dmPelsHeight);
  info.refresh = static_cast<int>(cur.dmDisplayFrequency);
  std::set<int> rates;
  DEVMODEW dm = {};
  dm.dmSize = sizeof dm;
  for (DWORD i = 0; EnumDisplaySettingsW(DevName(name), i, &dm); ++i) {
    if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight && dm.dmBitsPerPel == cur.dmBitsPerPel &&
        dm.dmDisplayOrientation == cur.dmDisplayOrientation && dm.dmDisplayFrequency > 1)
      rates.insert(static_cast<int>(dm.dmDisplayFrequency));
  }
  info.refreshRates.assign(rates.begin(), rates.end());

  // brightness: IOCTL first, then the active power scheme
  HANDLE h = OpenLcd();
  if (h != INVALID_HANDLE_VALUE) {
    DisplayBrightness b = {0, 0, 0};
    DWORD ret = 0;
    if (DeviceIoControl(h, kIoctlQueryBrightness, nullptr, 0, &b, sizeof b, &ret, nullptr) && ret >= sizeof b)
      info.brightness = OnAc() ? b.ac : b.dc;
    CloseHandle(h);
  }
  if (info.brightness < 0) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
      DWORD v = 0;
      const DWORD rc = OnAc() ? PowerReadACValueIndex(nullptr, scheme, &kSubVideo, &kVideoBrightness, &v)
                              : PowerReadDCValueIndex(nullptr, scheme, &kSubVideo, &kVideoBrightness, &v);
      LocalFree(scheme);
      if (rc == ERROR_SUCCESS && v <= 100) info.brightness = static_cast<int>(v);
    }
  }
  return info;
}

bool OsDisplay::SetBrightness(int pct, std::string* why) {
  pct = Clamp(pct, 0, 100);
  HANDLE h = OpenLcd();
  if (h != INVALID_HANDLE_VALUE) {
    // snap to a level the panel supports
    UCHAR levels[256] = {0};
    DWORD ret = 0;
    int level = pct;
    if (DeviceIoControl(h, kIoctlQuerySupported, nullptr, 0, levels, sizeof levels, &ret, nullptr) && ret > 0) {
      int best = levels[0];
      for (DWORD i = 0; i < ret; ++i)
        if (std::abs(static_cast<int>(levels[i]) - pct) < std::abs(best - pct)) best = levels[i];
      level = best;
    }
    DisplayBrightness cur = {3, 0, 0};
    DWORD r2 = 0;
    const bool have = DeviceIoControl(h, kIoctlQueryBrightness, nullptr, 0, &cur, sizeof cur, &r2, nullptr) && r2 >= sizeof cur;
    DisplayBrightness want = {static_cast<UCHAR>(OnAc() ? 1 : 2), static_cast<UCHAR>(have ? cur.ac : level), static_cast<UCHAR>(have ? cur.dc : level)};
    if (OnAc()) want.ac = static_cast<UCHAR>(level);
    else want.dc = static_cast<UCHAR>(level);
    DWORD r3 = 0;
    const bool ok = DeviceIoControl(h, kIoctlSetBrightness, &want, sizeof want, nullptr, 0, &r3, nullptr) != 0;
    const DWORD le = GetLastError();
    CloseHandle(h);
    if (ok) return true;
    if (why) *why = "brightness IOCTL failed: " + Win32Text(le);
  }
  // fallback: power scheme value (Windows applies it when the scheme is re-activated)
  GUID* scheme = nullptr;
  if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) {
    if (why && why->empty()) *why = "no brightness interface available";
    return false;
  }
  const DWORD rc = OnAc() ? PowerWriteACValueIndex(nullptr, scheme, &kSubVideo, &kVideoBrightness, static_cast<DWORD>(pct))
                          : PowerWriteDCValueIndex(nullptr, scheme, &kSubVideo, &kVideoBrightness, static_cast<DWORD>(pct));
  const DWORD rc2 = rc == ERROR_SUCCESS ? PowerSetActiveScheme(nullptr, scheme) : rc;
  LocalFree(scheme);
  if (rc2 != ERROR_SUCCESS) {
    if (why) *why = "power scheme brightness: " + Win32Text(rc2);
    return false;
  }
  if (why) why->clear();
  return true;
}

bool OsDisplay::SetRefresh(int hz, std::string* why) {
  const std::wstring name = InternalPanelName();
  DEVMODEW cur = {};
  cur.dmSize = sizeof cur;
  if (!EnumDisplaySettingsW(DevName(name), ENUM_CURRENT_SETTINGS, &cur)) {
    if (why) *why = "cannot read the current display mode";
    return false;
  }
  DisplayInfo info = Info();
  if (std::find(info.refreshRates.begin(), info.refreshRates.end(), hz) == info.refreshRates.end()) {
    if (why) *why = Fmt("%d Hz is not offered by the panel at %dx%d", hz, info.width, info.height);
    return false;
  }
  DEVMODEW dm = cur;
  dm.dmDisplayFrequency = static_cast<DWORD>(hz);
  dm.dmFields |= DM_DISPLAYFREQUENCY;
  LONG rc = ChangeDisplaySettingsExW(DevName(name), &dm, nullptr, CDS_TEST, nullptr);
  if (rc != DISP_CHANGE_SUCCESSFUL) {
    if (why) *why = Fmt("Windows rejected the mode (%ld)", static_cast<long>(rc));
    return false;
  }
  rc = ChangeDisplaySettingsExW(DevName(name), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
  if (rc != DISP_CHANGE_SUCCESSFUL) {
    if (why) *why = Fmt("mode change failed (%ld)", static_cast<long>(rc));
    return false;
  }
  return true;
}

bool OsDisplay::SetColor(const ColorSettings& c, std::string* why) {
  uint16_t ramp[3][256];
  BuildGammaRamp(c, ramp);
  const std::wstring name = InternalPanelName();
  HDC dc = CreateDCW(L"DISPLAY", DevName(name), nullptr, nullptr);
  bool own = dc != nullptr;
  if (!dc) dc = GetDC(nullptr);
  if (!dc) {
    if (why) *why = "no device context for the display";
    return false;
  }
  const BOOL ok = SetDeviceGammaRamp(dc, ramp);
  const DWORD le = GetLastError();
  if (own) DeleteDC(dc);
  else ReleaseDC(nullptr, dc);
  if (!ok) {
    if (why) *why = "Windows rejected the colour ramp: " + Win32Text(le);
    return false;
  }
  return true;
}

}  // namespace hw
}  // namespace bc
