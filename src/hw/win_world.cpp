#include "win_world.h"

#include <sddl.h>

namespace bc {
namespace hw {

NamedApplyLock::NamedApplyLock() {
  const wchar_t* name = L"Global\\MonsterGpuPower.ApplyLock";
  // SYSTEM + Administrators: everything; any authenticated user may wait on / release it (the GPU POWER app runs unelevated).
  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, FALSE};
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;AU)", SDDL_REVISION_1, &sd, nullptr)) sa.lpSecurityDescriptor = sd;
  h_ = CreateMutexW(sd ? &sa : nullptr, FALSE, name);
  if (!h_) h_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
  if (sd) LocalFree(sd);
}

NamedApplyLock::~NamedApplyLock() {
  if (h_) CloseHandle(h_);
}

bool NamedApplyLock::Acquire(uint32_t timeoutMs) {
  if (!h_) return true;  // no lock object available: do not block the user's own change
  const DWORD r = WaitForSingleObject(h_, timeoutMs);
  return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;  // abandoned: the previous owner died, the register state is re-verified by the engine anyway
}

void NamedApplyLock::Release() {
  if (h_) ReleaseMutex(h_);
}

WinWorld::WinWorld(const std::string& dataDir, const std::string& logName) : dataDir_(dataDir), log_(dataDir + "\\logs", logName) {
  hw_.clock = &clock_;
  hw_.log = &log_;
  host_.reset(new HostInfoWin(&smbios_));
  hw_.host = host_.get();
  hw_.smbios = &smbios_;
  ec_.reset(new AcpiEc());
  nv_.reset(new mgp::win::NvmlTelemetry());
  oemFiles_.reset(new mgp::win::WinOemPlatform());
  gpu_.reset(new NvmlExtra());
  power_.reset(new OsPower());
  oemProbe_.reset(new OemProbeWin(dataDir));
  storage_.reset(new StorageWin());
  lock_.reset(new NamedApplyLock());
  display_.reset(new OsDisplay());
  devices_.reset(new OsDevices(OsDevices::Scope::All));
  link_ = MakeOemLink();
  hw_.ec = ec_.get();
  hw_.nv = nv_.get();
  hw_.oemFiles = oemFiles_.get();
  hw_.gpu = gpu_.get();
  hw_.power = power_.get();
  hw_.oemSvc = oemProbe_.get();
  hw_.oemLink = link_.get();
  hw_.storage = storage_.get();
  hw_.applyLock = lock_.get();
  hw_.display = display_.get();
  hw_.devices = devices_.get();
}

WinWorld::~WinWorld() {}

}  // namespace hw
}  // namespace bc
