// Power-setting GUIDs written out so no SDK header variant matters (used by the service and by the window in local mode).
#pragma once
#include <windows.h>

namespace bc {
namespace hw {

inline const GUID kGuidConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
inline const GUID kGuidAcDcPowerSource = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};

}  // namespace hw
}  // namespace bc
