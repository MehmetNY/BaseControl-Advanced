#include "hostdata.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#include "util.h"

namespace bc {
namespace {

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Low 64 bits of a little-endian 128-bit counter; saturates when the high half is not zero.
long long Counter128(const uint8_t* p) {
  for (int i = 8; i < 16; i++)
    if (p[i]) return LLONG_MAX;
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v > static_cast<uint64_t>(LLONG_MAX) ? LLONG_MAX : static_cast<long long>(v);
}

bool IsHex4(const std::string& s) {
  if (s.size() != 4) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

}  // namespace

std::string CleanText(const std::string& s, size_t maxLen) {
  std::string out;
  out.reserve(s.size());
  bool blank = true;  // leading blanks are dropped
  for (unsigned char c : s) {
    const bool sep = c < 0x20 || c == 0x7F || c == '|' || c == ';' || c == ' ';
    if (sep) {
      if (!blank) out.push_back(' ');
      blank = true;
    } else {
      out.push_back(static_cast<char>(c));
      blank = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() > maxLen) {
    size_t cut = maxLen;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;  // never end in the middle of a UTF-8 character
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

std::string MemTypeName(int code) {
  switch (code) {
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x20: return "HBM";
    case 0x21: return "HBM2";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    case 0x24: return "HBM3";
    default: return "";
  }
}

std::string MemMakerName(const std::string& raw) {
  const std::string s = CleanText(raw, 40);
  const std::string low = Lower(s);
  if (s.empty() || low == "unknown" || low == "undefined" || low == "not specified" || low == "manufacturer" || low == "[empty]" || low == "none" ||
      low == "n/a" || low == "oem" || low == "to be filled by o.e.m." || low == "0000" || low == "ffff" || low == "0")
    return "";
  if (IsHex4(s)) {
    std::string up = s;
    for (char& c : up)
      if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    // JEDEC JEP106 ids that firmware commonly leaves unresolved
    static const struct {
      const char* id;
      const char* name;
    } kIds[] = {{"80CE", "Samsung"}, {"80AD", "SK hynix"}, {"802C", "Micron"},  {"859B", "Crucial"},
                {"0198", "Kingston"}, {"029E", "Corsair"}, {"04CD", "G.Skill"}, {"04CB", "ADATA"}};
    for (const auto& k : kIds)
      if (up == k.id) return k.name;
  }
  return s;
}

std::vector<MemModule> MemModulesFromSmbios(const mgp::SmbiosInfo& si) {
  std::vector<MemModule> out;
  for (const mgp::SmbiosMemory& m : si.memory) {
    if (m.sizeMb <= 0) continue;  // empty slot
    MemModule d;
    d.sizeMb = m.sizeMb;
    d.type = MemTypeName(m.typeCode);
    d.ratedMts = (m.ratedMts > 0 && m.ratedMts < 30000) ? m.ratedMts : 0;
    d.runMts = (m.configuredMts > 0 && m.configuredMts < 30000) ? m.configuredMts : 0;
    d.locator = CleanText(m.locator, 40);
    d.maker = MemMakerName(m.manufacturer);
    d.part = CleanText(m.part, 40);
    out.push_back(d);
  }
  return out;
}

std::string StorageTitle(const std::string& model) {
  std::string m = CleanText(model, 96);
  const size_t sp = m.rfind(' ');
  if (sp == std::string::npos || sp == 0) return m;
  const std::string last = Lower(m.substr(sp + 1));
  // trailing capacity token: digits, optional fraction, then "tb" or "gb"
  if (last.size() >= 3 && (EndsWith(last, "tb") || EndsWith(last, "gb"))) {
    bool dot = false, ok = true;
    for (size_t i = 0; i + 2 < last.size(); i++) {
      const char c = last[i];
      if (c == '.' && !dot && i > 0) dot = true;
      else if (c < '0' || c > '9') ok = false;
    }
    if (ok) return CleanText(m.substr(0, sp), 96);
  }
  return m;
}

std::string DriveModelName(const std::string& vendor, const std::string& product) {
  const std::string v = CleanText(vendor, 40), p = CleanText(product, 80);
  const std::string lv = Lower(v);
  if (v.empty() || lv == "nvme" || lv == "ata" || lv == "scsi" || lv == "generic") return p.empty() ? v : p;
  if (p.empty()) return v;
  if (Lower(p).compare(0, lv.size(), lv) == 0) return p;   // "WDC" + "WDC PC SN730" -> just the product
  return CleanText(v + " " + p, 96);
}

std::string StorageBusName(int busType) {
  switch (busType) {
    case 0x01: return "SCSI";
    case 0x03: return "ATA";
    case 0x08: return "RAID";
    case 0x0A: return "SAS";
    case 0x0B: return "SATA";
    case 0x11: return "NVMe";
    default: return "";
  }
}

bool ParseNvmeHealth(const uint8_t* log, size_t len, StorageDrive* d) {
  if (!log || !d || len < 144) return false;  // the fields used end at byte 143 (power-on hours)
  d->critWarn = log[0];
  const int kelvin = Le16(log + 1);
  if (kelvin >= 200 && kelvin <= 420) d->tempC = kelvin - 273;
  d->sparePct = log[3] <= 100 ? log[3] : -1;
  d->wearPct = log[5];
  // A counter that does not fit 63 bits is garbage from the drive: report "unknown" instead.
  const long long units = Counter128(log + 48);  // data units written, 1 unit = 1000 blocks of 512 bytes
  if (units != LLONG_MAX) d->writtenGb = static_cast<long long>(static_cast<double>(units) * 512000.0 / 1e9);
  const long long cycles = Counter128(log + 112), hours = Counter128(log + 128);
  if (cycles != LLONG_MAX) d->powerCycles = cycles;
  if (hours != LLONG_MAX) d->powerOnHours = hours;
  return true;
}

std::string FormatDriveSize(uint64_t bytes) {
  if (bytes == 0) return "";
  const double gb = static_cast<double>(bytes) / 1e9;
  if (gb >= 995) {
    const double tb = gb / 1000.0;
    // one decimal below 10 TB ("1.9 TB"), none above; a ".0" is never shown
    if (tb < 9.95) {
      const double r = static_cast<double>(static_cast<long long>(tb * 10.0 + 0.5)) / 10.0;
      const long long whole = static_cast<long long>(r);
      return r == static_cast<double>(whole) ? Fmt("%lld TB", whole) : Fmt("%.1f TB", r);
    }
    return Fmt("%lld TB", static_cast<long long>(tb + 0.5));
  }
  return Fmt("%lld GB", static_cast<long long>(gb + 0.5));
}

std::string DeviceSummary(const KV& s) {
  std::string out;
  auto line = [&](const std::string& k, const std::string& v) {
    if (!v.empty()) out += k + ": " + v + "\n";
  };
  line("Control Center Advanced", s.Str("app.version") + (s.Str("svc.mode").empty() ? "" : " (" + s.Str("svc.mode") + ")"));
  std::string model = Trim(s.Str("host.vendor") + " " + s.Str("host.product"));
  if (!s.Str("host.board").empty()) model += (model.empty() ? "" : " ") + std::string("(") + s.Str("host.board") + ")";
  line("Model", model);
  line("BIOS", s.Str("host.bios"));
  line("Support level", s.Str("dev.class"));
  line("OS", s.Str("host.os"));
  std::string cpu = s.Str("host.cpu");
  if (s.Int("host.cores") > 0) cpu += Fmt(" (%lld cores / %lld threads)", s.Int("host.cores"), s.Int("host.threads"));
  line("CPU", cpu);
  std::string gpu = s.Str("gpu.name");
  if (!gpu.empty() && s.Num("gpu.vram.total") > 0) gpu += Fmt(", %.0f GB", s.Num("gpu.vram.total") / 1024.0);
  if (gpu.empty()) gpu = Join(Split(s.Str("host.gpus"), ';'), ", ");
  line("GPU", gpu);
  std::string mem;
  if (s.Num("os.mem.total") > 0) mem = Fmt("%.1f GB", s.Num("os.mem.total") / 1024.0);
  std::string mods;
  for (int i = 0, n = static_cast<int>(s.Int("host.mem.n")); i < n && i < 8; i++) {
    const std::string p = Fmt("host.mem.%d.", i);
    std::string m = Fmt("%lld GB", s.Int(p + "mb") / 1024);
    const std::string type = s.Str(p + "type");
    const long long mts = s.Int(p + "run", s.Int(p + "rated"));
    if (!type.empty()) m += " " + type;
    if (mts > 0) m += Fmt("-%lld", mts);
    const std::string who = Trim(s.Str(p + "maker") + " " + s.Str(p + "part"));
    if (!who.empty()) m += " " + who;
    mods += (mods.empty() ? "" : "; ") + m;
  }
  if (!mods.empty()) mem += (mem.empty() ? "" : " - ") + mods;
  line("Memory", mem);
  std::string disks;
  for (int i = 0, n = static_cast<int>(s.Int("storage.n")); i < n && i < 8; i++) {
    const std::string p = Fmt("storage.%d.", i);
    std::string d = s.Str(p + "model");
    std::string extra = s.Str(p + "bus");
    const std::string size = FormatDriveSize(static_cast<uint64_t>(std::strtoull(s.Str(p + "size").c_str(), nullptr, 10)));
    if (!size.empty()) extra += (extra.empty() ? "" : ", ") + size;
    if (!extra.empty()) d += " (" + extra + ")";
    disks += (disks.empty() ? "" : "; ") + d;
  }
  line("Storage", disks);
  return out;
}

int ParseStorageTemperature(const uint8_t* buf, size_t len) {
  if (!buf || len < 24 + 16) return -1;
  const unsigned count = Le16(buf + 12);
  const size_t room = (len - 24) / 16;
  const size_t n = count < room ? count : room;
  int first = INT_MIN, comp = INT_MIN;
  for (size_t i = 0; i < n && i < 16; i++) {
    const uint8_t* e = buf + 24 + i * 16;
    const int idx = Le16(e);
    const int t = static_cast<int16_t>(Le16(e + 2));
    if (t < -40 || t > 150) continue;
    if (first == INT_MIN) first = t;
    if (idx == 0 && comp == INT_MIN) comp = t;
  }
  if (comp != INT_MIN) return comp;
  return first == INT_MIN ? -1 : first;
}

}  // namespace bc
