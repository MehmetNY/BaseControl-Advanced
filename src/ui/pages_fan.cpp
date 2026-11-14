// "Fan settings": the live fan readings, the temperature -> fan speed curves (draggable points that are drafted first and sent
// only when you press Apply), the cooling mode, a short history and the experimental fixed-speed controls.
#include <algorithm>
#include <climits>
#include <cmath>

#include "../common/fan.h"
#include "../common/util.h"
#include "page.h"

namespace bc {
namespace ui {
namespace {

constexpr float kTmin = 30.0f, kTmax = 105.0f;   // temperature axis of the editor
constexpr float kSideW = 380.0f;                 // right-hand column on a wide window
constexpr float kLiveH = 150.0f;
constexpr float kPlotH = 300.0f;
constexpr float kBoxH = 46.0f;

// ---------------------------------------------------------------------------------------------------------------- model
struct Plot {
  Rect r;
  float X(float t) const { return r.x + r.w * (t - kTmin) / (kTmax - kTmin); }
  float Y(float p) const { return r.b() - r.h * p / 100.0f; }
  float T(float x) const { return kTmin + (x - r.x) / r.w * (kTmax - kTmin); }
  float P(float y) const { return (r.b() - y) / r.h * 100.0f; }
};

bool Linked(App& a) { return a.PrefB("fan.link", false); }

std::string ModeName(const std::string& m) {
  if (m == "auto") return T("Auto");
  if (m == "boost") return T("Boost");
  if (m == "custom") return T("Custom curve");
  if (m == "fixed") return T("Fixed speed");
  return m;
}

// What the hardware uses now: the table the EC holds in Custom mode, otherwise the curve BaseControl will write when Custom is
// switched on (or the built-in default before anything was stored).
fan::Curve AppliedCurve(App& a, bool cpu) {
  fan::Curve c;
  if (a.S("fan.mode") == "custom" && fan::FromText(a.S(cpu ? "fan.cpu.curve" : "fan.gpu.curve"), &c) && fan::Validate(c, nullptr)) return c;
  if (fan::FromText(a.S(cpu ? "cfg.fan.cpu" : "cfg.fan.gpu"), &c) && fan::Validate(c, nullptr)) return c;
  return cpu ? fan::DefaultCpu() : fan::DefaultGpu();
}

struct View {
  fan::Curve applied, shown;
  bool dirty = false;
};

// `shown` is the draft while one exists, otherwise the applied curve (so the chart follows changes made elsewhere).
View ViewOf(App& a, bool cpu) {
  View v;
  v.applied = AppliedCurve(a, cpu);
  std::string& d = a.Str(cpu ? "fan.d.cpu" : "fan.d.gpu");
  fan::Curve dc;
  if (!d.empty() && fan::FromText(d, &dc) && fan::Validate(dc, nullptr) && fan::ToText(dc) != fan::ToText(v.applied)) {
    v.shown = dc;
    v.dirty = true;
  } else {
    d.clear();
    v.shown = v.applied;
  }
  return v;
}

void Commit(App& a, bool cpu, const fan::Curve& c) {
  const std::string t = fan::ToText(c);
  if (Linked(a)) {
    a.Str("fan.d.cpu") = t;
    a.Str("fan.d.gpu") = t;
  } else {
    a.Str(cpu ? "fan.d.cpu" : "fan.d.gpu") = t;
  }
}

struct Live {
  double temp = -1, duty = -1;
  int rpm = -1;
};

Live LiveOf(App& a, bool cpu) {
  Live l;
  const char* t1 = cpu ? "cpu.temp.ec" : "gpu.temp.ec";
  const char* t2 = cpu ? "cpu.temp" : "gpu.temp";
  if (a.Has(t1)) l.temp = a.N(t1);
  else if (a.Has(t2)) l.temp = a.N(t2);
  if (a.Has(cpu ? "fan.cpu.duty" : "fan.gpu.duty")) l.duty = a.N(cpu ? "fan.cpu.duty" : "fan.gpu.duty");
  if (a.Has(cpu ? "fan.cpu.rpm" : "fan.gpu.rpm")) l.rpm = static_cast<int>(std::lround(a.N(cpu ? "fan.cpu.rpm" : "fan.gpu.rpm")));
  return l;
}

int BoxRows(float innerW, int n) {
  const int per = std::max(1, std::min(n, 10));
  return (innerW - (per - 1) * 6.0f) / per >= 52.0f ? 1 : 2;
}

// Group of cards placed with a horizontal shift (the right-hand column).
struct ShiftGuard {
  Ui& ui;
  float dx;
  ShiftGuard(Ui& u, float d) : ui(u), dx(d) { ui.Offset(dx, 0); }
  ~ShiftGuard() { ui.Offset(-dx, 0); }
  ShiftGuard(const ShiftGuard&) = delete;
  ShiftGuard& operator=(const ShiftGuard&) = delete;
};

// ---------------------------------------------------------------------------------------------------------------- live cards
void LiveCard(App& a, const Rect& r, bool cpu) {
  Ui& ui = a.ui();
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const Live l = LiveOf(a, cpu);
  const uint32_t col = cpu ? th.accent : th.blue;
  ui.Panel(r);
  g.Text(Rect(r.x + 18, r.y + 16, r.w - 140, 24), cpu ? T("CPU fan") : T("GPU fan"), FontId::H3, th.text);
  if (l.temp >= 0) {
    const std::string t = Num(l.temp) + " \xC2\xB0""C";
    const float cw = ui.ChipW(t);
    const Rect chip(r.r() - 18 - cw, r.y + 17, cw, 22);
    ui.Chip(chip, t, th.muted);
    ui.Tip(chip, cpu ? T("CPU temperature as the laptop's controller reads it") : T("GPU temperature as the laptop's controller reads it"));
  }
  g.FillRound(Rect(r.x + 18, r.y + 54, 44, 44), 12, WithAlpha(col, 30));
  g.Icon(IconId::Fan, Rect(r.x + 26, r.y + 62, 28, 28), col, 1.7f);
  const std::string big = l.rpm >= 0 ? Num(l.rpm) : std::string("\xE2\x80\x94");
  const float bw = g.TextW(big, FontId::Mega);
  g.Text(Rect(r.x + 74, r.y + 50, bw + 6, 52), big, FontId::Mega, th.text);
  g.Text(Rect(r.x + 74 + bw + 10, r.y + 66, 60, 24), "RPM", FontId::Label, th.muted);
  const std::deque<float>& hs = cpu ? a.hist().cpuRpm : a.hist().gpuRpm;
  float top = 3000;
  for (float v : hs) top = std::max(top, v);
  const float sx = r.r() - 18 - std::min(170.0f, r.w * 0.30f);
  if (sx > r.x + 74 + bw + 80) Sparkline(ui, Rect(sx, r.y + 56, r.r() - 18 - sx, 40), hs, 0, std::ceil(top / 1000.0f) * 1000.0f, col);
  ui.Bar(Rect(r.x + 18, r.y + 114, r.w - 36, 6), l.duty >= 0 ? static_cast<float>(l.duty) / 100.0f : 0.0f, col);
  const std::string cap = l.duty >= 0 ? Tf("Fan load %d %%", static_cast<int>(std::lround(l.duty))) : T("Fan load unknown");
  g.Text(Rect(r.x + 18, r.y + 124, r.w - 36, 20), cap + "  \xC2\xB7  " + ModeName(a.S("fan.mode")), FontId::Small, th.faint);
}

// ---------------------------------------------------------------------------------------------------------------- the curve editor
// Safety minimum the firmware layer enforces on every custom curve (see fan.h SafetyFloor), drawn as a shaded staircase.
void DrawFloor(Gfx& g, const Plot& pl, const Theme& th) {
  struct S { float t0, t1; int pct; };
  static const S kS[] = {{70, 80, 30}, {80, 88, 50}, {88, 95, 70}, {95, 100, 90}, {100, kTmax, 100}};
  for (const S& s : kS) {
    const Rect box(pl.X(s.t0), pl.Y(static_cast<float>(s.pct)), pl.X(s.t1) - pl.X(s.t0), pl.Y(0) - pl.Y(static_cast<float>(s.pct)));
    g.Fill(box, WithAlpha(th.red, 22));
    g.Line({box.x, box.y}, {box.r(), box.y}, WithAlpha(th.red, 110), 1.2f);
    g.Line({box.x, box.y}, {box.x, pl.Y(0)}, WithAlpha(th.red, 60), 1.0f);
  }
  g.Text(Rect(pl.X(70) + 4, pl.Y(30) + 2, 120, 14), T("Safety minimum"), FontId::Tiny, WithAlpha(th.red, 190));
}

// The staircase of a curve: row i holds its duty for the temperatures above the previous row up to its own threshold.
std::vector<V2> Steps(const Plot& pl, const fan::Curve& c) {
  std::vector<V2> pts;
  float prev = kTmin;
  for (const fan::Row& row : c) {
    const float y = pl.Y(static_cast<float>(row.pct));
    pts.push_back({pl.X(std::max(kTmin, prev)), y});
    pts.push_back({pl.X(std::min(kTmax, static_cast<float>(row.temp))), y});
    prev = static_cast<float>(row.temp);
  }
  if (!c.empty()) pts.push_back({pl.X(kTmax), pl.Y(static_cast<float>(c.back().pct))});
  return pts;
}

void DrawCurve(Gfx& g, const Plot& pl, const fan::Curve& c, uint32_t col, float width, int alpha, bool fill) {
  std::vector<V2> pts = Steps(pl, c);
  if (pts.size() < 2) return;
  if (fill) {
    std::vector<V2> poly = pts;
    poly.push_back({pts.back().x, pl.Y(0)});
    poly.push_back({pts.front().x, pl.Y(0)});
    g.PolyFill(poly, WithAlpha(col, 20));
  }
  g.Poly(pts, WithAlpha(col, alpha), width);
}

void DrawLiveMarker(App& a, Gfx& g, const Plot& pl, const Theme& th, bool cpu, bool top) {
  const Live l = LiveOf(a, cpu);
  if (l.temp < kTmin || l.temp > kTmax) return;
  const uint32_t col = cpu ? th.accent : th.blue;
  const float x = pl.X(static_cast<float>(l.temp));
  g.Line({x, pl.r.y}, {x, pl.r.b()}, WithAlpha(col, 80), 1.2f);
  if (l.duty >= 0) {
    g.Dot({x, pl.Y(static_cast<float>(l.duty))}, 7.0f, WithAlpha(col, 55));
    g.Dot({x, pl.Y(static_cast<float>(l.duty))}, 3.8f, col);
  }
  const std::string t = Num(l.temp) + " \xC2\xB0""C" + (l.duty >= 0 ? "  \xC2\xB7  " + Pct(l.duty) : std::string());
  const float tw = g.TextW(t, FontId::Tiny) + 12;
  const float tx = std::min(pl.r.r() - tw, std::max(pl.r.x, x - tw * 0.5f));
  const Rect chip(tx, top ? pl.r.y + 3 : pl.r.b() - 21, tw, 18);
  g.FillRound(chip, 9, WithAlpha(0xFF000000, 120));
  g.FillRound(chip, 9, WithAlpha(col, 40));
  g.Text(chip, t, FontId::Tiny, th.text, Align::Center);
}

struct Edit {
  bool changed = false;
  int pt = 0;
};

// Draws the plot and handles the mouse / keyboard on it. `curve` is the working copy of the curve being edited.
Edit PlotEditor(App& a, const Rect& area, fan::Curve* curve, const View& view, bool editCpu, bool linked, const fan::Curve& otherCurve, bool canEdit, int pt) {
  Ui& ui = a.ui();
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Edit out;
  out.pt = pt;
  Plot pl;
  pl.r = Rect(area.x + 40, area.y + 8, area.w - 40 - 10, area.h - 8 - 26);
  const uint32_t col = editCpu ? th.accent : th.blue;
  const uint32_t colOther = editCpu ? th.blue : th.accent;

  // ---- grid and axes
  for (int t = 30; t <= 100; t += 10) {
    const float x = pl.X(static_cast<float>(t));
    g.Line({x, pl.r.y}, {x, pl.r.b()}, WithAlpha(th.line, t == 30 ? 200 : 100), 1);
    g.Text(Rect(x - 20, pl.r.b() + 5, 40, 14), Num(t) + "\xC2\xB0", FontId::Tiny, th.faint, Align::Center);
  }
  for (int p = 0; p <= 100; p += 25) {
    const float y = pl.Y(static_cast<float>(p));
    g.Line({pl.r.x, y}, {pl.r.r(), y}, WithAlpha(th.line, p == 0 ? 200 : 100), 1);
    g.Text(Rect(area.x, y - 8, 34, 16), Pct(p, 0, true), FontId::Tiny, th.faint, Align::Right);
  }
  g.StrokeRound(pl.r.Inset(-0.5f), 2, WithAlpha(th.line, 160), 1);
  DrawFloor(g, pl, th);

  // ---- interaction first, so this frame already draws the edited curve
  const int n = static_cast<int>(curve->size());
  bool anyHover = false;
  int hoverIdx = -1;
  const bool plotClick = ui.Click("fan.plot", pl.r, canEdit, nullptr, nullptr);   // registered first: the handles drawn after it win the press
  int& drag = a.Int("fan.drag", -1);
  for (int i = 0; i < n; i++) {
    const fan::Row& row = (*curve)[static_cast<size_t>(i)];
    const V2 c{pl.X(static_cast<float>(row.temp)), pl.Y(static_cast<float>(row.pct))};
    bool hov = false, held = false;
    ui.Click("fh#" + std::to_string(i), Rect(c.x - 13, c.y - 13, 26, 26), canEdit, &hov, &held);
    if (hov) {
      anyHover = true;
      hoverIdx = i;
    }
    if (held) {
      drag = i;
      out.pt = i;
    }
  }
  if (!ui.in.down) drag = -1;
  if (plotClick && !anyHover && drag < 0 && n > 0) {   // a click beside the handles selects the nearest one
    const float t = pl.T(ui.LocalMouse().x);
    float best = 1e9f;
    for (int i = 0; i < n; i++) {
      const float d = std::fabs(static_cast<float>((*curve)[static_cast<size_t>(i)].temp) - t);
      if (d < best) {
        best = d;
        out.pt = i;
      }
    }
  }
  if (drag >= 0 && drag < n && ui.in.down && canEdit) {
    const V2 m = ui.LocalMouse();
    const int t = static_cast<int>(std::lround(pl.T(m.x)));
    const int p = std::max(0, std::min(100, static_cast<int>(std::lround(pl.P(m.y)))));
    fan::Curve nc = fan::MovePoint(*curve, drag, t, p);
    if (fan::ToText(nc) != fan::ToText(*curve)) {
      *curve = nc;
      out.changed = true;
    }
  }
  // double-click on empty plot: add a point there
  if (canEdit && ui.in.dbl && !anyHover && ui.Hover(pl.r) && drag < 0) {
    const int t = static_cast<int>(std::lround(pl.T(ui.LocalMouse().x)));
    fan::Curve nc = fan::AddPoint(*curve, t);
    if (fan::ToText(nc) != fan::ToText(*curve)) {
      for (int i = 0; i < static_cast<int>(nc.size()); i++)
        if (nc[static_cast<size_t>(i)].temp == t) out.pt = i;
      *curve = nc;
      out.changed = true;
    }
  }
  // keyboard on the selected point
  if (canEdit && (ui.Focused("fh#" + std::to_string(out.pt)) || ui.Focused("fb#" + std::to_string(out.pt)))) {
    for (auto& k : ui.in.keys) {
      if (k.vk == 0) continue;
      const fan::Row cur = (*curve)[static_cast<size_t>(std::min(out.pt, static_cast<int>(curve->size()) - 1))];
      fan::Curve nc;
      if (k.vk == VK_LEFT) nc = fan::MovePoint(*curve, out.pt, cur.temp - 1, cur.pct);
      else if (k.vk == VK_RIGHT) nc = fan::MovePoint(*curve, out.pt, cur.temp + 1, cur.pct);
      else if (k.vk == VK_UP) nc = fan::MovePoint(*curve, out.pt, cur.temp, cur.pct + 1);
      else if (k.vk == VK_DOWN) nc = fan::MovePoint(*curve, out.pt, cur.temp, cur.pct - 1);
      else if (k.vk == VK_DELETE) nc = fan::RemovePoint(*curve, out.pt);
      else continue;
      k.vk = 0;
      if (!nc.empty() && fan::ToText(nc) != fan::ToText(*curve)) {
        if (nc.size() < curve->size()) out.pt = std::max(0, std::min(out.pt, static_cast<int>(nc.size()) - 1));
        *curve = nc;
        out.changed = true;
      }
    }
  }
  const int nn = static_cast<int>(curve->size());
  out.pt = std::max(0, std::min(out.pt, nn - 1));

  // ---- curves
  if (!linked) DrawCurve(g, pl, otherCurve, colOther, 1.6f, 90, false);
  if (fan::ToText(view.applied) != fan::ToText(*curve)) DrawCurve(g, pl, view.applied, col, 1.2f, 110, false);   // what the hardware uses now
  DrawCurve(g, pl, *curve, col, 2.6f, 255, true);
  DrawLiveMarker(a, g, pl, th, true, true);
  DrawLiveMarker(a, g, pl, th, false, false);

  // ---- handles
  for (int i = 0; i < nn; i++) {
    const fan::Row& row = (*curve)[static_cast<size_t>(i)];
    const V2 c{pl.X(static_cast<float>(row.temp)), pl.Y(static_cast<float>(row.pct))};
    const bool sel = i == out.pt;
    const bool hot = i == hoverIdx || i == drag;
    const float rad = hot ? 8.0f : 6.5f;
    if (hot || sel) g.Dot(c, rad + 5, WithAlpha(col, sel ? 60 : 36));
    g.Dot(c, rad, canEdit ? (sel ? col : th.panel) : th.elev);
    g.Circle(c, rad - 1.0f, canEdit ? col : th.faint, 2.2f);
  }
  const int bubble = drag >= 0 ? drag : (hoverIdx >= 0 ? hoverIdx : -1);
  if (bubble >= 0 && bubble < nn) {
    const fan::Row& row = (*curve)[static_cast<size_t>(bubble)];
    const V2 c{pl.X(static_cast<float>(row.temp)), pl.Y(static_cast<float>(row.pct))};
    const std::string t = bubble == 0 ? Tf("up to %d \xC2\xB0""C: the laptop decides", row.temp) : Tf("%d \xC2\xB0""C  \xE2\x86\x92  %d %%", row.temp, row.pct);
    const float tw = g.TextW(t, FontId::Small) + 18;
    float bx = std::min(pl.r.r() - tw, std::max(pl.r.x, c.x - tw * 0.5f));
    float by = c.y - 40;
    if (by < pl.r.y + 2) by = c.y + 16;
    const Rect bb(bx, by, tw, 24);
    g.FillRound(bb.Offset(0, 2).Inset(-1), 9, WithAlpha(0xFF000000, 80));
    g.FillRound(bb, 8, th.elev);
    g.StrokeRound(bb, 8, WithAlpha(col, 140), 1);
    g.Text(bb, t, FontId::Small, th.text, Align::Center);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------- applying
void ApplyCurves(App& a, bool useCustom) {
  const View cpu = ViewOf(a, true), gpu = ViewOf(a, false);
  std::vector<Request> reqs;
  auto add = [&](const char* key, const std::string& value) {
    Request r;
    r.op = "set";
    r.key = key;
    r.value = value;
    reqs.push_back(r);
  };
  if (cpu.dirty) add("fan.cpu.curve", fan::ToText(cpu.shown));
  if (gpu.dirty) add("fan.gpu.curve", fan::ToText(gpu.shown));
  if (useCustom && a.S("fan.mode") != "custom") add("fan.mode", "custom");
  if (reqs.empty()) return;
  std::vector<std::string> labels(reqs.size());
  labels.back() = useCustom ? T("Fan curves applied, Custom curve mode on") : T("Fan curves applied");
  a.Chain(reqs, labels);
}

void DiscardCurves(App& a) {
  a.Str("fan.d.cpu").clear();
  a.Str("fan.d.gpu").clear();
}

// ---------------------------------------------------------------------------------------------------------------- cards
void ChartCard(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const bool canEdit = a.CapOk("fan.curve");
  const bool linked = Linked(a);
  int& selI = a.Int("fan.sel", 0);
  if (linked) selI = 0;
  const bool editCpu = selI == 0;
  const View cpuV = ViewOf(a, true), gpuV = ViewOf(a, false);
  const View& cur = editCpu ? cpuV : gpuV;
  fan::Curve curve = cur.shown;
  const fan::Curve& other = editCpu ? gpuV.shown : cpuV.shown;
  const bool anyDirty = cpuV.dirty || gpuV.dirty;
  int& ptRef = a.Int("fan.pt", 0);
  ptRef = std::max(0, std::min(ptRef, static_cast<int>(curve.size()) - 1));

  const std::string mode = a.S("fan.mode");
  std::string banner;
  uint32_t bannerCol = th.amber;
  if (!canEdit) {
    banner = a.CapWhy("fan.curve").empty() ? T("Fan curves are not available on this system.") : a.CapWhy("fan.curve");
  } else if (mode != "custom") {
    if (mode == "boost") banner = T("Fan boost is on, so both fans run at full speed. These curves are used once boost is off and the mode is Custom curve.");
    else if (mode == "fixed") banner = T("A fixed fan speed is set. These curves are used once you switch to Custom curve.");
    else banner = T("The fan mode is Auto, so the laptop decides the fan speed. These curves are used once you switch to Custom curve.");
    bannerCol = th.blue;
  }
  const float innerW = m.InnerW();
  const float bannerH = banner.empty() ? 0.0f : g.Wrapped(Rect(0, 0, innerW - 42, 3000), banner, FontId::Small, 0, false) + 18;
  const int boxRows = BoxRows(innerW, static_cast<int>(curve.size()));
  const float boxesH = boxRows * kBoxH + (boxRows - 1) * 6.0f;
  const float h = 40 + (banner.empty() ? 0.0f : bannerH + 8) + kPlotH + 10 + boxesH + 10 + 38 + 8 + 38 + 10 + 24 + 46;
  Card k(c, m, T("Temperature and fan speed"), T("Drag the points. Nothing is sent until you press Apply."), IconId::Thermo, h);

  // ---- which curve is being edited
  {
    const Rect row = k.Row(40);
    float x = row.x;
    g.Text(Rect(x, row.y, 56, 32), T("Editing"), FontId::Small, th.faint);
    x += 58;
    auto pill = [&](const std::string& id, const std::string& label, uint32_t col, bool active, bool dirty) {
      const float pw = g.TextW(label, FontId::Label) + 40 + (dirty ? 10 : 0);
      const Rect pr(x, row.y + 1, pw, 30);
      bool hov = false;
      const bool click = ui.Click(id, pr, true, &hov, nullptr);
      g.FillRound(pr, 15, active ? WithAlpha(col, 40) : (hov ? th.hover : th.inset));
      g.StrokeRound(pr, 15, active ? WithAlpha(col, 150) : th.line, 1);
      g.Dot({pr.x + 15, pr.cy()}, 4.5f, col);
      g.Text(Rect(pr.x + 26, pr.y, pw - 30, pr.h), label, FontId::Label, active ? th.text : th.muted);
      if (dirty) g.Dot({pr.r() - 13, pr.cy()}, 3.2f, th.amber);
      x += pw + 8;
      return click;
    };
    if (linked) {
      pill("fs#both", T("CPU and GPU (linked)"), th.accent, true, cpuV.dirty || gpuV.dirty);
    } else {
      if (pill("fs#cpu", T("CPU curve"), th.accent, editCpu, cpuV.dirty)) selI = 0;
      if (pill("fs#gpu", T("GPU curve"), th.blue, !editCpu, gpuV.dirty)) selI = 1;
    }
    if (anyDirty) {
      const std::string t = T("Not applied yet");
      const float cw = ui.ChipW(t);
      ui.Chip(Rect(row.r() - cw, row.y + 4, cw, 22), t, th.amber);
    }
  }
  if (!banner.empty()) {
    const Rect row = k.Row(bannerH);
    g.FillRound(row, 10, WithAlpha(bannerCol, 24));
    g.StrokeRound(row, 10, WithAlpha(bannerCol, 90), 1);
    g.Icon(IconId::Info, Rect(row.x + 10, row.y + 9, 14, 14), bannerCol, 1.7f);
    g.Wrapped(Rect(row.x + 32, row.y + 8, row.w - 42, row.h - 8), banner, FontId::Small, bannerCol);
    k.Gap(8);
  }

  // ---- the plot
  const Rect area = k.Row(kPlotH);
  Edit e = PlotEditor(a, area, &curve, cur, editCpu, linked, other, canEdit, ptRef);
  ptRef = e.pt;
  if (e.changed) Commit(a, editCpu, curve);
  k.Gap(10);
  const int n = static_cast<int>(curve.size());
  const fan::Row sel = curve[static_cast<size_t>(std::min(ptRef, n - 1))];

  // ---- every point as a box
  {
    const Rect row = k.Row(boxesH);
    const int per = boxRows == 1 ? std::max(1, std::min(n, 10)) : (n + 1) / 2;
    const float bw = (row.w - (per - 1) * 6.0f) / per;
    for (int i = 0; i < n; i++) {
      const int rr = i / per, cc = i % per;
      const Rect b(row.x + cc * (bw + 6), row.y + rr * (kBoxH + 6), bw, kBoxH);
      const fan::Row& cr = curve[static_cast<size_t>(i)];
      bool hov = false;
      if (ui.Click("fb#" + std::to_string(i), b, canEdit, &hov, nullptr)) ptRef = i;
      const bool on = i == ptRef;
      g.FillRound(b, 9, on ? WithAlpha(th.accent, 26) : (hov ? th.hover : th.inset));
      g.StrokeRound(b, 9, on ? WithAlpha(th.accent, 170) : th.line, 1);
      g.Text(Rect(b.x, b.y + 5, b.w, 16), Num(cr.temp) + " \xC2\xB0""C", FontId::Tiny, th.muted, Align::Center);
      g.Text(Rect(b.x, b.y + 21, b.w, 20), i == 0 ? T("EC") : Pct(cr.pct), FontId::Label, on ? th.accent : th.text, Align::Center);
      ui.Tip(b, i == 0 ? Tf("Up to %d \xC2\xB0""C the laptop's own control decides the fan speed.", cr.temp)
                       : Tf("Above %d up to %d \xC2\xB0""C the fan runs at %d %%.", curve[static_cast<size_t>(i - 1)].temp, cr.temp, cr.pct));
    }
  }
  k.Gap(10);

  // ---- fine adjustment of the selected point, add / remove
  {
    const Rect row = k.Row(38);
    float x = row.x;
    auto stepBtn = [&](const std::string& id, const std::string& label, bool enabled) {
      const Rect b(x, row.y + 4, 30, 30);
      x += 32;
      return ui.Button(id, b, label, Btn::Normal, IconId::None, enabled && canEdit);
    };
    auto valueText = [&](const std::string& t, float w) {
      g.Text(Rect(x, row.y, w, 38), t, FontId::Label, th.text, Align::Center);
      x += w + 2;
    };
    const bool first = ptRef == 0, last = ptRef == n - 1;
    fan::Curve nc;
    if (stepBtn("fan.t-", "\xE2\x88\x92", true)) nc = fan::MovePoint(curve, ptRef, sel.temp - 1, sel.pct);
    valueText(Num(sel.temp) + " \xC2\xB0""C", 58);
    if (stepBtn("fan.t+", "+", true)) nc = fan::MovePoint(curve, ptRef, sel.temp + 1, sel.pct);
    x += 14;
    if (stepBtn("fan.p-", "\xE2\x88\x92", !first && !last)) nc = fan::MovePoint(curve, ptRef, sel.temp, sel.pct - 1);
    valueText(first ? T("EC") : Pct(sel.pct), 50);
    if (stepBtn("fan.p+", "+", !first && !last)) nc = fan::MovePoint(curve, ptRef, sel.temp, sel.pct + 1);
    const float addW = 116, remW = 126;
    const bool wideEnough = row.r() - x > addW + remW + 20;
    const Rect rem(row.r() - (wideEnough ? remW : 38), row.y + 2, wideEnough ? remW : 38, 34);
    const Rect add(rem.x - 8 - (wideEnough ? addW : 38), row.y + 2, wideEnough ? addW : 38, 34);
    bool full = n >= fan::kActiveRows;
    if (ui.Button("fan.add", add, wideEnough ? T("Add point") : "", Btn::Normal, IconId::Plus, canEdit && !full)) {
      // split the widest gap between two rows in the middle
      int bestT = -1, gap = 0;
      for (int i = 1; i < n; i++) {
        const int gp = curve[static_cast<size_t>(i)].temp - curve[static_cast<size_t>(i - 1)].temp;
        if (gp > gap) {
          gap = gp;
          bestT = (curve[static_cast<size_t>(i)].temp + curve[static_cast<size_t>(i - 1)].temp) / 2;
        }
      }
      if (bestT >= 0) {
        nc = fan::AddPoint(curve, bestT);
        for (int i = 0; i < static_cast<int>(nc.size()); i++)
          if (nc[static_cast<size_t>(i)].temp == bestT) ptRef = i;
      }
    }
    ui.Tip(add, full ? T("The table is full (10 rows).") : T("Split the widest step in two. You can also double-click the plot."));
    if (ui.Button("fan.rem", rem, wideEnough ? T("Remove point") : "", Btn::Normal, IconId::Trash, canEdit && !first && n > 2)) {
      nc = fan::RemovePoint(curve, ptRef);
      ptRef = std::max(0, std::min(ptRef, static_cast<int>(nc.size()) - 1));
    }
    ui.Tip(rem, first ? T("The first point cannot be removed.") : T("Remove the selected point (Delete key)."));
    if (!nc.empty() && fan::ToText(nc) != fan::ToText(curve)) {
      curve = nc;
      Commit(a, editCpu, curve);
    }
  }
  k.Gap(8);

  // ---- presets
  {
    const Rect row = k.Row(38);
    g.Text(Rect(row.x, row.y, 70, 38), T("Presets"), FontId::Small, th.faint);
    float x = row.x + 70;
    struct P { const char* id; std::string label; fan::Curve cpu, gpu; std::string tip; };
    const P presets[] = {{"fan.pq", T("Quiet"), fan::QuietCpu(), fan::QuietGpu(), T("Fans stay low longer. Temperatures rise further before they ramp up.")},
                         {"fan.pd", T("Default"), fan::DefaultCpu(), fan::DefaultGpu(), T("BaseControl's balanced starting curve.")},
                         {"fan.pt", T("Turbo"), fan::TurboCpu(), fan::TurboGpu(), T("Fans ramp up early for the lowest temperatures.")}};
    for (const P& p : presets) {
      const Rect b(x, row.y + 4, 88, 30);
      if (ui.Button(p.id, b, p.label, Btn::Normal, IconId::None, canEdit)) {
        curve = editCpu ? p.cpu : p.gpu;
        Commit(a, editCpu, curve);
        ptRef = 0;
      }
      ui.Tip(b, p.tip);
      x += 94;
    }
  }
  k.Gap(10);

  // ---- apply bar: a status line, then the buttons
  {
    const bool busy = a.Pending("fan.cpu.curve") || a.Pending("fan.gpu.curve") || a.Pending("fan.mode");
    const bool custom = mode == "custom";
    const std::string st = anyDirty ? T("You have changes that are not applied yet.")
                                    : (custom ? T("The curves match what the hardware uses.") : T("The stored curves are written to the hardware when Custom curve mode is on."));
    const Rect line = k.Row(24);
    g.Text(line, st, FontId::Small, anyDirty ? th.amber : th.faint);
    const Rect row = k.Row(46);
    float rx = row.r();
    auto btn = [&](const std::string& id, const std::string& label, Btn style, IconId icon, bool enabled, float w) {
      const Rect b(rx - w, row.y + 4, w, 38);
      rx -= w + 10;
      return ui.Button(id, b, label, style, icon, enabled);
    };
    if (custom) {
      if (btn("fan.apply", busy ? T("Applying") : T("Apply changes"), Btn::Primary, IconId::Check, anyDirty && canEdit && !busy, 170)) ApplyCurves(a, false);
    } else {
      if (btn("fan.applyuse", busy ? T("Applying") : T("Apply and use them"), Btn::Primary, IconId::Check, anyDirty && canEdit && !busy, 190)) ApplyCurves(a, true);
      if (btn("fan.save", T("Save curves"), Btn::Normal, IconId::None, anyDirty && canEdit && !busy, 124)) ApplyCurves(a, false);
    }
    if (btn("fan.discard", T("Discard"), Btn::Normal, IconId::Undo, anyDirty && !busy, 112)) DiscardCurves(a);
  }
}

void CoolingCard(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  const std::string mode = a.S("fan.mode");
  const float h = kRowSlider + 9 + kRowToggle + 9 + kRowToggle;
  Card k(c, m, T("Cooling behaviour"), "", IconId::Fan, h);
  k.EnumConfirm("fan.mode", T("Cooling mode"), T("Auto: the laptop decides. Custom curve: your curves. Fixed speed: one value (experimental)."),
                {T("Auto"), T("Boost"), T("Custom curve"), T("Fixed speed")});
  k.Divider();
  const bool boost = mode == "boost";
  if (k.Toggle("tg#fan.boost", T("Fan boost"), T("Both fans at maximum speed until you turn it off."), boost, a.CapOk("fan.ctl") && !a.Pending("fan.mode"))) {
    if (!boost) {
      a.Str("fan.prev") = mode;
      a.Set("fan.mode", "boost", T("Fan boost on"));
    } else {
      std::string prev = a.Str("fan.prev");
      if (prev.empty() || prev == "boost" || prev == "fixed") prev = "auto";
      a.Set("fan.mode", prev, T("Fan boost off"));
    }
  }
  k.Divider();
  if (k.PrefRow("fan.link", T("Same curve for both fans"), T("Editing the CPU curve changes the GPU curve too."), false)) {
    if (Linked(a)) {
      a.Int("fan.sel", 0) = 0;
      const View v = ViewOf(a, true);
      Commit(a, true, v.shown);
    }
  }
  (void)ui;
}

void HistoryCard(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Card k(c, m, T("Fan speed history"), T("The last two minutes"), IconId::Fan, 24 + 168);
  const Rect lg = k.Row(24);
  g.Line({lg.x + 2, lg.cy()}, {lg.x + 20, lg.cy()}, th.accent, 2.6f);
  const std::string l1 = T("CPU fan"), l2 = T("GPU fan");
  g.Text(Rect(lg.x + 28, lg.y, 90, lg.h), l1, FontId::Small, th.text);
  const float x2 = lg.x + 28 + g.TextW(l1, FontId::Small) + 28;
  g.Line({x2, lg.cy()}, {x2 + 18, lg.cy()}, th.blue, 2.6f);
  g.Text(Rect(x2 + 26, lg.y, 90, lg.h), l2, FontId::Small, th.text);
  const Rect cr = k.Row(168);
  float top = 3000;
  for (float v : a.hist().cpuRpm) top = std::max(top, v);
  for (float v : a.hist().gpuRpm) top = std::max(top, v);
  const float ymax = std::ceil(top * 1.05f / 1000.0f) * 1000.0f;
  LineChart(ui, Rect(cr.x, cr.y, cr.w, cr.h - 18), {&a.hist().cpuRpm, &a.hist().gpuRpm}, {th.accent, th.blue}, 0, ymax, "RPM");
  TimeAxis(ui, Rect(cr.x + 34, cr.y + 6, cr.w - 40, cr.h - 34), 120);
}

SliderCfg FixedCfg(App& a, bool cpu) {
  SliderCfg s;
  s.key = cpu ? "fan.fixed.cpu" : "fan.fixed.gpu";
  const bool fixedMode = a.S("fan.mode") == "fixed";
  if (fixedMode) {
    s.liveKey = s.key;
  } else {
    s.liveKey = cpu ? "cfg.fan.fixed.cpu" : "cfg.fan.fixed.gpu";
    s.fallback = 60;
    s.tag = T("STORED");
  }
  s.title = cpu ? T("Fixed CPU fan speed") : T("Fixed GPU fan speed");
  s.desc = fixedMode ? T("The fan duty held right now.") : T("Used when the mode is Fixed speed. Stored until then.");
  return s;
}

void AdvancedCard(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  const bool two = m.InnerW(true) >= 760;
  const float colGap = 30;
  const std::string note = Tf("Fixed speed holds one duty and bypasses the laptop's thermal control. While BaseControl (or its service) runs, a watchdog forces full speed at %d \xC2\xB0""C CPU or %d \xC2\xB0""C GPU. Without it nothing protects the laptop.", 97, 92);
  const float innerW = m.InnerW(true);
  const float colW = two ? (innerW - colGap) / 2 : innerW;
  const float noteH = ui.gfx.Wrapped(Rect(0, 0, colW, 3000), note, FontId::Small, 0, false) + 10;
  const float left = 2 * kRowSlider + 9;
  const float right = kRowSlider + 9 + noteH;
  const float h = two ? std::max(left, right) : left + 9 + right;
  Card k(c, m, T("Advanced fan control"), T("Experimental. Needs Safety and service > Allow experimental writes."), IconId::Sliders, h, true);
  const float x0 = k.x0, y0 = k.y, fullW = k.w;
  k.w = colW;
  k.Slider(FixedCfg(a, true));
  k.Divider();
  k.Slider(FixedCfg(a, false));
  const float yLeft = k.y;
  if (two) {
    k.x0 = x0 + colW + colGap;
    k.y = y0;
  } else {
    k.Divider();
  }
  SliderCfg ramp;
  ramp.key = "fan.ramp";
  ramp.title = T("Fan switching speed");
  ramp.desc = T("The value the embedded controller uses when the fans change speed (1 to 127). The effect is not documented.");
  k.Slider(ramp);
  k.Divider();
  k.Note(note);
  k.x0 = x0;
  k.w = fullW;
  k.y = std::max(k.y, yLeft);
}

}  // namespace

float PageFan(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  const float w = r.w;

  // ---- row 1: the two fans
  float row1 = kLiveH;
  if (w < 640) {
    LiveCard(a, Rect(0, 0, w, kLiveH), true);
    LiveCard(a, Rect(0, kLiveH + kGap, w, kLiveH), false);
    row1 = 2 * kLiveH + kGap;
  } else {
    const float cw = (w - kGap) / 2;
    LiveCard(a, Rect(0, 0, cw, kLiveH), true);
    LiveCard(a, Rect(cw + kGap, 0, cw, kLiveH), false);
  }

  // ---- row 2: curve editor, cooling mode, history
  Section s1(ui, row1 + kGap);
  float row2 = 0;
  if (w >= 1000) {
    const float lw = w - kSideW - kGap;
    Masonry left(lw, lw, 1), right(kSideW, kSideW, 1);
    ChartCard(c, left);
    {
      ShiftGuard sg(ui, lw + kGap);
      CoolingCard(c, right);
      HistoryCard(c, right);
    }
    row2 = std::max(left.Bottom(), right.Bottom());
  } else {
    Masonry m(w, w, 1);
    ChartCard(c, m);
    CoolingCard(c, m);
    HistoryCard(c, m);
    row2 = m.Bottom();
  }

  // ---- row 3: advanced
  Section s2(ui, row2 + kGap);
  Masonry m3(w, w, 1);
  AdvancedCard(c, m3);
  return row1 + kGap + row2 + kGap + m3.Bottom();
}

}  // namespace ui
}  // namespace bc
