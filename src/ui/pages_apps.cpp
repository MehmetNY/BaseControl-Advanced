// "Application profiles": programs that switch the performance profile while they run. A list in priority order (the first running
// entry wins), an inline editor with a "pick from the programs that are open now" list, and the master switch.
#include <algorithm>
#include <cctype>
#include <cstring>

#include "../common/profile_text.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"
#include "winapps.h"

namespace bc {
namespace ui {
namespace {

constexpr int kSlots = 32;
constexpr float kRowWide = 66, kRowNarrow = 96;
constexpr float kFieldH = 70, kPanelPad = 16, kTitleH = 30, kHintH = 26, kBtnH = 46;

struct Entry {
  int slot = 0;
  std::string exe, profile, name;
};

std::vector<Entry> Entries(App& a) {
  std::vector<Entry> v;
  for (int i = 0; i < kSlots; i++) {
    const std::string exe = a.S(Fmt("cfg.app.%d.exe", i));
    if (exe.empty()) continue;
    Entry e;
    e.slot = i;
    e.exe = exe;
    e.profile = a.S(Fmt("cfg.app.%d.profile", i));
    e.name = a.S(Fmt("cfg.app.%d.name", i));
    v.push_back(e);
  }
  return v;
}

std::string Stem(const std::string& exe) {
  if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) return exe.substr(0, exe.size() - 4);
  return exe;
}

std::string Shown(const Entry& e) { return e.name.empty() ? Stem(e.exe) : e.name; }

bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// What can be typed in the Program field: a file name, or a path (its last part is used). Mirrors the service's own check.
bool ProgramOk(const std::string& n) {
  if (n.size() < 5 || n.size() > 64 || n.compare(n.size() - 4, 4, ".exe") != 0) return false;
  for (unsigned char c : n)
    if (c < 0x20 || c == 0x7F || std::strchr("\\/:*?\"<>|", c)) return false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- running programs
struct RunCache {
  std::vector<RunningApp> apps;
  double at = -100;
};

const std::vector<RunningApp>& Running(App& a, bool force) {
  static RunCache c;
  const double now = a.ui().now;
  if (force || now - c.at > 4.0) {
    c.apps = ListRunningApps(a.simulated());
    c.at = now;
  }
  return c.apps;
}

// ---------------------------------------------------------------------------------------------------------------- editor state
// apps.edit: -2 closed, -1 adding a program, >= 0 editing that slot. apps.prof: index into the profile combo.
int& EditSlot(App& a) { return a.Int("apps.edit", -2); }

std::vector<std::string> ProfileItems(const std::vector<std::string>& names, const std::string& missing) {
  std::vector<std::string> items;
  for (const std::string& n : names) items.push_back(ProfileLabel(n));
  if (!missing.empty()) items.push_back(ProfileLabel(missing) + " (" + T("missing") + ")");
  return items;
}

void OpenEditor(App& a, int slot, const Entry* e) {
  const std::vector<std::string> names = ProfileList(a.snap());
  EditSlot(a) = slot;
  a.Str("apps.exe") = e ? e->exe : std::string();
  a.Str("apps.name") = e ? e->name : std::string();
  a.Str("apps.auto").clear();
  std::string want = e ? e->profile : std::string(Has(names, "Enthusiast") ? "Enthusiast" : (names.empty() ? "" : names.front()));
  int idx = 0;
  for (size_t i = 0; i < names.size(); i++)
    if (names[i] == want) idx = static_cast<int>(i);
  if (e && !Has(names, e->profile)) idx = static_cast<int>(names.size());   // the "(missing)" item
  a.Int("apps.prof", 0) = idx;
  Running(a, true);
}

void CloseEditor(App& a) {
  EditSlot(a) = -2;
  a.Str("apps.exe").clear();
  a.Str("apps.name").clear();
  a.Str("apps.auto").clear();
}

// ---------------------------------------------------------------------------------------------------------------- pieces
void Chevron(Ui& ui, const Rect& r, bool up, bool enabled, const std::string& id, const std::string& tip, bool* clicked) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  bool hov = false;
  *clicked = ui.Click(id, r, enabled, &hov, nullptr);
  if (hov && enabled) g.FillRound(r, 8, th.hover);
  const uint32_t col = enabled ? (hov ? th.text : th.muted) : WithAlpha(th.faint, 110);
  const float cx = r.cx(), cy = r.cy();
  const float d = up ? 1.0f : -1.0f;
  g.Poly({{cx - 5, cy + 2.5f * d}, {cx, cy - 2.5f * d}, {cx + 5, cy + 2.5f * d}}, col, 1.8f);
  ui.Tip(r, tip);
}

// Profile name as a coloured pill with its icon. Returns the width it used.
float ProfilePill(Ui& ui, float x, float y, const std::string& profile, bool missing) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::string text = ProfileLabel(profile);
  const uint32_t col = missing ? th.amber : ProfileColor(th, profile);
  const float w = g.TextW(text, FontId::Label) + 46 + (missing ? 0 : 0);
  const Rect r(x, y, w, 28);
  g.FillRound(r, 14, WithAlpha(col, 26));
  g.StrokeRound(r, 14, WithAlpha(col, 90), 1);
  g.Icon(missing ? IconId::Warn : ProfileIconOf(profile), Rect(r.x + 9, r.y + 6, 16, 16), col, 1.7f);
  g.Text(Rect(r.x + 31, r.y, w - 36, r.h), text, FontId::Label, col);
  return w;
}

struct RowAction {
  bool edit = false, up = false, down = false;
};

RowAction DrawRow(App& a, const Rect& row, const Entry& e, bool active, bool selected, bool narrow, bool canUp, bool canDown, bool missing) {
  Ui& ui = a.ui();
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  RowAction out;
  bool hov = false;
  const bool rowClick = ui.Click("ar#" + std::to_string(e.slot), row, true, &hov, nullptr);   // first: the buttons drawn after it win the press
  if (hov || selected) g.FillRound(row.Inset(0, 2), 12, selected ? WithAlpha(th.accent, 16) : WithAlpha(th.hover, 140));
  const Rect tile(row.x + 8, row.y + (kRowWide - 40) * 0.5f, 40, 40);
  g.FillRound(tile, 11, WithAlpha(active ? th.green : th.accent, 28));
  g.Icon(IconId::Apps, Rect(tile.x + 9, tile.y + 9, 22, 22), active ? th.green : th.accent, 1.7f);

  // buttons on the right
  float rx = row.r() - 8;
  const Rect down(rx - 30, row.y + (kRowWide - 30) * 0.5f, 30, 30);
  const Rect up(down.x - 4 - 30, down.y, 30, 30);
  bool c1 = false, c2 = false;
  Chevron(ui, up, true, canUp, "au#" + std::to_string(e.slot), T("Higher priority"), &c1);
  Chevron(ui, down, false, canDown, "ad#" + std::to_string(e.slot), T("Lower priority"), &c2);
  out.up = c1;
  out.down = c2;
  rx = up.x - 8;
  const float eb = narrow ? 36.0f : 80.0f;
  const Rect edit(rx - eb, row.y + (kRowWide - 34) * 0.5f, eb, 34);
  if (ui.Button("ae#" + std::to_string(e.slot), edit, narrow ? std::string() : (selected ? T("Close") : T("Edit")), Btn::Normal, narrow ? IconId::Settings : IconId::None, true)) out.edit = true;
  ui.Tip(edit, narrow ? T("Edit") : std::string());
  rx = edit.x - 12;
  if (rowClick) out.edit = true;

  // texts
  const float tx = tile.r() + 14;
  const std::string name = Shown(e);
  const float textRight = narrow ? rx : std::min(rx, row.x + row.w * 0.52f) - 10;
  g.Text(Rect(tx, row.y + 12, std::max(60.0f, textRight - tx), 22), name, FontId::Value, th.text);
  g.Text(Rect(tx, row.y + 36, std::max(60.0f, textRight - tx - (active ? 76.0f : 0.0f)), 18), e.exe, FontId::Mono, th.faint);
  if (active) {
    const float ew = g.TextW(e.exe, FontId::Mono);
    const float ax = std::min(tx + ew + 14, textRight - 62);
    g.Dot({ax + 4, row.y + 45}, 3.4f, th.green);
    g.Text(Rect(ax + 12, row.y + 36, 60, 18), T("Active"), FontId::Tiny, th.green);
  }
  if (narrow) {
    ProfilePill(ui, tx, row.y + 62, e.profile, missing);
  } else {
    const float px = std::max(tx + 120, row.x + row.w * 0.52f);
    ProfilePill(ui, std::min(px, rx - 150.0f), row.y + (kRowWide - 28) * 0.5f, e.profile, missing);
  }
  if (missing) ui.Tip(Rect(row.x, row.y, row.w, row.h), T("This profile no longer exists, so the entry is skipped. Edit it and pick another profile."));
  return out;
}

float EditorHeight(float innerW) {
  const bool twoCols = innerW - 2 * kPanelPad >= 600;
  return kPanelPad + kTitleH + (twoCols ? 2 : 4) * kFieldH + kHintH + kBtnH + kPanelPad;
}

// The editor. Returns nothing; actions are sent from here.
void DrawEditor(App& a, const Rect& panel, const std::vector<Entry>& entries, const std::vector<std::string>& names, const std::string& missing, bool adding,
                int slot) {
  Ui& ui = a.ui();
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  g.FillRound(panel, 14, th.inset);
  g.StrokeRound(panel, 14, WithAlpha(th.accent, 90), 1.2f);
  const float x0 = panel.x + kPanelPad, iw = panel.w - 2 * kPanelPad;
  float y = panel.y + kPanelPad;
  g.Text(Rect(x0, y, iw, kTitleH - 4), adding ? T("Add a program") : T("Edit program"), FontId::Value, th.text);
  y += kTitleH;

  const bool two = iw >= 600;
  const float colW = two ? (iw - 24) / 2 : iw;
  auto cell = [&](int col, int row) { return Rect(x0 + col * (colW + 24), y + row * kFieldH, colW, kFieldH); };
  auto label = [&](const Rect& c, const std::string& text) { g.Text(Rect(c.x, c.y, c.w, 18), text, FontId::Small, th.muted); };

  std::string& exe = a.Str("apps.exe");
  std::string& name = a.Str("apps.name");
  int& prof = a.Int("apps.prof", 0);
  const std::vector<RunningApp>& run = Running(a, false);

  // --- Program + Browse
  const Rect cProg = cell(0, 0);
  label(cProg, T("Program (file name)"));
  const float browseW = 104;
  const bool enter = ui.TextInput("in#apps.exe", Rect(cProg.x, cProg.y + 22, cProg.w - browseW - 8, 38), &exe, "game.exe", 260);
  const bool canBrowse = !a.simulated();
  if (ui.Button("apps.browse", Rect(cProg.r() - browseW, cProg.y + 22, browseW, 38), T("Browse"), Btn::Normal, IconId::Folder, canBrowse)) {
    const std::string path = BrowseForProgram(a.WindowHandle());
    if (!path.empty()) {
      exe = NormalizeProgramName(path);
      const std::string prod = ProductNameOf(path);
      if (!prod.empty() && (name.empty() || name == a.Str("apps.auto"))) {
        name = prod;
        a.Str("apps.auto") = prod;
      }
    }
  }
  ui.Tip(Rect(cProg.r() - browseW, cProg.y + 22, browseW, 38), canBrowse ? T("Pick a program file on this computer") : T("Not available in the simulator"));

  // --- Running now
  const Rect cRun = two ? cell(1, 0) : cell(0, 1);
  label(cRun, T("Programs that are open now"));
  std::vector<std::string> items;
  items.push_back(run.empty() ? T("Nothing found") : T("Choose a program"));
  for (const RunningApp& r : run) items.push_back((r.product.empty() ? Stem(r.exe) : r.product) + "  \xC2\xB7  " + r.exe);
  const int pick = ui.Combo("cb#apps.run", Rect(cRun.x, cRun.y + 22, cRun.w - 46, 38), items, 0, !run.empty());
  if (pick > 0 && static_cast<size_t>(pick) <= run.size()) {
    const RunningApp& r = run[static_cast<size_t>(pick - 1)];
    exe = r.exe;
    const std::string nm = r.product.empty() ? Stem(r.exe) : r.product;
    if (name.empty() || name == a.Str("apps.auto")) {
      name = nm;
      a.Str("apps.auto") = nm;
    }
  }
  const Rect refresh(cRun.r() - 38, cRun.y + 22, 38, 38);
  if (ui.IconButton("apps.refresh", refresh, IconId::Refresh, true)) Running(a, true);
  ui.Tip(refresh, T("Look again"));

  // --- Name
  const Rect cName = two ? cell(0, 1) : cell(0, 2);
  label(cName, T("Name (optional)"));
  ui.TextInput("in#apps.name", Rect(cName.x, cName.y + 22, cName.w, 38), &name, T("How it appears in the list"), 48);

  // --- Profile
  const Rect cProf = two ? cell(1, 1) : cell(0, 3);
  label(cProf, T("Profile while it runs"));
  const std::vector<std::string> pitems = ProfileItems(names, missing);
  prof = std::max(0, std::min(prof, static_cast<int>(pitems.size()) - 1));
  const int pp = ui.Combo("cb#apps.prof", Rect(cProf.x, cProf.y + 22, cProf.w, 38), pitems, prof, !pitems.empty());
  if (pp >= 0) prof = pp;
  y += (two ? 2 : 4) * kFieldH;

  // --- validation and hint
  const std::string norm = NormalizeProgramName(exe);
  std::string hint;
  uint32_t hintCol = th.faint;
  bool valid = true;
  if (norm.empty()) {
    valid = false;
  } else if (!ProgramOk(norm)) {
    valid = false;
    hint = T("A program is a file name such as game.exe.");
    hintCol = th.amber;
  } else {
    for (const Entry& e : entries)
      if (e.exe == norm && e.slot != slot) {
        valid = false;
        hint = T("That program is already in the list.");
        hintCol = th.amber;
      }
    if (valid && norm != exe) hint = Tf("It will be saved as %s.", norm.c_str());
  }
  const bool profReal = static_cast<size_t>(prof) < names.size();
  if (!profReal) {
    valid = false;
    hint = T("Pick a profile that exists.");
    hintCol = th.amber;
  }
  g.Text(Rect(x0, y, iw, kHintH), hint, FontId::Small, hintCol);
  y += kHintH;

  // --- buttons
  const bool pending = a.Pending("cmd:app.set") || a.Pending("cmd:app.delete");
  const float bw = 132;
  const Rect save(x0, y + 4, bw, 38);
  const bool doSave = ui.Button("apps.save", save, pending ? T("Saving") : (adding ? T("Add to list") : T("Save")), Btn::Primary, pending ? IconId::None : IconId::Check, valid && !pending);
  const Rect cancel(save.r() + 10, y + 4, 104, 38);
  if (ui.Button("apps.cancel", cancel, T("Cancel"), Btn::Normal, IconId::None, true)) CloseEditor(a);
  if (!adding) {
    const Rect rem(panel.r() - kPanelPad - 130, y + 4, 130, 38);
    if (ui.Button("apps.remove", rem, T("Remove"), Btn::Danger, IconId::Trash, !pending)) {
      ModalSpec ms;
      ms.title = Tf("Remove %s?", exe.c_str());
      ms.body = T("The program stays on your computer. Only the profile rule is removed.");
      ms.icon = IconId::Trash;
      ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Remove"), Btn::Danger, 1}};
      App* ap = &a;
      KV args;
      args.Set("i", std::to_string(slot));
      const std::string shown = exe;
      ui.ModalOpen(ms, [ap, args, shown](int id) {
        if (id != 1) return;
        ap->Cmd("app.delete", args, Tf("%s removed from the list", shown.c_str()), [ap](const Result& r) {
          if (r.ok) CloseEditor(*ap);
        });
      });
    }
  }
  if ((doSave || (enter && valid && !pending)) && valid) {
    KV args;
    if (!adding) args.Set("i", std::to_string(slot));
    args.Set("exe", norm);
    args.Set("profile", names[static_cast<size_t>(prof)]);
    args.Set("name", Trim(name));
    App* ap = &a;
    a.Cmd("app.set", args, adding ? Tf("%s added to the list", norm.c_str()) : Tf("%s saved", norm.c_str()), [ap](const Result& r) {
      if (r.ok) CloseEditor(*ap);
    });
  }
}

// ---------------------------------------------------------------------------------------------------------------- cards
void MatchingCard(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  const Theme& th = ui.th;
  const std::string active = a.S("app.active");
  const std::string note = T("If several listed programs run at once, the one higher in the list decides.");
  const std::string svcNote = (a.mode() == "local") ? T("No background service is installed, so switching only works while BaseControl is running. Safety and service can install it.") : std::string();
  float h = kRowToggle + (active.empty() ? 0.0f : kRowInfo) + NoteHeight(c, m, note, true) + (svcNote.empty() ? 0.0f : NoteHeight(c, m, svcNote, true));
  Card k(c, m, T("Automatic matching"), "", IconId::Apps, h, true);
  const bool on = a.B("cfg.app.auto", true);
  if (k.Toggle("tg#app.auto", T("Switch profile per application"), T("When the program closes, the previous profile comes back."), on, true)) a.Cfg("cfg.app.auto", on ? "0" : "1");
  if (!active.empty()) {
    const std::string prof = ProfileLabel(a.S("profile.active"));
    const std::string before = a.S("app.before");
    k.Info(T("Running now"), active + "  \xE2\x86\x92  " + prof, th.accent, before.empty() ? std::string() : Tf("The profile before it started: %s", ProfileLabel(before).c_str()));
  }
  k.Note(note);
  if (!svcNote.empty()) k.Note(svcNote, th.amber);
}

void ListCard(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::vector<Entry> entries = Entries(a);
  const std::vector<std::string> names = ProfileList(a.snap());
  const std::string active = a.S("app.active");
  const float iw = m.InnerW(true);
  const bool narrow = iw < 560;
  const float rowH = narrow ? kRowNarrow : kRowWide;
  int& edit = EditSlot(a);
  if (edit >= 0) {   // the entry vanished (another window removed it): close the editor
    bool found = false;
    for (const Entry& e : entries) found = found || e.slot == edit;
    if (!found) edit = -2;
  }
  const float editorH = EditorHeight(iw + 0.0f);
  float h = 0;
  const bool empty = entries.empty();
  if (!empty) h += 26;                       // column header
  if (edit == -1) h += editorH + 10;
  h += static_cast<float>(entries.size()) * rowH + (edit >= 0 ? editorH + 10 : 0.0f);
  if (empty && edit != -1) h += 150;
  if (!empty) h += 8;
  Card k(c, m, T("Programs"), T("Each one runs with the profile you choose while it is open."), IconId::Gamepad, h, true);

  // the add button sits in the card header
  const float bw = std::max(150.0f, g.TextW(T("Add a program"), FontId::Label) + 64);
  const Rect add(k.box.r() - kPad - bw, k.box.y + kPad, bw, 36);
  if (ui.Button("apps.add", add, T("Add a program"), Btn::Primary, IconId::Plus, entries.size() < static_cast<size_t>(kSlots) && edit != -1)) OpenEditor(a, -1, nullptr);

  if (!empty) {
    const Rect hd = k.Row(26);
    g.Text(Rect(hd.x + 62, hd.y, 200, 24), T("Program"), FontId::Tiny, th.faint);
    if (!narrow) g.Text(Rect(hd.x + hd.w * 0.52f, hd.y, 200, 24), T("Profile"), FontId::Tiny, th.faint);
    g.Line({hd.x, hd.b() - 1}, {hd.r(), hd.b() - 1}, WithAlpha(th.line, 150), 1);
  }
  if (edit == -1) {
    const Rect panel = k.Row(editorH);
    DrawEditor(a, panel, entries, names, "", true, -1);
    k.Gap(10);
  }
  if (empty && edit != -1) {
    const Rect r = k.Row(150);
    g.FillRound(Rect(r.cx() - 28, r.y + 14, 56, 56), 16, WithAlpha(th.accent, 24));
    g.Icon(IconId::Gamepad, Rect(r.cx() - 14, r.y + 28, 28, 28), th.accent, 1.8f);
    g.Text(Rect(r.x, r.y + 78, r.w, 24), T("No programs yet"), FontId::Value, th.text, Align::Center);
    g.WrappedClamped(Rect(r.x + r.w * 0.1f, r.y + 104, r.w * 0.8f, 44), T("Add a game or a tool and choose the profile it should run with. BaseControl switches to it while the program runs and back when it closes."),
                     FontId::Small, th.muted, 2, 3.0f, nullptr);
  }
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = entries[i];
    const Rect row = k.Row(rowH);
    const bool missing = !Has(names, e.profile);
    const bool selected = edit == e.slot;
    const RowAction act = DrawRow(a, row, e, !active.empty() && active == e.exe, selected, narrow, i > 0, i + 1 < entries.size(), missing);
    if (act.edit) {
      if (selected) CloseEditor(a);
      else OpenEditor(a, e.slot, &e);
    }
    if (act.up || act.down) {
      KV args;
      args.Set("i", std::to_string(e.slot));
      args.Set("dir", act.up ? "-1" : "1");
      a.Cmd("app.move", args);
    }
    if (selected) {
      k.Gap(0);
      const Rect panel = k.Row(editorH);
      DrawEditor(a, panel, entries, names, missing ? e.profile : std::string(), false, e.slot);
      k.Gap(10);
    } else if (i + 1 < entries.size()) {
      g.Line({row.x + 8, row.b()}, {row.r() - 8, row.b()}, WithAlpha(th.line, 110), 1);
    }
  }
}

}  // namespace

float PageApps(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, r.w, 1);
  MatchingCard(c, m);
  ListCard(c, m);
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
