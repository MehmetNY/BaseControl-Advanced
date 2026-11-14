#include "smbios.h"

#include <cstring>

namespace mgp {
namespace {

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (static_cast<unsigned char>(s[b]) <= 0x20)) ++b;
  while (e > b && (static_cast<unsigned char>(s[e - 1]) <= 0x20)) --e;
  std::string r = s.substr(b, e - b);
  for (char& c : r)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) c = '?';
  return r;
}

// SMBIOS dizesi: biçimli alanın sonundan itibaren NUL ile biten dizeler, çift NUL ile biter.
std::string GetString(const uint8_t* strs, const uint8_t* end, uint8_t index) {
  if (index == 0) return std::string();
  const uint8_t* p = strs;
  for (uint8_t i = 1; p < end && *p; ++i) {
    size_t n = strnlen(reinterpret_cast<const char*>(p), static_cast<size_t>(end - p));
    if (i == index) return Trim(std::string(reinterpret_cast<const char*>(p), n));
    p += n + 1;
  }
  return std::string();
}

}  // namespace

bool ParseSmbiosStructures(const uint8_t* data, size_t len, SmbiosInfo& out) {
  out = SmbiosInfo();
  if (!data || len < 4) return false;
  const uint8_t* const end = data + len;
  const uint8_t* p = data;
  bool haveType1 = false;
  int guard = 0;
  while (p + 4 <= end && guard++ < 4096) {
    const uint8_t type = p[0];
    const uint8_t length = p[1];
    if (length < 4 || p + length > end) break;
    const uint8_t* strs = p + length;
    // Dizelerin sonunu bul (çift NUL).
    const uint8_t* q = strs;
    while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
    const uint8_t* next = (q + 2 <= end) ? q + 2 : end;

    switch (type) {
      case 0:  // BIOS bilgisi
        if (length >= 0x09) {
          out.biosVendor = GetString(strs, end, p[4]);
          out.biosVersion = GetString(strs, end, p[5]);
        }
        if (length >= 0x18) {
          const uint8_t maj = p[0x16], min = p[0x17];
          if (maj != 0xFF && min != 0xFF) {
            out.ecMajor = maj;
            out.ecMinor = min;
          }
        }
        break;
      case 1:  // Sistem bilgisi (seri numarası ve UUID KASITLI olarak okunmaz)
        if (length >= 0x08) {
          out.sysManufacturer = GetString(strs, end, p[4]);
          out.sysProduct = GetString(strs, end, p[5]);
          out.sysVersion = GetString(strs, end, p[6]);
          haveType1 = true;
        }
        if (length >= 0x1B) {
          out.sysSku = GetString(strs, end, p[0x19]);
          out.sysFamily = GetString(strs, end, p[0x1A]);
        }
        break;
      case 2:  // Anakart bilgisi
        if (length >= 0x08) {
          if (out.boardProduct.empty()) {
            out.boardManufacturer = GetString(strs, end, p[4]);
            out.boardProduct = GetString(strs, end, p[5]);
            out.boardVersion = GetString(strs, end, p[6]);
          }
        }
        break;
      case 17:  // Bellek aygıtı (DIMM yuvası / lehimli bellek). Seri no (0x18) ve asset tag (0x19) KASITLI olarak okunmaz.
        if (length >= 0x15) {
          SmbiosMemory m;
          const unsigned rawSize = static_cast<unsigned>(p[0x0C]) | (static_cast<unsigned>(p[0x0D]) << 8);
          if (rawSize == 0x7FFF && length >= 0x20) {  // 32 GB ve üstü: genişletilmiş boyut (MB, bit 30:0)
            const uint32_t ext = static_cast<uint32_t>(p[0x1C]) | (static_cast<uint32_t>(p[0x1D]) << 8) | (static_cast<uint32_t>(p[0x1E]) << 16) |
                                 (static_cast<uint32_t>(p[0x1F]) << 24);
            m.sizeMb = static_cast<int>(ext & 0x7FFFFFFFu);
          } else if (rawSize == 0 || rawSize == 0xFFFF) {
            m.sizeMb = 0;  // takılı değil / bilinmiyor
          } else if (rawSize & 0x8000u) {
            m.sizeMb = static_cast<int>((rawSize & 0x7FFFu) / 1024u);  // KB cinsinden
          } else {
            m.sizeMb = static_cast<int>(rawSize);
          }
          m.formFactor = p[0x0E];
          m.locator = GetString(strs, end, p[0x10]);
          m.bank = GetString(strs, end, p[0x11]);
          m.typeCode = p[0x12];
          if (length >= 0x17) m.ratedMts = static_cast<int>(static_cast<unsigned>(p[0x15]) | (static_cast<unsigned>(p[0x16]) << 8));
          if (length >= 0x1B) {
            m.manufacturer = GetString(strs, end, p[0x17]);
            m.part = GetString(strs, end, p[0x1A]);
          }
          if (length >= 0x22) m.configuredMts = static_cast<int>(static_cast<unsigned>(p[0x20]) | (static_cast<unsigned>(p[0x21]) << 8));
          if (m.ratedMts == 0xFFFF && length >= 0x58) {  // genişletilmiş hız (SMBIOS 3.3+)
            const uint32_t e = static_cast<uint32_t>(p[0x54]) | (static_cast<uint32_t>(p[0x55]) << 8) | (static_cast<uint32_t>(p[0x56]) << 16) |
                               (static_cast<uint32_t>(p[0x57]) << 24);
            m.ratedMts = static_cast<int>(e & 0x7FFFFFFFu);
          }
          if (m.configuredMts == 0xFFFF && length >= 0x5C) {
            const uint32_t e = static_cast<uint32_t>(p[0x58]) | (static_cast<uint32_t>(p[0x59]) << 8) | (static_cast<uint32_t>(p[0x5A]) << 16) |
                               (static_cast<uint32_t>(p[0x5B]) << 24);
            m.configuredMts = static_cast<int>(e & 0x7FFFFFFFu);
          }
          if (m.ratedMts == 0xFFFF) m.ratedMts = 0;
          if (m.configuredMts == 0xFFFF) m.configuredMts = 0;
          if (out.memory.size() < 64) out.memory.push_back(m);
        }
        break;
      default:
        break;
    }
    if (type == 127) break;  // tablo sonu
    p = next;
  }
  out.valid = haveType1;
  return haveType1;
}

bool ParseRawSmbios(const uint8_t* raw, size_t len, SmbiosInfo& out) {
  out = SmbiosInfo();
  if (!raw || len < 8) return false;
  uint32_t tableLen = static_cast<uint32_t>(raw[4]) | (static_cast<uint32_t>(raw[5]) << 8) |
                      (static_cast<uint32_t>(raw[6]) << 16) | (static_cast<uint32_t>(raw[7]) << 24);
  if (tableLen > len - 8) tableLen = static_cast<uint32_t>(len - 8);
  return ParseSmbiosStructures(raw + 8, tableLen, out);
}

}  // namespace mgp
