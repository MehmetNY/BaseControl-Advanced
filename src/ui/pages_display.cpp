// Display: a preview that shows the colours you are choosing before anything is sent, the image profiles, the fine colour controls (every
// one with its own Confirm), refresh rate and panel brightness. The colours are BaseControl's own: a gamma ramp plus a colour effect.
// The panel's factory calibration is a different thing and stays with Windows' colour management (the last card says so).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../common/colorramp.h"
#include "../common/settings_reg.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {

namespace {

// every colour setting, in the order the sliders appear
const char* const kColorKeys[8] = {"disp.c.kelvin", "disp.c.r", "disp.c.g", "disp.c.b", "disp.c.sat", "disp.c.gamma", "disp.c.bright", "disp.c.contrast"};

std::string Dash() { return "\xE2\x80\x94"; }

std::string ProfileName(const std::string& id) {
  if (id == "standard") return T("Standard");
  if (id == "vibrant") return T("Vibrant");
  if (id == "cinema") return T("Cinema");
  if (id == "internet") return T("Internet");
  if (id == "photo") return T("Photo");
  if (id == "video") return T("Video");
  if (id == "lowblue") return T("Low blue light");
  return id;
}

std::string ProfileDesc(const std::string& id) {
  if (id == "standard") return T("Neutral. The panel looks the way Windows draws it.");
  if (id == "vibrant") return T("Stronger colours and a little more contrast.");
  if (id == "cinema") return T("Deeper tones, slightly warm. Made for dark rooms.");
  if (id == "internet") return T("Slightly richer colours for web pages.");
  if (id == "photo") return T("Gently boosted colours with a touch more contrast.");
  if (id == "video") return T("Brighter and punchier for films and clips.");
  if (id == "lowblue") return T("A warm tone that is easier on the eyes in the evening.");
  return "";
}

// ------------------------------------------------------------------------------------------------ colours in play
// What the panel applies right now: the values the controller reports (it stores what it applied).
ColorSettings LiveColor(App& a) {
  ColorSettings c;
  if (a.Has("disp.c.bright")) c.brightness = static_cast<int>(a.I("disp.c.bright"));
  if (a.Has("disp.c.contrast")) c.contrast = static_cast<int>(a.I("disp.c.contrast"));
  if (a.Has("disp.c.gamma")) c.gamma = static_cast<int>(a.I("disp.c.gamma"));
  if (a.Has("disp.c.kelvin")) c.kelvin = static_cast<int>(a.I("disp.c.kelvin"));
  if (a.Has("disp.c.r")) c.r = static_cast<int>(a.I("disp.c.r"));
  if (a.Has("disp.c.g")) c.g = static_cast<int>(a.I("disp.c.g"));
  if (a.Has("disp.c.b")) c.b = static_cast<int>(a.I("disp.c.b"));
  if (a.Has("disp.c.sat")) c.saturation = static_cast<int>(a.I("disp.c.sat"));
  return c;
}

bool ColorDraftActive(App& a) {
  for (const char* k : kColorKeys) {
    auto it = a.drafts().find(k);
    if (it != a.drafts().end() && it->second.active) return true;
  }
  return false;
}

// What the preview shows: the picked image profile, else the live colours with every slider draft laid over them.
ColorSettings Wanted(App& a, const ColorSettings& live, int profile) {
  const auto& presets = ColorPresets();
  if (profile >= 0 && profile < static_cast<int>(presets.size())) return presets[static_cast<size_t>(profile)].c;
  ColorSettings c = live;
  auto pick = [&](const char* key, int* v) {
    auto it = a.drafts().find(key);
    if (it != a.drafts().end() && it->second.active) *v = it->second.v;
  };
  pick("disp.c.bright", &c.brightness);
  pick("disp.c.contrast", &c.contrast);
  pick("disp.c.gamma", &c.gamma);
  pick("disp.c.kelvin", &c.kelvin);
  pick("disp.c.r", &c.r);
  pick("disp.c.g", &c.g);
  pick("disp.c.b", &c.b);
  pick("disp.c.sat", &c.saturation);
  return c;
}

std::string KeyOf(const ColorSettings& c) {
  return Fmt("%d,%d,%d,%d,%d,%d,%d,%d", c.brightness, c.contrast, c.gamma, c.kelvin, c.r, c.g, c.b, c.saturation);
}

// ------------------------------------------------------------------------------------------------ the picture colours are judged on
// A mountain lake at dusk, made once from noise (no image file ships with the program): sky and sun glow, snow on the ridge, a still lake
// that mirrors them, warm grass and grey rock in front. It has skin-ish tones, greens, blues, snow and deep shadow, which is what colour
// settings are judged on.
struct F3 {
  float r, g, b;
};
F3 Lerp(F3 a, F3 b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
F3 Scale(F3 a, float k) { return {a.r * k, a.g * k, a.b * k}; }
float Sstep(float a, float b, float x) {
  const float t = std::min(1.0f, std::max(0.0f, (x - a) / (b - a)));
  return t * t * (3.0f - 2.0f * t);
}
float Lattice(int x, int y, int seed) {
  unsigned n = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u + static_cast<unsigned>(seed) * 2147483647u;
  n = (n ^ (n >> 13)) * 1274126177u;
  return static_cast<float>((n ^ (n >> 16)) & 0xFFFFu) / 65535.0f;
}
float VNoise(float x, float y, int seed) {
  const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
  const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
  const float a = Lattice(xi, yi, seed), b = Lattice(xi + 1, yi, seed), c = Lattice(xi, yi + 1, seed), d = Lattice(xi + 1, yi + 1, seed);
  return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
float Fbm(float x, float y, int seed) {
  return 0.55f * VNoise(x, y, seed) + 0.27f * VNoise(x * 2.1f, y * 2.1f, seed + 1) + 0.13f * VNoise(x * 4.3f, y * 4.3f, seed + 2) +
         0.05f * VNoise(x * 8.9f, y * 8.9f, seed + 3);
}

constexpr int kSceneW = 448, kSceneH = 224;

std::vector<uint32_t> BuildScene() {
  const int W = kSceneW, H = kSceneH;
  const float fW = static_cast<float>(W), fH = static_cast<float>(H);
  const float horizon = fH * 0.60f;
  const float sunX = fW * 0.66f, sunY = horizon * 0.58f;
  std::vector<float> farR(static_cast<size_t>(W)), bigR(static_cast<size_t>(W)), slope(static_cast<size_t>(W));
  for (int x = 0; x < W; x++) {
    const float fx = static_cast<float>(x);
    farR[static_cast<size_t>(x)] = horizon - fH * (0.05f + 0.10f * Fbm(fx * 0.012f + 9.0f, 0.3f, 5));
    float v = 0, amp = 0.55f, freq = 0.011f;   // ridged noise: sharp peaks and saddles instead of rolling hills
    for (int i = 0; i < 4; i++) {
      const float nz = 1.0f - std::fabs(2.0f * VNoise(fx * freq + 5.0f, 0.5f, 11 + i) - 1.0f);
      v += amp * nz * nz;
      amp *= 0.5f;
      freq *= 2.1f;
    }
    bigR[static_cast<size_t>(x)] = horizon - fH * (0.05f + 0.42f * v);
  }
  for (int x = 0; x < W; x++)
    slope[static_cast<size_t>(x)] = bigR[static_cast<size_t>(std::min(W - 1, x + 3))] - bigR[static_cast<size_t>(std::max(0, x - 3))];   // > 0: the ridge falls to the right
  auto sky = [&](float x, float y) -> F3 {
    const float fy = std::min(1.0f, std::max(0.0f, y / horizon));
    const F3 top{0.09f, 0.16f, 0.38f}, mid{0.36f, 0.42f, 0.68f}, pink{0.90f, 0.64f, 0.68f}, low{1.0f, 0.82f, 0.58f};
    F3 c = fy < 0.45f ? Lerp(top, mid, fy / 0.45f) : (fy < 0.80f ? Lerp(mid, pink, (fy - 0.45f) / 0.35f) : Lerp(pink, low, (fy - 0.80f) / 0.20f));
    const float cl = Sstep(0.50f, 0.72f, Fbm(x * 0.011f, y * 0.045f + 3.0f, 61)) * (1.0f - fy * 0.55f);   // long, soft cloud bands
    c = Lerp(c, F3{1.0f, 0.78f, 0.74f}, cl * 0.45f);
    const float dx = (x - sunX) / fW, dy = (y - sunY) / fH;
    return Lerp(c, F3{1.0f, 0.90f, 0.68f}, 0.80f * std::exp(-(dx * dx * 10.0f + dy * dy * 30.0f) * 4.0f));
  };
  auto above = [&](float x, float y) -> F3 {   // sky and mountains, for any y above the horizon
    const size_t xi = static_cast<size_t>(std::min(W - 1, std::max(0, static_cast<int>(x))));
    F3 c = sky(x, y);
    if (y > farR[xi]) c = Lerp(Lerp(F3{0.50f, 0.52f, 0.72f}, F3{0.96f, 0.72f, 0.66f}, Sstep(farR[xi], horizon, y)), c, 0.12f);
    if (y > bigR[xi]) {
      const float rise = horizon - bigR[xi];
      const float depth = (y - bigR[xi]) / (rise + 1.0f);
      const float grain = Fbm(x * 0.17f, y * 0.14f, 29);
      const float facet = Fbm((x + y * 1.1f) * 0.06f, y * 0.04f, 37);   // diagonal facets, so the faces are not just columns
      const float lit = std::min(1.0f, std::max(0.0f, 0.50f + slope[xi] * 0.05f * (1.0f - 0.7f * depth) + (facet - 0.5f) * 1.2f + (grain - 0.5f) * 0.5f));   // the faces turned towards the sun
      F3 rock = Lerp(F3{0.14f, 0.17f, 0.28f}, F3{0.62f, 0.50f, 0.56f}, lit);
      rock = Lerp(rock, F3{0.50f, 0.52f, 0.70f}, Sstep(0.30f, 1.0f, depth) * 0.60f);                          // haze at the foot
      rock = Scale(rock, 0.88f + 0.24f * Fbm(x * 0.5f, y * 0.4f, 33));
      const float capDepth = rise * 0.30f * Sstep(0.30f, 0.65f, rise / (fH * 0.40f));
      const float snow = Sstep(0.0f, 1.0f, (capDepth - (y - bigR[xi]) + (grain - 0.5f) * 12.0f) / 4.0f);
      c = Lerp(rock, Lerp(F3{0.78f, 0.82f, 0.95f}, F3{1.0f, 0.88f, 0.84f}, lit), snow);
    }
    return c;
  };
  auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f)); };
  std::vector<uint32_t> px(static_cast<size_t>(W) * static_cast<size_t>(H));
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const float fx = static_cast<float>(x), fy = static_cast<float>(y);
      F3 c;
      if (fy < horizon) {
        c = above(fx, fy);
      } else {
        const float depthBelow = fy - horizon;
        const float my = std::max(0.0f, horizon - depthBelow * 1.05f);   // the lake mirrors what is above it, a little darker and rippled
        const float wob = std::sin(fy * 0.55f + VNoise(fx * 0.05f, fy * 0.30f, 31) * 6.0f) * (1.2f + depthBelow * 0.05f);
        c = above(fx + wob, my);
        c = Lerp(Scale(c, 0.78f), F3{0.10f, 0.17f, 0.30f}, 0.34f + 0.20f * Sstep(0.0f, fH - horizon, depthBelow));
        c = Scale(c, 0.93f + 0.09f * std::sin(fy * 1.6f + fx * 0.02f));
        const float sw = 5.0f + depthBelow * 0.22f, sd = (fx - sunX) / sw;   // the sun's glitter path
        c = Lerp(c, F3{1.0f, 0.82f, 0.58f}, 0.55f * std::exp(-sd * sd) * (0.45f + 0.55f * VNoise(fx * 0.4f, fy * 0.9f, 71)));
      }
      // the near shore: golden grass and dark rock, climbing towards the right-hand corner
      const float shore = fH * (0.99f - 0.38f * Sstep(0.38f, 1.0f, fx / fW));
      const float shoreL = fH * (0.975f - 0.13f * Sstep(0.22f, 0.0f, fx / fW));
      if (fy > std::min(shore, shoreL)) {
        const float stalk = Fbm(fx * 0.45f, fy * 0.09f, 41), patch = Fbm(fx * 0.05f, fy * 0.06f, 43), rk = VNoise(fx * 0.09f, fy * 0.13f, 51);
        F3 land = Lerp(F3{0.42f, 0.22f, 0.10f}, F3{0.95f, 0.58f, 0.20f}, Sstep(0.25f, 0.70f, stalk));
        land = Lerp(land, F3{1.0f, 0.78f, 0.34f}, Sstep(0.50f, 0.80f, patch) * 0.55f);
        const float edge = std::min(shore, shoreL), stone = Sstep(0.66f, 0.70f, rk) * (1.0f - Sstep(8.0f, 26.0f, fy - edge));   // stones hug the waterline
        land = Lerp(land, Lerp(F3{0.17f, 0.16f, 0.18f}, F3{0.40f, 0.37f, 0.38f}, VNoise(fx * 0.3f, fy * 0.3f, 53)), stone);
        c = Scale(land, 0.72f + 0.38f * (1.0f - fy / fH));
      }
      px[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = 0xFF000000u | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
    }
  return px;
}

// the swatch strip under the picture: seven colours and eight greys
const uint32_t kSwatches[15] = {0xE8E5DA, 0xB0AC96, 0xC8AA6E, 0xBE7869, 0x829670, 0x6E96AA, 0x585A82, 0x000000,
                                0x242424, 0x494949, 0x6D6D6D, 0x929292, 0xB6B6B6, 0xDBDBDB, 0xFFFFFF};

struct Shown {
  std::string key;
  std::vector<uint32_t> px;
  uint32_t strip[15] = {};
};

// Draws the scene under the colours `want`. A panel that already applies `live` applies it to this window as well, so the preview is
// taken through the inverse of `live` (the simulator applies nothing, so there it is left out).
void DrawPreview(Ui& ui, const Rect& box, const ColorSettings& want, const ColorSettings& live, bool undoLive, const Rect& strip) {
  static const std::vector<uint32_t> scene = BuildScene();
  static Shown shown;
  const std::string key = KeyOf(want) + "|" + (undoLive ? KeyOf(live) : std::string("-"));
  if (key != shown.key) {
    shown.key = key;
    const PreviewMap map(want, undoLive ? &live : nullptr);
    shown.px.resize(scene.size());
    for (size_t i = 0; i < scene.size(); i++) {
      int r, g, b;
      map.Map(static_cast<int>((scene[i] >> 16) & 255u), static_cast<int>((scene[i] >> 8) & 255u), static_cast<int>(scene[i] & 255u), &r, &g, &b);
      shown.px[i] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
    for (int i = 0; i < 15; i++) {
      int r, g, b;
      map.Map(static_cast<int>((kSwatches[i] >> 16) & 255u), static_cast<int>((kSwatches[i] >> 8) & 255u), static_cast<int>(kSwatches[i] & 255u), &r, &g, &b);
      shown.strip[i] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
  }
  Gfx& g = ui.gfx;
  g.FillRound(box.Inset(-3), 14, ui.th.inset);
  g.Image(box, shown.px.data(), kSceneW, kSceneH, 11);
  g.StrokeRound(box, 11, WithAlpha(0xFFFFFFFF, 26), 1.0f);
  const float gap = 3, sw = (strip.w - 14 * gap) / 15.0f;
  for (int i = 0; i < 15; i++) g.FillRound(Rect(strip.x + static_cast<float>(i) * (sw + gap), strip.y, sw, strip.h), 3, shown.strip[i]);
}

// ------------------------------------------------------------------------------------------------ profile tiles
struct TileLook {
  bool selected = false, live = false, enabled = true, hover = false;
};

void DrawProfileTile(Ui& ui, const Rect& r, const std::string& id, const TileLook& L, const PreviewMap& map, float descW, float descH) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  if (L.selected) g.FillGrad(r, 12, Mix(th.elev, th.accent, 0.17f), Mix(th.elev, th.accent, 0.05f), 90);
  else g.FillRound(r, 12, Mix(th.elev, th.hover, L.hover && L.enabled ? 0.7f : 0.0f));
  g.StrokeRound(r, 12, L.selected ? th.accent : (L.hover && L.enabled ? Mix(th.line, th.muted, 0.5f) : th.line), L.selected ? 1.5f : 1.0f);
  const uint32_t ink = L.enabled ? th.text : th.faint;
  const uint32_t sub = L.enabled ? th.muted : th.faint;

  float right = r.r() - 12;
  if (L.live) {
    const std::string t = T("Active");
    const float cw = ui.ChipW(t);
    ui.Chip(Rect(right - cw, r.y + 12, cw, 18), t, th.green);
    right -= cw + 8;
  }
  g.Text(Rect(r.x + 14, r.y + 10, std::max(20.0f, right - r.x - 14), 22), ProfileName(id), FontId::Value, ink);
  g.Wrapped(Rect(r.x + 14, r.y + 34, descW, 200), ProfileDesc(id), FontId::Small, sub, true);
  // what the profile does to a few familiar colours
  static const uint32_t kRef[5] = {0xCD9678, 0x6EA05A, 0x5A8CC8, 0x808080, 0xF0F0F0};
  const float sy = r.y + 34 + descH + 8;   // the same line in every tile, whatever the length of the text
  for (int i = 0; i < 5; i++) {
    int pr, pg, pb;
    map.Map(static_cast<int>((kRef[i] >> 16) & 255u), static_cast<int>((kRef[i] >> 8) & 255u), static_cast<int>(kRef[i] & 255u), &pr, &pg, &pb);
    g.FillRound(Rect(r.x + 14 + static_cast<float>(i) * 22, sy, 18, 12), 4, Argb(255, pr, pg, pb));
  }
}

struct StatTile {
  std::string label, value;
};

void DrawStats(Ui& ui, const Rect& row, const std::vector<StatTile>& tiles) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const float gap = 10, w = (row.w - gap * static_cast<float>(tiles.size() - 1)) / static_cast<float>(tiles.size());
  for (size_t i = 0; i < tiles.size(); i++) {
    const Rect r(row.x + static_cast<float>(i) * (w + gap), row.y, w, row.h);
    g.FillRound(r, 10, th.inset);
    g.StrokeRound(r, 10, th.line, 1.0f);
    g.Text(Rect(r.x + 12, r.y + 7, r.w - 24, 16), tiles[i].label, FontId::Tiny, th.faint);
    g.Text(Rect(r.x + 12, r.y + 24, r.w - 24, 22), g.Fit(tiles[i].value, FontId::Value, r.w - 24), FontId::Value, th.text);
    if (g.TextW(tiles[i].value, FontId::Value) > r.w - 24) ui.Tip(r, tiles[i].value);
  }
}

std::string Resolution(App& a) {
  std::string res = a.S("disp.res");
  if (res.empty()) return Dash();
  const size_t x = res.find('x');
  if (x != std::string::npos) res = res.substr(0, x) + " \xC3\x97 " + res.substr(x + 1);
  return res;
}

}  // namespace

float PageDisplay(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Masonry m(r.w, 440, 2);
  const auto& presets = ColorPresets();
  const int nPresets = static_cast<int>(presets.size());
  const bool colorOk = a.CapOk("disp.color");
  const ColorSettings live = LiveColor(a);
  const bool undoLive = !a.simulated();   // the simulator applies nothing to the window's own pixels

  // which profile is active, and the picked-but-not-applied one
  const std::string liveMode = a.S("disp.mode");
  int liveIdx = -1;
  for (int i = 0; i < nPresets; i++)
    if (liveMode == presets[static_cast<size_t>(i)].name) liveIdx = i;
  int& draft = a.Int("dsp.draft", -1);
  if (ColorDraftActive(a)) draft = -1;               // a slider was touched after the tile: the sliders win
  if (draft < 0 || draft >= nPresets || draft == liveIdx) draft = -1;
  const bool changed = draft >= 0;
  const ColorSettings want = Wanted(a, live, draft);

  // =============================================================== the built-in display: preview, facts, backlight, refresh rate
  {
    const float innerW = m.InnerW();
    const float prevH = std::min(230.0f, std::max(140.0f, std::floor(innerW * 0.44f)));
    const float stripH = 14, statH = 52;
    Card k(c, m, T("Built-in display"), T("What you are choosing is drawn here before anything is sent"), IconId::Display,
           prevH + 14 + stripH + 14 + statH + 14 + 2 * kRowSlider);
    const Rect prev = k.Row(prevH);
    k.Gap(14);
    const Rect strip = k.Row(stripH);
    DrawPreview(ui, prev, want, live, undoLive, strip);
    // draft / applied marker on the picture
    const bool differs = !(want == live);
    const std::string tag = differs ? T("DRAFT") : T("APPLIED");
    const float cw = ui.ChipW(tag);
    ui.Chip(Rect(prev.x + 10, prev.y + 10, cw, 18), tag, differs ? th.amber : th.green, WithAlpha(0xFF101010, 190));
    ui.Tip(Rect(prev.x + 10, prev.y + 10, cw, 18),
           differs ? T("The picture shows your draft. The panel keeps its current colours until you press Confirm.") : T("The picture shows the colours the panel uses right now."));
    k.Gap(14);

    std::string refresh = a.Has("disp.refresh") ? std::to_string(a.I("disp.refresh")) + " Hz" : Dash();
    std::string colours = !colorOk ? Dash() : (liveIdx >= 0 ? ProfileName(liveMode) : (liveMode == "custom" ? T("Custom") : Dash()));
    DrawStats(ui, k.Row(statH), {{T("Resolution"), Resolution(a)}, {T("Refresh rate"), refresh}, {T("Colours"), colours}});
    k.Gap(14);

    SliderCfg b;
    b.key = "disp.bright";
    b.title = T("Panel brightness");
    b.desc = T("The backlight of the built-in screen, the same control as the Windows brightness slider.");
    k.Slider(b);
    k.ListConfirm("disp.refresh", T("Refresh rate"), T("Higher is smoother, lower saves battery."), RefreshRates(a), "Hz");
  }

  // =============================================================== image profiles
  {
    const float innerW = m.InnerW();
    const bool across = innerW >= 380;
    const int cols = across ? 2 : 1;
    const int rows = (nPresets + cols - 1) / cols;
    const float gap = 10;
    const float tileW = std::floor((innerW - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
    const float descW = tileW - 28;
    float descH = 0;
    for (const ColorPreset& p : presets) descH = std::max(descH, g.Wrapped(Rect(0, 0, descW, 3000), ProfileDesc(p.name), FontId::Small, 0, false));
    const float tileH = 34 + descH + 8 + 12 + 14;
    const float tilesH = static_cast<float>(rows) * tileH + static_cast<float>(rows - 1) * gap;
    const std::string note = T("These are colour curves of BaseControl's own, applied to the display output. They do not touch the panel's factory calibration, and Standard puts everything back.");
    const float noteH = NoteHeight(c, m, note);
    Card k(c, m, T("Image profile"), T("A starting point for the colour controls"), IconId::Sliders, tilesH + 14 + 36 + 8 + noteH);

    static std::string mapKey;
    static std::vector<PreviewMap> maps;
    const std::string mk = KeyOf(live) + (undoLive ? "u" : "-");
    if (mk != mapKey || maps.size() != presets.size()) {
      mapKey = mk;
      maps.clear();
      for (const ColorPreset& p : presets) maps.emplace_back(p.c, undoLive ? &live : nullptr);
    }
    const bool pending = a.Pending("disp.mode");
    const bool enabled = colorOk && !pending;
    const Rect tiles = k.Row(tilesH);
    for (int i = 0; i < nPresets; i++) {
      const int cx = i % cols, cy = i / cols;
      const Rect tr(tiles.x + static_cast<float>(cx) * (tileW + gap), tiles.y + static_cast<float>(cy) * (tileH + gap), tileW, tileH);
      TileLook L;
      L.selected = (changed ? draft : liveIdx) == i;
      L.live = liveIdx == i;
      L.enabled = enabled;
      bool hover = false;
      const bool click = ui.Click("dp#" + std::to_string(i), tr, enabled, &hover);
      L.hover = hover;
      DrawProfileTile(ui, tr, presets[static_cast<size_t>(i)].name, L, maps[static_cast<size_t>(i)], descW, descH);
      if (click) {
        for (const char* key : kColorKeys) a.DiscardDraft(key);   // the profile sets every colour, so slider drafts would only be stale
        draft = i == liveIdx ? -1 : i;
      }
    }
    k.Gap(14);

    const Rect row = k.Row(36);
    const float btnW = 112, discW = 36;
    const Rect confirm(row.r() - btnW, row.y, btnW, 34);
    const Rect discard(confirm.x - discW - 8, row.y, discW, 34);
    std::string text;
    uint32_t col = th.muted;
    if (!colorOk) {
      text = a.CapWhy("disp.color").empty() ? T("Not available on this system.") : a.CapWhy("disp.color");
      col = th.amber;
    } else if (pending) {
      text = T("Applying\xE2\x80\xA6");
    } else if (changed) {
      text = Tf("Press Confirm to switch to %s.", ProfileName(presets[static_cast<size_t>(draft)].name).c_str());
    } else if (a.RecentOk("disp.mode") < 3.0) {
      text = T("Applied.");
      col = th.green;
    } else if (liveIdx >= 0) {
      text = Tf("Active now: %s", ProfileName(liveMode).c_str());
    } else if (liveMode == "custom") {
      text = T("Custom colours are active.");
    } else {
      text = T("No profile has been chosen yet.");
    }
    const float tw = (changed ? discard.x : confirm.x) - row.x - 14;
    g.Text(Rect(row.x, row.y, tw, row.h), text, FontId::Small, col);
    if (g.TextW(text, FontId::Small) > tw) ui.Tip(Rect(row.x, row.y, tw, row.h), text);
    if (changed && ui.IconButton("dc#disp.mode", discard, IconId::Undo, true)) draft = -1;
    if (changed) ui.Tip(discard, T("Discard the choice"));
    const bool clicked = ui.Button("ok#disp.mode", confirm, pending ? T("Applying") : T("Confirm"), Btn::Primary, pending ? IconId::None : IconId::Check, enabled && changed);
    if (pending) ui.Spinner(Rect(confirm.x + 10, confirm.cy() - 8, 16, 16), th.muted);
    ui.Tip(confirm, changed ? T("Apply this profile to the display now. Nothing is sent until you press this.") : T("Pick a profile, then press Confirm. Nothing is sent until then."));
    if (clicked && draft >= 0) {
      const std::string id = presets[static_cast<size_t>(draft)].name;
      a.Set("disp.mode", id, Tf("Image profile \xE2\x86\x92 %s", ProfileName(id).c_str()), KV(), [&a](const Result& res) {
        if (res.ok)
          for (const char* key : kColorKeys) a.DiscardDraft(key);
      });
    }
    k.Gap(8);
    k.Note(note);
  }

  // =============================================================== colour balance
  {
    Card k(c, m, T("Colour balance"), T("Temperature and the three colour channels"), IconId::Sun, 4 * kRowSlider);
    SliderCfg t;
    t.key = "disp.c.kelvin";
    t.title = T("Colour temperature");
    t.desc = T("Lower is warmer, higher is cooler. 6500 K is neutral.");
    t.unit = "K";
    k.Slider(t);
    const struct { const char* key; const char* title; const char* desc; } ch[3] = {
        {"disp.c.r", N_("Red"), N_("Takes some red out of the picture. 100 % leaves the channel alone.")},
        {"disp.c.g", N_("Green"), N_("Takes some green out of the picture. 100 % leaves the channel alone.")},
        {"disp.c.b", N_("Blue"), N_("Takes some blue out of the picture. 100 % leaves the channel alone.")}};
    for (const auto& e : ch) {
      SliderCfg s;
      s.key = e.key;
      s.title = T(e.title);
      s.desc = T(e.desc);
      k.Slider(s);
    }
  }

  // =============================================================== tone
  {
    Card k(c, m, T("Tone"), T("Saturation, gamma, brightness and contrast"), IconId::Display, 4 * kRowSlider + kRowButtons);
    SliderCfg s;
    s.key = "disp.c.sat";
    s.title = T("Saturation");
    s.desc = T("How strongly colours show. 100 % is neutral, 0 % is black and white.");
    k.Slider(s);
    SliderCfg ga;
    ga.key = "disp.c.gamma";
    ga.title = T("Gamma");
    ga.desc = T("Lower brightens the mid-tones, higher darkens them. 1.00 is neutral.");
    ga.fmt = [](int v) { return Num(v / 100.0, 2); };
    k.Slider(ga);
    SliderCfg br;
    br.key = "disp.c.bright";
    br.title = T("Image brightness");
    br.desc = T("Lifts or lowers the whole picture in software. 50 is neutral. For the backlight use Panel brightness.");
    k.Slider(br);
    SliderCfg co;
    co.key = "disp.c.contrast";
    co.title = T("Contrast");
    co.desc = T("Spreads the dark and light tones apart. 50 is neutral.");
    k.Slider(co);
    const bool neutral = live.Neutral() && liveMode != "custom";
    if (k.Button("dsp#reset", T("Reset colours to standard"), Btn::Ghost, IconId::Undo, colorOk && (!neutral || ColorDraftActive(a)) && !a.Pending("disp.mode"))) {
      for (const char* key : kColorKeys) a.DiscardDraft(key);
      draft = -1;
      a.Set("disp.mode", "standard", T("Colours reset to standard"));
    }
  }

  // =============================================================== calibration
  {
    const std::string n1 = T("Some laptops ship with a factory colour profile measured for the panel. The original Control Center downloads it from the manufacturer's server, using the serial number stored in the BIOS. BaseControl does not contact that server and does not include such a profile.");
    const std::string n2 = T("If you have the profile file (.icc or .icm), Windows applies it by itself: open colour management, choose the built-in display, add the profile and make it the default. It works together with the colours above.");
    const float h = NoteHeight(c, m, n1, true) + NoteHeight(c, m, n2, true) + kRowButtons;
    Card k(c, m, T("Colour calibration"), T("Factory profiles and the Windows colour tools"), IconId::Eye, h, true);
    k.Note(n1);
    k.Note(n2);
    const Rect row = k.Row(kRowButtons);
    struct B { const char* id; const char* tool; std::string label; Btn style; IconId icon; };
    const B bs[3] = {{"dsp#cm", "colorcpl", T("Open colour management"), Btn::Normal, IconId::External},
                     {"dsp#ds", "display", T("Display settings"), Btn::Ghost, IconId::External},
                     {"dsp#nl", "nightlight", T("Night light"), Btn::Ghost, IconId::Moon}};
    float x = row.x;
    for (const B& b : bs) {
      const float w = std::max(110.0f, g.TextW(b.label, FontId::Label) + 58.0f);
      if (x + w > row.r() && x > row.x) break;
      if (ui.Button(b.id, Rect(x, row.y + 5, w, 36), b.label, b.style, b.icon, true)) a.OpenWindowsTool(b.tool);
      x += w + 10;
    }
  }
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
