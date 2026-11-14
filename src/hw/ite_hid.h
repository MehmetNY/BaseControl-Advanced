// RGB lighting through the ITE USB HID controller (VID 048D). All lighting commands are 9-byte HID feature reports
// ([0]=report id 0, [1]=command, ...). Which interface drives which zone follows the published device topology of
// this chassis family: interface with HID usage 1 = keyboard (zone 0); usage 2 = light bar (zone 2) + logo (zone 3,
// shares the bar's controller). Only the documented command bytes {0x08,0x09,0x12,0x14,0x1A} are ever sent.
#pragma once
#include <map>

#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class IteHid : public ILightHid {
 public:
  IteHid();
  ~IteHid() override;
  IteHid(const IteHid&) = delete;
  IteHid& operator=(const IteHid&) = delete;

  std::vector<LightZone> Detect(std::string* info) override;
  bool Send(LightZone z, const uint8_t report[9]) override;

 private:
  struct Dev {
    std::wstring path;
    USHORT usage = 0, usagePage = 0, pid = 0;
  };
  bool OpenZone(int z);
  void CloseAll();

  std::map<int, std::wstring> paths_;  // zone -> device path
  std::map<int, HANDLE> handles_;
  CRITICAL_SECTION cs_;
};

}  // namespace hw
}  // namespace bc
