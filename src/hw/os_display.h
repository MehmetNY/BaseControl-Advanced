// Internal-panel display controls for the USER SESSION (the service cannot change a user's desktop):
// brightness (\\.\LCD IOCTL, power-scheme fallback), refresh rate (ChangeDisplaySettingsEx on the internal panel) and
// the software colour controls (gamma ramp). The internal panel is found with the display-config API, so an external
// monitor set as "primary" is never touched by mistake.
#pragma once
#include "../common/hw_iface.h"
#include "winutil.h"

namespace bc {
namespace hw {

class OsDisplay : public IOsDisplay {
 public:
  DisplayInfo Info() override;
  bool SetBrightness(int pct, std::string* why) override;
  bool SetRefresh(int hz, std::string* why) override;
  bool SetColor(const ColorSettings& c, std::string* why) override;
  bool SaturationSupported() override;

  // GDI device name of the internal panel ("\\.\DISPLAY1") or "" when it cannot be determined (primary is used then).
  static std::wstring InternalPanelName();

 private:
  bool SetSaturation(int percent, std::string* why);
  int lastSaturation_ = 100;
};

}  // namespace hw
}  // namespace bc
