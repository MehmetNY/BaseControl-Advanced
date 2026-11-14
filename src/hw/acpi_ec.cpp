#include "acpi_ec.h"

#include "../common/util.h"
#include "../mgp/core/ec_constants.h"

namespace bc {
namespace hw {
namespace {
// The EC register file of the supported chassis family is 0x000..0xFFF; anything above is a bug in the caller.
constexpr uint16_t kMaxAddr = 0x0FFF;
// Errors that mean "the handle is dead" (driver reloaded, resume from hibernate, ...): reopen once and retry.
bool StaleHandleError(DWORD e) {
  return e == ERROR_INVALID_HANDLE || e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_GEN_FAILURE || e == ERROR_NOT_READY ||
         e == ERROR_OPERATION_ABORTED || e == ERROR_DEV_NOT_EXIST || e == ERROR_BAD_COMMAND;
}
}  // namespace

AcpiEc::AcpiEc() { InitializeCriticalSection(&cs_); }

AcpiEc::~AcpiEc() {
  Close();
  DeleteCriticalSection(&cs_);
}

void AcpiEc::Close() {
  if (h_ != INVALID_HANDLE_VALUE) {
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
  if (buf_) {
    VirtualFree(buf_, 0, MEM_RELEASE);
    buf_ = nullptr;
  }
}

bool AcpiEc::EnsureOpen(DWORD* err) {
  if (h_ != INVALID_HANDLE_VALUE && buf_) return true;
  Close();
  HANDLE h = CreateFileW(mgp::kAcpiDevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = GetLastError();
    return false;
  }
  // MEM_COMMIT without touching: only the first page ever becomes resident.
  void* b = VirtualAlloc(nullptr, mgp::kIoBufferBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!b) {
    if (err) *err = GetLastError();
    CloseHandle(h);
    return false;
  }
  h_ = h;
  buf_ = b;
  if (err) *err = 0;
  return true;
}

mgp::DriverState AcpiEc::Probe(uint32_t* winError) {
  EnterCriticalSection(&cs_);
  DWORD err = 0;
  const bool ok = EnsureOpen(&err);
  LeaveCriticalSection(&cs_);
  if (ok) {
    if (winError) *winError = 0;
    return mgp::DriverState::Connected;
  }
  if (winError) *winError = err;
  switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
      return mgp::DriverState::Absent;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
      return mgp::DriverState::AccessDenied;
    default:
      return mgp::DriverState::Error;
  }
}

mgp::EcByte AcpiEc::Read(uint16_t addr) {
  EnterCriticalSection(&cs_);
  mgp::EcByte r = Transact(mgp::kIoctlReadEc, addr, 0, false);
  LeaveCriticalSection(&cs_);
  return r;
}

mgp::EcByte AcpiEc::Write(uint16_t addr, uint8_t value) {
  EnterCriticalSection(&cs_);
  mgp::EcByte r = Transact(mgp::kIoctlWriteEc, addr, value, true);
  LeaveCriticalSection(&cs_);
  return r;
}

mgp::EcByte AcpiEc::Transact(DWORD ioctl, uint16_t addr, uint8_t value, bool isWrite) {
  mgp::EcByte r;
  if (addr > kMaxAddr) {
    r.error = "EC address out of range";
    return r;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    DWORD err = 0;
    if (!EnsureOpen(&err)) {
      r.winError = err;
      r.error = "cannot open the ACPI driver: " + Win32Text(err);
      return r;
    }
    uint32_t* d = static_cast<uint32_t*>(buf_);
    d[0] = addr;
    d[1] = isWrite ? value : 0;
    DWORD ret = 0;
    const BOOL ok = DeviceIoControl(h_, ioctl, buf_, mgp::kIoBufferBytes, buf_, mgp::kIoBufferBytes, &ret, nullptr);
    const DWORD le = ok ? 0 : GetLastError();
    r.bytesReturned = ret;
    r.winError = le;
    if (!ok) {
      if (attempt == 0 && StaleHandleError(le)) {  // a byte write is idempotent, so one retry on a fresh handle is safe
        Close();
        ++reopens_;
        continue;
      }
      r.error = "DeviceIoControl failed: " + Win32Text(le);
      return r;
    }
    if (!isWrite) {
      if (ret < 1) {  // no data came back: the first byte is only the echoed input address
        r.error = "driver returned no data (stale echo suppressed)";
        return r;
      }
      r.value = *static_cast<uint8_t*>(buf_);
    }
    r.ok = true;
    return r;
  }
  return r;
}

}  // namespace hw
}  // namespace bc
