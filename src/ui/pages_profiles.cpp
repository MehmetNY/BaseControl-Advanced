// "Performance modes": the four modes of the vendor's Control Center (Office, Gaming, Turbo, Custom) as cards, and the fan boost. Switching a
// mode is done by the vendor's own service, exactly as in the original window; this page only asks and then shows what really changed.
#include <algorithm>
#include <cmath>

#include "../common/profile_text.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

IconId ProfileIcon(const std::string& n) {
  if (n == "Balanced") return IconId::Leaf;
  if (n == "Enthusiast") return IconId::Gamepad;
  if (n == "Overboost") return IconId::Bolt;
  return IconId::Sliders;
}

std::string Tagline(const std::string& n) {
  if (n == "Balanced") return T("Everyday work: moderate power and calm fans.");
  if (n == "Enthusiast") return T("Games and heavy work: high sustained power.");
  if (n == "Overboost") return T("The highest limits of this laptop. Best on the charger.");
  return T("Your own limits for power, temperature and the GPU (see Custom mode).");
}

std::string FanModeText(const std::string& m) {
  if (m == "auto") return T("Auto");
  if (m == "boost") return T("Turbo fan");
  if (m == "custom") return T("Custom curve");
  if (m == "fixed") return T("Fixed speed");
  return m;
}

// The vendor's own name for the mode, for the tooltip.
std::string VendorName(const std::string& n) {
  if (n == "Balanced") return "OperatingMode 0 (Balanced)";
  if (n == "Enthusiast") return "OperatingMode 1 (Enthusiast)";
  if (n == "Overboost") return "OperatingMode 2 (Overboost)";
  return "OperatingMode 3 (Customized)";
}

constexpr float kCardH = 176;

std::string Dash() { return "\xE2\x80\x94"; }

// Two short lines: what the active mode runs right now (live from the embedded controller where it can be read).
void LiveSummary(App& a, std::string* power, std::string* other) {
  power->clear();
  other->clear();
  if (a.Has("perf.pl1") && a.Has("perf.pl2")) {
    *power = Tf("PL1 %s \xC2\xB7 PL2 %s W", Num(a.N("perf.pl1")).c_str(), Num(a.N("perf.pl2")).c_str());
    if (a.Has("perf.pl4")) *power = Tf("PL1 %s \xC2\xB7 PL2 %s \xC2\xB7 PL4 %s W", Num(a.N("perf.pl1")).c_str(), Num(a.N("perf.pl2")).c_str(), Num(a.N("perf.pl4")).c_str());
  }
  if (a.Has("fan.mode")) *other = Tf("Fans: %s", FanModeText(a.S("fan.mode")).c_str());
  if (power->empty()) power->swap(*other);
}

// One mode card. Returns true when it was clicked.
bool ModeCard(App& a, const Rect& r, const std::string& name, bool active, bool enabled) {
  Ui& ui = a.ui();
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  bool hov = false, held = false;
  const bool click = ui.Click("pc#" + name, r, enabled, &hov, &held);
  const float h = ui.Anim("pch#" + name, hov ? 1.0f : 0.0f);
  const float act = ui.Anim("pca#" + name, active ? 1.0f : 0.0f);

  g.FillRound(r, th.radius, Mix(th.panel, th.elev, h * 0.6f));
  if (act > 0.01f) g.FillGrad(r, th.radius, WithAlpha(th.accent, static_cast<int>(40 * act)), WithAlpha(th.accent, static_cast<int>(8 * act)), 60);
  g.StrokeRound(r, th.radius, Mix(th.line, th.accent, std::max(act, h * 0.35f)), act > 0.5f ? 1.6f : 1.0f);

  const uint32_t tileCol = ProfileColor(th, name);
  const Rect tile(r.x + 18, r.y + 18, 40, 40);
  g.FillRound(tile, 11, WithAlpha(tileCol, active ? 70 : 40));
  g.Icon(ProfileIcon(name), Rect(tile.x + 8, tile.y + 8, 24, 24), active ? tileCol : Mix(tileCol, th.muted, 0.35f), 1.8f);
  const V2 rc{r.r() - 28, r.y + 30};
  g.Circle(rc, 9, active ? th.accent : th.line, 1.6f);
  if (active) g.Dot(rc, 5, th.accent);

  const float innerW = r.w - 36;
  g.Text(Rect(r.x + 18, r.y + 66, innerW, 26), ProfileLabel(name), FontId::H3, enabled ? th.text : th.faint);
  bool cut = false;
  g.WrappedClamped(Rect(r.x + 18, r.y + 96, innerW, 40), Tagline(name), FontId::Small, th.muted, 2, 2.0f, &cut);

  if (active) {
    std::string power, other;
    LiveSummary(a, &power, &other);
    g.Text(Rect(r.x + 18, r.y + r.h - 40, innerW, 18), g.Fit(power, FontId::Small, innerW), FontId::Small, th.text);
    if (!other.empty()) g.Text(Rect(r.x + 18, r.y + r.h - 22, innerW, 16), g.Fit(other, FontId::Small, innerW), FontId::Small, th.faint);
  } else {
    g.Text(Rect(r.x + 18, r.y + r.h - 32, innerW, 16), g.Fit(T("Click to switch"), FontId::Small, innerW), FontId::Small, th.faint);
  }
  ui.Tip(r, enabled ? VendorName(name) : T("The vendor's service is not reachable, so the mode cannot be changed. See the Service page."));
  return click;
}

}  // namespace

std::string ProfileLabel(const std::string& name) {
  if (name == "Balanced") return T("Office");
  if (name == "Enthusiast") return T("Gaming");
  if (name == "Overboost") return T("Turbo");
  if (name == "Customized") return T("Custom");
  return name;
}

IconId ProfileIconOf(const std::string& name) { return ProfileIcon(name); }

uint32_t ProfileColor(const Theme& th, const std::string& name) {
  if (name == "Balanced") return th.green;
  if (name == "Enthusiast") return th.amber;
  if (name == "Overboost") return th.red;
  if (name == "Customized") return th.blue;
  return th.muted;
}

float PageProfiles(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  const std::vector<std::string>& names = BuiltinProfiles();
  const std::string active = a.S("profile.active");
  const bool enabled = a.CapOk("fan.ctl") && !a.Pending("cmd:profile.apply");

  // ------------------------------------------------------------------ the four mode cards
  const int cols = r.w >= 944 ? 4 : (r.w >= 516 ? 2 : 1);
  const float cw = (r.w - (cols - 1) * kGap) / static_cast<float>(cols);
  const int rows = (static_cast<int>(names.size()) + cols - 1) / cols;
  for (size_t i = 0; i < names.size(); i++) {
    const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
    const Rect cr(col * (cw + kGap), row * (kCardH + kGap), cw, kCardH);
    if (ModeCard(a, cr, names[i], names[i] == active, enabled)) {
      // clicking the active card asks again, which is harmless and shows whether the service still answers
      KV args;
      args.Set("name", names[i]);
      a.Cmd("profile.apply", args, Tf("Mode %s applied", ProfileLabel(names[i]).c_str()));
    }
  }
  const float gridH = rows * kCardH + (rows - 1) * kGap;

  // ------------------------------------------------------------------ fan boost and what runs now
  Section sec(ui, gridH + kGap);
  Masonry m(r.w, 440, 2);
  {
    const std::string note = T("Both fans at full speed until you switch it off. It does not change the power limits.");
    const float h = kRowToggle + 9 + NoteHeight(c, m, note);
    Card k(c, m, T("Turbo fan"), "", IconId::Fan, h);
    const bool on = a.S("fan.mode") == "boost";
    const bool can = a.CapOk("fan.ctl") && a.Has("fan.mode") && !a.Pending("fan.mode");
    if (k.Toggle("tg#fanboost", T("Turbo fan"), T("Run the fans at maximum speed."), on, can,
                 can ? "" : T("The vendor's service is not reachable. See the Service page."))) {
      if (!on) {
        a.Str("fan.prev") = a.S("fan.mode");   // what to go back to
        a.Set("fan.mode", "boost", T("Turbo fan on"));
      } else {
        std::string prev = a.Str("fan.prev");
        if (prev != "custom") prev = "auto";
        a.Set("fan.mode", prev, T("Turbo fan off"));
      }
    }
    k.Gap(4);
    k.Note(note);
  }
  {
    const std::string note = T("The modes are switched by the vendor's Control Center service, exactly as in the original window. Your own limits live on the Custom mode page.");
    const float h = 4 * kRowInfo + 6 + NoteHeight(c, m, note);
    Card k(c, m, T("Right now"), "", IconId::Info, h);
    k.Info(T("Active mode"), active.empty() ? Dash() : ProfileLabel(active), ui.th.accent);
    std::string power, other;
    LiveSummary(a, &power, &other);
    k.Info(T("Power limits"), a.Has("perf.pl1") ? Tf("PL1 %s \xC2\xB7 PL2 %s \xC2\xB7 PL4 %s", Num(a.N("perf.pl1")).c_str(), Num(a.N("perf.pl2")).c_str(),
                                                    a.Has("perf.pl4") ? Num(a.N("perf.pl4")).c_str() : "\xE2\x80\x94") + " W"
                                                : Dash());
    k.Info(T("Fan mode"), a.Has("fan.mode") ? FanModeText(a.S("fan.mode")) : Dash());
    k.Info(T("Mode name in the service"), a.S("perf.profile.name").empty() ? Dash() : a.S("perf.profile.name"));
    k.Gap(6);
    k.Note(note);
  }
  return gridH + kGap + m.Bottom();
}

}  // namespace ui
}  // namespace bc
