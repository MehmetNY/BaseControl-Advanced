// General settings: in-game overlay (OSD). The preview uses the same painter as the real overlay window.
#include <algorithm>
#include <cmath>

#include "../common/osdcfg.h"
#include "../common/util.h"
#include "osd_paint.h"
#include "page.h"

namespace bc {
namespace ui {

float PageOsd(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Masonry m(r.w, 440, 2);
  const OsdConfig cfg = OsdFromKv(a.prefs());
  const OsdData data = OsdDataFrom(a.snap());
  const std::vector<OsdLine> lines = OsdLines(cfg, data);

  // ---- preview
  {
    const float sceneH = std::min(300.0f, std::max(170.0f, (m.InnerW() * 9.0f / 16.0f)));
    const std::string note = T("Works with windowed and borderless games. A game in exclusive full screen covers the overlay.");
    const float h = sceneH + 12 + kRowToggleLean + 9 + kRowToggleLean + NoteHeight(c, m, note);
    Card k(c, m, T("Overlay preview"), "", IconId::Eye, h);
    const Rect scene(k.x0, k.y, k.w, sceneH);
    OsdPreviewScene(g, scene);
    g.StrokeRound(scene, 4, WithAlpha(th.text, 40), 1.0f);
    // Approximate placement: the margins are real screen pixels, the panel is drawn at its real size.
    const V2 sz = OsdMeasure(g, cfg, lines);
    const float mx = cfg.mx * 0.55f, my = cfg.my * 0.55f;
    V2 p;
    p.x = (cfg.corner == 1 || cfg.corner == 3) ? scene.r() - mx - sz.x : scene.x + mx;
    p.y = (cfg.corner == 2 || cfg.corner == 3) ? scene.b() - my - sz.y : scene.y + my;
    p.x = std::max(scene.x + 4, std::min(p.x, scene.r() - sz.x - 4));
    p.y = std::max(scene.y + 4, std::min(p.y, scene.b() - sz.y - 4));
    if (!cfg.enabled) {
      // when switched off the preview still shows where it would appear, a little dimmer
      OsdConfig dim = cfg;
      dim.opacity = std::max(10, cfg.opacity / 2);
      OsdPaint(g, p, dim, lines);
    } else {
      OsdPaint(g, p, cfg, lines);
    }
    g.Text(Rect(scene.x + 10, scene.b() - 24, 200, 18), T("Preview"), FontId::Tiny, WithAlpha(0xFFFFFFFF, 150));
    k.Gap(sceneH + 12);
    if (k.PrefRow("osd.on", T("Overlay enabled"), "", false)) a.ApplySession();
    k.Divider();
    const auto& keys = OsdHotkeys();
    std::vector<std::string> labels;
    int cur = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      labels.push_back(HotkeyLabel(keys[i]));
      if (keys[i] == cfg.hotkey) cur = static_cast<int>(i);
    }
    const int pick = k.ComboRow("cmb#osd.hotkey", T("Show / hide shortcut"), "", labels, cur, true, 200);
    if (pick >= 0 && pick != cur) {
      a.SetPref("osd.hotkey", keys[static_cast<size_t>(pick)]);
      a.ApplySession();
    }
    k.Note(note);
  }

  // ---- visible data
  {
    // frame counting needs an event-tracing session; this version has none (hw_.fps is only provided by the simulator)
    const std::string fpsDesc = a.CapOk("fps") ? T("Frames per second of the game in front.") : T("Not available in this version: Control Center Advanced cannot count frames yet.");
    const float h = kRowToggle + 5 * kRowToggleLean + 5 * 9;
    Card k(c, m, T("Visible data"), "", IconId::Sliders, h);
    bool ch = false;
    ch |= k.PrefRow("osd.fps", T("FPS counter"), fpsDesc, true);
    k.Divider();
    ch |= k.PrefRow("osd.cpu", T("CPU temperature and load"), "", true);
    k.Divider();
    ch |= k.PrefRow("osd.gpu", T("GPU temperature and load"), "", true);
    k.Divider();
    ch |= k.PrefRow("osd.mem", T("Memory use"), "", false);
    k.Divider();
    ch |= k.PrefRow("osd.fan", T("Fan speed"), "", false);
    k.Divider();
    ch |= k.PrefRow("osd.power", T("Power draw"), "", false);
    if (ch) a.ApplySession();
  }

  // ---- position and appearance
  {
    const float h = kRowSegLean + 2 * kRowPrefSlider + 9 + kRowSegLean + kRowSwatch + kRowPrefSlider;
    Card k(c, m, T("Position and appearance"), "", IconId::Settings, h);
    const int pc = k.Segment("seg#osd.corner", T("Screen corner"), "", {T("Top left"), T("Top right"), T("Bottom left"), T("Bottom right")}, cfg.corner);
    if (pc >= 0 && pc != cfg.corner) {
      a.SetPref("osd.corner", std::to_string(pc));
      a.ApplySession();
    }
    int mx = cfg.mx, my = cfg.my;
    if (k.PrefSlider("sl#osd.mx", T("Horizontal margin"), &mx, 0, 200, 5, "px")) {
      a.SetPref("osd.mx", std::to_string(mx));
      a.ApplySession();
    }
    if (k.PrefSlider("sl#osd.my", T("Vertical margin"), &my, 0, 200, 5, "px")) {
      a.SetPref("osd.my", std::to_string(my));
      a.ApplySession();
    }
    k.Divider();
    const int ps = k.Segment("seg#osd.size", T("Text size"), "", {T("Small"), T("Medium"), T("Large")}, cfg.size);
    if (ps >= 0 && ps != cfg.size) {
      a.SetPref("osd.size", std::to_string(ps));
      a.ApplySession();
    }
    static const uint32_t kCols[] = {0xFFD9F0C4, 0xFFF1EEE8, 0xFFA7E3F0, 0xFFF2D38A, 0xFFF2A56B, 0xFFF0A6C0, 0xFFC9B8F0, 0xFFF29A9A};
    std::vector<uint32_t> cols(kCols, kCols + 8);
    int curC = -1;
    for (size_t i = 0; i < cols.size(); i++)
      if (cols[i] == cfg.color) curC = static_cast<int>(i);
    const int pcol = k.Swatches("osd.color", T("Text colour"), cols, curC);
    if (pcol >= 0) {
      a.SetPref("osd.color", FormatColor(cols[static_cast<size_t>(pcol)]));
      a.ApplySession();
    }
    int op = cfg.opacity;
    if (k.PrefSlider("sl#osd.opacity", T("Background opacity"), &op, 10, 100, 1, "%")) {
      a.SetPref("osd.opacity", std::to_string(op));
      a.ApplySession();
    }
  }

  // ---- game list
  {
    const std::string note = T("Names are matched against the running programs, for example cs2.exe.");
    const size_t n = cfg.games.size();
    const float listH = std::max<size_t>(1, n) * 46.0f;
    const float h = kRowToggle + 9 + listH + kRowButtons + NoteHeight(c, m, note);
    Card k(c, m, T("Game list"), "", IconId::Gamepad, h);
    if (k.PrefRow("osd.gamesonly", T("Only while a listed game is in front"), T("Otherwise the overlay appears whenever it is switched on."), false)) a.ApplySession();
    k.Divider();
    if (cfg.games.empty()) {
      const Rect row = k.Row(46);
      g.Text(Rect(row.x, row.y, row.w, row.h), T("No game yet. Add a program name below."), FontId::Body, th.faint);
    }
    std::string removeName;
    for (const std::string& name : cfg.games) {
      const Rect row = k.Row(46);
      g.Text(Rect(row.x, row.y, row.w - 44, row.h), name, FontId::Body, th.text);
      if (ui.IconButton("rm#game#" + name, Rect(row.r() - 34, row.cy() - 17, 34, 34), IconId::Cross, true)) removeName = name;
      ui.Tip(Rect(row.r() - 34, row.cy() - 17, 34, 34), T("Remove from the list"));
      g.Line({row.x, row.b() - 0.5f}, {row.r(), row.b() - 0.5f}, WithAlpha(th.line, 120), 1);
    }
    if (!removeName.empty()) {
      std::vector<std::string> v = cfg.games;
      v.erase(std::remove(v.begin(), v.end(), removeName), v.end());
      a.SetPref("osd.games", FormatGameList(v));
      a.ApplySession();
    }
    if (k.InputRow("osd.game", &a.Str("osd.game.in"), "game.exe", T("Add"))) {
      const std::string name = NormalizeExeName(a.Str("osd.game.in"));
      if (name.empty()) {
        ui.Toast(T("That is not a program name"), ToastKind::Warn, T("Type a name like game.exe."));
      } else {
        std::vector<std::string> v = cfg.games;
        if (std::find(v.begin(), v.end(), name) == v.end()) v.push_back(name);
        a.SetPref("osd.games", FormatGameList(v));
        a.Str("osd.game.in").clear();
        a.ApplySession();
      }
    }
    k.Note(note);
  }

  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
