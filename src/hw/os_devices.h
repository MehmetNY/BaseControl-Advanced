// Wireless / camera / touchpad switches.
//   Wi-Fi     WLAN API software radio state               (service / admin)
//   Bluetooth the radio's PnP device is disabled/enabled   (service / admin; the BTH* child devices are never touched)
//   Webcam    built-in (non-removable USB) camera devices  (service / admin; IR / Windows Hello cameras are skipped)
//   Touchpad  HKCU PrecisionTouchPad "Enabled"              (user session only)
// `Scope` decides which of them this instance serves; the others report "absent" (-1).
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class OsDevices : public IOsDevices {
 public:
  enum class Scope { All, ServiceOnly, UserOnly };
  explicit OsDevices(Scope s = Scope::All) : scope_(s) {}
  int Get(Dev d) override;
  bool Set(Dev d, bool on, std::string* why) override;

 private:
  int WifiGet();
  bool WifiSet(bool on, std::string* why);
  int PnpGet(Dev d);
  bool PnpSet(Dev d, bool on, std::string* why);
  int TouchpadGet();
  bool TouchpadSet(bool on, std::string* why);
  Scope scope_;
};

}  // namespace hw
}  // namespace bc
