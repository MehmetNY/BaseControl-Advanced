// General settings: key mapping. A drawn keyboard to pick a key (or press it), the action to give it and the saved list.
#include <algorithm>
#include <cmath>

#include "../common/keymap.h"
#include "../common/util.h"
#include "page.h"

namespace bc {
namespace ui {
namespace {

// Keys people type with: giving one of these another job needs a second thought.
bool IsTypingKey(int id) {
  const int s = id & 0xFF;
  if (id & kKeyExt) return false;
  return (s >= 0x02 && s <= 0x0E) || s == 0x0F || (s >= 0x10 && s <= 0x1C) || (s >= 0x1E && s <= 0x28) || (s >= 0x2B && s <= 0x35) || s == 0x39;
}

std::string Legend(int id, bool tr) {
  std::string l = KeyLegend(id, tr);
  if (l.empty()) l = Fmt("Key 0x%X", static_cast<unsigned>(id));
  return l;
}

// One keycap. `state`: 0 normal, 1 selected; `mapped` draws the "has a job" marker.
void KeyCap(Ui& ui, const Rect& r, const std::string& legend, bool selected, bool mapped, bool hover, bool dim, float fontPx) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  uint32_t fill = dim ? Mix(th.inset, th.elev, 0.25f) : Mix(th.elev, th.panel, 0.25f);
  if (hover && !dim) fill = th.hover;
  if (selected) fill = Mix(fill, th.accent, 0.20f);
  g.FillRound(r, 6, fill);
  g.StrokeRound(r, 6, selected ? th.accent : (mapped ? WithAlpha(th.accent, 120) : th.line), selected ? 1.6f : 1.0f);
  if (mapped) g.Dot({r.r() - 6, r.y + 6}, 2.6f, th.accent);
  g.Text(r.Inset(2, 0), legend, fontPx >= 12 ? FontId::Small : FontId::Tiny, dim ? th.faint : (selected ? th.accent : th.text), Align::Center);
}

}  // namespace

float PageKeys(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  Masonry m(r.w, 440, 2);

  const bool tr = a.Pref("keys.layout", GetLang() == Lang::Tr ? "tr" : "us") == "tr";
  std::vector<KeyBind> binds = ParseKeyMap(a.Pref("keys.map", ""));
  int& sel = a.Int("keys.sel", 0x42);                 // F8, like the first example
  int& selSeen = a.Int("keys.sel.seen", -1);
  int& actIdx = a.Int("keys.act", 0);
  const auto& actions = KeyActions();
  if (actIdx < 0 || actIdx >= static_cast<int>(actions.size())) actIdx = 0;

  // a key picked by pressing it
  {
    int id = 0;
    if (a.TakeCapturedKey(&id)) sel = id;
  }
  if (sel != selSeen) {          // a new key was picked: show what it does now
    selSeen = sel;
    if (const KeyBind* b = FindBind(binds, sel)) {
      for (size_t i = 0; i < actions.size(); i++)
        if (b->action == actions[i].id) actIdx = static_cast<int>(i);
    }
  }

  // =============================================================== the keyboard
  {
    const float innerW = m.InnerW(true);
    const float u = std::floor(std::min(58.0f, std::max(26.0f, (innerW - 28) / 15.0f)));
    const float kh = std::floor(std::min(44.0f, std::max(26.0f, u * 0.8f)));
    const float gap = 4;
    const float kbH = 6 * (kh + gap) + 18;
    Card k(c, m, T("Keyboard"), "", IconId::Keyboard, kbH, true);
    // layout switch in the header
    {
      const float sw = 150;
      const int pick = ui.Segmented("seg#kb.layout", Rect(k.box.r() - kPad - sw, k.box.y + kPad + 2, sw, 30), {"T\xC3\xBCrk\xC3\xA7""e Q", "US"}, tr ? 0 : 1);
      if (pick >= 0 && (pick == 0) != tr) a.SetPref("keys.layout", pick == 0 ? "tr" : "us");
    }
    const Rect board(k.x0, k.y, k.w, kbH);
    ui.Panel(board, th.inset, th.line, 12);
    const float bx = board.x + (board.w - 15 * u) * 0.5f;
    float by = board.y + 9 + gap * 0.5f;
    std::vector<std::pair<int, Rect>> enter;
    int clicked = -1;
    const auto& rows = KeyboardRows();
    for (size_t ri = 0; ri < rows.size(); ri++) {
      float x = bx;
      for (const PhysKey& pk : rows[ri]) {
        const Rect kr(x + gap * 0.5f, by, pk.w * u - gap, kh);
        x += pk.w * u;
        const std::string legend = tr ? pk.tr : pk.us;
        if (pk.id == 0x1C) {
          enter.push_back({pk.id, kr});
          continue;
        }
        bool hov = false;
        const std::string id = "kb#" + std::to_string(pk.id) + "#" + std::to_string(ri);
        if (pk.mappable && ui.Click(id, kr, true, &hov)) clicked = pk.id;
        const KeyBind* b = FindBind(binds, pk.id);
        KeyCap(ui, kr, legend, pk.mappable && sel == pk.id, b != nullptr, hov, !pk.mappable, pk.w * u > 34 ? 12.0f : 11.0f);
        if (!pk.mappable) {
          ui.Tip(kr, T("The Fn key is handled by the firmware. Windows never sees it, so it cannot be remapped."));
        } else if (b) {
          const KeyAction* fa = FindKeyAction(b->action);
          ui.Tip(kr, legend + "  \xE2\x86\x92  " + (fa ? T(fa->label) : b->action));
        }
      }
      by += kh + gap;
    }
    // the ISO Enter key: two rectangles that read as one key
    if (enter.size() == 2) {
      bool hov = false, hov2 = false;
      const bool c1 = ui.Click("kb#enter#0", enter[0].second, true, &hov);
      const bool c2 = ui.Click("kb#enter#1", enter[1].second, true, &hov2);
      if (c1 || c2) clicked = 0x1C;
      const bool h = hov || hov2;
      const bool s = sel == 0x1C;
      const bool mp = FindBind(binds, 0x1C) != nullptr;
      KeyCap(ui, enter[0].second, tr ? "Enter" : "Enter", s, mp, h, false, 12.0f);
      KeyCap(ui, enter[1].second, "", s, false, h, false, 12.0f);
      const Rect& t = enter[0].second;
      const Rect& l = enter[1].second;
      uint32_t fill = h ? th.hover : Mix(th.elev, th.panel, 0.25f);
      if (s) fill = Mix(fill, th.accent, 0.20f);
      g.Fill(Rect(l.x + 1.2f, t.b() - 1.5f, l.w - 2.4f, l.y - t.b() + 3.0f), fill);
      // the outline on the right of the seam, so the two rectangles read as one L-shaped key
      g.Line({t.r() - 0.5f, t.b() - 6.0f}, {t.r() - 0.5f, l.y + 6.0f}, s ? th.accent : (mp ? WithAlpha(th.accent, 120) : th.line), s ? 1.6f : 1.0f);
      if (s) g.Line({l.x + 0.5f, t.b()}, {l.x + 0.5f, l.y + 4.0f}, th.accent, 1.6f);   // and on the left of the lower part
    }
    if (clicked >= 0) sel = clicked;
  }

  // =============================================================== key function
  {
    const std::string note = T("Fn combinations that the firmware handles itself (brightness, radios ...) never reach Windows and cannot be remapped. Mappings work while BaseControl is running.");
    const float h = kRowToggleLean + 9 + 40 + kRowButtons + 56 + NoteHeight(c, m, note);
    Card k(c, m, T("Key function"), "", IconId::Key, h);
    if (k.PrefRow("keys.remap", T("Key mapping enabled"), "", false, a.SessionLive(), a.SessionLive() ? "" : T("Needs the keyboard hook, which only runs in the real app."))) a.ApplySession();
    k.Divider();
    {
      const Rect row = k.Row(40);
      g.Text(Rect(row.x, row.y, row.w * 0.5f, row.h), T("Selected key"), FontId::Body, th.text);
      const std::string lg = sel == 0 ? "-" : Legend(sel, tr);
      const float cw = std::max(34.0f, g.TextW(lg, FontId::Label) + 22);
      const Rect cap(row.r() - cw, row.cy() - 14, cw, 28);
      KeyCap(ui, cap, lg, true, false, false, false, 12.0f);
    }
    const bool detect = a.KeyCapturing();
    if (k.Button("keys.detect", detect ? T("Press a key now ...") : T("Detect a key by pressing it"), detect ? Btn::Primary : Btn::Ghost, IconId::Keyboard, a.SessionLive(), 0)) {
      if (detect) a.CancelKeyCapture();
      else a.BeginKeyCapture();
    }
    if (!a.SessionLive()) ui.Tip(Rect(k.x0, k.y - kRowButtons, k.w, kRowButtons), T("Needs the keyboard hook, which only runs in the real app."));
    // action + Map
    {
      const Rect row = k.Row(56);
      std::vector<std::string> items;
      for (const KeyAction& act : actions) items.push_back(T(act.label));
      const float bw = 92;
      const int pick = ui.Combo("cmb#keys.act", Rect(row.x, row.y + 8, row.w - bw - 10, 38), items, actIdx, true);
      if (pick >= 0) actIdx = pick;
      const bool can = sel != 0;
      if (ui.Button("keys.map", Rect(row.r() - bw, row.y + 8, bw, 38), T("Map"), Btn::Primary, IconId::None, can)) {
        const std::string id = actions[static_cast<size_t>(actIdx)].id;
        const int key = sel;
        const bool tr2 = tr;
        auto apply = [&a, key, id, tr2]() {
          std::vector<KeyBind> v = ParseKeyMap(a.Pref("keys.map", ""));
          SetBind(&v, key, id);
          a.SetPref("keys.map", FormatKeyMap(v));
          if (a.SessionLive() && !a.PrefB("keys.remap", false)) a.SetPref("keys.remap", "1");   // a mapping that is not switched on would do nothing
          a.ApplySession();
          const KeyAction* fa = FindKeyAction(id);
          a.ui().Toast(Legend(key, tr2) + "  \xE2\x86\x92  " + (fa ? T(fa->label) : id), ToastKind::Ok);
        };
        if (IsTypingKey(key)) {
          ModalSpec ms;
          ms.title = T("This is a typing key");
          ms.body = Tf("%s is a key you type with. If it gets another job you will not be able to type it until the mapping is removed here.", Legend(key, tr).c_str());
          ms.icon = IconId::Warn;
          ms.iconColor = th.amber;
          ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Map anyway"), Btn::Primary, 1}};
          ui.ModalOpen(ms, [apply](int pressed) {
            if (pressed == 1) apply();
          });
        } else {
          apply();
        }
      }
    }
    k.Note(note);
  }

  // =============================================================== saved mappings
  {
    const size_t n = binds.size();
    const float listH = std::max<size_t>(1, n) * 56.0f;
    Card k(c, m, T("Saved mappings"), "", IconId::Log, listH);
    {
      const float bw = std::max(100.0f, g.TextW(T("Reset all"), FontId::Label) + 40);
      if (ui.Button("keys.reset", Rect(k.box.r() - kPad - bw, k.box.y + kPad, bw, 34), T("Reset all"), Btn::Ghost, IconId::None, n > 0)) {
        ModalSpec ms;
        ms.title = T("Remove every mapping?");
        ms.body = T("All keys go back to their normal job.");
        ms.icon = IconId::Warn;
        ms.iconColor = th.amber;
        ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Remove all"), Btn::Danger, 1}};
        App* ap = &a;
        ui.ModalOpen(ms, [ap](int pressed) {
          if (pressed == 1) {
            ap->SetPref("keys.map", "");
            ap->ApplySession();
          }
        });
      }
    }
    if (binds.empty()) {
      const Rect row = k.Row(56);
      g.Text(Rect(row.x, row.y, row.w, row.h), T("No key is remapped yet."), FontId::Body, th.faint);
    }
    int removeKey = 0;
    for (const KeyBind& b : binds) {
      const Rect row = k.Row(56);
      const std::string lg = Legend(b.key, tr);
      const float cw = std::max(34.0f, g.TextW(lg, FontId::Label) + 22);
      KeyCap(ui, Rect(row.x, row.y + 6, cw, 26), lg, false, false, false, false, 12.0f);
      const KeyAction* fa = FindKeyAction(b.action);
      g.Text(Rect(row.x, row.y + 33, row.w - 50, 18), fa ? T(fa->label) : b.action, FontId::Small, th.muted);
      const Rect x(row.r() - 34, row.cy() - 17, 34, 34);
      if (ui.IconButton("rm#key#" + std::to_string(b.key), x, IconId::Cross, true)) removeKey = b.key;
      ui.Tip(x, T("Remove this mapping"));
      g.Line({row.x, row.b() - 0.5f}, {row.r(), row.b() - 0.5f}, WithAlpha(th.line, 120), 1);
    }
    if (removeKey) {
      RemoveBind(&binds, removeKey);
      a.SetPref("keys.map", FormatKeyMap(binds));
      a.ApplySession();
    }
  }

  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
