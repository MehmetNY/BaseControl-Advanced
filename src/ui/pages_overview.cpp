// Overview: device card, power status, live tiles, activity chart, the settings that are active right now, quick profile
// switch and a plain-language health list (what is limiting the laptop, if anything).
#include <algorithm>
#include <cmath>

#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

std::string Dash() { return "\xE2\x80\x94"; }

// A small original laptop drawing (screen with a soft landscape, base, notch). Pure vector, theme coloured.
void DrawLaptop(Ui& ui, const Rect& r) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const float sw = std::min(r.w / 1.18f, r.h * 1.62f), sh = sw * 0.62f;
  const Rect scr(r.cx() - sw * 0.5f, r.y + (r.h - sh - 14) * 0.5f, sw, sh);
  g.FillRound(scr.Offset(0, 8).Inset(-6, 0), 14, WithAlpha(0xFF000000, 70));
  g.FillRound(scr, 10, th.elev);
  g.StrokeRound(scr, 10, WithAlpha(th.text, 40), 1.2f);
  const Rect pic = scr.Inset(7);
  g.FillGrad(pic, 5, Mix(th.base, th.accent, 0.38f), Mix(th.base, th.blue, 0.2f), 70);
  // sun and two ridges
  g.Dot({pic.x + pic.w * 0.72f, pic.y + pic.h * 0.30f}, pic.h * 0.12f, WithAlpha(th.text, 90));
  std::vector<V2> back = {{pic.x, pic.b() - pic.h * 0.30f}, {pic.x + pic.w * 0.22f, pic.b() - pic.h * 0.52f}, {pic.x + pic.w * 0.40f, pic.b() - pic.h * 0.36f},
                          {pic.x + pic.w * 0.62f, pic.b() - pic.h * 0.60f}, {pic.r(), pic.b() - pic.h * 0.34f}, {pic.r(), pic.b()}, {pic.x, pic.b()}};
  g.PolyFill(back, WithAlpha(th.base, 150));
  std::vector<V2> front = {{pic.x, pic.b() - pic.h * 0.14f}, {pic.x + pic.w * 0.30f, pic.b() - pic.h * 0.26f}, {pic.x + pic.w * 0.55f, pic.b() - pic.h * 0.12f},
                           {pic.r(), pic.b() - pic.h * 0.22f}, {pic.r(), pic.b()}, {pic.x, pic.b()}};
  g.PolyFill(front, WithAlpha(th.base, 215));
  // base
  const float bw = sw * 1.12f, by = scr.b() + 3;
  std::vector<V2> base = {{scr.cx() - sw * 0.5f - 2, by}, {scr.cx() + sw * 0.5f + 2, by}, {scr.cx() + bw * 0.5f, by + 10}, {scr.cx() - bw * 0.5f, by + 10}};
  g.PolyFill(base, Mix(th.elev, th.text, 0.22f));
  g.Line({scr.cx() - sw * 0.5f - 2, by}, {scr.cx() + sw * 0.5f + 2, by}, WithAlpha(th.text, 50), 1);
  g.FillRound(Rect(scr.cx() - 22, by, 44, 4), 2, WithAlpha(th.base, 160));
}

struct TileSpec {
  std::string title;
  IconId icon;
  std::string big, unit, l1, l2;
  float frac = -1;                        // optional bar below
  uint32_t col = 0;
  const std::deque<float>* spark = nullptr;
  float sparkMax = 100;
};

void Tile(Ctx& c, Masonry& m, const TileSpec& t) {
  Card k(c, m, "", "", IconId::None, 98);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const Rect r(k.x0, k.y, k.w, 98);
  const uint32_t col = t.col ? t.col : th.accent;
  g.Text(Rect(r.x, r.y, r.w - 30, 20), t.title, FontId::Label, th.muted);
  g.Icon(t.icon, Rect(r.r() - 20, r.y, 20, 20), th.faint, 1.6f);
  const float bw = g.TextW(t.big, FontId::Mega);
  g.Text(Rect(r.x, r.y + 22, bw + 4, 42), t.big, FontId::Mega, th.text);
  if (!t.unit.empty()) g.Text(Rect(r.x + bw + 5, r.y + 36, 60, 24), t.unit, FontId::Value, th.muted);
  g.Text(Rect(r.x, r.y + 66, r.w, 16), t.l1, FontId::Small, th.muted);
  g.Text(Rect(r.x, r.y + 82, r.w, 16), t.l2, FontId::Tiny, th.faint);
  const float textEnd = r.x + bw + 5 + g.TextW(t.unit, FontId::Value);
  const float sparkW = std::min(std::min(110.0f, r.w * 0.42f), r.r() - textEnd - 12);
  if (sparkW >= 46) {
    if (t.spark) Sparkline(c.ui, Rect(r.r() - sparkW, r.y + 26, sparkW, 36), *t.spark, 0, t.sparkMax, col);
    if (t.frac >= 0) c.ui.Bar(Rect(r.r() - sparkW, r.y + 50, sparkW, 6), t.frac, col);
  }
}

// One "label ... value  >" row that jumps to a page.
void LinkRow(Card& k, const std::string& id, const std::string& label, const std::string& value, Page target, uint32_t col = 0) {
  Ui& ui = k.c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const Rect r = k.Row(40);
  bool hov = false;
  const bool click = ui.Click("lnk#" + id, r, true, &hov, nullptr);
  const float a = ui.Anim("lnk#a" + id, hov ? 1.0f : 0.0f, 14);
  if (a > 0.01f) g.FillRound(r.Inset(-6, 1), 9, WithAlpha(th.hover, static_cast<int>(200 * a)));
  g.Text(Rect(r.x, r.y, r.w * 0.5f, r.h), label, FontId::Body, th.text);
  const float arrow = 18;
  g.Icon(IconId::ChevronR, Rect(r.r() - 14, r.cy() - 7, 14, 14), Mix(th.faint, th.accent, a), 1.8f);
  g.Text(Rect(r.x + r.w * 0.45f, r.y, r.w * 0.55f - arrow - 4, r.h), value, FontId::Body, col ? col : Mix(th.muted, th.text, a), Align::Right);
  if (click) k.c.app.Go(target);
}

std::string EnumNice(const std::string& v) {
  if (v == "auto") return T("Automatic");
  if (v == "boost") return T("Maximum fans");
  if (v == "custom") return T("Custom curve");
  if (v == "fixed") return T("Fixed speed");
  if (v == "highcap" || v == "balanced" || v == "eco") return BattModeLabel(v);
  return v;
}

struct Health {
  uint32_t col;
  std::string text, detail;
};

}  // namespace

float PageOverview(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const History& h = a.hist();
  float y = 0;

  // =================================================================== 1. device + power
  {
    Section sec(ui, y);
    const bool stack = r.w < 860;
    const float heroW = stack ? r.w : std::floor(r.w * 0.64f - kGap * 0.5f);
    const float powW = stack ? r.w : r.w - heroW - kGap;
    const float heroH = 200;

    auto drawHero = [&](float W) {
      Masonry m(W, 200, 1);
      Card k(c, m, "", "", IconId::None, heroH - 2 * kPad);
      const Rect box = k.box;
      g.FillGrad(box.Inset(1), th.radius, WithAlpha(th.accent, 22), WithAlpha(th.accent, 0), 20);
      const float tx = k.x0, tw = box.w * 0.56f - kPad;
      g.Text(Rect(tx, box.y + 22, 300, 16), T("DEVICE"), FontId::Label, th.faint);
      std::string name = a.S("host.product");
      if (name.empty()) name = a.simulated() ? "TULPAR T5 V21.7" : T("Unknown model");
      g.Text(Rect(tx, box.y + 42, tw, 40), name, FontId::H1, th.text);
      std::string sub = a.S("host.board");
      const std::string vendor = a.S("host.vendor");
      if (!vendor.empty()) sub = vendor + (sub.empty() ? "" : "  \xC2\xB7  " + sub);
      g.Text(Rect(tx, box.y + 86, tw, 18), sub, FontId::Body, th.muted);
      std::string line2;
      if (a.Has("gpu.name")) line2 = a.S("gpu.name");
      if (!a.S("host.ram").empty()) line2 += (line2.empty() ? "" : "  \xC2\xB7  ") + a.S("host.ram");
      g.Text(Rect(tx, box.y + 110, tw, 16), a.S("host.cpu"), FontId::Small, th.faint);
      g.Text(Rect(tx, box.y + 127, tw, 16), line2, FontId::Small, th.faint);
      const Rect lnk(tx, box.b() - 40, 170, 24);
      bool hov = false;
      const bool click = ui.Click("ov#devinfo", lnk, true, &hov, nullptr);
      const float lw = g.TextW(T("Device info"), FontId::Label);
      g.Text(Rect(lnk.x, lnk.y, lw + 4, lnk.h), T("Device info"), FontId::Label, hov ? th.text : th.accent);
      g.Icon(IconId::ChevronR, Rect(lnk.x + lw + 8, lnk.y + 5, 14, 14), hov ? th.text : th.accent, 1.8f);
      if (click) a.Go(Page::DeviceInfo);
      DrawLaptop(ui, Rect(box.x + box.w * 0.56f, box.y + 14, box.w * 0.42f, box.h - 30));
    };
    auto drawPower = [&](float W) {
      Masonry m(W, 200, 1);
      Card k(c, m, T("Power status"), "", IconId::Battery, heroH - 64 - kPad);
      const bool haveBatt = a.Has("power.batt.pct");
      const double pct = haveBatt ? a.N("power.batt.pct") : 0;
      const bool ac = a.B("power.ac"), charging = a.B("power.charging");
      const Rect top = k.Row(44);
      const std::string big = haveBatt ? Num(pct) : Dash();
      const float bw = g.TextW(big, FontId::Mega);
      g.Text(Rect(top.x, top.y, bw + 4, 42), big, FontId::Mega, th.text);
      g.Text(Rect(top.x + bw + 6, top.y + 14, 30, 24), "%", FontId::Value, th.muted);
      const uint32_t bc2 = pct < 20 ? th.red : (pct < 40 ? th.amber : th.green);
      if (charging && haveBatt) g.Text(Rect(top.r() - 110, top.y + 10, 110, 24), T("Charging"), FontId::Label, th.green, Align::Right);
      const Rect bar = k.Row(12);
      ui.Bar(Rect(bar.x, bar.y + 2, bar.w, 8), static_cast<float>(pct / 100.0), bc2, 4);
      k.Gap(2);
      k.Info(T("Power source"), ac ? T("AC adapter") : T("Battery"), ac ? 0 : th.amber);
      if (a.Has("batt.full") && a.Has("batt.design") && a.N("batt.design") > 0) k.Info(T("Battery health"), Num(100.0 * a.N("batt.full") / a.N("batt.design")) + " %");
      else k.Gap(kRowInfo);
    };
    if (!stack) {
      drawHero(heroW);
      ui.Offset(heroW + kGap, 0);
      drawPower(powW);
      ui.Offset(-(heroW + kGap), 0);
      y += heroH + kGap;
    } else {
      drawHero(heroW);
      ui.Offset(0, heroH + kGap);
      drawPower(powW);
      ui.Offset(0, -(heroH + kGap));
      y += 2 * heroH + 2 * kGap;
    }
  }

  // =================================================================== 2. tiles
  {
    Section sec(ui, y);
    Masonry m(r.w, 230, 4);
    const double cpuLoad = a.N("os.cpu.pct"), gpuLoad = a.N("gpu.util");
    {
      TileSpec t;
      t.title = T("Processor");
      t.icon = IconId::Cpu;
      t.big = a.Has("os.cpu.pct") ? Num(cpuLoad) : Dash();
      t.unit = "%";
      std::string l1 = a.Has("cpu.temp") ? With(a.N("cpu.temp"), 0, "\xC2\xB0""C") : Dash();
      if (a.Has("cpu.pkg.w")) l1 += "  \xC2\xB7  " + With(a.N("cpu.pkg.w"), 0, "W");
      t.l1 = l1;
      t.l2 = a.Has("os.cpu.mhz") ? Tf("%.2f GHz average", a.N("os.cpu.mhz") / 1000.0) : "";
      t.spark = &h.cpuLoad;
      t.col = th.accent;
      Tile(c, m, t);
    }
    {
      TileSpec t;
      t.title = T("Graphics card");
      t.icon = IconId::Gpu;
      t.big = a.Has("gpu.util") ? Num(gpuLoad) : Dash();
      t.unit = "%";
      std::string l1 = a.Has("gpu.temp") ? With(a.N("gpu.temp"), 0, "\xC2\xB0""C") : Dash();
      if (a.Has("gpu.power")) l1 += "  \xC2\xB7  " + With(a.N("gpu.power"), 0, "W");
      t.l1 = l1;
      t.l2 = a.Has("gpu.clock") ? Tf("%d MHz core", static_cast<int>(a.N("gpu.clock"))) : "";
      t.spark = &h.gpuLoad;
      t.col = th.blue;
      Tile(c, m, t);
    }
    {
      TileSpec t;
      t.title = T("Memory");
      t.icon = IconId::Ram;
      t.big = a.Has("os.mem.used") ? Num(a.N("os.mem.used") / 1024.0, 1) : Dash();
      t.unit = "GB";
      t.l1 = a.Has("os.mem.total") ? Tf("of %.0f GB  \xC2\xB7  %d %%", a.N("os.mem.total") / 1024.0, static_cast<int>(a.N("os.mem.pct"))) : "";
      t.l2 = a.Has("gpu.vram.total") ? Tf("Video memory %.1f of %.0f GB", a.N("gpu.vram.used") / 1024.0, a.N("gpu.vram.total") / 1024.0) : "";
      t.frac = static_cast<float>(a.N("os.mem.pct") / 100.0);
      t.col = th.green;
      Tile(c, m, t);
    }
    {
      TileSpec t;
      t.title = T("Cooling");
      t.icon = IconId::Fan;
      const double rpm = std::max(a.N("fan.cpu.rpm"), a.N("fan.gpu.rpm"));
      t.big = a.Has("fan.cpu.rpm") ? Num(rpm) : Dash();
      t.unit = "rpm";
      t.l1 = Tf("CPU fan %d %%  \xC2\xB7  GPU fan %d %%", static_cast<int>(a.N("fan.cpu.duty")), static_cast<int>(a.N("fan.gpu.duty")));
      t.l2 = a.Has("ssd.temp") ? Tf("SSD %d \xC2\xB0""C", static_cast<int>(a.N("ssd.temp"))) : "";
      t.spark = &h.cpuRpm;
      t.sparkMax = 6500;
      t.col = th.amber;
      Tile(c, m, t);
    }
    y += m.Bottom() + kGap;
  }

  // =================================================================== 3. chart + side cards
  {
    Section sec(ui, y);
    Masonry m(r.w, 440, 2);
    // ---- activity chart
    {
      Card k(c, m, T("System activity"), T("The last two minutes, one sample per second"), IconId::Performance, 36 + 24 + 232);
      int& tab = a.Int("ov.chart", 0);
      const std::vector<std::string> names = {T("Load"), T("Power"), T("Temperature"), T("Fans")};
      const Rect sr = k.Row(36);
      const int pick = ui.Segmented("ov#chart", Rect(sr.x, sr.y, std::min(sr.w, 380.0f), 34), names, tab, true);
      if (pick >= 0) tab = pick;
      const Rect lg = k.Row(24);
      const Rect cr = k.Row(232);
      const std::deque<float>*s1 = &h.cpuLoad, *s2 = &h.gpuLoad;
      float ymin = 0, ymax = 100;
      std::string unit = "%";
      if (tab == 1) {
        s1 = &h.cpuW; s2 = &h.gpuW;
        float pk = 100;
        for (float v : *s1) pk = std::max(pk, v);
        for (float v : *s2) pk = std::max(pk, v);
        ymax = std::ceil(pk / 50.0f) * 50.0f;
        unit = "W";
      } else if (tab == 2) {
        s1 = &h.cpuT; s2 = &h.gpuT;
        ymin = 20; ymax = 100;
        unit = "C";
      } else if (tab == 3) {
        s1 = &h.cpuRpm; s2 = &h.gpuRpm;
        float pk = 1000;
        for (float v : *s1) pk = std::max(pk, v);
        for (float v : *s2) pk = std::max(pk, v);
        ymax = std::ceil(pk / 1000.0f) * 1000.0f;
        unit = "rpm";
      }
      const float cur1 = s1->empty() ? 0 : s1->back(), cur2 = s2->empty() ? 0 : s2->back();
      const std::string l1 = std::string(tab == 3 ? T("CPU fan") : "CPU") + "  " + Num(cur1) + " " + UnitText(unit);
      const std::string l2 = std::string(tab == 3 ? T("GPU fan") : "GPU") + "  " + Num(cur2) + " " + UnitText(unit);
      const float w1 = g.TextW(l1, FontId::Small) + 4, w2 = g.TextW(l2, FontId::Small) + 4;
      float lx = lg.x + 34;
      g.Line({lx - 34 + 2, lg.cy()}, {lx - 34 + 20, lg.cy()}, th.accent, 2.6f);
      g.Text(Rect(lx - 6, lg.y, w1, lg.h), l1, FontId::Small, th.text);
      lx += w1 + 40;
      g.Line({lx - 34 + 2, lg.cy()}, {lx - 34 + 20, lg.cy()}, th.blue, 2.6f);
      g.Text(Rect(lx - 6, lg.y, w2, lg.h), l2, FontId::Small, th.text);
      LineChart(ui, Rect(cr.x, cr.y, cr.w, cr.h - 18), {s1, s2}, {th.accent, th.blue}, ymin, ymax, "");
      TimeAxis(ui, Rect(cr.x + 34, cr.y + 6, cr.w - 40, cr.h - 34), 120);
    }
    // ---- settings that are active right now
    {
      Card k(c, m, T("Active settings"), T("Click a line to change it"), IconId::Sliders, 7 * 40);
      std::string prof = a.S("profile.active");
      LinkRow(k, "prof", T("Performance profile"), prof.empty() ? T("Custom values") : (prof == "Quiet" || prof == "Balanced" || prof == "Enthusiast" || prof == "Overboost" ? T(prof) : prof), Page::Profiles);
      LinkRow(k, "pl", T("CPU power limits"), a.Has("ec.pl1") ? Num(a.N("ec.pl1")) + " / " + Num(a.N("ec.pl2")) + " / " + Num(a.N("ec.pl4")) + " W" : Dash(), Page::Custom);
      LinkRow(k, "ctgp", T("GPU power (cTGP)"), a.Has("gpu.ctgp") ? With(a.N("gpu.ctgp"), 0, "W") : Dash(), Page::Custom);
      LinkRow(k, "fan", T("Cooling"), a.Has("fan.mode") ? EnumNice(a.S("fan.mode")) : Dash(), Page::Fan);
      std::string kb = Dash();
      if (a.Has("light.kbd.power")) kb = a.B("light.kbd.power") ? Tf("Brightness %d of 4", static_cast<int>(a.N("light.kbd.bright"))) : T("Off");
      LinkRow(k, "kbd", T("Keyboard light"), kb, Page::Lighting);
      LinkRow(k, "batt", T("Charge profile"), a.Has("batt.mode") ? EnumNice(a.S("batt.mode")) : Dash(), Page::Battery);
      std::string disp = Dash();
      if (a.Has("disp.refresh")) disp = Num(a.N("disp.refresh")) + " Hz" + (a.Has("disp.bright") ? "  \xC2\xB7  " + Num(a.N("disp.bright")) + " %" : "");
      LinkRow(k, "disp", T("Display"), disp, Page::Display);
    }
    // ---- quick profiles
    {
      std::vector<std::string> names;
      const std::string list = a.S("profile.list");
      if (!list.empty()) names = Split(list, ',');
      const std::string active = a.S("profile.active");
      static const char* const kBuiltin[] = {"Quiet", "Balanced", "Enthusiast", "Overboost"};
      auto builtin = [&](const std::string& n) {
        for (const char* b : kBuiltin)
          if (n == b) return true;
        return false;
      };
      const float inner = m.InnerW();
      float x = 0, yy = 0;
      struct Item { std::string name; Rect r; };
      std::vector<Item> items;
      for (const std::string& n : names) {
        const float w = std::min(inner, g.TextW(builtin(n) ? T(n) : n, FontId::Label) + 40);
        if (x > 0 && x + w > inner) { x = 0; yy += 46; }
        items.push_back({n, Rect(x, yy, w, 38)});
        x += w + 10;
      }
      const float flowH = names.empty() ? 0 : yy + 38;
      Card k(c, m, T("Quick profile"), T("Applies power, fan and CPU policy together"), IconId::Bolt, flowH + 4);
      for (const Item& it : items) {
        const Rect rc(k.x0 + it.r.x, k.y + it.r.y, it.r.w, it.r.h);
        const bool isActive = it.name == active;
        const std::string shown = builtin(it.name) ? T(it.name) : it.name;
        const std::string nm = it.name;
        if (ui.Button("qp#" + it.name, rc, shown, isActive ? Btn::Primary : Btn::Normal, isActive ? IconId::Check : IconId::None, !a.Pending("cmd:profile.apply"))) {
          KV args;
          args.Set("name", nm);
          a.Cmd("profile.apply", args, Tf("Profile %s applied", shown.c_str()));
        }
      }
    }
    // ---- health
    {
      std::vector<Health> rows;
      const bool ecOk = a.S("ec.link") == "connected";
      rows.push_back({ecOk ? th.green : th.red, T("Embedded controller"), ecOk ? T("Connected") : T("Not reachable")});
      if (ecOk) {
        const bool act = a.B("ec.active");
        rows.push_back({act ? th.green : th.amber, T("Power-management loop"), act ? T("Running") : T("Off: limits are stored but not enforced until it is switched on")});
      }
      const bool anyThr = a.B("cpu.st.thermal") || a.B("cpu.st.prochot") || a.B("cpu.st.power") || a.B("cpu.st.current") || a.N("cpu.throttle.pct") > 2.0;
      std::string why;
      if (a.B("cpu.st.thermal")) why += T("temperature") + ", ";
      if (a.B("cpu.st.prochot")) why += "PROCHOT, ";
      if (a.B("cpu.st.power")) why += T("power limit") + ", ";
      if (a.B("cpu.st.current")) why += T("current limit") + ", ";
      if (!why.empty()) why = why.substr(0, why.size() - 2);
      rows.push_back({anyThr ? th.amber : th.green, T("CPU"), anyThr ? T("Limited by") + ": " + (why.empty() ? Tf("%.0f %% of the time", a.N("cpu.throttle.pct")) : why) : T("Running free")});
      const bool gThr = a.B("gpu.thr.power") || a.B("gpu.thr.thermal") || a.B("gpu.thr.hwslow");
      std::string gwhy;
      if (a.B("gpu.thr.power")) gwhy += T("power") + ", ";
      if (a.B("gpu.thr.thermal")) gwhy += T("temperature") + ", ";
      if (a.B("gpu.thr.hwslow")) gwhy += T("hardware slowdown") + ", ";
      if (!gwhy.empty()) gwhy = gwhy.substr(0, gwhy.size() - 2);
      rows.push_back({gThr ? th.amber : th.green, T("GPU"), gThr ? T("Limited by") + ": " + gwhy : T("Running free")});
      const bool guardOk = a.S("fan.guard", "ok") == "ok";
      rows.push_back({guardOk ? th.green : th.red, T("Fan watchdog"), guardOk ? T("Watching temperatures") : (a.S("fan.guard.why").empty() ? T("Forcing maximum fans") : a.S("fan.guard.why"))});
      if (a.B("oem.running")) rows.push_back({th.blue, T("OEM Control Center service"), T("Running next to BaseControl")});
      if (!a.S("last.error").empty()) rows.push_back({th.red, T("Last error"), a.S("last.error")});
      const float hh = 30.0f * static_cast<float>(rows.size());
      Card k(c, m, T("Health"), T("What is limiting the laptop right now"), IconId::Shield, hh);
      for (const Health& r2 : rows) {
        const Rect rr = k.Row(30);
        g.Dot({rr.x + 5, rr.cy()}, 4, r2.col);
        g.Text(Rect(rr.x + 20, rr.y, rr.w * 0.42f, rr.h), r2.text, FontId::Body, th.text);
        const float vx = rr.x + 20 + rr.w * 0.42f;
        g.Text(Rect(vx, rr.y, rr.r() - vx, rr.h), r2.detail, FontId::Small, r2.col == th.green ? th.muted : th.text, Align::Right);
        if (g.TextW(r2.detail, FontId::Small) > rr.r() - vx) ui.Tip(rr, r2.detail);
      }
    }
    y += m.Bottom();
  }
  return y;
}

}  // namespace ui
}  // namespace bc
