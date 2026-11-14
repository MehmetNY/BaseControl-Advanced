#include "osdcfg.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "util.h"

namespace bc {
namespace {

const char* kDash = "\xE2\x80\x94";
const char* kDeg = "\xC2\xB0""C";
const char* kDot = " \xC2\xB7 ";

std::string Whole(double v) { return Fmt("%d", static_cast<int>(std::lround(v))); }

bool Known(double v) { return v >= 0; }

}  // namespace

float OsdSizeFactor(int size) { return size >= 2 ? 1.5f : (size == 1 ? 1.25f : 1.0f); }

std::string NormalizeExeName(const std::string& text) {
  std::string s = Trim(text);
  const size_t sl = s.find_last_of("\\/");
  if (sl != std::string::npos) s = s.substr(sl + 1);
  s = Lower(s);
  if (s.empty() || s.size() > 64) return "";
  if (!EndsWith(s, ".exe")) s += ".exe";
  if (s.size() <= 4) return "";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '"' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' || c == ':' || c == ';' || c == ',') return "";
  }
  return s;
}

std::vector<std::string> ParseGameList(const std::string& text) {
  std::vector<std::string> out;
  for (const std::string& p : Split(text, ';')) {
    const std::string n = NormalizeExeName(p);
    if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
  }
  return out;
}

std::string FormatGameList(const std::vector<std::string>& v) { return Join(v, ";"); }

bool ParseColor(const std::string& text, uint32_t* argb) {
  const std::string s = Trim(text);
  if (s.size() != 7 || s[0] != '#') return false;
  uint32_t v = 0;
  for (size_t i = 1; i < 7; i++) {
    const char c = s[i];
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    v = v * 16 + static_cast<uint32_t>(c <= '9' ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10));
  }
  if (argb) *argb = 0xFF000000u | v;
  return true;
}

std::string FormatColor(uint32_t argb) { return Fmt("#%06x", argb & 0xFFFFFFu); }

const std::vector<std::string>& OsdHotkeys() {
  static const std::vector<std::string> k = {"ctrl+shift+o", "ctrl+alt+o", "ctrl+shift+f10", "ctrl+alt+f12", "alt+f10", "ctrl+alt+insert"};
  return k;
}

bool ParseHotkey(const std::string& text, unsigned* mods, unsigned* vk) {
  unsigned m = 0, key = 0;
  for (const std::string& raw : Split(Lower(Trim(text)), '+')) {
    const std::string t = Trim(raw);
    if (t == "ctrl" || t == "control") m |= 2;
    else if (t == "alt") m |= 1;
    else if (t == "shift") m |= 4;
    else if (key) return false;
    else if (t.size() == 1 && std::isalnum(static_cast<unsigned char>(t[0]))) key = static_cast<unsigned>(std::toupper(static_cast<unsigned char>(t[0])));
    else if (t.size() >= 2 && t[0] == 'f' && std::isdigit(static_cast<unsigned char>(t[1]))) {
      long long n = 0;
      if (!ParseInt(t.substr(1), n) || n < 1 || n > 24) return false;
      key = 0x70u + static_cast<unsigned>(n - 1);
    } else if (t == "insert") key = 0x2D;
    else if (t == "home") key = 0x24;
    else if (t == "end") key = 0x23;
    else if (t == "pause") key = 0x13;
    else return false;
  }
  if (!key || !m) return false;   // a bare key would steal it from every program
  if (mods) *mods = m;
  if (vk) *vk = key;
  return true;
}

std::string HotkeyLabel(const std::string& text) {
  unsigned m = 0, vk = 0;
  if (!ParseHotkey(text, &m, &vk)) return text;
  std::string s;
  auto add = [&](const std::string& p) { s += (s.empty() ? "" : " + ") + p; };
  if (m & 2) add("Ctrl");
  if (m & 1) add("Alt");
  if (m & 4) add("Shift");
  if (vk >= 0x70 && vk <= 0x87) add(Fmt("F%u", vk - 0x70 + 1));
  else if (vk == 0x2D) add("Insert");
  else if (vk == 0x24) add("Home");
  else if (vk == 0x23) add("End");
  else if (vk == 0x13) add("Pause");
  else add(std::string(1, static_cast<char>(vk)));
  return s;
}

OsdConfig OsdFromKv(const KV& p) {
  OsdConfig c;
  c.enabled = p.Bool("osd.on", false);
  c.fps = p.Bool("osd.fps", true);
  c.cpu = p.Bool("osd.cpu", true);
  c.gpu = p.Bool("osd.gpu", true);
  c.mem = p.Bool("osd.mem", false);
  c.fan = p.Bool("osd.fan", false);
  c.power = p.Bool("osd.power", false);
  c.corner = static_cast<int>(Clamp<long long>(p.Int("osd.corner", 0), 0, 3));
  c.mx = static_cast<int>(Clamp<long long>(p.Int("osd.mx", 20), 0, 200));
  c.my = static_cast<int>(Clamp<long long>(p.Int("osd.my", 20), 0, 200));
  c.size = static_cast<int>(Clamp<long long>(p.Int("osd.size", 0), 0, 2));
  uint32_t col = 0;
  if (ParseColor(p.Str("osd.color", ""), &col)) c.color = col;
  c.opacity = static_cast<int>(Clamp<long long>(p.Int("osd.opacity", 78), 10, 100));
  c.gamesOnly = p.Bool("osd.gamesonly", false);
  c.games = ParseGameList(p.Str("osd.games", ""));
  unsigned m = 0, vk = 0;
  const std::string hk = p.Str("osd.hotkey", c.hotkey);
  if (ParseHotkey(hk, &m, &vk)) c.hotkey = Lower(Trim(hk));
  return c;
}

OsdData OsdDataFrom(const KV& s) {
  OsdData d;
  auto get = [&](const char* k, double* out) {
    if (s.Has(k)) *out = s.Num(k, -1);
  };
  get("fps.cur", &d.fps);
  get("cpu.temp", &d.cpuT);
  get("os.cpu.pct", &d.cpuLoad);
  get("cpu.pkg.w", &d.cpuW);
  get("gpu.temp", &d.gpuT);
  get("gpu.util", &d.gpuLoad);
  get("gpu.power", &d.gpuW);
  get("os.mem.pct", &d.memPct);
  if (s.Has("os.mem.used")) d.memUsedGb = s.Num("os.mem.used", 0) / 1024.0;
  get("fan.cpu.rpm", &d.cpuRpm);
  get("fan.gpu.rpm", &d.gpuRpm);
  return d;
}

std::vector<OsdLine> OsdLines(const OsdConfig& c, const OsdData& d) {
  std::vector<OsdLine> out;
  auto temp_load = [&](double t, double load) {
    std::string a = Known(t) ? Whole(t) + " " + kDeg : "";
    std::string b = Known(load) ? Whole(load) + " %" : "";
    if (a.empty() && b.empty()) return std::string(kDash);
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + kDot + b;
  };
  if (c.fps) out.push_back({"FPS", Known(d.fps) ? Whole(d.fps) : kDash});
  if (c.cpu) out.push_back({"CPU", temp_load(d.cpuT, d.cpuLoad)});
  if (c.gpu) out.push_back({"GPU", temp_load(d.gpuT, d.gpuLoad)});
  if (c.mem) {
    std::string v = kDash;
    if (Known(d.memUsedGb) && Known(d.memPct)) v = Fmt("%.1f GB", d.memUsedGb) + kDot + Whole(d.memPct) + " %";
    else if (Known(d.memPct)) v = Whole(d.memPct) + " %";
    out.push_back({"RAM", v});
  }
  if (c.fan) {
    std::string v = kDash;
    if (Known(d.cpuRpm) && Known(d.gpuRpm)) v = Whole(d.cpuRpm) + " / " + Whole(d.gpuRpm) + " rpm";
    else if (Known(d.cpuRpm)) v = Whole(d.cpuRpm) + " rpm";
    out.push_back({"FAN", v});
  }
  if (c.power) {
    std::string v = kDash;
    if (Known(d.cpuW) && Known(d.gpuW)) v = Whole(d.cpuW) + " W" + kDot + Whole(d.gpuW) + " W";
    else if (Known(d.cpuW)) v = Whole(d.cpuW) + " W";
    else if (Known(d.gpuW)) v = Whole(d.gpuW) + " W";
    out.push_back({"PWR", v});
  }
  return out;
}

}  // namespace bc
