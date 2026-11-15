// "Custom mode": the values that shape power and performance, each with a LIVE reading that follows changes made by other programs, and its
// own Confirm button - nothing is sent to the laptop until you press it. The CPU and GPU values go through the vendor's service (as its
// own window does); the GPU power target (cTGP) is this program's own, separately verified engine.
#include <algorithm>
#include <cmath>

#include "../common/profile_text.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

// Every slider on this page that takes part in "Confirm all".
const char* const kKeys[] = {"perf.pl1", "perf.pl2", "perf.pl4", "perf.tcc", "perf.gpu.core", "perf.gpu.mem", "perf.gpu.temp", "gpu.ctgp"};
// A limit that goes up is applied from the outermost gate inwards, one that goes down from the innermost outwards, so that
// PL1 <= PL2 <= PL4 holds after every single step.
const char* const kRaiseOrder[] = {"perf.pl4", "perf.pl2", "perf.pl1"};
const char* const kLowerOrder[] = {"perf.pl1", "perf.pl2", "perf.pl4"};

struct Pend {
  std::string key, value, title;
  int v = 0, live = 0;
};

int LiveInt(const App& a, const std::string& k) { return static_cast<int>(std::lround(a.N(k))); }

std::string TitleOf(const std::string& key) {
  static const struct {
    const char* k;
    const char* t;
  } kT[] = {{"perf.pl1", N_("PL1")},
            {"perf.pl2", N_("PL2")},
            {"perf.pl4", N_("PL4")},
            {"perf.tcc", N_("Thermal offset (TCC)")},
            {"perf.gpu.core", N_("GPU core offset")},
            {"perf.gpu.mem", N_("GPU memory offset")},
            {"perf.gpu.temp", N_("GPU temperature target")},
            {"gpu.ctgp", N_("GPU power (cTGP)")}};
  for (auto& e : kT)
    if (key == e.k) return T(e.t);
  return key;
}

// readyMade: one of the vendor's ready-made modes is active, so the values the service keeps for it cannot be changed and no draft counts.
std::vector<Pend> Collect(App& a, bool readyMade) {
  std::vector<Pend> out;
  for (const char* k : kKeys) {
    if (readyMade && std::string(k) != "gpu.ctgp") continue;
    auto it = a.drafts().find(k);
    if (it == a.drafts().end() || !it->second.active || !a.Has(k)) continue;
    const int live = LiveInt(a, k);
    if (it->second.v == live) continue;
    Pend p;
    p.key = k;
    p.v = it->second.v;
    p.live = live;
    p.value = std::to_string(p.v);
    p.title = TitleOf(k);
    out.push_back(p);
  }
  return out;
}

const Pend* FindPend(const std::vector<Pend>& v, const char* key) {
  for (const Pend& p : v)
    if (p.key == key) return &p;
  return nullptr;
}

// Value a key will have once every pending change is applied.
int Eff(App& a, const std::vector<Pend>& v, const char* key) {
  if (const Pend* p = FindPend(v, key)) return p->v;
  return LiveInt(a, key);
}

bool AnyPend(const std::vector<Pend>& v, std::initializer_list<const char*> ks) {
  for (const char* k : ks)
    if (FindPend(v, k)) return true;
  return false;
}

// What stops the whole set from being applied (empty = fine). Uses the values as they will be AFTER the change.
std::string ValidateAll(App& a, const std::vector<Pend>& v) {
  if (AnyPend(v, {"perf.pl1", "perf.pl2", "perf.pl4"})) {
    const int p1 = Eff(a, v, "perf.pl1"), p2 = Eff(a, v, "perf.pl2"), p4 = Eff(a, v, "perf.pl4");
    if (p1 > p2) return Tf("PL1 (%d W) cannot be above PL2 (%d W).", p1, p2);
    if (p2 > p4) return Tf("PL2 (%d W) cannot be above PL4 (%d W).", p2, p4);
  }
  return "";
}

void ConfirmAll(App& a, const std::vector<Pend>& v) {
  std::vector<Request> reqs;
  std::vector<bool> used(v.size(), false);
  auto add = [&](size_t i) {
    Request r;
    r.op = "set";
    r.key = v[i].key;
    r.value = v[i].value;
    reqs.push_back(r);
    used[i] = true;
  };
  auto idx = [&](const char* k) {
    for (size_t i = 0; i < v.size(); i++)
      if (v[i].key == k) return static_cast<int>(i);
    return -1;
  };
  for (const char* k : kRaiseOrder) {
    const int i = idx(k);
    if (i >= 0 && v[static_cast<size_t>(i)].v > v[static_cast<size_t>(i)].live) add(static_cast<size_t>(i));
  }
  auto ordered = [&](const char* k) {
    for (const char* o : kRaiseOrder)
      if (std::string(o) == k) return true;
    return false;
  };
  for (size_t i = 0; i < v.size(); i++)   // everything that has no ordering constraint
    if (!used[i] && !ordered(v[i].key.c_str())) add(i);
  for (const char* k : kLowerOrder) {
    const int i = idx(k);
    if (i >= 0 && !used[static_cast<size_t>(i)]) add(static_cast<size_t>(i));
  }
  std::vector<std::string> labels(reqs.size());
  if (!labels.empty()) labels.back() = reqs.size() == 1 ? T("1 change applied") : Tf("%d changes applied", static_cast<int>(reqs.size()));
  a.Chain(reqs, labels);
}

SliderCfg Cfg(const char* key, const std::string& title, const std::string& desc, const std::string& lockWhy = "") {
  SliderCfg c;
  c.key = key;
  c.title = title;
  c.desc = desc;
  c.lockWhy = lockWhy;
  return c;
}

float SliderRows(int n) { return static_cast<float>(n) * kRowSlider; }

std::string Dash() { return "\xE2\x80\x94"; }

}  // namespace

float PageCustom(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 450, 2);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  // The vendor's ready-made modes keep their own stored values and the original window only edits the Custom mode, so while one of them is active the
  // rows below only SHOW the live values; the GPU power target (this program's own engine) works in every mode.
  const bool linkUp = a.CapOk("oem");
  const long long mode = a.I("perf.mode", -1);
  const bool readyMade = linkUp && mode >= 0 && mode != 3;
  const std::string lockWhy = readyMade ? T("The vendor's ready-made modes keep their own values. Switch to the Custom mode to change them.") : std::string();
  const std::vector<Pend> pend = Collect(a, readyMade);
  const std::string verr = ValidateAll(a, pend);

  // =================================================================== pending changes bar
  {
    const bool compact = r.w < 700;
    const bool any = !pend.empty();
    const float rowH = compact ? 58.0f : 46.0f;
    Card k(c, m, "", "", IconId::None, compact ? rowH + 8.0f + 38.0f : rowH, true);
    const Rect row = k.Row(rowH);
    g.FillRound(Rect(row.x, row.y + 3, 40, 40), 12, WithAlpha(any ? th.amber : th.green, 36));
    g.Icon(any ? IconId::Sliders : IconId::Check, Rect(row.x + 9, row.y + 12, 22, 22), any ? th.amber : th.green, 1.8f);

    const float bw3 = 132, bw2 = 118;
    Rect b2, b3;
    float textR = row.r();
    if (!compact) {
      b3 = Rect(row.r() - bw3, row.y + 4, bw3, 38);
      b2 = Rect(b3.x - 12 - bw2, row.y + 4, bw2, 38);
      textR = b2.x - 16;
    } else {
      const Rect brow = k.Row(46);
      const float by = brow.y + 8;
      b3 = Rect(brow.r() - bw3, by, bw3, 38);
      b2 = Rect(b3.x - 12 - bw2, by, bw2, 38);
    }
    const float tx = row.x + 54, tw = std::max(80.0f, textR - tx);
    std::string head;
    if (any) head = pend.size() == 1 ? T("1 change is waiting for your confirmation") : Tf("%d changes are waiting for your confirmation", static_cast<int>(pend.size()));
    else head = T("Showing live values. Nothing is waiting.");
    std::string sub = any ? T("Nothing has been sent to the laptop yet. Use each Confirm button, or Confirm all.")
                          : T("Move a slider, then press its Confirm button. The values follow changes made by other programs.");
    uint32_t sc = th.muted;
    if (any && !verr.empty()) {
      sub = verr;
      sc = th.amber;
    }
    g.Text(Rect(tx, row.y, tw, 24), head, FontId::H3, th.text);
    bool cut = false;
    g.WrappedClamped(Rect(tx, row.y + 26, tw, 36), sub, FontId::Small, sc, compact ? 2 : 1, 2.0f, &cut);
    if (cut) c.ui.Tip(Rect(tx, row.y + 24, tw, 36), sub);

    if (c.ui.Button("cu#discard", b2, T("Discard all"), Btn::Ghost, IconId::Undo, any)) a.ClearDrafts();
    if (c.ui.Button("cu#confirmall", b3, T("Confirm all"), Btn::Primary, IconId::Check, any && verr.empty())) ConfirmAll(a, pend);
    c.ui.Tip(b3, T("Applies every pending change in a safe order (limits that go up from the outside in, limits that go down from the inside out) and stops at the first problem."));
  }

  // =================================================================== which mode these values belong to
  {
    if (!linkUp || readyMade) {
      const std::string text = !linkUp ? T("The vendor's service is not reachable, so changes cannot be sent. The values shown come from the embedded controller where it can be read. See the Service page.")
                                       : Tf("The active mode is %s. The vendor's ready-made modes keep their own values, so they cannot be changed here. Switch to the Custom mode first.",
                                            ProfileLabel(a.S("profile.active")).c_str());
      const float nh = NoteHeight(c, m, text, true);
      Card k(c, m, "", "", IconId::None, nh + 6 + kRowButtons, true);
      k.Note(text, th.amber);
      k.Gap(6);
      if (!linkUp) {
        if (k.Button("cu#toservice", T("Open the Service page"), Btn::Normal, IconId::Service, true)) a.Go(Page::Service);
      } else {
        if (k.Button("cu#tocustom", T("Switch to Custom mode"), Btn::Primary, IconId::Sliders, !a.Pending("cmd:profile.apply"))) {
          KV args;
          args.Set("name", "Customized");
          a.Cmd("profile.apply", args, Tf("Mode %s applied", ProfileLabel("Customized").c_str()));
        }
      }
    }
  }

  // =================================================================== CPU power limits
  {
    const std::string note = T("The embedded controller enforces these limits. The range of each slider is the one the vendor's service reports for the active mode.");
    const float nh = NoteHeight(c, m, note);
    Card k(c, m, T("CPU power limits"), T("Sustained, boost and peak power of the processor"), IconId::Cpu, SliderRows(3) + 9 + nh);
    SliderCfg s1 = Cfg("perf.pl1", T("PL1 - sustained power"), T("Long-term CPU package power. Sets how hot the laptop runs under a lasting load."), lockWhy);
    s1.validate = [&a](int v) {
      const int pl2 = LiveInt(a, "perf.pl2");
      return v > pl2 ? Tf("PL1 cannot be above PL2 (now %d W). Confirm PL2 first, or use Confirm all.", pl2) : std::string();
    };
    k.Slider(s1);
    SliderCfg s2 = Cfg("perf.pl2", T("PL2 - boost power"), T("Short-term boost limit used for the first seconds of a load."), lockWhy);
    s2.validate = [&a](int v) {
      if (v < LiveInt(a, "perf.pl1")) return Tf("PL2 cannot be below PL1 (now %d W). Confirm PL1 first, or use Confirm all.", LiveInt(a, "perf.pl1"));
      if (v > LiveInt(a, "perf.pl4")) return Tf("PL2 cannot be above PL4 (now %d W). Confirm PL4 first, or use Confirm all.", LiveInt(a, "perf.pl4"));
      return std::string();
    };
    k.Slider(s2);
    SliderCfg s4 = Cfg("perf.pl4", T("PL4 - peak power"), T("The highest instantaneous power the CPU may draw."), lockWhy);
    s4.validate = [&a](int v) { return v < LiveInt(a, "perf.pl2") ? Tf("PL4 cannot be below PL2 (now %d W).", LiveInt(a, "perf.pl2")) : std::string(); };
    k.Slider(s4);
    k.Divider();
    k.Note(note);
  }

  // =================================================================== CPU temperature offset
  {
    const std::string note = T("The CPU starts slowing down this many degrees earlier than it normally would. Switch it off to use the CPU's own limit.");
    const float nh = NoteHeight(c, m, note);
    Card k(c, m, T("CPU temperature"), T("Thermal offset (TCC) of the processor"), IconId::Thermo, kRowToggle + 9 + SliderRows(1) + 9 + nh);
    k.BoolRow("perf.tcc.on", T("Use a thermal offset"), T("Lets the offset below apply."), "", "", lockWhy);
    k.Divider();
    k.Slider(Cfg("perf.tcc", T("Thermal offset"), T("Degrees below the CPU's maximum temperature at which throttling starts."), lockWhy));
    k.Divider();
    k.Note(note);
  }

  // =================================================================== GPU: offsets and temperature target (vendor service)
  {
    const std::string note = T("The values are stored in the Custom mode by the vendor's service and applied by the NVIDIA driver. Raise offsets slowly and test for stability.");
    const float nh = NoteHeight(c, m, note);
    Card k(c, m, T("GPU clocks and temperature"), T("Clock offsets and temperature target of the RTX GPU"), IconId::Chip, SliderRows(3) + 9 + nh);
    k.Slider(Cfg("perf.gpu.core", T("Core clock offset"), T("Added to the graphics clock. Too high a value causes crashes."), lockWhy));
    k.Slider(Cfg("perf.gpu.mem", T("Memory clock offset"), T("Added to the video memory clock. Too high a value causes artifacts."), lockWhy));
    k.Slider(Cfg("perf.gpu.temp", T("Temperature target"), T("The GPU slows itself down to stay at this temperature."), lockWhy));
    k.Divider();
    k.Note(note);
  }

  // =================================================================== GPU power (cTGP): this program's own engine
  {
    const bool oemSyncOk = a.CapOk("oem.sync");
    Card k(c, m, T("GPU power"), T("Dynamic power target (cTGP) of the RTX GPU"), IconId::Gpu, kRowSlider + 9 + 3 * kRowInfo + 2 * kRowToggle);
    SliderCfg s = Cfg("gpu.ctgp", T("cTGP - GPU power target"), T("Total power the EC lets the GPU use. The OEM profiles stop at 140 W."));
    s.softMax = 140;
    k.Slider(s);
    k.Divider();
    const bool ecOn = a.I("gpu.ec.ctrl") == 5;
    k.Info(T("EC cTGP switch"), a.Has("gpu.ec.ctrl") ? (ecOn ? T("On") : T("Off")) : Dash(), ecOn ? th.green : th.muted);
    k.Info(T("EC offset above base"), a.Has("gpu.ec.offset") ? With(a.N("gpu.ec.offset"), 0, "W") : Dash());
    const bool cons = a.B("gpu.ctgp.consistent", true);
    k.Info(T("Driver power limit"), a.Has("gpu.limit") ? With(a.N("gpu.limit"), 0, "W") + (cons ? "" : "  (" + T("differs from EC") + ")") : Dash(), cons ? 0 : th.amber,
           T("What the NVIDIA driver reports as the board power limit."));
    {
      const Rect r1 = k.Row(kRowToggle);
      const float tw = r1.w - 70;
      g.Text(Rect(r1.x, r1.y + 6, tw, 22), T("Keep my settings after sleep and display wake"), FontId::Body, th.text);
      g.Text(Rect(r1.x, r1.y + 28, tw, 18), T("The firmware drops cTGP after a display off/on. Control Center Advanced re-applies it."), FontId::Small, th.muted);
      c.ui.Tip(Rect(r1.x, r1.y + 28, tw, 18), g.TextW(T("The firmware drops cTGP after a display off/on. Control Center Advanced re-applies it."), FontId::Small) > tw
                                                    ? T("The firmware drops cTGP after a display off/on. Control Center Advanced re-applies it.")
                                                    : "");
      if (c.ui.Switch("sw#cfg.keep", Rect(r1.r() - 44, r1.cy() - 11, 44, 22), a.B("cfg.keep", true), true)) a.Cfg("cfg.keep", a.B("cfg.keep", true) ? "0" : "1");
      const Rect r2 = k.Row(kRowToggle);
      g.Text(Rect(r2.x, r2.y + 6, tw, 22), T("Also write the OEM profile file"), FontId::Body, oemSyncOk ? th.text : th.faint);
      const std::string d2 = oemSyncOk ? T("Makes the value survive when the OEM Control Center service restarts.")
                                       : (a.CapWhy("oem.sync").empty() ? T("OEM Control Center profile files were not found.") : a.CapWhy("oem.sync"));
      g.Text(Rect(r2.x, r2.y + 28, tw, 18), d2, FontId::Small, oemSyncOk ? th.muted : th.amber);
      c.ui.Tip(Rect(r2.x, r2.y + 28, tw, 18), g.TextW(d2, FontId::Small) > tw ? d2 : "");
      if (c.ui.Switch("sw#cfg.oemSync", Rect(r2.r() - 44, r2.cy() - 11, 44, 22), a.B("cfg.oemSync", false), oemSyncOk)) a.Cfg("cfg.oemSync", a.B("cfg.oemSync", false) ? "0" : "1");
    }
  }

  // =================================================================== the vendor's defaults for this mode
  {
    const std::string note = T("Asks the vendor's service to put its default values back into the Custom mode: the power limits, the thermal offset, the GPU clocks and the temperature target. The GPU power above is not part of it.");
    const float nh = NoteHeight(c, m, note);
    Card k(c, m, T("Vendor defaults"), T("Start over with the values the vendor ships"), IconId::Undo, nh + 6 + kRowButtons);
    k.Note(note);
    k.Gap(6);
    const bool can = linkUp && mode == 3 && !a.B("cfg.readonly") && !a.Pending("cmd:perf.restore");
    if (k.Button("cu#restore", T("Restore defaults"), Btn::Normal, IconId::Undo, can)) {
      ModalSpec ms;
      ms.title = T("Restore the vendor's defaults?");
      ms.body = T("The vendor's service puts its own default values back into the Custom mode (power limits, thermal offset, GPU clocks and temperature target). What you set there is replaced. The GPU power (cTGP) is not changed.");
      ms.icon = IconId::Undo;
      ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Restore"), Btn::Primary, 1}};
      App* ap = &a;
      c.ui.ModalOpen(ms, [ap](int id) {
        if (id != 1) return;
        ap->ClearDrafts();
        ap->Cmd("perf.restore", KV(), T("The vendor's default values were asked for"));
      });
    }
  }
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
