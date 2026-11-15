// Lighting: a drawing of the keyboard in the colours the keyboard reports (one colour per light block), and the light's settings - on/off,
// effect, direction, brightness, speed, the idle timer, the colour of each block, and "save to the keyboard". Everything is sent through the vendor's
// service, exactly as its own window does, and shown only after the service has reported it back.
#include <algorithm>
#include <cmath>

#include "../common/keymap.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

struct Rgb {
  uint8_t r = 255, g = 255, b = 255;
};

uint32_t ToArgb(const Rgb& c, int a = 255) { return Argb(a, c.r, c.g, c.b); }

bool ParseHex(const std::string& s, Rgb* out) {
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (size_t i = 1; i < 7; i++) {
    const char ch = s[i];
    unsigned d;
    if (ch >= '0' && ch <= '9') d = static_cast<unsigned>(ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = static_cast<unsigned>(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') d = static_cast<unsigned>(ch - 'A' + 10);
    else return false;
    v = v * 16 + d;
  }
  out->r = static_cast<uint8_t>((v >> 16) & 255);
  out->g = static_cast<uint8_t>((v >> 8) & 255);
  out->b = static_cast<uint8_t>(v & 255);
  return true;
}

std::string ToHex(const Rgb& c) { return Fmt("#%02x%02x%02x", c.r, c.g, c.b); }

Rgb FromHsv(double h, double s, double v) {
  h = std::fmod(h, 360.0);
  if (h < 0) h += 360.0;
  const double c = v * s, x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0)), m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  Rgb o;
  o.r = static_cast<uint8_t>(std::lround((r + m) * 255.0));
  o.g = static_cast<uint8_t>(std::lround((g + m) * 255.0));
  o.b = static_cast<uint8_t>(std::lround((b + m) * 255.0));
  return o;
}

double Luma(uint32_t c) { return (0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255)) / 255.0; }

struct Hsv {
  double h = 0, s = 0, v = 1;   // degrees, 0..1, 0..1
};

Hsv ToHsv(const Rgb& c, double keepHue) {
  const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
  const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
  Hsv o;
  o.v = mx;
  o.s = mx <= 0 ? 0 : d / mx;
  if (d < 1e-6) {
    o.h = keepHue;   // grey: the hue is undefined, so the handle stays where the user left it
  } else {
    double h = mx == r ? std::fmod((g - b) / d, 6.0) : (mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0);
    h *= 60.0;
    if (h < 0) h += 360.0;
    o.h = h;
  }
  return o;
}

// The effects the vendor's window offers for this kind of keyboard, as numbers of its RGBKB_Effect list, in its order.
struct EffectDef {
  int id;
  const char* label;
  const char* help;
};
const EffectDef kEffects[] = {
    {0, N_("Static"), N_("One steady colour per light block.")},
    {2, N_("Wave"), N_("A wave of light that moves across the keyboard.")},
    {5, N_("Ripple"), N_("Rings of light spread from the key you press.")},
    {4, N_("Rainbow"), N_("Runs through every colour. The colours below are not used.")},
    {17, N_("Mix"), N_("A mix of moving colours.")},
    {3, N_("Reactive"), N_("Keys light up when you press them.")},
    {13, N_("Music"), N_("The light follows the sound that is playing.")},
};

std::string EffectLabel(int id) {
  for (const EffectDef& e : kEffects)
    if (e.id == id) return T(e.label);
  return Tf("Effect %d", id);
}

std::string EffectHelp(int id) {
  for (const EffectDef& e : kEffects)
    if (e.id == id) return T(e.help);
  return T("An effect this window does not describe.");
}

// Idle timer choices, in seconds (0 = never). The value that is active is added to the list when it is not one of these.
const int kTimerChoices[] = {0, 10, 30, 60, 120, 300, 600, 1800};

std::string TimerLabel(int s) {
  switch (s) {
    case 0: return T("Never");
    case 10: return T("10 seconds");
    case 30: return T("30 seconds");
    case 60: return T("1 minute");
    case 120: return T("2 minutes");
    case 300: return T("5 minutes");
    case 600: return T("10 minutes");
    case 1800: return T("30 minutes");
    default: break;
  }
  if (s % 60 == 0) return Tf("%d minutes", s / 60);
  if (s < 60) return Tf("%d seconds", s);
  return Tf("%d min %d s", s / 60, s % 60);
}

// What the keyboard shows right now.
struct Look {
  std::vector<Rgb> zones;   // one colour per light block
  double frac = 1.0;
  bool lit = false;
  bool absent = false;
};

std::vector<Rgb> ZonesOf(App& a) {
  std::vector<Rgb> z;
  for (const std::string& p : Split(a.S("light.kbd.colors"), ',')) {
    Rgb c;
    if (ParseHex(Trim(p), &c)) z.push_back(c);
  }
  Rgb all;
  if (z.empty() && ParseHex(a.S("light.kbd.color", "#ffffff"), &all)) z.push_back(all);
  if (z.empty()) z.push_back(Rgb{});
  return z;
}

Look LookNow(App& a) {
  Look l;
  if (!a.CapOk("light.kbd")) {
    l.absent = true;
    return l;
  }
  l.zones = ZonesOf(a);
  const int lvl = static_cast<int>(std::max(0LL, std::min(4LL, a.I("light.kbd.bright", 4))));
  l.frac = (lvl + 1) / 5.0;
  l.lit = a.B("light.kbd.power", true);
  return l;
}

Rgb ZoneAt(const Look& L, float xFrac) {
  if (L.zones.empty()) return Rgb{};
  const int n = static_cast<int>(L.zones.size());
  const int i = std::max(0, std::min(n - 1, static_cast<int>(std::floor(xFrac * static_cast<float>(n)))));
  return L.zones[static_cast<size_t>(i)];
}

void LitCap(Ui& ui, const Rect& r, const std::string& legend, const Look& L, const Rgb& zone, bool fontBig) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const uint32_t unlit = Mix(th.elev, th.panel, 0.25f);
  uint32_t fill = unlit, border = th.line, text = th.faint;
  if (L.lit) {
    const uint32_t lc = ToArgb(zone);
    fill = Mix(unlit, lc, 0.20f + 0.60f * static_cast<float>(L.frac));
    border = Mix(th.line, lc, 0.50f * static_cast<float>(L.frac));
    text = Luma(fill) > 0.58 ? Argb(255, 28, 29, 27) : Argb(255, 244, 241, 235);
  }
  g.FillRound(r, 6, fill);
  g.StrokeRound(r, 6, border, 1.0f);
  g.Text(r.Inset(2, 0), legend, fontBig ? FontId::Small : FontId::Tiny, text, Align::Center);
}

void Glow(Ui& ui, const Rect& r, const Look& L, const Rgb& zone) {
  if (!L.lit) return;
  ui.gfx.FillRound(r.Inset(-2.5f), 8, WithAlpha(ToArgb(zone), static_cast<int>(40.0 * L.frac)));
}

// A polygon with every corner rounded (quadratic curves), for the L-shaped ISO Enter key.
std::vector<V2> RoundedPoly(const std::vector<V2>& v, float rad) {
  std::vector<V2> out;
  const size_t n = v.size();
  for (size_t i = 0; i < n; i++) {
    const V2 p = v[i], a = v[(i + n - 1) % n], b = v[(i + 1) % n];
    auto unit = [](V2 from, V2 to, float* len) {
      const V2 d{to.x - from.x, to.y - from.y};
      *len = std::sqrt(d.x * d.x + d.y * d.y);
      return V2{d.x / *len, d.y / *len};
    };
    float la = 0, lb = 0;
    const V2 da = unit(p, a, &la), db = unit(p, b, &lb);
    const float r = std::min(rad, std::min(la, lb) * 0.5f);
    const V2 p1{p.x + da.x * r, p.y + da.y * r}, p2{p.x + db.x * r, p.y + db.y * r};
    for (int k = 0; k <= 6; k++) {
      const float t = static_cast<float>(k) / 6.0f, u = 1.0f - t;
      out.push_back({u * u * p1.x + 2 * u * t * p.x + t * t * p2.x, u * u * p1.y + 2 * u * t * p.y + t * t * p2.y});
    }
  }
  return out;
}

// The keyboard drawing (geometry as on the key-mapping page). Two passes: the glow of every key first, then the caps on top. Each key
// takes the colour of the light block under it (blocks run left to right).
void DrawKeyboard(Ui& ui, const Rect& board, bool tr, const Look& L) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const float u = std::floor(std::min(58.0f, std::max(26.0f, (board.w - 28) / 15.0f)));
  const float kh = std::floor(std::min(44.0f, std::max(26.0f, u * 0.8f)));
  const float gap = 4;
  const float bx = board.x + (board.w - 15 * u) * 0.5f;
  ui.Panel(board, th.inset, th.line, 12);
  const auto& rows = KeyboardRows();
  auto zoneOfX = [&](float cx) { return ZoneAt(L, std::max(0.0f, std::min(0.999f, (cx - bx) / (15 * u)))); };
  for (int pass = 0; pass < 2; pass++) {
    float by = board.y + 9 + gap * 0.5f;
    std::vector<Rect> enter;
    for (const auto& row : rows) {
      float x = bx;
      for (const PhysKey& pk : row) {
        const Rect kr(x + gap * 0.5f, by, pk.w * u - gap, kh);
        x += pk.w * u;
        if (pk.id == 0x1C) {
          enter.push_back(kr);
          continue;
        }
        const Rgb z = zoneOfX(kr.cx());
        if (pass == 0) Glow(ui, kr, L, z);
        else LitCap(ui, kr, tr ? pk.tr : pk.us, L, z, pk.w * u > 34);
      }
      by += kh + gap;
    }
    if (enter.size() == 2) {   // the ISO Enter key: two rectangles that read as one key
      const Rect& t = enter[0];
      const Rect& l = enter[1];
      const Rgb z = zoneOfX(t.cx());
      if (pass == 0) {
        Glow(ui, t, L, z);
        Glow(ui, l, L, z);
      } else {
        const std::vector<V2> shape = RoundedPoly({{t.x, t.y}, {t.r(), t.y}, {t.r(), l.b()}, {l.x, l.b()}, {l.x, t.b() + 4}, {t.x, t.b() + 4}}, 6.0f);
        uint32_t fill = Mix(th.elev, th.panel, 0.25f), border = th.line, text = th.faint;
        if (L.lit) {
          const uint32_t lc = ToArgb(z);
          fill = Mix(fill, lc, 0.20f + 0.60f * static_cast<float>(L.frac));
          border = Mix(th.line, lc, 0.50f * static_cast<float>(L.frac));
          text = Luma(fill) > 0.58 ? Argb(255, 28, 29, 27) : Argb(255, 244, 241, 235);
        }
        g.PolyFill(shape, fill);
        std::vector<V2> outline = shape;
        outline.push_back(shape.front());
        g.Poly(outline, border, 1.0f);
        g.Text(t.Inset(2, 0), "Enter", FontId::Small, text, Align::Center);
      }
    }
  }
}

struct PickResult {
  bool held = false, changed = false, released = false;
};

// A saturation / brightness square over a hue strip. `hsv` is the draft the user is editing.
PickResult ColorPicker(Ui& ui, App& a, const Rect& area, Hsv* hsv, bool enabled) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  PickResult out;
  const Rect sv(area.x, area.y, area.w, 122);
  const Rect hue(area.x, sv.b() + 12, area.w, 18);
  // ---- drawing
  g.FillRound(sv, 8, ToArgb(FromHsv(hsv->h, 1, 1)));
  g.FillGrad(sv, 8, Argb(255, 255, 255, 255), Argb(0, 255, 255, 255), 0);
  g.FillGrad(sv, 8, Argb(0, 0, 0, 0), Argb(255, 0, 0, 0), 90);
  g.StrokeRound(sv, 8, th.line, 1.0f);
  for (int i = 0; i < 6; i++) {
    const float x0 = hue.x + hue.w * static_cast<float>(i) / 6.0f, x1 = hue.x + hue.w * static_cast<float>(i + 1) / 6.0f;
    g.FillGrad(Rect(x0, hue.y, x1 - x0 + 0.6f, hue.h), 0, ToArgb(FromHsv(i * 60.0, 1, 1)), ToArgb(FromHsv((i + 1) * 60.0, 1, 1)), 0);
  }
  g.StrokeRound(hue, 4, th.line, 1.0f);
  // ---- input (registered after drawing so the frame already shows the new draft next time)
  bool hs = false, hh = false;
  ui.Click("lp.sv", sv, enabled, nullptr, &hs);
  ui.Click("lp.hue", hue, enabled, nullptr, &hh);
  const V2 m = ui.LocalMouse();
  if (hs) {
    const double s = std::min(1.0, std::max(0.0, static_cast<double>((m.x - sv.x) / sv.w)));
    const double v = 1.0 - std::min(1.0, std::max(0.0, static_cast<double>((m.y - sv.y) / sv.h)));
    if (std::fabs(s - hsv->s) > 1e-4 || std::fabs(v - hsv->v) > 1e-4) out.changed = true;
    hsv->s = s;
    hsv->v = v;
  }
  if (hh) {
    const double h = std::min(359.9, std::max(0.0, static_cast<double>((m.x - hue.x) / hue.w) * 360.0));
    if (std::fabs(h - hsv->h) > 1e-3) out.changed = true;
    hsv->h = h;
  }
  out.held = hs || hh;
  bool& was = a.Flag("light.pick.held");
  out.released = was && !out.held;
  was = out.held;
  // ---- handles
  const Rgb now = FromHsv(hsv->h, hsv->s, hsv->v);
  const V2 hp{static_cast<float>(sv.x + sv.w * hsv->s), static_cast<float>(sv.y + sv.h * (1.0 - hsv->v))};
  g.Circle(hp, 8, Argb(200, 0, 0, 0), 3.0f);
  g.Circle(hp, 8, Argb(255, 255, 255, 255), 1.8f);
  g.Dot(hp, 5.5f, ToArgb(now));
  const float hx = static_cast<float>(hue.x + hue.w * hsv->h / 360.0);
  const Rect hb(hx - 4, hue.y - 3, 8, hue.h + 6);
  g.FillRound(hb, 4, ToArgb(FromHsv(hsv->h, 1, 1)));
  g.StrokeRound(hb, 4, Argb(255, 255, 255, 255), 2.0f);
  return out;
}

const uint32_t kSwatches[] = {0xFFFFFFFF, 0xFFFFD9A8, 0xFFFF2D2D, 0xFFFF8C1A, 0xFFFFD400, 0xFF8BE000, 0xFF19D45A, 0xFF14D4C4, 0xFF1EC8FF, 0xFF2F6BFF, 0xFF8A5CFF, 0xFFFF4FB8};

std::string HexOf(uint32_t argb) {
  Rgb c;
  c.r = static_cast<uint8_t>((argb >> 16) & 255);
  c.g = static_cast<uint8_t>((argb >> 8) & 255);
  c.b = static_cast<uint8_t>(argb & 255);
  return ToHex(c);
}

constexpr int kMaxEditableZones = 8;   // beyond this the colour blocks are edited together

// Sends `hex` to the selected block (1..n) or to every block (0).
void SendColour(App& a, int zone, const std::string& hex) {
  const std::string label = Tf("Keyboard colour: %s", hex.c_str());
  if (zone <= 0) {
    a.Set("light.kbd.color", hex, label);
    return;
  }
  std::vector<std::string> parts = Split(a.S("light.kbd.colors"), ',');
  if (static_cast<size_t>(zone) > parts.size()) parts.resize(static_cast<size_t>(zone), "#ffffff");
  parts[static_cast<size_t>(zone - 1)] = hex;
  a.Set("light.kbd.colors", Join(parts, ","), label);
}

void StateLine(App& a, Ui& ui, const Rect& r, const Look& L) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  std::string text;
  uint32_t col = th.muted;
  IconId icon = IconId::Info;
  if (L.absent) {
    text = a.CapWhy("light.kbd").empty() ? T("No keyboard lighting was reported by the vendor's service.") : a.CapWhy("light.kbd");
    col = th.amber;
    icon = IconId::Warn;
  } else if (!L.lit) {
    text = T("The keyboard light is switched off.");
  } else {
    text = Tf("%s \xC2\xB7 brightness %lld of 4", EffectLabel(static_cast<int>(a.I("light.kbd.effect.id", 0))).c_str(), a.I("light.kbd.bright", 4));
    icon = IconId::Bolt;
  }
  g.Icon(icon, Rect(r.x, r.cy() - 8, 16, 16), col, 1.7f);
  g.Text(Rect(r.x + 24, r.y, r.w * 0.55f - 24, r.h), text, FontId::Small, col);
  if (g.TextW(text, FontId::Small) > r.w * 0.55f - 24) ui.Tip(Rect(r.x + 24, r.y, r.w * 0.55f - 24, r.h), text);
  const long long n = a.I("light.kbd.blocks", 1);
  const std::string right = n <= 1 ? T("One light block: every key shows the same colour.") : Tf("%lld light blocks, left to right.", n);
  g.Text(Rect(r.x + r.w * 0.55f, r.y, r.w * 0.45f, r.h), right, FontId::Tiny, th.faint, Align::Right);
  if (g.TextW(right, FontId::Tiny) > r.w * 0.45f) ui.Tip(Rect(r.x + r.w * 0.55f, r.y, r.w * 0.45f, r.h), right);
}

}  // namespace

float PageLighting(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Masonry m(r.w, 440, 2);
  const bool kbd = a.CapOk("light.kbd");
  const bool tr = a.Pref("keys.layout", GetLang() == Lang::Tr ? "tr" : "us") == "tr";
  const int effectId = static_cast<int>(a.I("light.kbd.effect.id", 0));
  const int blocks = static_cast<int>(std::max(1LL, a.I("light.kbd.blocks", 1)));
  const bool zonesEditable = blocks > 1 && blocks <= kMaxEditableZones;
  int& zone = a.Int("light.zone", 0);   // 0 = every block, 1..n = one block
  if (!zonesEditable || zone > blocks) zone = 0;

  // ---- the colour being edited: it follows the keyboard until you pick something
  const std::vector<Rgb> zs = ZonesOf(a);
  Rgb live = zs[static_cast<size_t>(std::min<int>(std::max(zone - 1, 0), static_cast<int>(zs.size()) - 1))];
  int& h10 = a.Int("light.pick.h", 0);
  int& s1k = a.Int("light.pick.s", 0);
  int& v1k = a.Int("light.pick.v", 1000);
  bool& editing = a.Flag("light.pick.edit");
  editing = editing || a.Flag("light.pick.held");
  const bool pendingColour = a.Pending("light.kbd.color") || a.Pending("light.kbd.colors");
  if (!editing && !pendingColour) {
    const Hsv f = ToHsv(live, h10 / 10.0);
    h10 = static_cast<int>(std::lround(f.h * 10.0));
    s1k = static_cast<int>(std::lround(f.s * 1000.0));
    v1k = static_cast<int>(std::lround(f.v * 1000.0));
  }
  Hsv draft{h10 / 10.0, s1k / 1000.0, v1k / 1000.0};

  // =============================================================== the keyboard
  {
    const float innerW = m.InnerW(true);
    const float u = std::floor(std::min(58.0f, std::max(26.0f, (innerW - 28) / 15.0f)));
    const float kh = std::floor(std::min(44.0f, std::max(26.0f, u * 0.8f)));
    const float kbH = 6 * (kh + 4) + 18;
    const std::string note = T("The picture shows the colours the keyboard reports. Moving effects such as Wave or Ripple are run by the keyboard itself and are not animated here.");
    const float nh = NoteHeight(c, m, note, true);
    Card k(c, m, T("Keyboard"), "", IconId::Keyboard, kbH + 46 + nh, true);
    const Look L = LookNow(a);
    DrawKeyboard(ui, Rect(k.x0, k.y, k.w, kbH), tr, L);
    k.Gap(kbH + 12);
    StateLine(a, ui, Rect(k.x0, k.y, k.w, 26), L);
    k.Gap(34);
    k.Note(note);
  }

  // =============================================================== light settings
  {
    const std::string note = T("\"Save to the keyboard\" also keeps the lighting in the keyboard's own memory, so it applies before Windows has started.");
    const float h = kRowToggle + 9 + kRowCombo + 9 + kRowCombo + 9 + kRowSeg + 9 + kRowSeg + 9 + kRowCombo + 9 + kRowButtons + NoteHeight(c, m, note);
    Card k(c, m, T("Keyboard light"), "", IconId::Lighting, h);
    k.BoolRow("light.kbd.power", T("Lighting on"), T("Turn the keyboard backlight on or off."));
    k.Divider();

    // effect: the vendor's list, plus the one that is active if it is not on it
    std::vector<std::string> items;
    int curIdx = -1;
    for (size_t i = 0; i < sizeof kEffects / sizeof kEffects[0]; i++) {
      items.push_back(T(kEffects[i].label));
      if (kEffects[i].id == effectId) curIdx = static_cast<int>(i);
    }
    if (curIdx < 0 && a.Has("light.kbd.effect.id")) {
      items.push_back(EffectLabel(effectId));
      curIdx = static_cast<int>(items.size()) - 1;
    }
    const int pe = k.ComboRow("cmb#light.effect", T("Effect"), EffectHelp(effectId), items, std::max(curIdx, 0), kbd, 200);
    if (pe >= 0 && pe != curIdx && static_cast<size_t>(pe) < sizeof kEffects / sizeof kEffects[0]) {
      const SettingDef* d = FindSetting("light.kbd.effect");
      const std::string name = d ? EnumName(*d, kEffects[pe].id) : "";
      if (!name.empty()) a.Set("light.kbd.effect", name, T("Effect") + ": " + items[static_cast<size_t>(pe)]);
    }
    k.Divider();

    const SettingDef* dd = FindSetting("light.kbd.dir");
    const std::vector<std::string> dirs = {T("None"), T("Left to right"), T("Right to left"), T("Bottom to top"), T("Top to bottom"), T("From the pressed key"), T("Synchronised")};
    const int curDir = dd && a.Has("light.kbd.dir") ? std::max(0, EnumIndex(*dd, a.S("light.kbd.dir"))) : 0;
    const int pd = k.ComboRow("cmb#light.dir", T("Direction"), T("Used by effects that move, such as Wave and Ripple."), dirs, curDir, kbd, 200);
    if (pd >= 0 && pd != curDir && dd) a.Set("light.kbd.dir", EnumName(*dd, pd), T("Direction") + ": " + dirs[static_cast<size_t>(pd)]);
    k.Divider();

    const int bright = static_cast<int>(std::max(0LL, std::min(4LL, a.I("light.kbd.bright", 4))));
    const int pb = k.Segment("seg#light.bright", T("Brightness"), T("The keyboard light has five steps."), {"0", "1", "2", "3", "4"}, bright, kbd);
    if (pb >= 0 && pb != bright) a.Set("light.kbd.bright", std::to_string(pb), T("Brightness") + ": " + std::to_string(pb));
    k.Divider();

    const int speed = static_cast<int>(std::max(0LL, std::min(4LL, a.I("light.kbd.speed", 2))));
    const int ps = k.Segment("seg#light.speed", T("Effect speed"), T("0 is slow, 4 is fast. Used by effects that move."), {"0", "1", "2", "3", "4"}, speed, kbd);
    if (ps >= 0 && ps != speed) a.Set("light.kbd.speed", std::to_string(ps), T("Effect speed") + ": " + std::to_string(ps));
    k.Divider();

    {   // idle timer: the vendor's service switches the light off after this long without input
      const bool haveTimer = a.Has("light.kbd.timer");
      const int curSec = haveTimer ? static_cast<int>(a.I("light.kbd.timer", 0)) : 0;
      std::vector<std::string> titems;
      int tcur = -1;
      for (size_t i = 0; i < sizeof kTimerChoices / sizeof kTimerChoices[0]; i++) {
        titems.push_back(TimerLabel(kTimerChoices[i]));
        if (kTimerChoices[i] == curSec) tcur = static_cast<int>(i);
      }
      if (tcur < 0 && haveTimer) {
        titems.push_back(TimerLabel(curSec));
        tcur = static_cast<int>(titems.size()) - 1;
      }
      const int pt = k.ComboRow("cmb#light.timer", T("Idle timer"), T("The light goes out after this idle time."), titems, std::max(tcur, 0),
                                a.CapOk("light.kbd.timer"), 200);
      if (pt >= 0 && pt != tcur && static_cast<size_t>(pt) < sizeof kTimerChoices / sizeof kTimerChoices[0])
        a.Set("light.kbd.timer", std::to_string(kTimerChoices[pt]), T("Idle timer") + ": " + titems[static_cast<size_t>(pt)]);
    }
    k.Divider();

    if (k.Button("light.save", T("Save to the keyboard"), Btn::Normal, IconId::Download, kbd && !a.Pending("cmd:kb.save"))) a.Cmd("kb.save", KV(), T("Lighting saved to the keyboard"));
    k.Note(note);
  }

  // =============================================================== colour
  {
    const bool ignored = effectId == 4;   // Rainbow
    const std::string ign = T("This effect chooses its own colours, so the colours below are not used.");
    const std::string note = T("Pick a colour and let go: it is sent to the keyboard when you release the mouse button.");
    float h = (zonesEditable ? 50.0f : 0.0f) + 168 + kRowSwatch + kRowButtons + 9 + kRowButtons + NoteHeight(c, m, note);
    if (ignored) h += NoteHeight(c, m, ign);
    Card k(c, m, T("Colour"), "", IconId::Sun, h);
    if (ignored) k.Note(ign, th.amber);

    if (zonesEditable) {   // which light block the picker edits: all of them, or one
      const Rect row = k.Row(50);
      const float chipW = 46;
      float x = row.x;
      auto pickBtn = [&](int zi, const std::string& label, uint32_t swatch) {
        const bool sel = zone == zi;
        const std::string id = Fmt("zn#%d", zi);
        const float w = zi == 0 ? std::max(chipW, g.TextW(label, FontId::Label) + 28) : chipW;
        const Rect b(x, row.y + 6, w, 36);
        bool hov = false;
        const bool click = ui.Click(id, b, kbd, &hov, nullptr);
        g.FillRound(b, 9, sel ? WithAlpha(th.accent, 40) : (hov ? th.hover : th.inset));
        g.StrokeRound(b, 9, sel ? th.accent : th.line, sel ? 1.6f : 1.0f);
        if (zi == 0) {
          g.Text(b, label, FontId::Label, sel ? th.accent : th.text, Align::Center);
        } else {
          g.FillRound(Rect(b.x + 6, b.y + 6, b.w - 12, 10), 4, swatch);
          g.Text(Rect(b.x, b.y + 17, b.w, 17), label, FontId::Small, sel ? th.accent : th.muted, Align::Center);
        }
        x += w + 8;
        if (click && zone != zi) {
          zone = zi;
          editing = false;
        }
      };
      pickBtn(0, T("All"), 0);
      for (int i = 1; i <= blocks; i++) pickBtn(i, std::to_string(i), ToArgb(zs[static_cast<size_t>(std::min<int>(i - 1, static_cast<int>(zs.size()) - 1))]));
      ui.Tip(Rect(row.x, row.y, row.w, row.h), T("Choose which light block the colour below applies to: all of them, or one (counted from the left)."));
    }

    const PickResult pr = ColorPicker(ui, a, Rect(k.x0, k.y, k.w, 152), &draft, kbd && !ignored);
    k.Gap(168);
    if (pr.changed) {
      editing = true;
      h10 = static_cast<int>(std::lround(draft.h * 10.0));
      s1k = static_cast<int>(std::lround(draft.s * 1000.0));
      v1k = static_cast<int>(std::lround(draft.v * 1000.0));
    }
    if (pr.released) SendColour(a, zone, ToHex(FromHsv(draft.h, draft.s, draft.v)));
    if (editing && !pr.held && !pendingColour) editing = false;

    // preset colours
    std::vector<uint32_t> cols(std::begin(kSwatches), std::end(kSwatches));
    int curSw = -1;
    for (size_t i = 0; i < cols.size(); i++)
      if (HexOf(cols[i]) == ToHex(live)) curSw = static_cast<int>(i);
    const int pw = k.Swatches("sw#light.preset", T("Presets"), cols, curSw, kbd && !ignored);
    if (pw >= 0) {
      editing = false;
      SendColour(a, zone, HexOf(cols[static_cast<size_t>(pw)]));
    }

    // hex field: it follows the colour (picker, presets, other programs) but keeps what you typed until the colour itself changes
    std::string& hexText = a.Str("light.hex");
    {
      const std::string src = ToHex((editing || pendingColour) ? FromHsv(draft.h, draft.s, draft.v) : live);
      std::string& seen = a.Str("light.hex.seen");
      if (seen != src) {
        seen = src;
        hexText = src;
      }
    }
    if (k.InputRow("light.hex", &hexText, "#rrggbb", T("Set"), kbd && !ignored, 7, true)) {
      Rgb t;
      if (ParseHex(Trim(hexText), &t)) {
        editing = false;
        SendColour(a, zone, ToHex(t));
        ui.ClearFocus();
      } else {
        ui.Toast(T("Use a colour like #ff8800."), ToastKind::Warn);
      }
    }
    k.Divider();
    if (k.Button("light.reset", T("Restore defaults"), Btn::Normal, IconId::Undo, kbd)) {
      std::vector<Request> reqs;
      std::vector<std::string> labels;
      const std::pair<const char*, const char*> kDef[] = {{"light.kbd.power", "1"}, {"light.kbd.effect", "static"}, {"light.kbd.color", "#ffffff"}, {"light.kbd.bright", "4"}};
      for (auto& d : kDef) {
        Request q;
        q.op = "set";
        q.key = d.first;
        q.value = d.second;
        reqs.push_back(q);
        labels.push_back("");
      }
      labels.back() = T("Lighting restored to white, static, full brightness.");
      editing = false;
      a.Chain(reqs, labels);
    }
    k.Note(note);
  }

  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
