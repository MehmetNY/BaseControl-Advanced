#include "ite_hid.h"

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <setupapi.h>

#include <algorithm>

#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

constexpr USHORT kVidIte = 0x048D;
constexpr LONG kHidpSuccess = 0x00110000;

bool CommandAllowed(uint8_t c) { return c == 0x08 || c == 0x09 || c == 0x12 || c == 0x14 || c == 0x1A; }

std::wstring LowerPath(const std::wstring& p) {
  std::wstring s = p;
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

HANDLE OpenForFeature(const std::wstring& path) {
  for (DWORD access : {DWORD(GENERIC_WRITE), DWORD(GENERIC_READ | GENERIC_WRITE)}) {
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
  }
  return INVALID_HANDLE_VALUE;
}

}  // namespace

IteHid::IteHid() { InitializeCriticalSection(&cs_); }

IteHid::~IteHid() {
  CloseAll();
  DeleteCriticalSection(&cs_);
}

void IteHid::CloseAll() {
  for (auto& p : handles_)
    if (p.second != INVALID_HANDLE_VALUE) CloseHandle(p.second);
  handles_.clear();
}

std::vector<LightZone> IteHid::Detect(std::string* info) {
  EnterCriticalSection(&cs_);
  CloseAll();
  paths_.clear();
  std::vector<LightZone> zones;
  std::string log;
  GUID hid;
  HidD_GetHidGuid(&hid);
  HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  std::vector<Dev> found;
  if (set != INVALID_HANDLE_VALUE) {
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); ++i) {
      DWORD need = 0;
      SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
      if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || need > 4096) continue;
      std::vector<BYTE> buf(need);
      SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
      det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
      const std::wstring path = det->DevicePath;
      if (LowerPath(path).find(L"vid_048d") == std::wstring::npos) continue;
      HANDLE h = INVALID_HANDLE_VALUE;
      for (DWORD access : {DWORD(0), DWORD(GENERIC_READ | GENERIC_WRITE), DWORD(GENERIC_WRITE)}) {
        h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
      }
      if (h == INVALID_HANDLE_VALUE) {
        log += Fmt("ITE interface: cannot open (%lu); ", static_cast<unsigned long>(GetLastError()));
        continue;
      }
      HIDD_ATTRIBUTES attr;
      attr.Size = sizeof attr;
      PHIDP_PREPARSED_DATA pre = nullptr;
      HIDP_CAPS caps;
      Dev d;
      d.path = path;
      bool good = false;
      if (HidD_GetAttributes(h, &attr)) d.pid = attr.ProductID;
      if (HidD_GetPreparsedData(h, &pre)) {
        if (HidP_GetCaps(pre, &caps) == kHidpSuccess) {
          d.usage = caps.Usage;
          d.usagePage = caps.UsagePage;
          good = caps.FeatureReportByteLength == 9;  // the lighting controller's feature report is 9 bytes
          log += Fmt("ITE %04X usage %04X/%04X feature=%u%s; ", d.pid, caps.UsagePage, caps.Usage, caps.FeatureReportByteLength, good ? " (lighting)" : "");
        }
        HidD_FreePreparsedData(pre);
      }
      CloseHandle(h);
      if (good) found.push_back(d);
    }
    SetupDiDestroyDeviceInfoList(set);
  }
  std::sort(found.begin(), found.end(), [](const Dev& a, const Dev& b) { return a.path < b.path; });
  for (const Dev& d : found) {
    if (d.usage == 1 && !paths_.count(0)) paths_[0] = d.path;
    if (d.usage == 2 && !paths_.count(2)) {
      paths_[2] = d.path;
      paths_[3] = d.path;
    }
  }
  if (paths_.empty()) log += found.empty() ? "no ITE 048D lighting controller" : "lighting controllers found but with an unknown topology (not used)";
  for (auto& p : paths_) {
    if (p.first == 0) zones.push_back(LightZone::Keyboard);
    if (p.first == 2) zones.push_back(LightZone::LightBar);
    if (p.first == 3) zones.push_back(LightZone::Logo);
  }
  if (info) *info = log;
  LeaveCriticalSection(&cs_);
  return zones;
}

bool IteHid::OpenZone(int z) {
  auto it = handles_.find(z);
  if (it != handles_.end() && it->second != INVALID_HANDLE_VALUE) return true;
  auto p = paths_.find(z);
  if (p == paths_.end()) return false;
  HANDLE h = OpenForFeature(p->second);
  if (h == INVALID_HANDLE_VALUE) return false;
  handles_[z] = h;
  return true;
}

bool IteHid::Send(LightZone zone, const uint8_t report[9]) {
  const int z = static_cast<int>(zone);
  if (report[0] != 0x00 || !CommandAllowed(report[1])) return false;  // never forward anything outside the documented set
  EnterCriticalSection(&cs_);
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
    if (!OpenZone(z)) break;
    UCHAR buf[9];
    for (int i = 0; i < 9; ++i) buf[i] = report[i];
    ok = HidD_SetFeature(handles_[z], buf, 9) != 0;
    if (!ok) {
      const DWORD e = GetLastError();
      // stale handle after resume / re-enumeration: reopen once
      if (e == 6 || e == 31 || e == 55 || e == 433 || e == 1167 || e == 1617) {
        CloseHandle(handles_[z]);
        handles_.erase(z);
      } else {
        break;
      }
    }
  }
  LeaveCriticalSection(&cs_);
  return ok;
}

}  // namespace hw
}  // namespace bc
