// "Performance profiles": one card per profile (the four built-in ones and the user's own), what switches profiles by itself
// (the power source) and a way to save the live values as a new profile of your own.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "../common/profile_text.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

IconId ProfileIcon(const std::string& n) {
  if (n == "Quiet") return IconId::Moon;
  if (n == "Balanced") return IconId::Performance;
  if (n == "Enthusiast") return IconId::Gamepad;
  if (n == "Overboost") return IconId::Bolt;
  return IconId::Sliders;
}

std::string Tagline(const std::string& n) {
  if (n == "Quiet") return T("Low power and quiet fans. Made for battery and office work.");
  if (n == "Balanced") return T("Balanced power for games and everyday use.");
  if (n == "Enthusiast") return T("High sustained power for demanding games and long renders.");
  if (n == "Overboost") return T("Highest limits and boosted fans. Best on the charger.");
  return T("Your saved profile.");
}

std::string Shown(const std::string& n) { return ProfileLabel(n); }

std::string FanModeText(const std::string& m) {
  if (m == "auto") return T("Auto");
  if (m == "boost") return T("Boost");
  if (m == "custom") return T("Custom curve");
  if (m == "fixed") return T("Fixed speed");
  return m;
}

std::string BattModeText(const std::string& m) { return BattModeLabel(m); }

std::string Intf(const KV& v, const char* key) { return std::to_string(v.Int(key, 0)); }

// Two short lines for the card: the power limits and the fan / temperature settings the profile stores.
void Summary(const KV& v, std::string* power, std::string* other) {
  power->clear();
  other->clear();
  if (v.Has("ec.pl1") && v.Has("ec.pl2") && v.Has("ec.pl4")) *power = Tf("PL1 %s \xC2\xB7 PL2 %s \xC2\xB7 PL4 %s W", Intf(v, "ec.pl1").c_str(), Intf(v, "ec.pl2").c_str(), Intf(v, "ec.pl4").c_str());
  else if (v.Has("ec.pl1")) *power = Tf("PL1 %s W", Intf(v, "ec.pl1").c_str());
  std::vector<std::string> o;
  if (v.Has("fan.mode")) o.push_back(Tf("Fans: %s", FanModeText(v.Str("fan.mode")).c_str()));
  if (v.Has("ec.tcc")) o.push_back(Intf(v, "ec.tcc") + " \xC2\xB0""C");
  if (v.Has("gpu.ctgp")) o.push_back(Intf(v, "gpu.ctgp") + " W GPU");
  *other = Join(o, " \xC2\xB7 ");
  if (power->empty()) power->swap(*other);
}

// Everything the profile stores, one line per value, for the tooltip.
std::string Details(const KV& v) {
  std::vector<std::string> l;
  auto add = [&](const char* key, const std::string& label, const std::string& unit) {
    if (v.Has(key)) l.push_back(label + ": " + Intf(v, key) + (unit.empty() ? "" : " " + unit));
  };
  add("ec.pl1", T("PL1"), "W");
  add("ec.pl2", T("PL2"), "W");
  add("ec.pl4", T("PL4"), "W");
  add("ec.tcc", T("Thermal throttle point"), "\xC2\xB0""C");
  add("gpu.ctgp", T("GPU power (cTGP)"), "W");
  if (v.Has("fan.mode")) l.push_back(T("Fan mode") + ": " + FanModeText(v.Str("fan.mode")));
  if (v.Has("fan.cpu.curve") || v.Has("fan.gpu.curve")) l.push_back(T("Custom fan curves are stored"));
  if (v.Has("batt.mode")) l.push_back(T("Charge profile") + ": " + BattModeText(v.Str("batt.mode")));
  add("os.min", T("Minimum processor state"), "%");
  add("os.max", T("Maximum processor state"), "%");
  add("os.epp", T("Energy preference"), "%");
  add("os.park", T("Core parking"), "%");
  if (v.Has("os.boost")) l.push_back(T("Boost mode stored"));
  if (l.empty()) return T("This profile stores no values.");
  return Join(l, "\n");
}

// True when the live hardware values no longer match what the profile stores (changed by hand, on another page or by another program).
bool Drifted(App& a, const KV& v) {
  for (const char* k : {"ec.pl1", "ec.pl2", "ec.pl4", "ec.tcc", "gpu.ctgp", "os.min", "os.max", "os.epp", "os.park"}) {
    if (!v.Has(k) || !a.Has(k)) continue;
    if (std::lround(a.N(k)) != v.Int(k, 0)) return true;
  }
  for (const char* k : {"fan.mode", "batt.mode", "os.boost"})
    if (v.Has(k) && a.Has(k) && a.S(k) != v.Str(k)) return true;
  return false;
}

constexpr float kCardH = 196;

// One profile card. Returns the action: 0 none, 1 apply, 2 delete.
int ProfileCard(App& a, const Rect& r, const std::string& name, bool active, bool busy) {
  Ui& ui = a.ui();
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const KV vals = ParseProfileValues(a.S("profile.v." + name));
  const bool custom = !IsBuiltinProfile(name);
  const bool drift = active && Drifted(a, vals);

  bool hov = false, held = false;
  const bool click = ui.Click("pc#" + name, r, !busy, &hov, &held);
  const float h = ui.Anim("pch#" + name, hov ? 1.0f : 0.0f);
  const float act = ui.Anim("pca#" + name, active ? 1.0f : 0.0f);

  g.FillRound(r, th.radius, Mix(th.panel, th.elev, h * 0.6f));
  if (act > 0.01f) g.FillGrad(r, th.radius, WithAlpha(th.accent, static_cast<int>(36 * act)), WithAlpha(th.accent, static_cast<int>(6 * act)), 60);
  g.StrokeRound(r, th.radius, Mix(th.line, th.accent, std::max(act, h * 0.35f)), act > 0.5f ? 1.4f : 1.0f);

  // icon tile and the radio mark
  const Rect tile(r.x + 18, r.y + 18, 40, 40);
  g.FillRound(tile, 11, WithAlpha(th.accent, active ? 46 : 26));
  g.Icon(ProfileIcon(name), Rect(tile.x + 8, tile.y + 8, 24, 24), active ? th.accent : th.muted, 1.8f);
  const V2 rc{r.r() - 28, r.y + 30};
  g.Circle(rc, 9, active ? th.accent : th.line, 1.6f);
  if (active) g.Dot(rc, 5, th.accent);

  const std::string shown = Shown(name);
  const float innerW = r.w - 36;
  g.Text(Rect(r.x + 18, r.y + 66, innerW, 26), shown, FontId::H3, th.text);
  if (drift) {
    const std::string t = T("Modified");
    const float cw = ui.ChipW(t);
    const Rect chip(r.r() - 18 - cw, r.y + 66 + 4, cw, 18);
    if (g.TextW(shown, FontId::H3) < innerW - cw - 8) {
      ui.Chip(chip, t, th.amber);
      ui.Tip(chip, T("The live values no longer match this profile. Something was changed by hand, on another page or by another program."));
    }
  }
  bool cut = false;
  g.WrappedClamped(Rect(r.x + 18, r.y + 96, innerW, 40), Tagline(name), FontId::Small, th.muted, 2, 2.0f, &cut);

  std::string power, other;
  Summary(vals, &power, &other);
  g.Text(Rect(r.x + 18, r.y + r.h - 56, innerW - (custom ? 34 : 0), 18), g.Fit(power, FontId::Small, innerW - (custom ? 34 : 0)), FontId::Small, th.text);
  g.Text(Rect(r.x + 18, r.y + r.h - 36, innerW - (custom ? 34 : 0), 18), g.Fit(other, FontId::Small, innerW - (custom ? 34 : 0)), FontId::Small, th.faint);
  ui.Tip(Rect(r.x, r.y, r.w, r.h - (custom ? 40 : 0)), Details(vals));

  int result = click ? 1 : 0;
  if (custom) {
    const Rect del(r.r() - 18 - 30, r.b() - 18 - 30, 30, 30);
    if (ui.IconButton("pd#" + name, del, IconId::Trash, !busy, th.red)) result = 2;
    ui.Tip(del, T("Delete this profile"));
  }
  return result;
}

void ConfirmDelete(App& a, const std::string& name) {
  const Theme& th = a.ui().th;
  ModalSpec ms;
  ms.title = T("Delete this profile?");
  ms.body = Tf("The saved profile \"%s\" will be removed. Your current hardware settings are not changed.", name.c_str());
  ms.icon = IconId::Trash;
  ms.iconColor = th.red;
  ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Delete"), Btn::Danger, 1}};
  App* ap = &a;
  a.ui().ModalOpen(ms, [ap, name](int id) {
    if (id != 1) return;
    KV args;
    args.Set("name", name);
    ap->Cmd("profile.delete", args, Tf("Profile %s deleted", name.c_str()));
  });
}

// Letters (any language), digits, space, - and _ ; 1..32 bytes.
bool NameOk(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c >= 0x80)) return false;
  return true;
}

}  // namespace

std::string ProfileLabel(const std::string& name) {
  if (name == "Quiet") return T("Quiet");
  if (name == "Balanced") return T("Balanced");
  if (name == "Enthusiast") return T("Enthusiast");
  if (name == "Overboost") return T("Overboost");
  return name;
}

IconId ProfileIconOf(const std::string& name) { return ProfileIcon(name); }

uint32_t ProfileColor(const Theme& th, const std::string& name) {
  if (name == "Quiet") return th.blue;
  if (name == "Balanced") return th.green;
  if (name == "Enthusiast") return th.amber;
  if (name == "Overboost") return th.accent;
  return th.muted;
}

float PageProfiles(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  const std::vector<std::string> names = ProfileList(a.snap());
  const std::string active = a.S("profile.active");
  const bool busy = a.Pending("cmd:profile.apply");

  // ------------------------------------------------------------------ profile cards
  const int cols = r.w >= 944 ? 4 : (r.w >= 516 ? 2 : 1);
  const float cw = (r.w - (cols - 1) * kGap) / static_cast<float>(cols);
  const int rows = names.empty() ? 0 : (static_cast<int>(names.size()) + cols - 1) / cols;
  std::string toDelete;
  for (size_t i = 0; i < names.size(); i++) {
    const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
    const Rect cr(col * (cw + kGap), row * (kCardH + kGap), cw, kCardH);
    const int act = ProfileCard(a, cr, names[i], names[i] == active, busy);
    if (act == 1) {
      // clicking the active card applies it once more, which also pulls back values that drifted
      KV args;
      args.Set("name", names[i]);
      a.Cmd("profile.apply", args, Tf("Profile %s applied", Shown(names[i]).c_str()));
    } else if (act == 2) {
      toDelete = names[i];
    }
  }
  if (!toDelete.empty()) ConfirmDelete(a, toDelete);
  const float gridH = rows ? rows * kCardH + (rows - 1) * kGap : 0;

  // ------------------------------------------------------------------ behaviour + save
  Section sec(ui, gridH + (rows ? kGap : 0));
  Masonry m(r.w, 440, 2);
  {
    const bool known = a.Has("power.ac");
    const bool on = a.B("cfg.prof.auto", false);
    const std::string note = T("It reacts when the charger is plugged in or pulled out, so a profile you pick by hand stays until the next change. While a program from Application profiles runs, its profile wins.");
    const float h = kRowToggle + 9 + 2 * kRowToggleLean + 4 + NoteHeight(c, m, note);
    Card k(c, m, T("Profile behaviour"), "", IconId::Plug, h);
    if (k.Toggle("tg#prof.auto", T("Switch by power source"), T("Use another profile on battery and go back when the charger returns."), on, known,
                 known ? "" : T("No battery information is available on this system."))) {
      a.Cfg("cfg.prof.auto", on ? "0" : "1");
    }
    k.Divider();
    std::vector<std::string> shown, shownAc;
    shownAc.push_back(T("The profile from before"));
    for (const std::string& n : names) {
      shown.push_back(Shown(n));
      shownAc.push_back(Shown(n));
    }
    const std::string batt = a.S("cfg.prof.batt", "Quiet");
    int curB = 0;
    for (size_t i = 0; i < names.size(); i++)
      if (names[i] == batt) curB = static_cast<int>(i);
    const int pb = k.ComboRow("cb#prof.batt", T("Profile on battery"), "", shown, curB, on && !names.empty(), 230);
    if (pb >= 0 && pb < static_cast<int>(names.size()) && names[static_cast<size_t>(pb)] != batt) a.Cfg("cfg.prof.batt", names[static_cast<size_t>(pb)]);
    const std::string onAc = a.S("cfg.prof.ac", "");
    int curA = 0;
    for (size_t i = 0; i < names.size(); i++)
      if (names[i] == onAc) curA = static_cast<int>(i) + 1;
    const int pa = k.ComboRow("cb#prof.ac", T("Profile on charger"), "", shownAc, curA, on && !names.empty(), 230);
    if (pa >= 0 && pa != curA) a.Cfg("cfg.prof.ac", pa == 0 ? "" : names[static_cast<size_t>(pa) - 1]);
    k.Gap(4);
    k.Note(note);
  }
  {
    std::string& nm = a.Str("prof.newname");
    const std::string trimmed = Trim(nm);
    const bool valid = NameOk(trimmed) && !IsBuiltinProfile(trimmed);
    const bool exists = std::find(names.begin(), names.end(), trimmed) != names.end();
    const std::string note = T("A saved profile stores the live power limits, temperature limit, GPU power, fan mode and curves, Windows power values and the charge mode.");
    std::string hint;
    uint32_t hintCol = ui.th.muted;
    if (!trimmed.empty() && !valid) {
      hint = IsBuiltinProfile(trimmed) ? T("That name belongs to a built-in profile.") : T("Use letters, digits, spaces, - and _ (up to 32 characters).");
      hintCol = ui.th.amber;
    } else if (exists) {
      hint = T("A profile with this name exists. Saving replaces its values.");
      hintCol = ui.th.amber;
    }
    const float hintH = hint.empty() ? 0.0f : NoteHeight(c, m, hint);
    const float h = kRowInfo + kRowButtons + hintH + 6 + NoteHeight(c, m, note);
    Card k(c, m, T("Save the current settings"), "", IconId::Profiles, h);
    k.Info(T("Currently active"), active.empty() ? T("None (set by hand)") : Shown(active), ui.th.accent);
    if (k.InputRow("prof.save", &nm, T("Name for the new profile"), exists ? T("Replace") : T("Save"), valid && !a.Pending("cmd:profile.save"), 32)) {
      KV args;
      args.Set("name", trimmed);
      a.Cmd("profile.save", args, Tf("Profile %s saved", trimmed.c_str()));
      nm.clear();
    }
    if (!hint.empty()) k.Note(hint, hintCol);
    k.Gap(6);
    k.Note(note);
  }
  return gridH + (rows ? kGap : 0) + m.Bottom();
}

}  // namespace ui
}  // namespace bc
