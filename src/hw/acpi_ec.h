// EC access through the OEM "ACPIDriver" kernel device (\\.\ACPIDriver, IOCTL 0x9C40A488 read / 0x9C40A48C write).
// Unlike the vendored one-shot MGP wrapper this keeps ONE device handle and ONE 4 MiB transfer buffer for the life of
// the process (a poll cycle reads ~20 registers per second). The ABI is identical to the OEM DLL's:
//   in/out buffer = { DWORD address; DWORD value }, buffers are 4 MiB, the read result is the first output byte and
//   only valid when DeviceIoControl succeeded AND bytesReturned >= 1 (the OEM DLL echoes the address on failure).
// The class has no allow-list of its own: bc::GuardedEc (WriteGuard) sits above it and is the single gatekeeper.
#pragma once
#include "winutil.h"

namespace bc {
namespace hw {

class AcpiEc : public mgp::IEcDriver {
 public:
  AcpiEc();
  ~AcpiEc() override;
  AcpiEc(const AcpiEc&) = delete;
  AcpiEc& operator=(const AcpiEc&) = delete;

  mgp::DriverState Probe(uint32_t* winError) override;
  mgp::EcByte Read(uint16_t addr) override;
  mgp::EcByte Write(uint16_t addr, uint8_t value) override;

  uint32_t reopens() const { return reopens_; }

 private:
  bool EnsureOpen(DWORD* err);
  void Close();
  mgp::EcByte Transact(DWORD ioctl, uint16_t addr, uint8_t value, bool isWrite);

  HANDLE h_ = INVALID_HANDLE_VALUE;
  void* buf_ = nullptr;
  CRITICAL_SECTION cs_;
  uint32_t reopens_ = 0;
};

}  // namespace hw
}  // namespace bc
