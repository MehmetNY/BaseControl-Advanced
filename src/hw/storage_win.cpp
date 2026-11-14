#include "storage_win.h"

#include <winioctl.h>

#include <cstring>
#include <cwchar>

#include "../common/hostdata.h"
#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

// The storage query structures are spelled out here (layout from the Windows SDK documentation) instead of taken from <ntddstor.h>:
// the protocol-specific ones are missing from older MinGW headers, and one definition for all of them keeps the layout visible.
constexpr DWORD kIoctlQueryProperty = 0x002D1400;   // IOCTL_STORAGE_QUERY_PROPERTY
constexpr DWORD kPropDevice = 0;                    // StorageDeviceProperty
constexpr DWORD kPropSeekPenalty = 7;               // StorageDeviceSeekPenaltyProperty
constexpr DWORD kPropProtocolSpecific = 50;         // StorageDeviceProtocolSpecificProperty
constexpr DWORD kPropTemperature = 52;              // StorageDeviceTemperatureProperty
constexpr DWORD kQueryStandard = 0;                 // PropertyStandardQuery
constexpr DWORD kProtocolNvme = 3;                  // ProtocolTypeNvme
constexpr DWORD kNvmeLogPage = 2;                   // NVMeDataTypeLogPage
constexpr DWORD kNvmeLogHealth = 2;                 // SMART / health information

#pragma pack(push, 1)
struct PropertyQuery {          // STORAGE_PROPERTY_QUERY header (the property-specific parameters follow it)
  DWORD propertyId;
  DWORD queryType;
};
struct ProtocolData {           // STORAGE_PROTOCOL_SPECIFIC_DATA, the 40-byte layout of current Windows (older drivers accept the larger offset)
  DWORD protocolType, dataType, requestValue, requestSubValue, dataOffset, dataLength, fixedReturn, subValue2, subValue3, subValue4;
};
struct NvmeHealthQuery {
  PropertyQuery q;
  ProtocolData p;
  BYTE log[512];
};
#pragma pack(pop)
static_assert(sizeof(PropertyQuery) == 8, "STORAGE_PROPERTY_QUERY header");
static_assert(sizeof(ProtocolData) == 40, "STORAGE_PROTOCOL_SPECIFIC_DATA");

DWORD Le32(const BYTE* p) { return static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8) | (static_cast<DWORD>(p[2]) << 16) | (static_cast<DWORD>(p[3]) << 24); }

// NUL-terminated ASCII string inside a descriptor at `offset` (0 = absent).
std::string DescString(const BYTE* buf, DWORD len, DWORD offset) {
  if (offset == 0 || offset >= len) return "";
  const char* s = reinterpret_cast<const char*>(buf + offset);
  return std::string(s, strnlen(s, len - offset));
}

bool Query(HANDLE h, DWORD prop, void* out, DWORD outLen, DWORD* got = nullptr) {
  PropertyQuery q = {prop, kQueryStandard};
  DWORD n = 0;
  const BOOL ok = DeviceIoControl(h, kIoctlQueryProperty, &q, sizeof q, out, outLen, &n, nullptr);
  if (got) *got = n;
  return ok != FALSE;
}

bool Describe(HANDLE h, StorageDrive* d) {
  BYTE buf[1024];
  std::memset(buf, 0, sizeof buf);
  DWORD n = 0;
  if (!Query(h, kPropDevice, buf, sizeof buf, &n) || n < 36) return false;
  // STORAGE_DEVICE_DESCRIPTOR: removable flag at 10, vendor / product offsets at 12 / 16, bus type at 28. The serial-number offset (24) is not used.
  if (buf[10]) return false;                                  // removable media is not internal storage
  d->bus = StorageBusName(static_cast<int>(Le32(buf + 28)));
  if (d->bus.empty()) return false;                           // USB, SD, virtual and file-backed disks are not listed
  const DWORD len = n < sizeof buf ? n : static_cast<DWORD>(sizeof buf);
  d->model = DriveModelName(DescString(buf, len, Le32(buf + 12)), DescString(buf, len, Le32(buf + 16)));
  if (d->model.empty()) d->model = d->bus + " drive";
  if (d->bus == "NVMe") {
    d->ssd = true;
  } else {
    BYTE sp[32];
    std::memset(sp, 0, sizeof sp);
    DWORD m = 0;
    if (Query(h, kPropSeekPenalty, sp, sizeof sp, &m) && m >= 9) d->ssd = sp[8] == 0;   // DEVICE_SEEK_PENALTY_DESCRIPTOR.IncursSeekPenalty
  }
  DISK_GEOMETRY_EX geo;
  std::memset(&geo, 0, sizeof geo);
  DWORD g = 0;
  if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &g, nullptr) && geo.DiskSize.QuadPart > 0)
    d->sizeBytes = static_cast<uint64_t>(geo.DiskSize.QuadPart);
  return true;
}

bool NvmeHealth(HANDLE h, StorageDrive* d) {
  NvmeHealthQuery b;
  std::memset(&b, 0, sizeof b);
  b.q.propertyId = kPropProtocolSpecific;
  b.q.queryType = kQueryStandard;
  b.p.protocolType = kProtocolNvme;
  b.p.dataType = kNvmeLogPage;
  b.p.requestValue = kNvmeLogHealth;
  b.p.requestSubValue = 0;
  b.p.dataOffset = sizeof(ProtocolData);
  b.p.dataLength = sizeof b.log;
  DWORD n = 0;
  // the same buffer carries the query in and the STORAGE_PROTOCOL_DATA_DESCRIPTOR (version, size, protocol data, log) out
  if (!DeviceIoControl(h, kIoctlQueryProperty, &b, sizeof b, &b, sizeof b, &n, nullptr)) return false;
  if (n < sizeof(PropertyQuery) + sizeof(ProtocolData) + 144) return false;
  if (b.p.dataOffset < sizeof(ProtocolData) || b.p.dataOffset > sizeof(ProtocolData) + sizeof b.log - 144) return false;
  const BYTE* log = reinterpret_cast<const BYTE*>(&b.p) + b.p.dataOffset;
  const size_t avail = sizeof b - (sizeof(PropertyQuery) + b.p.dataOffset);
  return ParseNvmeHealth(log, avail, d);
}

void Health(HANDLE h, const std::wstring& path, StorageDrive* d) {
  bool got = d->bus == "NVMe" && NvmeHealth(h, d);
  if (!got && d->bus == "NVMe") {
    // A handle without access rights may be refused the log page: retry once with read access (still no write access).
    HANDLE r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (r != INVALID_HANDLE_VALUE) {
      got = NvmeHealth(r, d);
      CloseHandle(r);
    }
  }
  if (d->tempC < 0) {   // any drive: the generic temperature property (composite sensor)
    BYTE t[256];
    std::memset(t, 0, sizeof t);
    DWORD n = 0;
    if (Query(h, kPropTemperature, t, sizeof t, &n)) d->tempC = ParseStorageTemperature(t, n < sizeof t ? n : sizeof t);
  }
}

}  // namespace

bool StorageWin::Read(std::vector<StorageDrive>* out, bool health) {
  out->clear();
  bool listed = false;
  for (int i = 0; i < 16; i++) {
    wchar_t path[48];
    swprintf(path, 48, L"\\\\.\\PhysicalDrive%d", i);
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    StorageDrive d;
    if (Describe(h, &d)) {
      listed = true;
      if (health) Health(h, path, &d);
      out->push_back(d);
    }
    CloseHandle(h);
  }
  return listed;
}

}  // namespace hw
}  // namespace bc
