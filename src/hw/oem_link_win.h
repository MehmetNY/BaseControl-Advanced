// How this program reaches the vendor's service on Windows: which client class the loopback broker accepts. The vendor's own window decides
// it from two registry values of the installed Control Center; this reads the same values (both registry views) and builds the link's
// settings from them. Nothing is written to the registry.
#pragma once
#include <memory>

#include "../oem/link.h"

namespace bc {
namespace hw {

struct OemLinkRegistry {
  int bridgeType = 0;        // HKLM\SOFTWARE\OEM\GamingCenter2 "BridgeType" (0 when absent)
  int customizeTarget = 0;   // ... "CustomizeTarget"
  bool found = false;        // at least one of the two values was readable
};
OemLinkRegistry ReadOemLinkRegistry();

// The link, ready to Start(): classes to try (the one the registry selects first), slot, loopback endpoint.
std::unique_ptr<oem::IOemLink> MakeOemLink();

}  // namespace hw
}  // namespace bc
