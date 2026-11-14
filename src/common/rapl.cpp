#include "rapl.h"

#include <cmath>

namespace bc {
namespace rapl {

Units DecodeUnits(uint64_t v) {
  Units u;
  u.powerW = 1.0 / static_cast<double>(1u << (v & 0xF));
  u.energyJ = 1.0 / static_cast<double>(1u << ((v >> 8) & 0x1F));
  u.timeS = 1.0 / static_cast<double>(1u << ((v >> 16) & 0xF));
  return u;
}

double DecodeTimeWindow(uint32_t y, uint32_t z, const Units& u) {
  return static_cast<double>(1u << (y & 0x1F)) * (1.0 + (z & 3) / 4.0) * u.timeS;
}

bool EncodeTimeWindow(double seconds, const Units& u, uint32_t* y, uint32_t* z) {
  if (!(seconds > 0)) return false;
  double best = 1e30;
  uint32_t by = 0, bz = 0;
  for (uint32_t yy = 0; yy < 32; yy++)
    for (uint32_t zz = 0; zz < 4; zz++) {
      double t = DecodeTimeWindow(yy, zz, u);
      double d = std::fabs(std::log(t / seconds));
      if (d < best) { best = d; by = yy; bz = zz; }
    }
  if (best > 0.35) return false;  // more than ~42% away: not representable, refuse rather than silently clamp
  if (y) *y = by;
  if (z) *z = bz;
  return true;
}

PkgLimit DecodePkgLimit(uint64_t v, const Units& u) {
  PkgLimit p;
  p.pl1W = static_cast<double>(v & 0x7FFF) * u.powerW;
  p.pl1En = (v >> 15) & 1;
  p.pl1Clamp = (v >> 16) & 1;
  p.tau1S = DecodeTimeWindow(static_cast<uint32_t>((v >> 17) & 0x1F), static_cast<uint32_t>((v >> 22) & 3), u);
  p.pl2W = static_cast<double>((v >> 32) & 0x7FFF) * u.powerW;
  p.pl2En = (v >> 47) & 1;
  p.pl2Clamp = (v >> 48) & 1;
  p.tau2S = DecodeTimeWindow(static_cast<uint32_t>((v >> 49) & 0x1F), static_cast<uint32_t>((v >> 54) & 3), u);
  p.lock = (v >> 63) & 1;
  return p;
}

bool EncodePkgLimit(uint64_t old, const PkgLimit& w, const Units& u, uint64_t* out) {
  if (!out) return false;
  if ((old >> 63) & 1) return false;  // locked: refuse
  uint64_t v = old;
  auto setPl = [&](double watts, int shift) {
    long long raw = std::llround(watts / u.powerW);
    if (raw < 0 || raw > 0x7FFF) return false;
    v &= ~(0x7FFFULL << shift);
    v |= static_cast<uint64_t>(raw) << shift;
    return true;
  };
  if (!setPl(w.pl1W, 0) || !setPl(w.pl2W, 32)) return false;
  auto setBit = [&](int bit, bool on) { v = on ? (v | (1ULL << bit)) : (v & ~(1ULL << bit)); };
  setBit(15, w.pl1En); setBit(16, w.pl1Clamp); setBit(47, w.pl2En); setBit(48, w.pl2Clamp);
  uint32_t y, z;
  if (w.tau1S > 0) {
    if (!EncodeTimeWindow(w.tau1S, u, &y, &z)) return false;
    v &= ~(0x1FULL << 17); v &= ~(3ULL << 22);
    v |= static_cast<uint64_t>(y) << 17; v |= static_cast<uint64_t>(z) << 22;
  }
  if (w.tau2S > 0) {
    if (!EncodeTimeWindow(w.tau2S, u, &y, &z)) return false;
    v &= ~(0x1FULL << 49); v &= ~(3ULL << 54);
    v |= static_cast<uint64_t>(y) << 49; v |= static_cast<uint64_t>(z) << 54;
  }
  v &= ~(1ULL << 63);  // never set lock
  *out = v;
  return true;
}

PkgInfo DecodePkgInfo(uint64_t v, const Units& u) {
  PkgInfo i;
  i.tdpW = static_cast<double>(v & 0x7FFF) * u.powerW;
  i.minW = static_cast<double>((v >> 16) & 0x7FFF) * u.powerW;
  i.maxW = static_cast<double>((v >> 32) & 0x7FFF) * u.powerW;
  i.maxTauS = static_cast<double>((v >> 48) & 0x7F) * u.timeS;
  return i;
}

double EnergyToWatts(uint32_t prev, uint32_t cur, double dt, const Units& u) {
  if (dt <= 0) return 0;
  uint32_t d = cur - prev;  // unsigned wrap is exactly what we want
  return static_cast<double>(d) * u.energyJ / dt;
}

double DecodeIccMaxA(uint64_t v) { return static_cast<double>(v & 0x1FFF) / 8.0; }

bool EncodeIccMax(uint64_t old, double amps, uint64_t* out) {
  if (!out || IccLocked(old)) return false;
  long long raw = std::llround(amps * 8.0);
  if (raw < 8 || raw > 0x1FFF) return false;
  *out = (old & ~0x1FFFULL) | static_cast<uint64_t>(raw);
  return true;
}

uint64_t OcReadVoltageCmd(int plane) {
  return (1ULL << 63) | (static_cast<uint64_t>(plane & 7) << 40) | (0x10ULL << 32);
}

uint64_t OcWriteVoltageCmd(int plane, int offsetMv) {
  long long units = std::llround(offsetMv * 1.024);
  uint64_t enc = (static_cast<uint64_t>(units) & 0x7FFULL) << 21;
  return (1ULL << 63) | (static_cast<uint64_t>(plane & 7) << 40) | (0x11ULL << 32) | enc;
}

bool OcBusy(uint64_t v) { return (v >> 63) & 1; }

int OcDecodeOffsetMv(uint64_t r) {
  int units = static_cast<int>((r >> 21) & 0x7FF);
  if (units & 0x400) units -= 0x800;
  return static_cast<int>(std::lround(units / 1.024));
}

int OcResponseCode(uint64_t v) { return static_cast<int>((v >> 32) & 0xFF); }

}  // namespace rapl
}  // namespace bc
