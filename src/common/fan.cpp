#include "fan.h"

#include <algorithm>

#include "util.h"

namespace bc {
namespace fan {

int SafetyFloor(int t) {
  if (t >= 100) return 100;
  if (t >= 95) return 90;
  if (t >= 88) return 70;
  if (t >= 80) return 50;
  if (t >= 70) return 30;
  return 0;
}

bool Validate(const Curve& c, std::string* why) {
  auto no = [&](const std::string& w) { if (why) *why = w; return false; };
  if (c.size() < 2 || c.size() > static_cast<size_t>(kActiveRows)) return no(Fmt("a curve needs 2..%d rows", kActiveRows));
  for (size_t i = 0; i < c.size(); i++) {
    if (c[i].temp < 0 || c[i].temp > kMaxTemp) return no(Fmt("row %zu: temperature %d out of range 0..%d", i, c[i].temp, kMaxTemp));
    if (i == 0) {
      if (c[i].pct != 0) return no("row 0 must have duty 0");
    } else {
      if (c[i].pct < 1 || c[i].pct > 100) return no(Fmt("row %zu: duty %d%% out of range 1..100", i, c[i].pct));
      if (c[i].temp - c[i - 1].temp < kMinSpacing) return no(Fmt("row %zu: thresholds must be at least %d C apart", i, kMinSpacing));
    }
  }
  for (int t = 0; t <= kMaxTemp; t++)
    if (DutyAt(c, t) < SafetyFloor(t)) return no(Fmt("safety floor: at %d C the duty must be at least %d%% (curve gives %d%%)", t, SafetyFloor(t), DutyAt(c, t)));
  return true;
}

bool Monotonic(const Curve& c) {
  for (size_t i = 1; i < c.size(); i++)
    if (c[i].pct < c[i - 1].pct) return false;
  return true;
}

Curve Sanitize(Curve c) {
  if (c.size() > static_cast<size_t>(kActiveRows)) c.resize(kActiveRows);
  if (c.empty()) return c;
  c[0].pct = 0;
  c[0].temp = Clamp(c[0].temp, 0, kMaxTemp - kMinSpacing * static_cast<int>(c.size() - 1));
  for (size_t i = 1; i < c.size(); i++) {
    c[i].temp = std::max(c[i].temp, c[i - 1].temp + kMinSpacing);
    c[i].pct = Clamp(c[i].pct, 1, 100);
    c[i].pct = std::max(c[i].pct, std::max(1, c[i - 1].pct));
  }
  // push down from the end if the last thresholds overflow
  for (size_t i = c.size(); i-- > 0;) {
    int lim = kMaxTemp - kMinSpacing * static_cast<int>(c.size() - 1 - i);
    if (c[i].temp > lim) c[i].temp = lim;
  }
  for (size_t i = 1; i < c.size(); i++)
    if (c[i].temp - c[i - 1].temp < kMinSpacing) c[i - 1].temp = c[i].temp - kMinSpacing;
  // safety floor: duty of a row must cover the highest temperature it applies to; the last row also covers everything above it
  for (size_t i = 1; i < c.size(); i++) c[i].pct = std::max(c[i].pct, SafetyFloor(c[i].temp));
  c.back().pct = 100;
  for (size_t i = 1; i < c.size(); i++) c[i].pct = std::max(c[i].pct, c[i - 1].pct == 0 ? 1 : c[i - 1].pct);
  return c;
}

TableImage Pack(const Curve& c) {
  TableImage t;
  const int n = static_cast<int>(c.size());
  for (int i = 0; i < kSlots; i++) {
    if (i < n) {
      t.up[i] = static_cast<uint8_t>(c[i].temp);
      t.down[i] = i > 0 ? static_cast<uint8_t>(c[i - 1].temp - kMinSpacing) : 0;
      t.duty[i] = i == 0 ? 0 : static_cast<uint8_t>(c[i].pct * 2);
    } else {
      t.up[i] = 0xFF;
      t.down[i] = (i == n) ? static_cast<uint8_t>(t.down[n - 1] + kMinSpacing) : 0xFF;
      t.duty[i] = static_cast<uint8_t>(c[n - 1].pct * 2);
    }
  }
  return t;
}

bool Unpack(const TableImage& t, Curve* out, std::string* why) {
  auto no = [&](const std::string& w) { if (why) *why = w; return false; };
  Curve c;
  for (int i = 0; i < kSlots; i++) {
    if (t.up[i] == 0xFF) break;
    if (t.up[i] > kMaxTemp) return no(Fmt("implausible threshold %u in row %d", t.up[i], i));
    if (t.duty[i] > 200) return no(Fmt("implausible duty %u in row %d", t.duty[i], i));
    Row r;
    r.temp = t.up[i];
    r.pct = i == 0 ? 0 : (t.duty[i] + 1) / 2;
    c.push_back(r);
  }
  if (c.size() < 2) return no("fewer than two active rows");
  for (size_t i = 1; i < c.size(); i++)
    if (c[i].temp <= c[i - 1].temp) return no("thresholds are not increasing");
  if (out) *out = c;
  return true;
}

int DutyAt(const Curve& c, int t) {
  if (c.empty()) return 0;
  if (t <= c[0].temp) return 0;
  for (size_t i = 1; i < c.size(); i++)
    if (t <= c[i].temp) return c[i].pct;
  return c.back().pct;
}

std::string ToText(const Curve& c) {
  std::string s;
  for (size_t i = 0; i < c.size(); i++) {
    if (i) s += ',';
    s += Fmt("%d:%d", c[i].temp, c[i].pct);
  }
  return s;
}

bool FromText(const std::string& s, Curve* out) {
  Curve c;
  for (const std::string& part : Split(s, ',')) {
    auto kv = Split(part, ':');
    long long t, p;
    if (kv.size() != 2 || !ParseInt(kv[0], t) || !ParseInt(kv[1], p)) return false;
    c.push_back(Row{static_cast<int>(t), static_cast<int>(p)});
  }
  if (!Validate(c, nullptr)) return false;
  if (out) *out = c;
  return true;
}

// Defaults are conservative, monotonic and conventional for a 11800H + 3070 class machine.
Curve DefaultCpu() { return {{40, 0}, {50, 25}, {58, 32}, {66, 42}, {74, 55}, {82, 70}, {88, 82}, {94, 92}, {100, 100}}; }
Curve DefaultGpu() { return {{40, 0}, {50, 25}, {58, 32}, {64, 42}, {70, 55}, {76, 68}, {82, 80}, {88, 92}, {94, 100}}; }
Curve QuietCpu() { return {{45, 0}, {58, 22}, {68, 30}, {76, 40}, {84, 55}, {90, 72}, {96, 92}, {102, 100}}; }
Curve TurboCpu() { return {{35, 0}, {44, 35}, {52, 48}, {60, 62}, {68, 75}, {76, 88}, {84, 100}}; }

}  // namespace fan
}  // namespace bc
