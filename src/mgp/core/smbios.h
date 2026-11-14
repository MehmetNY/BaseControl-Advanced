// SMBIOS ayrıştırıcı (saf C++). Seri numarası, UUID, asset tag GİBİ kimlik verilerini OKUMAZ/SAKLAMAZ.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mgp {

// One entry of the SMBIOS "Memory Device" table (type 17): a DIMM slot or a soldered package. Serial number and asset tag are never read.
struct SmbiosMemory {
  int sizeMb = 0;           // 0 = empty slot
  int typeCode = 0;         // SMBIOS memory type (0x1A DDR4, 0x22 DDR5, 0x1E LPDDR4, 0x23 LPDDR5 ...)
  int formFactor = 0;       // 0x09 DIMM, 0x0D SODIMM, 0x0B row of chips (soldered) ...
  int ratedMts = 0;         // maximum speed of the module in MT/s (0 = not reported)
  int configuredMts = 0;    // speed it actually runs at (SMBIOS 2.7+; 0 = not reported)
  std::string locator, bank, manufacturer, part;
};

struct SmbiosInfo {
  bool valid = false;  // en az Type 1 bulundu
  std::string sysManufacturer, sysProduct, sysVersion, sysSku, sysFamily;
  std::string biosVendor, biosVersion;
  std::string boardManufacturer, boardProduct, boardVersion;
  int ecMajor = -1, ecMinor = -1;  // Type 0 EC firmware sürümü (yoksa -1)
  std::vector<SmbiosMemory> memory;  // Type 17 girdileri (boş yuvalar dahil), tablo sırasıyla
};

// Ham tablo (başlıksız yapı listesi).
bool ParseSmbiosStructures(const uint8_t* data, size_t len, SmbiosInfo& out);
// GetSystemFirmwareTable('RSMB') çıktısı: 8 baytlık RawSMBIOSData başlığı + tablo.
bool ParseRawSmbios(const uint8_t* raw, size_t len, SmbiosInfo& out);

}  // namespace mgp
