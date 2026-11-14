// About: which version this is and what it was compared against, the language, the projects Control Center Advanced learned from (each tile opens
// the project's page), and the plain statements about warranty and independence.
// Nothing here touches the hardware; the language is the same preference the General settings page and the top bar change.
#include <algorithm>

#include "../common/util.h"
#include "../common/version.h"
#include "page.h"

namespace bc {
namespace ui {
namespace {

constexpr const char* kOemReference = "4.9.47.11";   // the OEM Control Center release the feature list was compared against

struct Credit {
  std::string name, licence, url, desc;
  int kind = 0;   // 0 = free licence (green), 1 = no licence stated (amber), 2 = not included (grey)
};

// Everything Control Center Advanced learned from. Licences were read from the projects' own files.
std::vector<Credit> Credits() {
  const std::string readOnly = T("Read as a reference only.");
  return {
      {"OEM Control Center " + std::string(kOemReference), T("Not included"), "",
       T("The feature list Control Center Advanced is compared against. No OEM code, driver or file is included or copied."), 2},
      {"MONSTER GPU POWER", "GPL-3.0", "https://github.com/MehmetNY/Monster-XMG-Gpu-Power-Control",
       T("The author's own GPU power project. Its engine drives the GPU power (cTGP) control and the OEM profile sync."), 0},
      {"RevoHelper", "MIT", "https://github.com/AstralMemory/RevoHelper",
       T("A light, free control app for a sibling laptop brand: a model for a smaller and faster alternative to an OEM control center."), 0},
      {"MechControl", T("No licence stated"), "https://github.com/Liddo-kun/MechControl-RE",
       T("A native Windows replacement for the OEM app on Uniwill-built laptops.") + " " + readOnly, 1},
      {"tongfang-gm7mg7p-re", T("No licence stated"), "https://github.com/ElDavoo/tongfang-gm7mg7p-re",
       T("Reverse-engineering notes for the embedded controller of a sibling laptop.") + " " + readOnly, 1},
      {"Unbound-Power", T("No licence stated"), "https://github.com/Heavyfail/Unbound-Power", T("A GPU power-unlock tool.") + " " + readOnly, 1},
      {"tongfang-uniwill-dkms / tuxedo-drivers", "GPL-2.0+", "https://github.com/arbitrary-string/tongfang-uniwill-dkms",
       T("Linux drivers for the Uniwill embedded controller: what its registers mean and how the keyboard light behaves."), 0},
      {"clevo-control-panel", "GPL-3.0", "https://github.com/arbitrary-string/clevo-control-panel",
       T("Keyboard light, charge limit and performance-mode ideas for Clevo and Tongfang laptops."), 0},
      {"TUXEDO Control Center", "GPL-3.0", "https://github.com/arbitrary-string/tuxedo-control-center-archive",
       T("Profiles, fan curves and settings of a mature control center for the same laptop family."), 0},
      {"HydroControl", "MIT", "https://github.com/Gumwars/HydroControl",
       T("A Linux control center for a sibling laptop: its notes on the shared embedded controller and on guarding writes."), 0},
      {"PawnIO", "GPL-2.0 / LGPL-2.1", "https://github.com/namazso/PawnIO",
       T("The signed driver that gives the advanced CPU panel access to processor registers. Not included: you install it yourself."), 0},
  };
}

constexpr float kTileGap = 12;
constexpr float kTilePad = 14;

float TileHeight(Ctx& c, float w, const Credit& cr) {
  return kTilePad + 24 + 4 + c.ui.gfx.Wrapped(Rect(0, 0, w - 2 * kTilePad, 3000), cr.desc, FontId::Small, 0, false) + kTilePad;
}

void CardApp(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  const std::string winVer = BC_VERSION;
  const std::string svcVer = a.S("app.version");
  const bool mismatch = !svcVer.empty() && svcVer != winVer;   // only a service installed from another build can differ: the window and the in-process controller share one binary
  const std::string note = T("The service was installed from another version than this window. Restart the service, or restart Windows, so both match.");

  const std::vector<BtnSpec> btns = {{"ab#site", T("Project page"), Btn::Normal, IconId::External, true, T("Opens the project page on GitHub: source code, releases and the user guide.")},
                                     {"ab#issue", T("Report a problem"), Btn::Normal, IconId::Warn, true,
                                      T("Opens the issue page. Use \"Copy diagnostics\" on Safety and service to add the details.")},
                                     {"ab#copy", T("Copy version info"), Btn::Normal, IconId::Copy, true, ""}};
  const float contentH = (mismatch ? 5 : 4) * kRowInfo + (mismatch ? NoteHeight(c, m, note) + 4 : 0.0f) + 8 + static_cast<float>(BtnRows(c, m.InnerW(), btns)) * kRowButtons;
  Card k(c, m, BC_NAME, Tf("Version %s", winVer.c_str()), IconId::Bolt, contentH);
  k.Info(mismatch ? T("Window version") : T("Version"), winVer);
  if (mismatch) k.Info(T("Service version"), svcVer, th.amber);
  k.Info(T("Compared against"), "OEM Control Center " + std::string(kOemReference), 0,
         T("The OEM release whose feature list Control Center Advanced was compared against."));
  k.Info(T("Reference laptop"), "Tulpar T5 V21.7 (GM5TG7Y)", 0,
         T("The laptop everything was verified on. Other models with the same embedded controller are supported at the Family level."));
  k.Info(T("Licence"), "GNU GPL v3", 0, T("Free software: you may use, study, change and share it under the terms of the GNU General Public License, version 3."));
  if (mismatch) {
    k.Gap(4);
    k.Note(note, th.amber);
  }
  k.Gap(8);
  const int hit = BtnFlow(k, btns);
  if (hit == 0) {
    a.OpenUrl(BC_REPO_URL);
  } else if (hit == 1) {
    a.OpenUrl(std::string(BC_REPO_URL) + "/issues/new");
  } else if (hit == 2) {
    std::string t = Fmt("Control Center Advanced %s\n", winVer.c_str());
    if (!svcVer.empty()) t += Fmt("Service: %s (%s)\n", svcVer.c_str(), a.mode().c_str());
    t += Fmt("Compared against: OEM Control Center %s\n", kOemReference);
    t += "Licence: GNU GPL v3\n";
    t += std::string(BC_REPO_URL) + "\n";
    a.CopyText(t);
  }
}

void CardLanguage(Ctx& c, Masonry& m) {
  App& a = c.app;
  const std::string note = T("The EN | TR switch in the top bar changes the same setting.");
  Card k(c, m, T("Language"), "", IconId::Info, kRowCombo + 4 + NoteHeight(c, m, note));
  const std::string lang = a.Pref("lang", "auto");
  const int cur = lang == "en" ? 1 : (lang == "tr" ? 2 : 0);
  const int pick = k.ComboRow("cb#lang", T("Interface language"), T("Applies at once."), {T("Automatic"), "English", "T\xC3\xBCrk\xC3\xA7""e"}, cur);
  k.Gap(4);
  k.Note(note);
  if (pick >= 0 && pick != cur) {
    a.SetPref("lang", pick == 1 ? "en" : (pick == 2 ? "tr" : "auto"));
    a.ApplyLanguage();
  }
}

void CardGoodToKnow(Ctx& c, Masonry& m) {
  const Theme& th = c.ui.th;
  const std::vector<std::string> notes = {
      T("Control Center Advanced is an independent project. It is not made, approved or supported by Monster Notebook, XMG, Tongfang, Uniwill, Intel or NVIDIA; their names only say which hardware it works with."),
      T("It comes with no warranty. Changing power, fan and firmware-related settings can make a laptop unstable and, in the worst case, damage it. The safety switches and the list of hard limits are on the Safety and service page."),
      T("Settings of the service and its log are kept in %ProgramData%\\ControlCenterAdvanced. This window's own preferences are in %LocalAppData%\\ControlCenterAdvanced."),
  };
  float h = 0;
  for (const std::string& s : notes) h += NoteHeight(c, m, s) + 6;
  Card k(c, m, T("Good to know"), "", IconId::Shield, h);
  for (const std::string& s : notes) {
    k.Note(s, th.muted);
    k.Gap(6);
  }
}

void CardCredits(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::vector<Credit> cr = Credits();
  const std::string intro =
      T("Control Center Advanced is free software under the GNU GPL v3. Apart from the GPU power engine, which is the author's own MONSTER GPU POWER project, it was written from scratch. "
        "These projects were studied for how the hardware behaves and how a control center can be organised. Thank you to everyone who published them. Click one to open its page.");
  const float innerW = m.InnerW(true);
  const int cols = innerW >= 760 ? 2 : 1;
  const float tw = (innerW - (cols - 1) * kTileGap) / static_cast<float>(cols);
  const int n = static_cast<int>(cr.size());
  const int rows = (n + cols - 1) / cols;
  std::vector<float> rowH(static_cast<size_t>(rows), 0.0f);
  for (int i = 0; i < n; i++) rowH[static_cast<size_t>(i / cols)] = std::max(rowH[static_cast<size_t>(i / cols)], TileHeight(c, tw, cr[static_cast<size_t>(i)]));
  float tilesH = 0;
  for (float h : rowH) tilesH += h + kTileGap;
  Card k(c, m, T("Credits and licences"), T("Projects Control Center Advanced learned from"), IconId::Profiles, NoteHeight(c, m, intro, true) + 8 + tilesH - kTileGap, true);
  k.Note(intro);
  k.Gap(8);
  float y = k.y;
  for (int r = 0; r < rows; r++) {
    for (int col = 0; col < cols; col++) {
      const int i = r * cols + col;
      if (i >= n) break;
      const Credit& e = cr[static_cast<size_t>(i)];
      const Rect t(k.x0 + col * (tw + kTileGap), y, tw, rowH[static_cast<size_t>(r)]);
      const bool link = !e.url.empty();
      bool hover = false;
      const bool clicked = ui.Click("cr#" + std::to_string(i), t, link, &hover);
      g.FillRound(t, 12, hover && link ? Mix(th.inset, th.accent, 0.08f) : th.inset);
      g.StrokeRound(t, 12, hover && link ? WithAlpha(th.accent, 120) : th.line, 1.0f);
      const uint32_t chipCol = e.kind == 0 ? th.green : (e.kind == 1 ? th.amber : th.muted);
      const float cw = ui.ChipW(e.licence);
      ui.Chip(Rect(t.r() - kTilePad - cw, t.y + kTilePad + 3, cw, 18), e.licence, chipCol);
      g.Text(Rect(t.x + kTilePad, t.y + kTilePad, t.w - 2 * kTilePad - cw - 10, 24), g.Fit(e.name, FontId::Body, t.w - 2 * kTilePad - cw - 10), FontId::Body, th.text);
      g.Wrapped(Rect(t.x + kTilePad, t.y + kTilePad + 28, t.w - 2 * kTilePad, t.h - kTilePad - 28), e.desc, FontId::Small, th.muted);
      if (link) ui.Tip(t, e.url);
      if (clicked) a.OpenUrl(e.url);
    }
    y += rowH[static_cast<size_t>(r)] + kTileGap;
  }
}

}  // namespace

float PageAbout(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 430, 2);
  CardApp(c, m);
  CardGoodToKnow(c, m);
  CardLanguage(c, m);
  CardCredits(c, m);
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
