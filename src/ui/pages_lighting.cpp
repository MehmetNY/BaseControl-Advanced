// Lighting: a live drawing of the keyboard (the same effect maths the keyboard runs, see common/lightfx), the light's own
// settings, a colour picker that only sends the colour when you let go, the idle timeout and the rules by power state.
#include <algorithm>
#include <cmath>

#include "../common/keymap.h"
#include "../common/lightfx.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

using lightfx::Rgb;

uint32_t ToArgb(const Rgb& c, int a = 255) { return Argb(a, c.r, c.g, c.b); }

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

const char* const kEffectIds[lightfx::kEffectCount] = {"static", "breathing", "cycle", "temp", "pulse"};

int EffectIdx(const std::string& n) {
  for (int i = 0; i < lightfx::kEffectCount; i++)
    if (n == kEffectIds[i]) return i;
  return 0;
}

std::vector<std::string> EffectLabels() { return {T("Static"), T("Breathing"), T("Cycle"), T("CPU temp"), T("Pulse")}; }

std::string EffectHelp(int e) {
  switch (e) {
    case lightfx::kBreathing: return T("The colour fades in and out.");
    case lightfx::kCycle: return T("Runs through every colour. The chosen colour is not used.");
    case lightfx::kTemp: return T("Green while the CPU is cool, red near 95 \xC2\xB0""C. The chosen colour is not used.");
    case lightfx::kPulse: return T("A short flash that fades out, again and again.");
    default: return T("One steady colour.");
  }
}

struct IdleChoice {
  int sec;
  std::string text;
};

std::string Duration(long long s) {
  if (s <= 0) return T("Off");
  if (s < 60) return Fmt("%lld s", s);
  if (s % 60 == 0) return Fmt("%lld min", s / 60);
  return Fmt("%lld s", s);
}

std::vector<IdleChoice> IdleChoices(long long current) {
  std::vector<int> v = {10, 15, 30, 60, 120, 300, 600};
  if (current > 0 && std::find(v.begin(), v.end(), static_cast<int>(current)) == v.end()) {
    v.push_back(static_cast<int>(current));   // a value set from elsewhere stays selectable
    std::sort(v.begin(), v.end());
  }
  std::vector<IdleChoice> out;
  for (int s : v) out.push_back({s, Duration(s)});
  return out;
}

// What the keyboard shows right now.
struct Look {
  Rgb col;
  double frac = 1.0;
  bool lit = false;
  std::string why;   // "" = lit, otherwise: user | idle | display | sleep | absent
};

Look LookNow(App& a, const Ui& ui, const Rgb* draft) {
  Look l;
  if (!a.CapOk("light.kbd")) {
    l.why = "absent";
    return l;
  }
  Rgb base;
  if (!lightfx::ParseHex(a.S("light.kbd.color", "#ffffff"), &base)) base = Rgb{255, 255, 255};
  if (draft) base = *draft;
  const int e = EffectIdx(a.S("light.kbd.effect", "static"));
  l.col = lightfx::EffectColor(e, static_cast<int>(a.I("light.kbd.speed", 5)), static_cast<uint64_t>(ui.now * 1000.0), base, a.I("cpu.temp", 60));
  const int lvl = static_cast<int>(a.Has("light.bright.eff") ? a.I("light.bright.eff") : a.I("light.kbd.bright", 4));
  l.frac = lightfx::LevelFraction(lvl);
  l.why = !a.B("light.kbd.power", true) ? std::string("user") : a.S("light.off.why");
  l.lit = l.why.empty();
  return l;
}

void LitCap(Ui& ui, const Rect& r, const std::string& legend, const Look& L, bool fontBig) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const uint32_t unlit = Mix(th.elev, th.panel, 0.25f);
  uint32_t fill = unlit, border = th.line, text = th.faint;
  if (L.lit) {
    const uint32_t lc = ToArgb(L.col);
    fill = Mix(unlit, lc, 0.20f + 0.50f * static_cast<float>(L.frac));
    border = Mix(th.line, lc, 0.45f * static_cast<float>(L.frac));
    text = Luma(fill) > 0.58 ? Argb(255, 28, 29, 27) : Argb(255, 244, 241, 235);
  }
  g.FillRound(r, 6, fill);
  g.StrokeRound(r, 6, border, 1.0f);
  g.Text(r.Inset(2, 0), legend, fontBig ? FontId::Small : FontId::Tiny, text, Align::Center);
}

void Glow(Ui& ui, const Rect& r, const Look& L) {
  if (!L.lit) return;
  ui.gfx.FillRound(r.Inset(-2.5f), 8, WithAlpha(ToArgb(L.col), static_cast<int>(34.0 * L.frac)));
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

// The keyboard drawing (geometry as on the key-mapping page). Two passes: the glow of every key first, then the caps on top.
void DrawKeyboard(Ui& ui, const Rect& board, bool tr, const Look& L) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const float u = std::floor(std::min(58.0f, std::max(26.0f, (board.w - 28) / 15.0f)));
  const float kh = std::floor(std::min(44.0f, std::max(26.0f, u * 0.8f)));
  const float gap = 4;
  const float bx = board.x + (board.w - 15 * u) * 0.5f;
  ui.Panel(board, th.inset, th.line, 12);
  const auto& rows = KeyboardRows();
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
        if (pass == 0) Glow(ui, kr, L);
        else LitCap(ui, kr, tr ? pk.tr : pk.us, L, pk.w * u > 34);
      }
      by += kh + gap;
    }
    if (enter.size() == 2) {   // the ISO Enter key: two rectangles that read as one key
      const Rect& t = enter[0];
      const Rect& l = enter[1];
      if (pass == 0) {
        Glow(ui, t, L);
        Glow(ui, l, L);
      } else {
        const std::vector<V2> shape = RoundedPoly({{t.x, t.y}, {t.r(), t.y}, {t.r(), l.b()}, {l.x, l.b()}, {l.x, t.b() + 4}, {t.x, t.b() + 4}}, 6.0f);
        uint32_t fill = Mix(th.elev, th.panel, 0.25f), border = th.line, text = th.faint;
        if (L.lit) {
          const uint32_t lc = ToArgb(L.col);
          fill = Mix(fill, lc, 0.20f + 0.50f * static_cast<float>(L.frac));
          border = Mix(th.line, lc, 0.45f * static_cast<float>(L.frac));
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
  g.FillRound(sv, 8, ToArgb(lightfx::FromHsv(hsv->h, 1, 1)));
  g.FillGrad(sv, 8, Argb(255, 255, 255, 255), Argb(0, 255, 255, 255), 0);
  g.FillGrad(sv, 8, Argb(0, 0, 0, 0), Argb(255, 0, 0, 0), 90);
  g.StrokeRound(sv, 8, th.line, 1.0f);
  for (int i = 0; i < 6; i++) {
    const float x0 = hue.x + hue.w * static_cast<float>(i) / 6.0f, x1 = hue.x + hue.w * static_cast<float>(i + 1) / 6.0f;
    g.FillGrad(Rect(x0, hue.y, x1 - x0 + 0.6f, hue.h), 0, ToArgb(lightfx::FromHsv(i * 60.0, 1, 1)), ToArgb(lightfx::FromHsv((i + 1) * 60.0, 1, 1)), 0);
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
  const Rgb now = lightfx::FromHsv(hsv->h, hsv->s, hsv->v);
  const V2 hp{static_cast<float>(sv.x + sv.w * hsv->s), static_cast<float>(sv.y + sv.h * (1.0 - hsv->v))};
  g.Circle(hp, 8, Argb(200, 0, 0, 0), 3.0f);
  g.Circle(hp, 8, Argb(255, 255, 255, 255), 1.8f);
  g.Dot(hp, 5.5f, ToArgb(now));
  const float hx = static_cast<float>(hue.x + hue.w * hsv->h / 360.0);
  const Rect hb(hx - 4, hue.y - 3, 8, hue.h + 6);
  g.FillRound(hb, 4, ToArgb(lightfx::FromHsv(hsv->h, 1, 1)));
  g.StrokeRound(hb, 4, Argb(255, 255, 255, 255), 2.0f);
  return out;
}

const uint32_t kSwatches[] = {0xFFFFFFFF, 0xFFFFD9A8, 0xFFFF2D2D, 0xFFFF8C1A, 0xFFFFD400, 0xFF8BE000, 0xFF19D45A, 0xFF14D4C4, 0xFF1EC8FF, 0xFF2F6BFF, 0xFF8A5CFF, 0xFFFF4FB8};

std::string HexOf(uint32_t argb) {
  Rgb c;
  c.r = static_cast<uint8_t>((argb >> 16) & 255);
  c.g = static_cast<uint8_t>((argb >> 8) & 255);
  c.b = static_cast<uint8_t>(argb & 255);
  return lightfx::ToHex(c);
}

void StateLine(App& a, Ui& ui, const Rect& r, const Look& L) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  std::string text;
  uint32_t col = th.muted;
  IconId icon = IconId::Info;
  if (L.why == "absent") {
    text = a.CapWhy("light.kbd").empty() ? T("No keyboard lighting controller was found on this computer.") : a.CapWhy("light.kbd");
    col = th.amber;
    icon = IconId::Warn;
  } else if (L.why == "user") {
    text = T("The keyboard light is switched off.");
  } else if (L.why == "idle") {
    text = T("Off because you are idle. It comes back with the next key press or mouse move.");
    col = th.amber;
    icon = IconId::Moon;
  } else if (L.why == "display") {
    text = T("Off while the display is off. It comes back when the display wakes.");
    col = th.amber;
    icon = IconId::Moon;
  } else if (L.why == "sleep") {
    text = T("Off while the laptop sleeps.");
    col = th.amber;
    icon = IconId::Moon;
  } else {
    const long long set = a.I("light.kbd.bright", 4), eff = a.Has("light.bright.eff") ? a.I("light.bright.eff") : set;
    text = Tf("%s \xC2\xB7 brightness %lld %%", EffectLabels()[static_cast<size_t>(EffectIdx(a.S("light.kbd.effect", "static")))].c_str(), eff * 25);
    if (eff < set) text += " \xC2\xB7 " + T("limited while on battery");
    icon = IconId::Bolt;
    col = th.muted;
  }
  g.Icon(icon, Rect(r.x, r.cy() - 8, 16, 16), col, 1.7f);
  g.Text(Rect(r.x + 24, r.y, r.w * 0.62f - 24, r.h), text, FontId::Small, col);
  if (g.TextW(text, FontId::Small) > r.w * 0.62f - 24) ui.Tip(Rect(r.x + 24, r.y, r.w * 0.62f - 24, r.h), text);
  const std::string right = T("One light zone: every key shows the same colour.");
  g.Text(Rect(r.x + r.w * 0.62f, r.y, r.w * 0.38f, r.h), right, FontId::Tiny, th.faint, Align::Right);
  if (g.TextW(right, FontId::Tiny) > r.w * 0.38f) ui.Tip(Rect(r.x + r.w * 0.62f, r.y, r.w * 0.38f, r.h), right);
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
  const int effect = EffectIdx(a.S("light.kbd.effect", "static"));

  // ---- the colour being edited: it follows the hardware until you pick something
  Rgb live{255, 255, 255};
  lightfx::ParseHex(a.S("light.kbd.color", "#ffffff"), &live);
  int& h10 = a.Int("light.pick.h", 0);
  int& s1k = a.Int("light.pick.s", 0);
  int& v1k = a.Int("light.pick.v", 1000);
  bool& editing = a.Flag("light.pick.edit");
  editing = editing || a.Flag("light.pick.held");
  if (!editing && !a.Pending("light.kbd.color")) {
    const Hsv f = ToHsv(live, h10 / 10.0);
    h10 = static_cast<int>(std::lround(f.h * 10.0));
    s1k = static_cast<int>(std::lround(f.s * 1000.0));
    v1k = static_cast<int>(std::lround(f.v * 1000.0));
  }
  Hsv draft{h10 / 10.0, s1k / 1000.0, v1k / 1000.0};
  const Rgb draftRgb = lightfx::FromHsv(draft.h, draft.s, draft.v);
  const bool showDraft = editing || a.Pending("light.kbd.color");

  // =============================================================== the keyboard
  {
    const float innerW = m.InnerW(true);
    const float u = std::floor(std::min(58.0f, std::max(26.0f, (innerW - 28) / 15.0f)));
    const float kh = std::floor(std::min(44.0f, std::max(26.0f, u * 0.8f)));
    const float kbH = 6 * (kh + 4) + 18;
    Card k(c, m, T("Keyboard"), "", IconId::Keyboard, kbH + 46, true);
    const Look L = LookNow(a, ui, showDraft ? &draftRgb : nullptr);
    if (L.lit && effect != lightfx::kStatic) ui.RequestFrame();   // animated effects redraw every frame
    DrawKeyboard(ui, Rect(k.x0, k.y, k.w, kbH), tr, L);
    k.Gap(kbH + 12);
    StateLine(a, ui, Rect(k.x0, k.y, k.w, 26), L);
    k.Gap(34);
  }

  // =============================================================== light settings
  {
    float h = kRowToggle + 9 + kRowSeg + 9 + kRowSlider;
    if (effect != lightfx::kStatic) h += 9 + kRowSlider;
    Card k(c, m, T("Keyboard light"), "", IconId::Lighting, h);
    k.BoolRow("light.kbd.power", T("Lighting on"), T("Turn the keyboard backlight on or off."));
    k.Divider();
    const int cur = effect;
    const int pick = k.Segment("seg#light.effect", T("Effect"), EffectHelp(effect), EffectLabels(), cur, kbd);
    if (pick >= 0 && pick != cur) a.Set("light.kbd.effect", kEffectIds[pick], T("Effect") + ": " + EffectLabels()[static_cast<size_t>(pick)]);
    k.Divider();
    SliderCfg b;
    b.key = "light.kbd.bright";
    b.title = T("Brightness");
    b.desc = T("The keyboard light has four steps.");
    b.fmt = [](int v) { return Fmt("%d %%", v * 25); };
    k.Slider(b);
    if (effect != lightfx::kStatic) {
      k.Divider();
      SliderCfg sp;
      sp.key = "light.kbd.speed";
      sp.title = T("Effect speed");
      sp.desc = T("1 is slow, 10 is fast.");
      sp.fmt = [](int v) { return Fmt("%d / 10", v); };
      k.Slider(sp);
    }
  }

  // =============================================================== colour
  {
    const bool ignored = effect == lightfx::kCycle || effect == lightfx::kTemp;
    const std::string note = T("BaseControl keeps your lighting and applies it again at start-up, after sleep and when the display wakes.");
    const std::string ign = T("This effect chooses its own colours, so the colour below is not used.");
    float h = 122 + 12 + 18 + 16 + kRowSwatch + kRowButtons + 9 + kRowButtons + NoteHeight(c, m, note);
    if (ignored) h += NoteHeight(c, m, ign);
    Card k(c, m, T("Colour"), "", IconId::Sun, h);
    if (ignored) k.Note(ign, th.amber);
    const PickResult pr = ColorPicker(ui, a, Rect(k.x0, k.y, k.w, 152), &draft, kbd && !ignored);
    k.Gap(168);
    if (pr.changed) {
      editing = true;
      h10 = static_cast<int>(std::lround(draft.h * 10.0));
      s1k = static_cast<int>(std::lround(draft.s * 1000.0));
      v1k = static_cast<int>(std::lround(draft.v * 1000.0));
    }
    if (pr.released) {
      const std::string hex = lightfx::ToHex(lightfx::FromHsv(draft.h, draft.s, draft.v));
      a.Set("light.kbd.color", hex, Tf("Keyboard colour: %s", hex.c_str()));
    }
    if (editing && !pr.held && !a.Pending("light.kbd.color")) editing = false;
    // preset colours
    std::vector<uint32_t> cols(std::begin(kSwatches), std::end(kSwatches));
    int curSw = -1;
    for (size_t i = 0; i < cols.size(); i++)
      if (HexOf(cols[i]) == lightfx::ToHex(live)) curSw = static_cast<int>(i);
    const int ps = k.Swatches("sw#light.preset", T("Presets"), cols, curSw, kbd && !ignored);
    if (ps >= 0) {
      editing = false;
      a.Set("light.kbd.color", HexOf(cols[static_cast<size_t>(ps)]), Tf("Keyboard colour: %s", HexOf(cols[static_cast<size_t>(ps)]).c_str()));
    }
    // hex field
    std::string& hexText = a.Str("light.hex");
    {
      // the field follows the colour (picker, presets, other programs) but keeps what you typed until the colour itself changes
      const std::string src = lightfx::ToHex(showDraft ? draftRgb : live);
      std::string& seen = a.Str("light.hex.seen");
      if (seen != src) {
        seen = src;
        hexText = src;
      }
    }
    if (k.InputRow("light.hex", &hexText, "#rrggbb", T("Set"), kbd && !ignored, 7, true)) {
      Rgb t;
      if (lightfx::ParseHex(Trim(hexText), &t)) {
        editing = false;
        a.Set("light.kbd.color", lightfx::ToHex(t), Tf("Keyboard colour: %s", lightfx::ToHex(t).c_str()));
        ui.ClearFocus();
      } else {
        ui.Toast(T("Use a colour like #ff8800."), ToastKind::Warn);
      }
    }
    k.Divider();
    if (k.Button("light.reset", T("Restore defaults"), Btn::Normal, IconId::Undo, kbd)) {
      std::vector<Request> reqs;
      std::vector<std::string> labels;
      const std::pair<const char*, const char*> kDef[] = {{"light.kbd.power", "1"}, {"light.kbd.effect", "static"}, {"light.kbd.color", "#ffffff"}, {"light.kbd.bright", "4"}, {"light.kbd.speed", "5"}};
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

  // =============================================================== idle timeout
  {
    const std::string note = T("Counted from your last key press or mouse move. BaseControl watches for it, so the tray icon or the window has to be running.");
    const long long idle = a.I("light.idle", 0);
    const float h = 80 + NoteHeight(c, m, note) + kRowToggle + 9 + kRowCombo;
    Card k(c, m, T("Idle timeout"), "", IconId::Moon, h);
    const Rect row = k.Row(80);
    const Rect box(row.x, row.y + 8, 56, 56);
    g.FillRound(box, 14, Mix(th.inset, th.elev, 0.5f));
    g.StrokeRound(box, 14, th.line, 1.0f);
    g.Icon(IconId::Keyboard, Rect(box.cx() - 13, box.cy() - 13, 26, 26), idle > 0 ? th.accent : th.faint, 1.7f);
    std::string big, unit;
    if (idle <= 0) {
      big = T("Off");
    } else if (idle < 60 || idle % 60 != 0) {
      big = Fmt("%lld", idle);
      unit = "s";
    } else {
      big = Fmt("%lld", idle / 60);
      unit = "min";
    }
    const float bw = g.TextW(big, FontId::Mega);
    const std::string cap = T("Light turns off after");
    g.Text(Rect(box.r() + 16, row.y + 4, 220, 16), cap, FontId::Tiny, th.faint, Align::Left, false);
    g.Text(Rect(box.r() + 16, row.y + 22, bw + 6, 52), big, FontId::Mega, idle > 0 ? th.text : th.faint);
    if (!unit.empty()) g.Text(Rect(box.r() + 22 + bw, row.y + 40, 60, 30), unit, FontId::H3, th.muted, Align::Left, false);
    // status chip
    std::string chip;
    uint32_t cc = th.faint;
    if (idle <= 0) {
      chip = T("Not used");
    } else if (a.S("light.off.why") == "idle") {
      chip = T("Idle: lights off");
      cc = th.amber;
    } else {
      chip = T("Watching");
      cc = th.green;
    }
    const float cw = ui.ChipW(chip);
    ui.Chip(Rect(row.r() - cw, row.y + 8, cw, 22), chip, cc);
    k.Note(note);
    // switch + wait time
    if (k.Toggle("sw#light.idle", T("Turn off when idle"), T("Any key press or mouse move turns the light back on."), idle > 0, kbd)) {
      const std::string last = a.Pref("light.idle.last", "30");
      a.Set("light.idle", idle > 0 ? "0" : last, T("Idle timeout") + ": " + (idle > 0 ? T("off") : T("on")));
    }
    k.Divider();
    const auto ch = IdleChoices(idle > 0 ? idle : std::atoll(a.Pref("light.idle.last", "30").c_str()));
    std::vector<std::string> items;
    int curIdx = 0;
    const long long shown = idle > 0 ? idle : std::atoll(a.Pref("light.idle.last", "30").c_str());
    for (size_t i = 0; i < ch.size(); i++) {
      items.push_back(ch[i].text);
      if (ch[i].sec == shown) curIdx = static_cast<int>(i);
    }
    const int pick = k.ComboRow("cmb#light.idle", T("Wait time"), "", items, curIdx, kbd && idle > 0, 150);
    if (pick >= 0 && pick != curIdx) {
      a.SetPref("light.idle.last", std::to_string(ch[static_cast<size_t>(pick)].sec));
      a.Set("light.idle", std::to_string(ch[static_cast<size_t>(pick)].sec), T("Idle timeout") + ": " + ch[static_cast<size_t>(pick)].text);
    }
  }

  // =============================================================== by power state
  {
    const bool dim = a.B("light.battdim", false);
    float h = kRowToggle + 9 + kRowToggle;
    if (dim) h += kRowSegLean;
    Card k(c, m, T("By power state"), "", IconId::Battery, h);
    k.BoolRow("light.battdim", T("Dim on battery"), T("Limit the brightness while the charger is unplugged."));
    if (dim) {
      const int lvl = static_cast<int>(a.I("light.battlevel", 2));
      const int cur = std::max(1, std::min(3, lvl)) - 1;
      const int pick = k.Segment("seg#light.battlevel", T("Brightness on battery"), "", {"25 %", "50 %", "75 %"}, cur, kbd);
      if (pick >= 0 && pick != cur) a.Set("light.battlevel", std::to_string(pick + 1), T("Brightness on battery") + ": " + Fmt("%d %%", (pick + 1) * 25));
    }
    k.Divider();
    k.BoolRow("light.sleepoff", T("Off with the display and in sleep"), T("The lights go out with the screen and come back when it wakes."));
  }

  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
