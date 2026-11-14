// Window chrome: navigation rail, top bar, page header with tabs, the "not connected" page and the frame loop.
#include <algorithm>
#include <cmath>

#include "../common/util.h"
#include "../common/version.h"
#include "app_internal.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

struct Meta {
  Page p;
  const char* section;
  const char* title;
  const char* sub;
};

const Meta kMeta[] = {
    {Page::Overview, N_("Device"), N_("Overview"), N_("Live status of your laptop at a glance.")},
    {Page::Prefs, N_("General settings"), N_("Preferences"), N_("How BaseControl starts and looks, plus keyboard and power-on options.")},
    {Page::Devices, N_("General settings"), N_("Device manager"), N_("Switch the built-in radios and devices on or off.")},
    {Page::GpuInfo, N_("General settings"), N_("GPU settings"), N_("GPU power target, clock offsets and the graphics mode.")},
    {Page::Osd, N_("General settings"), N_("In-game OSD"), N_("Choose what is shown on top of your games.")},
    {Page::Keys, N_("General settings"), N_("Key remapping"), N_("Assign functions to keys and review the saved mappings.")},
    {Page::Profiles, N_("Performance"), N_("Performance profiles"), N_("One-click profiles that set power, fans and the Windows CPU policy together.")},
    {Page::Fan, N_("Performance"), N_("Fan settings"), N_("Fan mode, temperature curves and ramp speed.")},
    {Page::Custom, N_("Performance"), N_("Custom profiles"), N_("Set power limits, GPU targets and CPU policy yourself. Nothing is sent until you press Confirm.")},
    {Page::Apps, N_("Performance"), N_("Application profiles"), N_("Switch profiles automatically when a program starts.")},
    {Page::Lighting, N_("Lighting"), N_("Lighting"), N_("Keyboard backlight colour, effects and idle behaviour.")},
    {Page::Battery, N_("Battery"), N_("Battery"), N_("Charging profile and battery health.")},
    {Page::Display, N_("Display"), N_("Display settings"), N_("Brightness, refresh rate, colour and colour modes.")},
    {Page::DeviceInfo, N_("Device info"), N_("Components and status"), N_("Hardware components with live readings.")},
    {Page::About, N_("Device info"), N_("About"), N_("Version, licences and credits.")},
    {Page::Safety, N_("System"), N_("Safety and service"), N_("Safety switches, the OEM service and the BaseControl service.")},
};

const Meta& MetaOf(Page p) {
  for (const Meta& m : kMeta)
    if (m.p == p) return m;
  return kMeta[0];
}

struct Group {
  const char* label;
  IconId icon;
  std::vector<Page> pages;
};

const std::vector<Group>& Groups() {
  static const std::vector<Group> g = {
      {N_("Overview"), IconId::Overview, {Page::Overview}},
      {N_("General settings"), IconId::Settings, {Page::Prefs, Page::Devices, Page::GpuInfo, Page::Osd, Page::Keys}},
      {N_("Performance"), IconId::Performance, {Page::Profiles, Page::Fan, Page::Custom, Page::Apps}},
      {N_("Lighting"), IconId::Lighting, {Page::Lighting}},
      {N_("Battery"), IconId::Battery, {Page::Battery}},
      {N_("Display settings"), IconId::Display, {Page::Display}},
      {N_("Device info"), IconId::Info, {Page::DeviceInfo, Page::About}},
  };
  return g;
}

const Group* GroupOf(Page p) {
  for (const Group& g : Groups())
    for (Page q : g.pages)
      if (q == p) return &g;
  return nullptr;
}

constexpr float kRailWide = 236, kRailNarrow = 68, kTopH = 58, kPadX = 30;

std::string ShortModel(const App& a) {
  std::string p = a.S("host.product");
  if (p.empty()) return a.simulated() ? "Monster Tulpar T5 V21.7" : "";
  return p;
}
}  // namespace

std::string PageTitle(Page p) { return T(MetaOf(p).title); }

// ------------------------------------------------------------------------------------------------ rail
void App::DrawRail(const Rect& r) {
  Gfx& g = ui_.gfx;
  const Theme& th = ui_.th;
  const bool narrow = r.w < 120;
  g.Fill(r, th.rail);
  g.Line({r.r() - 0.5f, r.y}, {r.r() - 0.5f, r.b()}, th.line, 1);

  // brand
  const Rect mark(r.x + (narrow ? (r.w - 38) / 2 : 20), r.y + 18, 38, 38);
  g.FillGrad(mark, 11, WithAlpha(th.accent, 70), WithAlpha(th.accent, 26), 60);
  g.StrokeRound(mark, 11, WithAlpha(th.accent, 90), 1);
  g.Icon(IconId::Bolt, mark.Inset(8), th.accent, 1.9f);
  if (!narrow) {
    g.Text(Rect(mark.r() + 12, mark.y - 2, r.w - 80, 22), "BaseControl", FontId::H3, th.text);
    g.Text(Rect(mark.r() + 12, mark.y + 19, r.w - 80, 18), "ADVANCED", FontId::Label, th.faint);
  }

  // nav
  float y = r.y + 88;
  const Page cur = page_;
  const Group* curGroup = GroupOf(cur);
  auto item = [&](const std::string& id, const std::string& label, IconId icon, bool active, Page target) {
    const Rect row(r.x + 12, y, r.w - 24, 42);
    bool hov = false;
    const bool click = ui_.Click("nav#" + id, row, true, &hov, nullptr);
    const float a = ui_.Anim("nav#a" + id, active ? 1.0f : (hov ? 0.5f : 0.0f), 14);
    if (a > 0.01f) g.FillRound(row, 11, active ? Mix(th.rail, WithAlpha(th.accent, 255), 0.13f * a / 1.0f) : WithAlpha(th.hover, static_cast<int>(180 * a)));
    if (active) {
      g.StrokeRound(row, 11, WithAlpha(th.accent, 70), 1);
      g.FillRound(Rect(row.r() - 5, row.y + 11, 3, row.h - 22), 1.5f, th.accent);
    }
    const uint32_t col = active ? th.accent : Mix(th.muted, th.text, a * 0.6f);
    g.Icon(icon, Rect(row.x + (narrow ? (row.w - 20) / 2 : 14), row.cy() - 10, 20, 20), col, 1.7f);
    if (!narrow) g.Text(Rect(row.x + 46, row.y, row.w - 60, row.h), label, FontId::Body, active ? th.text : Mix(th.muted, th.text, a * 0.6f));
    if (narrow) ui_.Tip(row, label);
    y += 46;
    if (click) Go(target);
  };
  for (const Group& grp : Groups()) item(grp.label, T(grp.label), grp.icon, curGroup == &grp, grp.pages.front());

  // bottom block
  float by = r.b() - 22;
  if (!narrow) {
    const std::string model = ShortModel(*this);
    const std::string board = S("host.board");
    by -= 34;
    g.Text(Rect(r.x + 20, by, r.w - 36, 18), model.empty() ? std::string("-") : g.Fit(model, FontId::Small, r.w - 40), FontId::Small, th.text);
    g.Text(Rect(r.x + 20, by + 17, r.w - 36, 16), board.empty() ? std::string() : board, FontId::Mono, th.faint);
    by -= 12;
    g.Line({r.x + 20, by}, {r.r() - 20, by}, th.line, 1);
  } else {
    by -= 8;
  }
  y = by - 48;
  const Rect row(r.x + 12, y, r.w - 24, 42);
  bool hov = false;
  const bool click = ui_.Click("nav#safety", row, true, &hov, nullptr);
  const bool active = page_ == Page::Safety;
  const float a = ui_.Anim("nav#a-safety", active ? 1.0f : (hov ? 0.5f : 0.0f), 14);
  if (a > 0.01f) g.FillRound(row, 11, active ? Mix(th.rail, WithAlpha(th.accent, 255), 0.13f * a) : WithAlpha(th.hover, static_cast<int>(180 * a)));
  if (active) g.StrokeRound(row, 11, WithAlpha(th.accent, 70), 1);
  const uint32_t col = active ? th.accent : Mix(th.muted, th.text, a * 0.6f);
  g.Icon(IconId::Shield, Rect(row.x + (narrow ? (row.w - 20) / 2 : 14), row.cy() - 10, 20, 20), col, 1.7f);
  if (!narrow) g.Text(Rect(row.x + 46, row.y, row.w - 60, row.h), T("Safety and service"), FontId::Body, active ? th.text : Mix(th.muted, th.text, a * 0.6f));
  else ui_.Tip(row, T("Safety and service"));
  if (click) Go(Page::Safety);
}

// ------------------------------------------------------------------------------------------------ top bar
void App::DrawTopBar(const Rect& r) {
  Gfx& g = ui_.gfx;
  const Theme& th = ui_.th;
  g.Fill(r, th.base);
  g.Line({r.x, r.b() - 0.5f}, {r.r(), r.b() - 0.5f}, th.line, 1);
  const Meta& m = MetaOf(page_);
  const std::string crumb = "BaseControl";
  const float cw = g.TextW(crumb, FontId::Body);
  g.Text(Rect(r.x + kPadX, r.y, cw + 4, r.h), crumb, FontId::Body, th.muted);
  g.Text(Rect(r.x + kPadX + cw + 10, r.y, 14, r.h), "/", FontId::Body, th.faint);
  g.Text(Rect(r.x + kPadX + cw + 28, r.y, 400, r.h), T(m.title), FontId::Label, th.text);

  // chips, right to left
  float x = r.r() - kPadX;
  {   // interface language: a small EN | TR switch (the choice is remembered)
    const float lw = 76;
    x -= lw;
    const Rect rc(x, r.cy() - 14, lw, 28);
    const int cur = GetLang() == Lang::Tr ? 1 : 0;
    const int pick = ui_.Segmented("top#lang", rc, {"EN", "TR"}, cur, true);
    if (pick >= 0 && pick != cur) {
      opt_.lang.clear();
      SetPref("lang", pick == 1 ? "tr" : "en");
      ApplyLanguage();
    }
    ui_.Tip(rc, T("Interface language"));
    x -= 14;
  }
  auto chip = [&](const std::string& text, uint32_t col, bool dot, const std::string& tip) {
    const float w = ui_.ChipW(text) + (dot ? 12.0f : 0.0f);
    x -= w;
    const Rect rc(x, r.cy() - 13, w, 26);
    if (dot) {
      g.FillRound(rc, rc.h * 0.5f, WithAlpha(col, 36));
      g.Dot({rc.x + 13, rc.cy()}, 3.2f, col);
      g.Text(Rect(rc.x + 22, rc.y, rc.w - 26, rc.h), text, FontId::Label, col, Align::Left);
    } else {
      ui_.Chip(rc, text, col);
    }
    ui_.Tip(rc, tip);
    x -= 8;
  };
  std::string modeText;
  uint32_t modeCol = th.green;
  std::string modeTip;
  if (!backend_ || !backend_->Connected()) {
    modeText = T("Offline");
    modeCol = th.red;
    modeTip = BackendStatus();
  } else if (mode_ == "sim") {
    modeText = T("SIMULATION");
    modeCol = th.amber;
    modeTip = T("A simulated laptop. No hardware is touched.");
  } else if (mode_ == "local") {
    modeText = T("Local (admin)");
    modeCol = th.blue;
    modeTip = T("Running elevated without the service. Closing the window stops hardware control.");
  } else {
    modeText = T("Service");
    modeCol = th.green;
    modeTip = T("Connected to the BaseControl service.");
  }
  chip(modeText, modeCol, true, modeTip);
  if (connected()) {
    const std::string pr = S("profile.active");
    if (!pr.empty()) chip(ProfileLabel(pr), th.accent, false, T("Active profile"));
    if (Has("power.ac")) {
      const bool ac = B("power.ac");
      std::string t = ac ? T("AC power") : T("Battery");
      if (Has("power.batt.pct")) t += " " + Pct(N("power.batt.pct"), 0, true);
      chip(t, ac ? th.muted : th.amber, false, ac ? T("Running on the adapter") : T("Running on battery"));
    }
  }
}

// ------------------------------------------------------------------------------------------------ page + header
namespace {
constexpr float kDockW = 470;
}

void App::DrawPage(const Rect& view0) {
  Gfx& g = ui_.gfx;
  const Theme& th = ui_.th;
  const Meta& m = MetaOf(page_);
  const Group* grp = GroupOf(page_);

  // A page may ask for a docked side panel (today: the advanced CPU registers on "Custom profiles"). It needs room; on a
  // narrow window the page draws the same cards inline instead.
  const bool wantDock = page_ == Page::Custom && Flag("custom.adv");
  docked_ = wantDock && view0.w >= 980;
  Rect view = view0;
  if (docked_) view.w -= kDockW;

  const std::string scrollId = std::string("page#") + PageKey(page_);
  ui_.BeginScroll(scrollId, view);
  ui_.Offset(kPadX, 0);
  const float w = view.w - 2 * kPadX - 6;
  contentW_ = w;

  float y = 26;
  g.Text(Rect(0, y, w, 16), T(m.section), FontId::Label, th.faint);
  y += 20;
  g.Text(Rect(0, y, w, 44), T(m.title), FontId::H1, th.text);
  y += 46;
  const float subH = g.Wrapped(Rect(0, y, std::min(w, 760.0f), 40), T(m.sub), FontId::Body, th.muted, true);
  y += std::max(28.0f, subH + 8);
  if (grp && grp->pages.size() > 1) {
    y += 8;
    float x = 0;
    for (Page p : grp->pages) {
      const std::string label = PageTitle(p);
      const float tw = g.TextW(label, FontId::Label) + 4;
      if (x > 0 && x + tw + 8 > w) {   // tabs wrap on a narrow page
        x = 0;
        y += 36;
      }
      const Rect tab(x, y, tw + 8, 34);
      bool hov = false;
      const bool click = ui_.Click(std::string("tab#") + PageKey(p), tab, true, &hov, nullptr);
      const bool act = p == page_;
      g.Text(Rect(x + 4, y, tw, 32), label, FontId::Label, act ? th.accent : (hov ? th.text : th.muted));
      if (act) g.FillRound(Rect(x, y + 32, tw + 8, 2), 1, th.accent);
      if (click) Go(p);
      x += tw + 34;
    }
    g.Line({0, y + 33.5f}, {w, y + 33.5f}, th.line, 1);
    y += 34;
  }
  y += 22;

  ui_.Offset(0, y);
  float h = 0;
  const Rect pr(0, 0, w, view.h);
  switch (page_) {
    case Page::Overview: h = PageOverview(*this, pr); break;
    case Page::Prefs: h = PagePrefs(*this, pr); break;
    case Page::Devices: h = PageDevices(*this, pr); break;
    case Page::GpuInfo: h = PageGpuInfo(*this, pr); break;
    case Page::Osd: h = PageOsd(*this, pr); break;
    case Page::Keys: h = PageKeys(*this, pr); break;
    case Page::Profiles: h = PageProfiles(*this, pr); break;
    case Page::Fan: h = PageFan(*this, pr); break;
    case Page::Custom: h = PageCustom(*this, pr); break;
    case Page::Apps: h = PageApps(*this, pr); break;
    case Page::Lighting: h = PageLighting(*this, pr); break;
    case Page::Battery: h = PageBattery(*this, pr); break;
    case Page::Display: h = PageDisplay(*this, pr); break;
    case Page::DeviceInfo: h = PageDeviceInfo(*this, pr); break;
    case Page::Safety: h = PageSafety(*this, pr); break;
    case Page::About: h = PageAbout(*this, pr); break;
    default: break;
  }
  ui_.EndScroll(y + h + 48);
  if (docked_) DrawDock(Rect(view0.r() - kDockW, view0.y, kDockW, view0.h));
}

void App::DrawDock(const Rect& r) {
  Gfx& g = ui_.gfx;
  const Theme& th = ui_.th;
  g.Fill(r, th.rail);
  g.Line({r.x + 0.5f, r.y}, {r.x + 0.5f, r.b()}, th.line, 1);
  const float hh = 76;
  g.FillRound(Rect(r.x + 22, r.y + 20, 36, 36), 11, WithAlpha(th.accent, 30));
  g.Icon(IconId::Cpu, Rect(r.x + 29, r.y + 27, 22, 22), th.accent, 1.8f);
  g.Text(Rect(r.x + 70, r.y + 17, r.w - 140, 22), T("Advanced CPU"), FontId::H3, th.text);
  g.Text(Rect(r.x + 70, r.y + 40, r.w - 140, 18), T("ThrottleStop-style registers"), FontId::Small, th.muted);
  const Rect close(r.r() - 56, r.y + 20, 36, 36);
  if (ui_.IconButton("dock#close", close, IconId::Cross, true)) Flag("custom.adv") = false;
  ui_.Tip(close, T("Close the advanced panel"));
  g.Line({r.x, r.y + hh - 0.5f}, {r.r(), r.y + hh - 0.5f}, th.line, 1);

  const Rect view(r.x, r.y + hh, r.w, r.h - hh);
  ui_.BeginScroll("dock#custom", view);
  ui_.Offset(22, 0);
  const float w = view.w - 22 - 28;
  ui_.Offset(0, 20);
  const float h = PageCustomDock(*this, Rect(0, 0, w, view.h));
  ui_.EndScroll(20 + h + 40);
}

// ------------------------------------------------------------------------------------------------ not connected
void App::DrawOffline(const Rect& view) {
  Gfx& g = ui_.gfx;
  const Theme& th = ui_.th;
  static double lastQ = -10;
  static svc::State state = svc::State::Unknown;
  if (ui_.now - lastQ > 1.5) {
    lastQ = ui_.now;
    state = svc::Query();
  }
  ui_.RequestFrame();

  const float w = std::min(640.0f, view.w - 60);
  const Rect box(view.x + (view.w - w) / 2, view.y + std::max(30.0f, (view.h - 430) / 2), w, 0);
  float h = 0;
  const std::string status = BackendStatus();
  std::string head, body;
  std::string primaryLabel;
  std::wstring primaryArgs;
  std::string primaryDone;
  switch (state) {
    case svc::State::NotInstalled:
      head = T("The BaseControl service is not installed");
      body = T("BaseControl talks to the laptop's embedded controller, the CPU registers and the GPU through a small Windows service, so this window never needs administrator rights. Install it once and it starts with Windows.");
      primaryLabel = T("Install and start the service");
      primaryArgs = L"--install-service";
      primaryDone = T("Service installed and started");
      break;
    case svc::State::Stopped:
      head = T("The BaseControl service is stopped");
      body = T("The service is installed but not running. Start it to control power, fans, GPU and lighting.");
      primaryLabel = T("Start the service");
      primaryArgs = L"--start-service";
      primaryDone = T("Service started");
      break;
    case svc::State::Starting:
      head = T("The BaseControl service is starting");
      body = T("Waiting for the service to finish its hardware probe...");
      break;
    default:
      head = T("Connecting to the BaseControl service...");
      body = status.empty() ? T("Waiting for the service pipe.") : status;
      break;
  }
  const float bodyH = g.Wrapped(Rect(0, 0, w - 64, 2000), body, FontId::Body, 0, false);
  h = 40 + 50 + 12 + bodyH + 26 + (primaryLabel.empty() ? 0 : 56) + 74 + 40;
  Rect card(box.x, box.y, w, h);
  ui_.Panel(card);
  g.FillRound(Rect(card.x + 32, card.y + 32, 46, 46), 13, WithAlpha(th.accent, 28));
  g.Icon(IconId::Service, Rect(card.x + 42, card.y + 42, 26, 26), th.accent, 1.8f);
  g.Text(Rect(card.x + 94, card.y + 30, w - 126, 30), head, FontId::H3, th.text);
  g.Text(Rect(card.x + 94, card.y + 58, w - 126, 20), status.empty() ? T("Not connected") : g.Fit(status, FontId::Small, w - 126), FontId::Small, th.faint);
  float y = card.y + 100;
  y += g.Wrapped(Rect(card.x + 32, y, w - 64, 2000), body, FontId::Body, th.muted, true) + 22;
  if (!primaryLabel.empty()) {
    if (ui_.Button("offline#primary", Rect(card.x + 32, y, std::min(w - 64, 330.0f), 40), primaryLabel, Btn::Primary, IconId::Shield, d_->elevProc == nullptr))
      RunElevated(primaryArgs, primaryDone);
    y += 56;
  }
  g.Line({card.x + 32, y}, {card.r() - 32, y}, th.line, 1);
  y += 18;
  g.Text(Rect(card.x + 32, y, w - 64, 18), T("Other ways to run"), FontId::Label, th.faint);
  y += 26;
  const float bw = (w - 64 - 12) / 2;
  if (ui_.Button("offline#local", Rect(card.x + 32, y, bw, 38), T("Run elevated without the service"), Btn::Normal, IconId::Bolt, true)) {
    ShellExecuteW(d_->hwnd, L"runas", hw::SelfPath().c_str(), L"--local", nullptr, SW_SHOWNORMAL);
    Quit();
  }
  if (ui_.Button("offline#sim", Rect(card.x + 44 + bw, y, bw, 38), T("Try the simulator"), Btn::Normal, IconId::Eye, true)) {
    ShellExecuteW(d_->hwnd, L"open", hw::SelfPath().c_str(), L"--sim", nullptr, SW_SHOWNORMAL);
    Quit();
  }
  ui_.Tip(Rect(card.x + 32, y, bw, 38), T("One elevated process does everything. Closing the window ends hardware control."));
  ui_.Tip(Rect(card.x + 44 + bw, y, bw, 38), T("A simulated laptop with a real-time model. Nothing touches your hardware."));
}

// ------------------------------------------------------------------------------------------------ frame
void App::Frame(Gdiplus::Graphics* gr, float scale, const Rect& win) {
  Gfx& g = ui_.gfx;
  g.Begin(gr, scale);
  ui_.Begin(NowSeconds() - d_->t0, win);
  g.Fill(win, ui_.th.base);
  const float railW = win.w < 1000 ? kRailNarrow : kRailWide;
  const Rect rail(win.x, win.y, railW, win.h);
  const Rect main(win.x + railW, win.y, win.w - railW, win.h);
  DrawRail(rail);
  DrawTopBar(main.Top(kTopH));
  const Rect content(main.x, main.y + kTopH, main.w, main.h - kTopH);
  if (!backend_) {
    ui_.Spinner(Rect(content.cx() - 14, content.cy() - 30, 28, 28));
    g.Text(Rect(content.x, content.cy() + 6, content.w, 24), T("Starting..."), FontId::Body, ui_.th.muted, Align::Center);
    ui_.RequestFrame();
  } else if (!backend_->Connected()) {
    DrawOffline(content);
  } else {
    DrawPage(content);
  }
  ui_.End();
  g.End();
  dirty_ = false;
}

}  // namespace ui
}  // namespace bc
