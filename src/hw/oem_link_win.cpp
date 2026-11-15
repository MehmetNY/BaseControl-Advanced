#include "oem_link_win.h"

#include "winutil.h"

namespace bc {
namespace hw {
namespace {

// REG_DWORD, or a decimal REG_SZ (the installers of the vendor's package have used both)
bool ReadNumber(HKEY k, const wchar_t* name, int* out) {
  DWORD type = 0, size = 0;
  if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return false;
  if (type == REG_DWORD && size == sizeof(DWORD)) {
    DWORD v = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size) != ERROR_SUCCESS) return false;
    *out = static_cast<int>(v);
    return true;
  }
  if (type == REG_SZ && size >= sizeof(wchar_t) && size < 64) {
    wchar_t buf[32] = {0};
    size = sizeof(buf) - sizeof(wchar_t);
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) != ERROR_SUCCESS) return false;
    wchar_t* end = nullptr;
    const long v = wcstol(buf, &end, 10);
    if (end == buf) return false;
    *out = static_cast<int>(v);
    return true;
  }
  return false;
}

}  // namespace

OemLinkRegistry ReadOemLinkRegistry() {
  OemLinkRegistry r;
  bool haveBridge = false, haveTarget = false;
  for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OEM\\GamingCenter2", 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) continue;
    int v = 0;
    if (!haveBridge && ReadNumber(k, L"BridgeType", &v)) {
      r.bridgeType = v;
      haveBridge = true;
    }
    if (!haveTarget && ReadNumber(k, L"CustomizeTarget", &v)) {
      r.customizeTarget = v;
      haveTarget = true;
    }
    RegCloseKey(k);
  }
  r.found = haveBridge || haveTarget;
  return r;
}

std::unique_ptr<oem::IOemLink> MakeOemLink() {
  const OemLinkRegistry reg = ReadOemLinkRegistry();
  oem::LinkConfig cfg;
  cfg.prefixes = oem::UiPrefixCandidates(reg.bridgeType, reg.customizeTarget);
  return std::unique_ptr<oem::IOemLink>(new oem::MqttOemLink(cfg));
}

}  // namespace hw
}  // namespace bc
