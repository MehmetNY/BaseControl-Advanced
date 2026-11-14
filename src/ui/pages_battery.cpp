// Battery: the charging profile (picked on a tile, sent only when you press Confirm), what the battery is doing right now, its health,
// and the last eight hours of charge as a chart. The history is recorded by the controller (one sample a minute), so it keeps filling
// while this window is closed.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../common/settings_reg.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {

std::string BattModeLabel(const std::string& m) {
  if (m == "highcap") return T("Full capacity");
  if (m == "balanced") return T("Balanced");
  if (m == "eco") return T("Eco");
  return m;
}

namespace {

const char* const kModeKeys[3] = {"highcap", "balanced", "eco"};
const IconId kModeIcons[3] = {IconId::Bolt, IconId::Battery, IconId::Leaf};

std::string ModeDesc(int i) {
  switch (i) {
    case 0: return T("The most usable capacity and quick charging. Best if you often run on battery.");
    case 1: return T("Charges for a balance between runtime and battery life.");
    default: return T("The gentlest on the battery. Made for laptops that stay on the charger most of the time.");
  }
}

std::string Dash() { return "\xE2\x80\x94"; }

// "2 h 05 min", "45 min"
std::string Hm(long long secs) {
  if (secs < 0) return Dash();
  const long long mins = (secs + 30) / 60;
  if (mins < 60) return Tf("%lld min", mins);
  return Tf("%lld h %02lld min", mins / 60, mins % 60);
}

// ------------------------------------------------------------------------------------------------ charge profile tiles
struct TileLook {
  bool selected = false, live = false, enabled = true, hover = false, compact = false;
};

void DrawTile(Ui& ui, const Rect& r, int i, const TileLook& L, const std::string& title, const std::string& desc, float descW) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  if (L.selected) g.FillGrad(r, 12, Mix(th.elev, th.accent, 0.17f), Mix(th.elev, th.accent, 0.05f), 90);
  else g.FillRound(r, 12, Mix(th.elev, th.hover, L.hover && L.enabled ? 0.7f : 0.0f));
  g.StrokeRound(r, 12, L.selected ? th.accent : (L.hover && L.enabled ? Mix(th.line, th.muted, 0.5f) : th.line), L.selected ? 1.5f : 1.0f);

  const uint32_t ink = L.enabled ? th.text : th.faint;
  const uint32_t sub = L.enabled ? th.muted : th.faint;
  const Rect box(r.x + 16, r.y + 16, 36, 36);
  g.FillRound(box, 10, L.selected ? WithAlpha(th.accent, 48) : th.inset);
  g.Icon(kModeIcons[i], box.Inset(7), L.selected && L.enabled ? th.accent : sub, 1.8f);

  const V2 rc{r.r() - 26, r.y + 34};
  g.Circle(rc, 9, L.selected ? th.accent : th.faint, 1.6f);
  if (L.selected) g.Dot(rc, 4.6f, th.accent);

  if (L.live) {
    const std::string t = T("Active");
    const float cw = ui.ChipW(t);
    ui.Chip(Rect(rc.x - 20 - cw, r.y + 25, cw, 18), t, th.green);
  }
  if (L.compact) {
    const float tx = box.r() + 14;
    g.Text(Rect(tx, r.y + 14, r.w - (tx - r.x) - 70, 24), title, FontId::H3, ink);
    g.Wrapped(Rect(tx, r.y + 40, descW, 200), desc, FontId::Small, sub, true);
  } else {
    g.Text(Rect(r.x + 16, box.b() + 12, r.w - 32, 24), title, FontId::H3, ink);
    g.Wrapped(Rect(r.x + 16, box.b() + 40, descW, 200), desc, FontId::Small, sub, true);
  }
}

// ------------------------------------------------------------------------------------------------ history
struct Sample {
  int pct = -1;      // -1: no reading (the machine slept)
  bool ac = false;   // the charger was connected
};

// "a,b,c" from the controller: charge in percent, +200 while the charger was connected, -1 for a missing minute.
const std::vector<Sample>& ParseHistory(const std::string& text) {
  static std::string last;
  static std::vector<Sample> cache;
  static bool init = false;
  if (init && text == last) return cache;
  init = true;
  last = text;
  cache.clear();
  size_t i = 0;
  while (!text.empty() && i <= text.size()) {
    size_t j = text.find(',', i);
    if (j == std::string::npos) j = text.size();
    const long v = std::strtol(text.substr(i, j - i).c_str(), nullptr, 10);
    Sample s;
    if (v >= 0) {
      s.ac = v >= 200;
      s.pct = static_cast<int>(std::min<long>(100, v % 200));
    }
    cache.push_back(s);
    i = j + 1;
  }
  return cache;
}

constexpr int kWindowSamples = 480;   // 8 hours at one sample a minute: the width of the chart

void DrawHistory(Ui& ui, const Rect& r, const std::vector<Sample>& h, int stepSec, bool haveBattery) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const Rect plot(r.x + 42, r.y + 8, r.w - 52, r.h - 8 - 24);
  for (int i = 0; i <= 4; i++) {
    const float y = plot.b() - plot.h * static_cast<float>(i) / 4.0f;
    g.Line({plot.x, y}, {plot.r(), y}, WithAlpha(th.line, i == 0 ? 210 : 110), 1);
    g.Text(Rect(r.x, y - 8, 36, 16), Num(i * 25) + " %", FontId::Tiny, th.faint, Align::Right);
  }
  const long windowSec = static_cast<long>(kWindowSamples) * std::max(1, stepSec);
  const int hours = static_cast<int>(std::lround(windowSec / 3600.0));
  for (int i = 0; i <= 4; i++) {
    const float x = plot.x + plot.w * static_cast<float>(i) / 4.0f;
    if (i > 0 && i < 4) g.Line({x, plot.y}, {x, plot.b()}, WithAlpha(th.line, 70), 1);
    const int back = static_cast<int>(std::lround(hours * (4 - i) / 4.0));
    const std::string t = back == 0 ? T("Now") : Tf("\xE2\x88\x92%d h", back);
    const Align al = i == 0 ? Align::Left : (i == 4 ? Align::Right : Align::Center);
    const float bw = 60;
    const float bx = i == 0 ? x - 4 : (i == 4 ? x - bw + 4 : x - bw / 2);
    g.Text(Rect(bx, plot.b() + 5, bw, 16), t, FontId::Tiny, th.faint, al);
  }

  const int n = static_cast<int>(h.size());
  int valid = 0;
  for (const Sample& s : h)
    if (s.pct >= 0) valid++;
  if (valid < 2) {
    const std::string msg = !haveBattery ? T("No battery was found on this computer.") : T("Collecting data. The chart fills in as BaseControl keeps running: one point a minute.");
    if (g.TextW(msg, FontId::Small) <= plot.w - 48) g.Text(Rect(plot.x, plot.cy() - 12, plot.w, 24), msg, FontId::Small, th.faint, Align::Center);
    else g.Wrapped(Rect(plot.x + 24, plot.cy() - 18, plot.w - 48, 60), msg, FontId::Small, th.faint, true);
    return;
  }
  auto X = [&](int i) { return plot.r() - plot.w * static_cast<float>(n - 1 - i) / static_cast<float>(kWindowSamples - 1); };
  auto Y = [&](float pct) { return plot.b() - plot.h * pct / 100.0f; };
  auto colorOf = [&](bool ac) { return ac ? th.green : th.accent; };

  ui.Mark("batt.chart", plot);
  // The line is drawn from a light moving average over each unbroken stretch (whole percentages would show as a staircase); the readout
  // under the mouse always quotes the recorded value.
  std::vector<float> smooth(static_cast<size_t>(n), 0.0f);
  for (int i = 0; i < n; i++) {
    if (h[static_cast<size_t>(i)].pct < 0) continue;
    float sum = 0;
    int cnt = 0;
    for (int d = -4; d <= 4; d++) {
      const int j = i + d;
      if (j < 0 || j >= n) continue;
      const Sample& sj = h[static_cast<size_t>(j)];
      if (sj.pct < 0 || sj.ac != h[static_cast<size_t>(i)].ac) continue;
      bool whole = true;   // every sample between i and j must belong to the same stretch
      for (int q = std::min(i, j); q <= std::max(i, j) && whole; q++) whole = h[static_cast<size_t>(q)].pct >= 0 && h[static_cast<size_t>(q)].ac == h[static_cast<size_t>(i)].ac;
      if (!whole) continue;
      sum += static_cast<float>(sj.pct);
      cnt++;
    }
    smooth[static_cast<size_t>(i)] = cnt ? sum / static_cast<float>(cnt) : static_cast<float>(h[static_cast<size_t>(i)].pct);
  }
  g.PushClip(Rect(plot.x - 2, plot.y - 4, plot.w + 4, plot.h + 8));
  std::vector<V2> pts;
  bool cur = false;
  auto flush = [&]() {
    if (pts.size() >= 2) {
      std::vector<V2> poly = pts;
      poly.push_back({pts.back().x, plot.b()});
      poly.push_back({pts.front().x, plot.b()});
      g.PolyFill(poly, WithAlpha(colorOf(cur), 34));
      g.Poly(pts, colorOf(cur), 2.0f);
    } else if (pts.size() == 1) {
      g.Dot(pts[0], 2.2f, colorOf(cur));
    }
    pts.clear();
  };
  for (int i = 0; i < n; i++) {
    const Sample& s = h[static_cast<size_t>(i)];
    if (s.pct < 0) {
      flush();   // a gap: the line does not cross the minutes nothing was recorded
      continue;
    }
    const V2 p{X(i), Y(smooth[static_cast<size_t>(i)])};
    if (pts.empty()) {
      cur = s.ac;
      pts.push_back(p);
    } else if (s.ac != cur) {
      pts.push_back(p);   // the point where the charger came or went belongs to both runs, so the line stays whole
      flush();
      cur = s.ac;
      pts.push_back(p);
    } else {
      pts.push_back(p);
    }
  }
  flush();

  // hover: a hairline, the point under it and what it says
  if (ui.Hover(plot) && !ui.ModalOpen()) {
    const V2 mp = ui.LocalMouse();
    const int slot = static_cast<int>(std::lround((mp.x - plot.x) / plot.w * static_cast<float>(kWindowSamples - 1)));
    const int i = slot - (kWindowSamples - n);
    if (i >= 0 && i < n) {
      const Sample& s = h[static_cast<size_t>(i)];
      const float x = X(i);
      g.Line({x, plot.y}, {x, plot.b()}, WithAlpha(th.muted, 110), 1);
      std::string what;
      if (s.pct >= 0) {
        g.Dot({x, Y(smooth[static_cast<size_t>(i)])}, 4.2f, colorOf(s.ac));
        g.Circle({x, Y(smooth[static_cast<size_t>(i)])}, 4.2f, th.panel, 1.4f);
        what = Tf("%d %%", s.pct) + (s.ac ? "  \xC2\xB7  " + T("Plugged in") : "  \xC2\xB7  " + T("On battery"));
      } else {
        what = T("No reading (the computer was asleep)");
      }
      const long ago = static_cast<long>(n - 1 - i) * std::max(1, stepSec);
      const std::string when = ago < 60 ? T("Now") : Tf("%s ago", Hm(ago).c_str());
      const std::string text = when + "   " + what;
      const float tw = g.TextW(text, FontId::Small) + 22;
      const float bx = x + 12 + tw > plot.r() ? x - 12 - tw : x + 12;
      const Rect box(bx, plot.y + 6, tw, 26);
      g.FillRound(box, 8, WithAlpha(th.base, 235));
      g.StrokeRound(box, 8, th.line, 1);
      g.Text(box.Inset(11, 0), text, FontId::Small, th.text);
    }
  }
  g.PopClip();
}

// ------------------------------------------------------------------------------------------------ live readings
struct Now {
  bool have = false;   // the OS reports a battery
  int pct = 0;
  bool ac = true, charging = false;
  int secs = -1;       // the OS's estimate while running on the battery
  // from the embedded controller (absent without the EC link)
  bool haveEc = false;
  double design = 0, full = 0, cycles = -1, volt = 0, rate = 0, temp = -1000;
  bool haveHealth = false, haveRate = false;
};

Now Read(App& a) {
  Now n;
  n.have = a.Has("power.batt.pct");
  n.pct = n.have ? static_cast<int>(std::lround(a.N("power.batt.pct"))) : (a.Has("batt.ec.pct") ? static_cast<int>(a.N("batt.ec.pct")) : 0);
  n.have = n.have || a.Has("batt.ec.pct");
  n.ac = a.Has("power.ac") ? a.B("power.ac") : true;
  n.charging = a.B("power.charging");
  n.secs = a.Has("power.batt.secs") ? static_cast<int>(a.N("power.batt.secs")) : -1;
  if (a.Has("batt.design") && a.Has("batt.full") && a.N("batt.design") > 0) {
    n.haveHealth = true;
    n.design = a.N("batt.design");
    n.full = a.N("batt.full");
  }
  if (a.Has("batt.cycles")) n.cycles = a.N("batt.cycles");
  if (a.Has("batt.volt")) n.volt = a.N("batt.volt");
  if (a.Has("batt.rate")) {
    n.haveRate = true;
    n.rate = a.N("batt.rate");
  }
  if (a.Has("batt.temp")) n.temp = a.N("batt.temp");
  return n;
}

}  // namespace

float PageBattery(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Masonry m(r.w, 440, 2);
  const SettingDef* def = FindSetting("batt.mode");
  const Now n = Read(a);

  // =============================================================== charge profile
  if (def) {
    const bool capOk = a.CapOk(def->cap);
    const bool pending = a.Pending("batt.mode");
    const int liveIdx = a.Has("batt.mode") ? EnumIndex(*def, a.S("batt.mode")) : -1;
    int& draft = a.Int("bt.draft", -1);
    if (draft < 0 || draft > 2 || draft == liveIdx) draft = -1;
    const bool changed = draft >= 0;
    const int shown = changed ? draft : liveIdx;

    const float innerW = m.InnerW(true);
    const bool across = innerW >= 560;
    const float gap = 12;
    const float tileW = across ? std::floor((innerW - 2 * gap) / 3.0f) : innerW;
    const float descW = across ? tileW - 32 : innerW - 16 - 36 - 14 - 16;
    float descH = 0;
    for (int i = 0; i < 3; i++) descH = std::max(descH, g.Wrapped(Rect(0, 0, descW, 3000), ModeDesc(i), FontId::Small, 0, false));
    const float tileH = across ? 16 + 36 + 12 + 24 + 4 + descH + 18 : std::max(72.0f, 14 + 26 + descH + 16);
    const float tilesH = across ? tileH : 3 * tileH + 2 * 10;
    const std::string note = T("The laptop's embedded controller applies the limit, so the exact charge levels are decided by its firmware.");
    const float noteH = NoteHeight(c, m, note, true);
    Card k(c, m, T("Charge profile"), T("How far the battery is charged when the charger stays connected"), IconId::Battery, tilesH + 14 + 36 + 8 + noteH, true);
    if (def->ev != Ev::Os) {
      const std::string ev = EvLabel(def->ev);
      const float cw = ui.ChipW(ev);
      const Rect chip(k.box.r() - kPad - cw, k.box.y + kPad + 8, cw, 18);
      ui.Chip(chip, ev, EvColor(th, def->ev));
      ui.Tip(chip, EvTip(def->ev));
    }
    const Rect tiles = k.Row(tilesH);
    for (int i = 0; i < 3; i++) {
      const Rect tr = across ? Rect(tiles.x + i * (tileW + gap), tiles.y, tileW, tileH) : Rect(tiles.x, tiles.y + i * (tileH + 10), tileW, tileH);
      TileLook L;
      L.selected = shown == i;
      L.live = liveIdx == i;
      L.enabled = capOk && !pending;
      L.compact = !across;
      bool hover = false;
      const bool click = ui.Click("bt#" + std::to_string(i), tr, L.enabled, &hover);
      L.hover = hover;
      DrawTile(ui, tr, i, L, BattModeLabel(kModeKeys[i]), ModeDesc(i), descW);
      if (click) draft = i == liveIdx ? -1 : i;
    }
    k.Gap(14);

    // status line + Confirm
    const Rect row = k.Row(36);
    const float btnW = 112, discW = 36;
    const Rect confirm(row.r() - btnW, row.y, btnW, 34);
    const Rect discard(confirm.x - discW - 8, row.y, discW, 34);
    std::string text;
    uint32_t col = th.muted;
    if (!capOk) {
      text = a.CapWhy(def->cap).empty() ? T("Not available on this system.") : a.CapWhy(def->cap);
      col = th.amber;
    } else if (pending) {
      text = T("Applying\xE2\x80\xA6");
    } else if (changed) {
      text = Tf("Press Confirm to switch to %s. Nothing is sent until then.", BattModeLabel(kModeKeys[draft]).c_str());
    } else if (a.RecentOk("batt.mode") < 3.0) {
      text = T("Applied and verified by reading the value back.");
      col = th.green;
    } else if (liveIdx >= 0) {
      text = Tf("Active now: %s", BattModeLabel(kModeKeys[liveIdx]).c_str());
    } else {
      text = T("The controller did not report a charge profile.");
    }
    const float tw = (changed ? discard.x : confirm.x) - row.x - 14;
    if (liveIdx >= 0 && !changed && col == th.muted) g.Dot({row.x + 4, row.cy()}, 3.2f, th.green);
    const float tx = liveIdx >= 0 && !changed && col == th.muted ? row.x + 16 : row.x;
    g.Text(Rect(tx, row.y, tw - (tx - row.x), row.h), text, FontId::Small, col);
    if (g.TextW(text, FontId::Small) > tw - (tx - row.x)) ui.Tip(Rect(tx, row.y, tw - (tx - row.x), row.h), text);
    if (changed && ui.IconButton("dc#batt.mode", discard, IconId::Undo, true)) draft = -1;
    ui.Tip(discard, T("Discard the choice"));
    const bool clicked = ui.Button("ok#batt.mode", confirm, pending ? T("Applying") : T("Confirm"), Btn::Primary, pending ? IconId::None : IconId::Check, capOk && changed && !pending);
    if (pending) ui.Spinner(Rect(confirm.x + 10, confirm.cy() - 8, 16, 16), th.muted);
    ui.Tip(confirm, changed ? T("Send the chosen profile to the hardware now. Nothing is sent until you press this.") : T("Pick a profile, then press Confirm. Nothing is sent until then."));
    if (clicked && draft >= 0) a.Set("batt.mode", EnumName(*def, draft), T("Charge profile") + " \xE2\x86\x92 " + BattModeLabel(kModeKeys[draft]));
    k.Gap(8);
    k.Note(note);
  }

  // =============================================================== right now
  {
    struct Line {
      std::string label, value;
      uint32_t col = 0;
      std::string tip;
    };
    std::vector<Line> rows;
    const std::string stateText = !n.ac ? T("Discharging") : (n.charging ? T("Charging") : T("Not charging"));
    rows.push_back({T("Power source"), n.ac ? T("AC adapter") : T("Battery"), n.ac ? 0u : th.amber, ""});
    rows.push_back({T("Charge state"), stateText, 0u,
                    n.ac && !n.charging ? T("The charger is connected but the battery is not taking charge: it is full, or the charge profile stops here.") : ""});
    // the time: the OS's estimate when unplugged, otherwise (or when it has none yet) an estimate from the battery's own current
    const double amps = std::fabs(n.rate);
    if (!n.ac) {
      long long secs = n.secs;
      std::string est;
      if (secs < 0 && n.haveHealth && n.haveRate && amps > 50) {
        secs = static_cast<long long>(3600.0 * (n.pct / 100.0 * n.full) / amps);
        est = T("An estimate from the current draw.");
      }
      rows.push_back({T("Time left"), secs >= 0 ? Hm(secs) : Dash(), 0u, est});
    } else if (n.charging) {
      long long secs = -1;
      if (n.haveHealth && n.haveRate && amps > 50) secs = static_cast<long long>(3600.0 * ((100 - n.pct) / 100.0 * n.full) / amps);
      rows.push_back({T("Until full"), secs >= 0 ? Hm(secs) : Dash(), 0u, secs >= 0 ? T("An estimate from the present charging current. It gets slower near the top.") : ""});
    } else {
      rows.push_back({T("Time left"), Dash(), 0u, ""});
    }
    if (n.volt > 0) rows.push_back({T("Voltage"), Fmt("%.2f V", n.volt / 1000.0), 0u, ""});
    if (n.volt > 0 && n.haveRate && amps > 20) rows.push_back({n.ac ? T("Charging power") : T("Discharge power"), Fmt("%.1f W", amps * n.volt / 1.0e6), 0u, ""});
    if (n.temp > -100) rows.push_back({T("Battery temperature"), Fmt("%.1f \xC2\xB0""C", n.temp), n.temp >= 45 ? th.amber : 0u, ""});

    Card k(c, m, T("Battery"), T("Right now"), IconId::Plug, 58 + static_cast<float>(rows.size()) * kRowInfo);
    const Rect top = k.Row(44);
    const std::string big = n.have ? std::to_string(n.pct) : Dash();
    const float bw = g.TextW(big, FontId::Mega);
    g.Text(Rect(top.x, top.y, bw + 4, 42), big, FontId::Mega, th.text);
    g.Text(Rect(top.x + bw + 6, top.y + 14, 30, 24), "%", FontId::Value, th.muted);
    if (n.have) {
      const std::string chipText = !n.ac ? T("On battery") : (n.charging ? T("Charging") : T("Plugged in"));
      const uint32_t chipCol = !n.ac ? th.amber : (n.charging ? th.green : th.blue);
      const float cw = ui.ChipW(chipText);
      ui.Chip(Rect(top.r() - cw, top.y + 12, cw, 22), chipText, chipCol);
    }
    const Rect bar = k.Row(12);
    const uint32_t bc2 = n.pct < 20 ? th.red : (n.pct < 40 ? th.amber : th.green);
    ui.Bar(Rect(bar.x, bar.y + 2, bar.w, 8), n.have ? static_cast<float>(n.pct) / 100.0f : 0.0f, bc2, 4);
    k.Gap(2);
    for (const Line& l : rows) k.Info(l.label, l.value, l.col, l.tip);
  }

  // =============================================================== health
  {
    const double health = n.haveHealth ? 100.0 * n.full / n.design : 0.0;
    std::vector<std::pair<std::string, std::string>> rows;
    if (n.cycles >= 0) rows.push_back({T("Charge cycles"), Num(n.cycles)});
    if (n.haveHealth) {
      rows.push_back({T("Design capacity"), Num(n.design) + " mAh"});
      rows.push_back({T("Full-charge capacity"), Num(n.full) + " mAh"});
    }
    const std::string missing = a.S("ec.link") != "connected" ? T("Battery health is read from the laptop's embedded controller, which is not reachable right now.")
                                                              : T("The battery did not report its capacity.");
    const float noteH = n.haveHealth ? 0.0f : NoteHeight(c, m, missing);
    Card k(c, m, T("Battery health"), T("How much of its original capacity it still holds"), IconId::Shield,
           (n.haveHealth ? 58 : 0) + static_cast<float>(rows.size()) * kRowInfo + noteH);
    if (n.haveHealth) {
      const Rect top = k.Row(44);
      const std::string big = Num(std::min(100.0, health));
      const float bw = g.TextW(big, FontId::Mega);
      g.Text(Rect(top.x, top.y, bw + 4, 42), big, FontId::Mega, th.text);
      g.Text(Rect(top.x + bw + 6, top.y + 14, 30, 24), "%", FontId::Value, th.muted);
      const uint32_t col = health >= 85 ? th.green : (health >= 70 ? th.amber : th.red);
      const std::string verdict = health >= 85 ? T("Good") : (health >= 70 ? T("Fair") : T("Worn"));
      const float cw = ui.ChipW(verdict);
      const Rect chip(top.r() - cw, top.y + 12, cw, 22);
      ui.Chip(chip, verdict, col);
      ui.Tip(chip, T("A rule of thumb: 85 % and up is good, 70 % to 85 % is fair, below that the battery is worn."));
      const Rect bar = k.Row(12);
      ui.Bar(Rect(bar.x, bar.y + 2, bar.w, 8), static_cast<float>(std::min(100.0, health) / 100.0), col, 4);
      k.Gap(2);
    } else {
      k.Note(missing, th.amber);
    }
    for (auto& l : rows) k.Info(l.first, l.second);
  }

  // =============================================================== history
  {
    const std::string note = T("The service records one point a minute while it runs. The history starts again when the service restarts.");
    const float chartH = 214;
    const float legendH = 28;
    const float noteH = NoteHeight(c, m, note, true);
    Card k(c, m, T("Charge history"), T("Battery level over the last 8 hours"), IconId::Performance, chartH + legendH + noteH, true);
    const std::vector<Sample>& h = ParseHistory(a.S("batt.hist"));
    const int step = a.Has("batt.hist.step") ? static_cast<int>(a.N("batt.hist.step")) : 60;
    DrawHistory(ui, Rect(k.x0, k.y, k.w, chartH), h, step, n.have);
    k.Gap(chartH + 6);
    const Rect lg = k.Row(legendH - 6);
    float x = lg.x + 42;
    for (int i = 0; i < 2; i++) {
      const std::string t = i == 0 ? T("On battery") : T("Plugged in");
      g.FillRound(Rect(x, lg.cy() - 2, 16, 4), 2, i == 0 ? th.accent : th.green);
      g.Text(Rect(x + 24, lg.y, 160, lg.h), t, FontId::Small, th.muted);
      x += 24 + g.TextW(t, FontId::Small) + 26;
    }
    k.Gap(6);
    k.Note(note);
  }
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
