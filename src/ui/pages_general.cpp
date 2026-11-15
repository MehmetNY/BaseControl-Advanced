// General settings group, part 1: Preferences, Device manager and GPU settings. (The in-game overlay and the key mapping page
// live in pages_osd.cpp / pages_keys.cpp.)
#include <algorithm>
#include <climits>
#include <cmath>

#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

std::string Dash() { return "\xE2\x80\x94"; }

// "RTX 3070 Laptop GPU" is enough on a small box; the full name stays in the tooltip.
std::string ShortGpu(std::string n) {
  for (const char* p : {"NVIDIA GeForce ", "NVIDIA ", "AMD ", "Intel(R) ", "Intel "}) {
    const std::string pre = p;
    if (StartsWith(n, pre) && n.size() > pre.size() + 3) {
      n = n.substr(pre.size());
      break;
    }
  }
  return n;
}

bool IsDiscrete(const std::string& n) {
  const std::string l = Lower(n);
  return l.find("nvidia") != std::string::npos || l.find("geforce") != std::string::npos || l.find("rtx") != std::string::npos ||
         l.find("radeon rx") != std::string::npos || l.find("radeon pro") != std::string::npos;
}

// One box of the display-path drawing: small icon, two-line name.
void PathBox(Ui& ui, const Rect& r, IconId icon, const std::string& name, bool lit) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  g.FillRound(r, 11, Mix(th.inset, th.elev, 0.5f));
  g.StrokeRound(r, 11, lit ? WithAlpha(th.accent, 120) : th.line, 1.0f);
  g.Icon(icon, Rect(r.cx() - 11, r.y + 10, 22, 22), lit ? th.accent : th.muted, 1.7f);
  g.Wrapped(Rect(r.x + 8, r.y + 38, r.w - 16, r.h - 42), name, FontId::Small, th.text);
  ui.Tip(r, name);
}

// Radio mark of the graphics-mode cards.
void Radio(Gfx& g, V2 c, bool on, uint32_t accent, uint32_t line) {
  g.Circle(c, 9, on ? accent : line, 1.6f);
  if (on) g.Dot(c, 5, accent);
}

std::string ThrottleText(App& a) {
  std::vector<std::string> v;
  if (a.B("gpu.thr.power")) v.push_back(T("power limit"));
  if (a.B("gpu.thr.thermal")) v.push_back(T("temperature"));
  if (a.B("gpu.thr.hwslow")) v.push_back(T("hardware slowdown"));
  if (a.B("gpu.thr.idle")) v.push_back(T("idle"));
  if (v.empty()) return a.Has("gpu.throttle") ? T("None") : Dash();
  return Join(v, ", ");
}

}  // namespace

std::vector<int> RefreshRates(App& a) {
  std::vector<int> out;
  for (const std::string& p : Split(a.S("disp.refresh.list"), ',')) {
    long long n = 0;
    if (ParseInt(Trim(p), n) && n >= 30 && n <= 480) out.push_back(static_cast<int>(n));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// =================================================================================================== Preferences
float PagePrefs(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  const Theme& th = ui.th;
  Masonry m(r.w, 440, 2);

  // ---- keyboard and input
  {
    const std::string note = T("Windows key lock, Num Lock lock and the touchpad corner work while Control Center Advanced is running; the tray is enough.");
    const float h = 4 * kRowToggle + 3 * 9 + NoteHeight(c, m, note);
    Card k(c, m, T("Keyboard and input"), "", IconId::Keyboard, h);
    if (k.PrefRow("keys.winlock", T("Windows key lock"), T("Stop the Start menu from opening by accident."), false, a.SessionLive(),
                  a.SessionLive() ? "" : T("Needs the keyboard hook, which only runs in the real app."))) a.ApplySession();
    k.Divider();
    if (k.PrefRow("keys.numlock", T("Num Lock lock"), T("Keep Num Lock on; the key stops switching it off."), false, a.SessionLive(),
                  a.SessionLive() ? "" : T("Needs the keyboard hook, which only runs in the real app."))) a.ApplySession();
    k.Divider();
    k.BoolRow("pref.fnlock", T("Fn lock (F1\xE2\x80\x93""F12)"), T("Use F1\xE2\x80\x93""F12 as normal function keys without holding Fn."));
    k.Divider();
    const bool tp = a.TouchpadGesture();
    if (k.PrefRow("keys.tpcorner", T("Touchpad corner shortcut"), T("Double-tap the top-left corner to turn the touchpad on or off."), false, tp,
                  tp ? "" : T("Needs a precision touchpad and the real app."))) a.ApplySession();
    k.Note(note);
  }

  // ---- power and start-up
  {
    const std::string note = T("USB power and automatic power-on cannot be active together. Switching one on switches the other off.");
    const float nh = ui.gfx.Wrapped(Rect(0, 0, m.InnerW() - 42, 3000), note, FontId::Small, 0, false) + 16;
    const float h = 2 * kRowToggle + 9 + 6 + nh;
    Card k(c, m, T("Power and start-up"), "", IconId::Plug, h);
    k.BoolRow("pref.usbpower", T("USB power when off"), T("Keep USB ports powered in standby and when shut down."));
    k.Divider();
    k.BoolRow("pref.acboot", T("Power on with the adapter"), T("Turn the laptop on when the adapter is plugged in."));
    k.Gap(6);
    const bool both = a.B("pref.usbpower") && a.B("pref.acboot");
    const Rect nb(k.x0, k.y, k.w, nh);
    ui.gfx.FillRound(nb, 10, WithAlpha(th.amber, both ? 44 : 22));
    ui.gfx.StrokeRound(nb, 10, WithAlpha(th.amber, both ? 150 : 70), 1.0f);
    ui.gfx.Icon(IconId::Info, Rect(nb.x + 10, nb.y + 9, 14, 14), th.amber, 1.7f);
    ui.gfx.Wrapped(Rect(nb.x + 32, nb.y + 6, nb.w - 42, nb.h - 6), note, FontId::Small, th.amber);
  }

  // ---- appearance
  {
    const float h = kRowSeg + kRowSwatch + 9;
    Card k(c, m, T("Appearance"), "", IconId::Sun, h);
    const std::string lang = a.Pref("lang", "auto");
    const int cur = lang == "en" ? 1 : (lang == "tr" ? 2 : 0);
    const int pick = k.Segment("seg#lang", T("Language"), T("Applies at once. The EN | TR switch in the top bar changes the same setting."),
                               {T("Automatic"), "English", "T\xC3\xBCrk\xC3\xA7""e"}, cur);
    if (pick >= 0 && pick != cur) {
      a.SetPref("lang", pick == 1 ? "en" : (pick == 2 ? "tr" : "auto"));
      a.ApplyLanguage();
    }
    k.Divider();
    int accN = 0;
    const AccentDef* accs = Accents(&accN);
    std::vector<uint32_t> cols;
    int curA = 0;
    const std::string acc = NormalAccent(a.Pref("accent", kDefaultAccent));
    for (int i = 0; i < accN; i++) {
      cols.push_back(accs[i].col);
      if (acc == accs[i].key) curA = i;
    }
    const int pa = k.Swatches("acc", T("Accent colour"), cols, curA);
    if (pa >= 0 && pa < accN) {
      a.SetPref("accent", accs[pa].key);
      a.ApplyAccent();
    }
  }

  // ---- application behaviour
  {
    const float h = 5 * kRowToggle + 4 * 9;
    Card k(c, m, T("Application behaviour"), "", IconId::Settings, h);
    // Asking the Task Scheduler runs a helper process, so the answer is kept for half a minute (and refreshed right after a change).
    int& cached = a.Int("autostart.val", -1);
    int& stamp = a.Int("autostart.t", -100);
    const int nowS = static_cast<int>(ui.now);
    if (cached < 0 || nowS - stamp >= 30) {
      cached = a.Autostart() ? 1 : 0;
      stamp = nowS;
    }
    if (k.Toggle("tg#autostart", T("Start with Windows"), T("Starts hidden in the tray when you sign in, and re-applies your settings."), cached == 1, true,
                 T("Windows does not start programs that need administrator rights from its start-up list, so this creates a scheduled task that starts Control Center Advanced at sign-in with the highest rights of your account. No prompt appears, so the task is only created for the copy in Program Files, where only administrators can change files."))) {
      a.SetAutostart(cached != 1);   // a toast says why when it did not work
      cached = a.Autostart() ? 1 : 0;
      stamp = static_cast<int>(ui.now);
    }
    k.Divider();
    if (k.PrefRow("tray.icon", T("Tray icon"), T("Show Control Center Advanced in the notification area."), true)) a.ApplySession();
    k.Divider();
    k.PrefRow("tray", T("Close to the tray"), T("The close button hides the window; Control Center Advanced keeps running."), true);
    k.Divider();
    k.PrefRow("tray.min", T("Minimise to the tray"), T("The minimise button hides the window instead of using the taskbar."), false);
    k.Divider();
    k.PrefRow("notify", T("Shortcut notifications"), T("Show a short message when a key action runs (mute, profile change ...)."), true);
  }

  return m.Bottom();
}

// =================================================================================================== Device manager
float PageDevices(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Masonry m(r.w, 400, 2);

  {
    Card k(c, m, T("Wireless"), "", IconId::Wifi, 2 * kRowToggle + 9);
    k.BoolRow("dev.wifi", T("Wi-Fi"), T("The wireless network adapter."));
    k.Divider();
    k.BoolRow("dev.bt", T("Bluetooth"), T("Headphones and other wireless devices."));
  }
  {
    Card k(c, m, T("Built-in devices"), "", IconId::Camera, 2 * kRowToggle + 9);
    k.BoolRow("dev.cam", T("Camera"), T("The built-in webcam."));
    k.Divider();
    k.BoolRow("dev.touchpad", T("Touchpad"), T("The built-in pointing device."));
  }
  {
    Card k(c, m, T("Built-in display"), "", IconId::Display, kRowSlider + kRowInfo + kRowButtons, true);
    const std::string res = a.S("disp.res");
    std::string panel = res.empty() ? Dash() : res;
    if (a.Has("disp.refresh")) panel += " @ " + std::to_string(a.I("disp.refresh")) + " Hz";
    SliderCfg br;
    br.key = "disp.bright";
    br.title = T("Screen brightness");
    br.desc = T("The backlight of the built-in screen, the same control as the Windows brightness slider.");
    k.Slider(br);
    k.Info(T("Panel"), panel);
    if (k.Button("go#display", T("Open display settings"), Btn::Ghost, IconId::ChevronR)) a.Go(Page::Display);
  }
  (void)ui;
  return m.Bottom();
}

// =================================================================================================== GPU settings
float PageGpuInfo(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  float y = 0;

  // adapters Windows lists (the integrated one is whichever is not an NVIDIA / Radeon RX part)
  std::string dgpu = a.S("gpu.name"), igpu;
  int adapters = 0;
  for (const std::string& n : Split(a.S("host.gpus"), ';')) {
    if (Trim(n).empty()) continue;
    adapters++;
    if (IsDiscrete(n)) {
      if (dgpu.empty()) dgpu = n;
    } else if (igpu.empty()) {
      igpu = n;
    }
  }
  const bool hybrid = adapters >= 2 || (!igpu.empty() && !dgpu.empty());
  const bool canSwitch = a.CapOk("gpu.mux");

  // ---- graphics mode (two cards)
  {
    Section sec(ui, y);
    Masonry mm(r.w, 360, 2);
    struct Mode {
      IconId icon;
      std::string title, desc;
      bool selected;
    } modes[2] = {
        {IconId::Chip, T("Hybrid graphics"), T("The picture goes through the integrated graphics. The NVIDIA GPU joins in when a program needs it."), hybrid},
        {IconId::Gpu, T("Discrete GPU"), T("The built-in screen is driven straight by the NVIDIA GPU."), !hybrid}};
    for (const Mode& md : modes) {
      Card k(c, mm, "", "", IconId::None, 100);
      const Rect b = k.box;
      if (md.selected) g.FillGrad(b.Inset(1), th.radius, WithAlpha(th.accent, 26), WithAlpha(th.accent, 0), 30);
      g.StrokeRound(b, th.radius, md.selected ? WithAlpha(th.accent, 150) : th.line, md.selected ? 1.6f : 1.0f);
      g.FillRound(Rect(k.x0, k.y, 38, 38), 10, WithAlpha(md.selected ? th.accent : th.muted, 30));
      g.Icon(md.icon, Rect(k.x0 + 7, k.y + 7, 24, 24), md.selected ? th.accent : th.muted, 1.7f);
      Radio(g, {b.r() - kPad - 9, k.y + 19}, md.selected, th.accent, th.faint);
      g.Text(Rect(k.x0, k.y + 46, k.w - 4, 24), md.title, FontId::H3, th.text);
      g.WrappedClamped(Rect(k.x0, k.y + 72, k.w, 34), md.desc, FontId::Small, th.muted, 2, 2.0f);
      const Rect hit = Rect(b.x, b.y, b.w, b.h);
      if (!canSwitch) {
        if (!md.selected) {
          const std::string chip = T("Not available");
          const float cw = ui.ChipW(chip);
          ui.Chip(Rect(b.r() - kPad - 36 - cw, k.y + 9, cw, 20), chip, th.amber);
        }
        ui.Tip(hit, T("Control Center Advanced found no verified graphics-mode (MUX) switch for this model, so it only shows the mode Windows reports. Nothing here can change what drives the screen."));
      }
    }
    y += mm.Bottom() + kGap;
  }

  // ---- the rest in a masonry
  {
    Section sec(ui, y);
    Masonry m(r.w, 440, 2);

    // display path
    {
      Card k(c, m, T("Display path"), "", IconId::Display, 96);
      const std::string chip = hybrid ? T("Hybrid") : T("Discrete");
      const float cw = ui.ChipW(chip);
      ui.Chip(Rect(k.box.r() - kPad - cw, k.box.y + kPad + 7, cw, 20), chip, th.accent);
      const float arrow = 26;
      const bool three = hybrid;
      const int n = three ? 3 : 2;
      const float bw = (k.w - arrow * static_cast<float>(n - 1)) / static_cast<float>(n);
      float x = k.x0;
      const std::string d = dgpu.empty() ? T("NVIDIA GPU") : ShortGpu(dgpu);
      const std::string i = igpu.empty() ? T("Integrated graphics") : ShortGpu(igpu);
      std::vector<std::pair<IconId, std::string>> boxes;
      boxes.push_back({IconId::Gpu, d});
      if (three) boxes.push_back({IconId::Chip, i});
      boxes.push_back({IconId::Display, T("Built-in screen")});
      for (size_t b = 0; b < boxes.size(); b++) {
        PathBox(ui, Rect(x, k.y, bw, 92), boxes[b].first, boxes[b].second, b == 0);
        x += bw;
        if (b + 1 < boxes.size()) {
          g.Icon(IconId::ChevronR, Rect(x + arrow * 0.5f - 7, k.y + 38, 14, 14), th.faint, 1.8f);
          x += arrow;
        }
      }
    }

    // refresh behaviour
    {
      const std::vector<int> rates = RefreshRates(a);
      Card k(c, m, T("Refresh behaviour"), "", IconId::Refresh, kRowSlider + 2 * kRowToggle + 2 * 9);
      k.ListConfirm("disp.refresh", T("Refresh rate"), T("Applies to the built-in panel. The screen may flicker for a moment."), rates);
      k.Divider();
      k.PrefRow("disp.auto.batt", T("Lower the rate on battery"), T("Use the lowest rate unplugged and go back to your choice on AC."), false, !rates.empty());
      k.Divider();
      k.PrefRow("disp.auto.mode", T("Follow the performance mode"), T("Office mode uses the lowest rate; the other modes use the highest."), false, !rates.empty());
    }

    // GPU status
    {
      Card k(c, m, T("GPU status"), a.Has("gpu.name") ? a.S("gpu.name") : "", IconId::Gpu, 9 * kRowInfo);
      k.Info(T("Temperature"), a.Has("gpu.temp") ? With(a.N("gpu.temp"), 0, "\xC2\xB0""C") : Dash());
      k.Info(T("Load"), a.Has("gpu.util") ? With(a.N("gpu.util"), 0, "%") : Dash());
      std::string core = a.Has("gpu.clock") ? With(a.N("gpu.clock"), 0, "MHz") : Dash();
      if (a.Has("gpu.clock") && a.Has("gpu.clock.max")) core += " / " + With(a.N("gpu.clock.max"), 0, "MHz");
      k.Info(T("Core clock"), core, 0, T("Current / maximum boost clock"));
      k.Info(T("Memory clock"), a.Has("gpu.memclock") ? With(a.N("gpu.memclock"), 0, "MHz") : Dash());
      std::string vram = Dash();
      if (a.Has("gpu.vram.used") && a.Has("gpu.vram.total"))
        vram = Tf("%.1f / %.1f GB", a.N("gpu.vram.used") / 1024.0, a.N("gpu.vram.total") / 1024.0);
      k.Info(T("Video memory"), vram);
      std::string pw = a.Has("gpu.power") ? With(a.N("gpu.power"), 0, "W") : Dash();
      if (a.Has("gpu.power") && a.Has("gpu.limit")) pw += " / " + With(a.N("gpu.limit"), 0, "W");
      k.Info(T("Power"), pw, 0, T("Draw / driver power limit"));
      k.Info(T("Performance state"), a.Has("gpu.pstate") ? "P" + std::to_string(a.I("gpu.pstate")) : Dash());
      k.Info(T("Total graphics power"), a.Has("gpu.ctgp") ? With(a.N("gpu.ctgp"), 0, "W") : Dash(), 0, T("cTGP: the GPU power budget the firmware allows"));
      k.Info(T("Limited by"), ThrottleText(a));
    }

    // Clock offsets: read-only here, changed in Custom mode
    {
      Card k(c, m, T("Clock offsets"), "", IconId::Bolt, 2 * kRowInfo + 8 + kRowButtons);
      k.Info(T("Core clock offset"), a.Has("perf.gpu.core") ? With(a.N("perf.gpu.core"), 0, "MHz") : Dash(), 0,
             T("What the graphics driver applies now. It is changed in Performance \xE2\x80\xA2 Custom mode."));
      k.Info(T("Memory clock offset"), a.Has("perf.gpu.mem") ? With(a.N("perf.gpu.mem"), 0, "MHz") : Dash());
      k.Gap(8);
      if (k.Button("go#custom", T("Open custom mode"), Btn::Ghost, IconId::ChevronR)) a.Go(Page::Custom);
    }

    y += m.Bottom() + kGap;
  }

  // ---- honest note
  {
    Section sec(ui, y);
    const std::string note = T("A graphics-mode change would need a restart on real hardware. Control Center Advanced does not offer one for this model because the firmware control is not verified; it never touches how the screen is wired.");
    const float h = g.Wrapped(Rect(0, 0, r.w - 56, 3000), note, FontId::Small, 0, false) + 20;
    const Rect nb(0, 0, r.w, h);
    g.FillRound(nb, 12, WithAlpha(th.amber, 22));
    g.StrokeRound(nb, 12, WithAlpha(th.amber, 80), 1.0f);
    g.Icon(IconId::Info, Rect(14, 13, 16, 16), th.amber, 1.7f);
    g.Wrapped(Rect(40, 9, r.w - 56, h - 8), note, FontId::Small, th.amber);
    y += h;
  }
  return y;
}

}  // namespace ui
}  // namespace bc
