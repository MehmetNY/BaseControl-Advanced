// "Custom profiles": every value that shapes power and performance, each with a LIVE reading and a Confirm button,
// a pending-changes bar, saved profiles, and the advanced (ThrottleStop-style) CPU register panel, which is docked to the
// right of the page (or drawn inline on a narrow window).
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
const char* const kKeys[] = {"ec.pl1",        "ec.pl2",       "ec.pl4",         "ec.tcc",        "gpu.ctgp",      "nv.offset.core", "nv.offset.mem",
                             "os.min",        "os.max",       "os.epp",         "os.park",       "msr.pl1",       "msr.pl2",        "msr.tau1",
                             "msr.tau2",      "msr.psys.pl1", "msr.psys.pl2",   "msr.icc",       "msr.volt.core", "msr.volt.cache", "msr.volt.sa"};
// A limit that goes up is applied from the outermost gate inwards, one that goes down from the innermost outwards, so that
// PL1 <= PL2 <= PL4 holds after every single step.
const char* const kRaiseOrder[] = {"ec.pl4", "ec.pl2", "ec.pl1", "msr.psys.pl2", "msr.psys.pl1", "msr.pl2", "msr.pl1", "os.max", "os.min"};
const char* const kLowerOrder[] = {"ec.pl1", "ec.pl2", "ec.pl4", "msr.pl1", "msr.pl2", "msr.psys.pl1", "msr.psys.pl2", "os.min", "os.max"};

struct Pend {
  std::string key, value, title;
  int v = 0, live = 0;
  bool isEnum = false;
};

int LiveInt(const App& a, const std::string& k) { return static_cast<int>(std::lround(a.N(k))); }

std::string TitleOf(const std::string& key) {
  static const struct {
    const char* k;
    const char* t;
  } kT[] = {{"ec.pl1", N_("PL1")},
            {"ec.pl2", N_("PL2")},
            {"ec.pl4", N_("PL4")},
            {"ec.tcc", N_("Thermal throttle point")},
            {"gpu.ctgp", N_("GPU power (cTGP)")},
            {"nv.offset.core", N_("GPU core offset")},
            {"nv.offset.mem", N_("GPU memory offset")},
            {"os.min", N_("Minimum processor state")},
            {"os.max", N_("Maximum processor state")},
            {"os.epp", N_("Energy preference")},
            {"os.park", N_("Core parking")},
            {"os.boost", N_("Boost mode")},
            {"msr.pl1", N_("CPU PL1")},
            {"msr.pl2", N_("CPU PL2")},
            {"msr.tau1", N_("PL1 time window")},
            {"msr.tau2", N_("PL2 time window")},
            {"msr.psys.pl1", N_("Platform PL1")},
            {"msr.psys.pl2", N_("Platform PL2")},
            {"msr.icc", N_("Current limit (IccMax)")},
            {"msr.volt.core", N_("Core voltage offset")},
            {"msr.volt.cache", N_("Cache voltage offset")},
            {"msr.volt.sa", N_("System agent voltage offset")}};
  for (auto& e : kT)
    if (key == e.k) return T(e.t);
  return key;
}

std::vector<Pend> Collect(App& a) {
  std::vector<Pend> out;
  for (const char* k : kKeys) {
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
  const SettingDef* bd = FindSetting("os.boost");
  const int bi = a.Int("ed#os.boost", -1);
  if (bd && a.Has("os.boost") && bi >= 0 && bi < EnumCount(*bd) && EnumIndex(*bd, a.S("os.boost")) != bi) {
    Pend p;
    p.key = "os.boost";
    p.isEnum = true;
    p.v = bi;
    p.value = EnumName(*bd, bi);
    p.title = TitleOf("os.boost");
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
  if (AnyPend(v, {"ec.pl1", "ec.pl2", "ec.pl4"})) {
    const int p1 = Eff(a, v, "ec.pl1"), p2 = Eff(a, v, "ec.pl2"), p4 = Eff(a, v, "ec.pl4");
    if (p1 > p2) return Tf("PL1 (%d W) cannot be above PL2 (%d W).", p1, p2);
    if (p2 > p4) return Tf("PL2 (%d W) cannot be above PL4 (%d W).", p2, p4);
    if (a.I("ec.pl4.scale", 1) == 2 && p4 % 2) return T("PL4 must be a multiple of 2 W on this firmware.");
  }
  if (AnyPend(v, {"msr.pl1", "msr.pl2"}) && Eff(a, v, "msr.pl1") > Eff(a, v, "msr.pl2"))
    return Tf("CPU PL1 (%d W) cannot be above PL2 (%d W).", Eff(a, v, "msr.pl1"), Eff(a, v, "msr.pl2"));
  if (AnyPend(v, {"msr.psys.pl1", "msr.psys.pl2"}) && Eff(a, v, "msr.psys.pl1") > Eff(a, v, "msr.psys.pl2"))
    return Tf("Platform PL1 (%d W) cannot be above PL2 (%d W).", Eff(a, v, "msr.psys.pl1"), Eff(a, v, "msr.psys.pl2"));
  if (AnyPend(v, {"os.min", "os.max"}) && Eff(a, v, "os.min") > Eff(a, v, "os.max"))
    return Tf("Minimum processor state (%d%%) cannot be above the maximum (%d%%).", Eff(a, v, "os.min"), Eff(a, v, "os.max"));
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

SliderCfg Cfg(const char* key, const std::string& title, const std::string& desc) {
  SliderCfg c;
  c.key = key;
  c.title = title;
  c.desc = desc;
  return c;
}

float SliderRows(int n) { return static_cast<float>(n) * kRowSlider; }

std::string Dash() { return "\xE2\x80\x94"; }

// ---------------------------------------------------------------------------------------------------------------
// The advanced CPU cards. `m` is the dock's single column or the page's own masonry (narrow window).
void AdvancedCards(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  Gfx& g = c.ui.gfx;
  (void)g;

  if (!a.CapOk("msr")) {
    const std::string why = a.CapWhy("msr").empty() ? T("The register driver is not available.") : a.CapWhy("msr");
    const std::string body = T("The advanced panel writes CPU registers through the PawnIO driver with its IntelMSR module. Control Center Advanced does not ship them. Follow docs/SETUP.md to install both once, then restart the service.");
    const float nh = NoteHeight(c, m, why) + NoteHeight(c, m, body);
    Card k(c, m, T("Advanced CPU registers unavailable"), T("Direct register access is switched off"), IconId::Lock, nh + kRowButtons);
    k.Note(why, th.amber);
    k.Note(body);
    if (k.Button("cu#setup", T("Open the setup guide"), Btn::Normal, IconId::External)) a.OpenUrl("https://github.com/MehmetNY/Control-Center-Advanced/blob/main/docs/SETUP.md");
    return;
  }

  // ---- package power limits ------------------------------------------------------------------------------------
  {
    const std::string cmp = T("The EC limits and the CPU's own registers are two separate gates, and the lower one wins. Compare them here.");
    const float nh = NoteHeight(c, m, cmp);
    Card k(c, m, T("Package power limits"), T("CPU register 0x610 (the CPU's own PL1 / PL2)"), IconId::Bolt, nh + 3 * kRowInfo + 9 + SliderRows(4) + 3 * kRowToggleLean);
    k.Note(cmp);
    const bool ecAll = a.Has("ec.pl1") && a.Has("ec.pl2");
    const bool msrAll = a.Has("msr.pl1") && a.Has("msr.pl2");
    k.Info(T("EC limits (PL1 / PL2 / PL4)"), a.Has("ec.pl1") ? Num(a.N("ec.pl1")) + " / " + Num(a.N("ec.pl2")) + " / " + Num(a.N("ec.pl4")) + " W" : Dash());
    k.Info(T("CPU registers (PL1 / PL2)"), msrAll ? Num(a.N("msr.pl1")) + " / " + Num(a.N("msr.pl2")) + " W" : Dash());
    k.Info(T("Effective now (lower wins)"),
           ecAll && msrAll ? Num(std::min(a.N("ec.pl1"), a.N("msr.pl1"))) + " / " + Num(std::min(a.N("ec.pl2"), a.N("msr.pl2"))) + " W" : Dash(), th.accent,
           T("What the CPU is actually held to: the smaller of the EC value and the register value."));
    k.Divider();
    SliderCfg p1 = Cfg("msr.pl1", T("CPU PL1"), T("Sustained limit written straight to the CPU."));
    p1.softMax = 120;
    p1.validate = [&a](int v) { return v > LiveInt(a, "msr.pl2") ? Tf("PL1 cannot be above PL2 (now %d W). Confirm PL2 first, or use Confirm all.", LiveInt(a, "msr.pl2")) : std::string(); };
    k.Slider(p1);
    SliderCfg p2 = Cfg("msr.pl2", T("CPU PL2"), T("Short boost limit written straight to the CPU."));
    p2.softMax = 120;
    p2.validate = [&a](int v) { return v < LiveInt(a, "msr.pl1") ? Tf("PL2 cannot be below PL1 (now %d W). Confirm PL1 first, or use Confirm all.", LiveInt(a, "msr.pl1")) : std::string(); };
    k.Slider(p2);
    k.Slider(Cfg("msr.tau1", T("PL1 time window"), T("How long the CPU may average above PL1, in seconds.")));
    SliderCfg t2 = Cfg("msr.tau2", T("PL2 time window"), T("How long the PL2 boost may last, in milliseconds."));
    t2.vmax = 100;   // the hardware default is about 2.4 ms; a linear track up to 10 s would be unusable
    k.Slider(t2);
    k.BoolRow("msr.pl1.en", T("PL1 limit enabled"), "");
    k.BoolRow("msr.pl2.en", T("PL2 limit enabled"), "");
    k.BoolRow("msr.clamp", T("Allow going below the OS-requested speed"), "");
  }

  // ---- platform (PSys) limit -----------------------------------------------------------------------------------
  {
    const std::string note = T("The platform limit bounds the whole machine as the CPU's PSys meter sees it. If it is lower than what CPU and GPU want together, both are slowed down at the same time. A firmware that locks it makes the row read-only.");
    const float nh = NoteHeight(c, m, note);
    Card k(c, m, T("Platform power (PSys)"), T("CPU register 0x65C: one budget for CPU and GPU together"), IconId::Plug, nh + SliderRows(2) + 2 * kRowToggleLean + kRowInfo);
    k.Note(note);
    SliderCfg p1 = Cfg("msr.psys.pl1", T("Platform PL1"), T("Sustained limit for the whole platform."));
    p1.softMax = 230;
    p1.validate = [&a](int v) { return v > LiveInt(a, "msr.psys.pl2") ? Tf("Platform PL1 cannot be above PL2 (now %d W). Confirm PL2 first, or use Confirm all.", LiveInt(a, "msr.psys.pl2")) : std::string(); };
    k.Slider(p1);
    SliderCfg p2 = Cfg("msr.psys.pl2", T("Platform PL2"), T("Short-term platform limit."));
    p2.softMax = 300;
    p2.validate = [&a](int v) { return v < LiveInt(a, "msr.psys.pl1") ? Tf("Platform PL2 cannot be below PL1 (now %d W). Confirm PL1 first, or use Confirm all.", LiveInt(a, "msr.psys.pl1")) : std::string(); };
    k.Slider(p2);
    k.BoolRow("msr.psys.pl1.en", T("Platform PL1 enabled"), "");
    k.BoolRow("msr.psys.pl2.en", T("Platform PL2 enabled"), "");
    const bool lk = a.B("msr.psys.lock");
    k.Info(T("Locked until reboot"), a.Has("msr.psys.lock") ? (lk ? T("Yes") : T("No")) : Dash(), lk ? th.amber : 0);
  }

  // ---- current + voltage ---------------------------------------------------------------------------------------
  {
    Card k(c, m, T("Current and voltage"), T("Current limit and voltage offsets (undervolt only)"), IconId::Thermo, SliderRows(4));
    SliderCfg ic = Cfg("msr.icc", T("Current limit (IccMax)"), T("Largest current the voltage regulator may deliver to the CPU."));
    ic.softMax = 140;
    k.Slider(ic);
    k.Slider(Cfg("msr.volt.core", T("Core voltage offset"), T("Negative values lower the core voltage. The BIOS may ignore it.")));
    k.Slider(Cfg("msr.volt.cache", T("Cache voltage offset"), T("Ring / cache voltage offset (usually tied to the core value).")));
    k.Slider(Cfg("msr.volt.sa", T("System agent voltage offset"), T("Memory controller voltage offset.")));
  }

  // ---- protection + prefetchers --------------------------------------------------------------------------------
  {
    Card k(c, m, T("Protection and prefetchers"), T("Throttle request line and hardware prefetch switches"), IconId::Shield, kRowToggle + 9 + 30 + 4 * kRowToggleLean);
    k.BoolRow("msr.bdp", T("Let other chips throttle the CPU (BD PROCHOT)"),
              T("Turn off to stop the EC or VRM from forcing the CPU down. Protection is lower."),
              T("Without this line the embedded controller and the power stage can no longer slow the CPU down when they get too hot or are overloaded. The CPU still protects itself, but other parts may not be. Turn it off only while you watch the temperatures."),
              "");
    k.Divider();
    k.SubHead(T("Hardware prefetchers"));
    k.BoolRow("msr.pf.l2", T("L2 hardware prefetcher"), "");
    k.BoolRow("msr.pf.l2adj", T("L2 adjacent line prefetcher"), "");
    k.BoolRow("msr.pf.dcu", T("DCU streamer prefetcher"), "");
    k.BoolRow("msr.pf.dcuip", T("DCU IP prefetcher"), "");
  }

  // ---- facts ---------------------------------------------------------------------------------------------------
  {
    Card k(c, m, T("CPU facts"), T("Read-only values from the CPU registers"), IconId::Info, 9 * kRowInfo);
    k.Info(T("Rated power (TDP)"), a.Has("msr.tdp") ? With(a.N("msr.tdp"), 0, "W") : Dash());
    k.Info(T("Allowed power range"), a.Has("msr.minw") ? Num(a.N("msr.minw")) + " - " + Num(a.N("msr.maxw")) + " W" : Dash());
    k.Info(T("Maximum junction temperature"), a.Has("msr.tjmax") ? With(a.N("msr.tjmax"), 0, "\xC2\xB0""C") : Dash());
    k.Info(T("Thermal offset (TCC)"), a.Has("msr.tcc") ? With(a.N("msr.tcc"), 0, "\xC2\xB0""C") : Dash());
    k.Info(T("Base ratio"), a.Has("cpu.base.ratio") ? "x" + Num(a.N("cpu.base.ratio")) : Dash());
    k.Info(T("Turbo ratios (1 to 8 cores)"), a.S("msr.turbo").empty() ? Dash() : a.S("msr.turbo"));
    std::string locks;
    bool anyLock = false;
    auto lock = [&](const char* key, const std::string& name) {
      if (!a.Has(key)) return;
      const bool l = a.B(key);
      anyLock |= l;
      locks += (locks.empty() ? "" : "  ") + name + (l ? " \xE2\x9C\x97" : " \xE2\x9C\x93");
    };
    lock("msr.lock", T("package"));
    lock("msr.icc.lock", T("current"));
    lock("msr.psys.lock", T("platform"));
    k.Info(T("Locked until reboot"), locks.empty() ? Dash() : locks, anyLock ? th.amber : th.green,
           T("A locked register ignores writes until the computer restarts. The check mark means writable."));
    auto reasons = [&](const char* thermal, const char* prochot, const char* power, const char* extra) {
      std::string out;
      if (a.B(thermal)) out += T("thermal") + "  ";
      if (a.B(prochot)) out += "PROCHOT  ";
      if (a.B(power)) out += T("power limit") + "  ";
      if (extra && a.B(extra)) out += T("current limit") + "  ";
      return out.empty() ? T("none") : Trim(out);
    };
    const std::string now = reasons("cpu.st.thermal", "cpu.st.prochot", "cpu.st.power", "cpu.st.current");
    k.Info(T("Throttling right now"), now, now == T("none") ? th.green : th.amber);
    const std::string log = reasons("cpu.log.thermal", "cpu.log.prochot", "cpu.log.power", nullptr);
    k.Info(T("Throttled since last clear"), log, log == T("none") ? th.green : th.amber, T("The CPU keeps a record of why it was slowed down since the last reset of the register."));
  }
}

}  // namespace

float PageCustomDock(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 380, 1);
  AdvancedCards(c, m);
  return m.Bottom();
}

float PageCustom(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 450, 3);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const bool adv = a.Flag("custom.adv");
  const std::vector<Pend> pend = Collect(a);
  const std::string verr = ValidateAll(a, pend);

  // =================================================================== pending changes bar
  {
    const bool compact = r.w < 800;
    const bool any = !pend.empty();
    const float rowH = compact ? 58.0f : 46.0f;
    Card k(c, m, "", "", IconId::None, compact ? rowH + 8.0f + 38.0f : rowH, true);
    const Rect row = k.Row(rowH);
    g.FillRound(Rect(row.x, row.y + 3, 40, 40), 12, WithAlpha(any ? th.amber : th.green, 30));
    g.Icon(any ? IconId::Sliders : IconId::Check, Rect(row.x + 9, row.y + 12, 22, 22), any ? th.amber : th.green, 1.8f);

    const float bw3 = 132, bw2 = 118, bw1 = adv ? 176.0f : 168.0f;
    const float btnsW = bw1 + bw2 + bw3 + 24;
    Rect b1, b2, b3;
    float textR = row.r();
    if (!compact) {
      b3 = Rect(row.r() - bw3, row.y + 4, bw3, 38);
      b2 = Rect(b3.x - 12 - bw2, row.y + 4, bw2, 38);
      b1 = Rect(b2.x - 12 - bw1, row.y + 4, bw1, 38);
      textR = b1.x - 16;
    } else {
      const Rect brow = k.Row(46);
      const float by = brow.y + 8;
      b3 = Rect(brow.r() - bw3, by, bw3, 38);
      b2 = Rect(b3.x - 12 - bw2, by, bw2, 38);
      b1 = Rect(b2.x - 12 - bw1, by, bw1, 38);
      (void)btnsW;
    }
    const float tx = row.x + 54, tw = std::max(80.0f, textR - tx);
    std::string head;
    if (any) head = pend.size() == 1 ? T("1 change is waiting for your confirmation") : Tf("%d changes are waiting for your confirmation", static_cast<int>(pend.size()));
    else head = T("Showing live values. Nothing is waiting.");
    std::string sub = any ? T("Nothing has been sent to the hardware yet. Use each Confirm button, or Confirm all.")
                          : T("Move a slider, then press its Confirm button. Values follow changes made by other programs.");
    uint32_t sc = th.muted;
    if (any && !verr.empty()) {
      sub = verr;
      sc = th.amber;
    }
    g.Text(Rect(tx, row.y, tw, 24), head, FontId::H3, th.text);
    bool cut = false;
    g.WrappedClamped(Rect(tx, row.y + 26, tw, 36), sub, FontId::Small, sc, compact ? 2 : 1, 2.0f, &cut);
    if (cut) c.ui.Tip(Rect(tx, row.y + 24, tw, 36), sub);

    if (c.ui.Button("cu#adv", b1, adv ? T("Hide advanced CPU") : T("Advanced CPU..."), adv ? Btn::Primary : Btn::Normal, adv ? IconId::ChevronR : IconId::Cpu, true))
      a.Flag("custom.adv") = !adv;
    c.ui.Tip(b1, T("ThrottleStop-style registers: package and platform power limits, time windows, current limit, voltage offsets, prefetchers, BD PROCHOT."));
    if (c.ui.Button("cu#discard", b2, T("Discard all"), Btn::Ghost, IconId::Undo, any)) {
      a.ClearDrafts();
      a.Int("ed#os.boost", -1) = -1;
    }
    if (c.ui.Button("cu#confirmall", b3, T("Confirm all"), Btn::Primary, IconId::Check, any && verr.empty())) ConfirmAll(a, pend);
    c.ui.Tip(b3, T("Applies every pending change in a safe order (limits that go up from the outside in, limits that go down from the inside out) and stops at the first problem."));
  }

  // =================================================================== advanced cards inline (narrow window)
  if (adv && !a.docked()) AdvancedCards(c, m);

  // =================================================================== saved profiles
  {
    std::vector<std::string> names;
    const std::string list = a.S("profile.list");
    if (!list.empty()) names = Split(list, ',');
    const std::string active = a.S("profile.active");
    auto builtin = [&](const std::string& n) { return IsBuiltinProfile(n); };
    const float inner = m.InnerW(true);
    float x = 0, y = 0;
    struct Item {
      std::string name;
      Rect r;
      bool del;
    };
    std::vector<Item> items;
    for (const std::string& n : names) {
      const bool custom = !builtin(n);
      const float w = std::min(inner, g.TextW(ProfileLabel(n), FontId::Label) + 40 + (custom ? 38 : 0));
      if (x > 0 && x + w > inner) {
        x = 0;
        y += 46;
      }
      items.push_back({n, Rect(x, y, w, 38), custom});
      x += w + 10;
    }
    const float flowH = names.empty() ? 0 : y + 38 + 12;
    Card k(c, m, T("Profiles"), T("Apply a saved set of values, or save the current live values under a new name."), IconId::Profiles, flowH + kRowButtons + 6, true);
    for (const Item& it : items) {
      const Rect rc(k.x0 + it.r.x, k.y + it.r.y, it.r.w, it.r.h);
      const bool isActive = it.name == active;
      const Rect main(rc.x, rc.y, rc.w - (it.del ? 38 : 0), rc.h);
      const std::string shown = ProfileLabel(it.name);
      const std::string nm = it.name;
      if (c.ui.Button("pf#" + it.name, main, shown, isActive ? Btn::Primary : Btn::Normal, isActive ? IconId::Check : IconId::None, !a.Pending("cmd:profile.apply"))) {
        KV args;
        args.Set("name", nm);
        a.Cmd("profile.apply", args, Tf("Profile %s applied", shown.c_str()));
      }
      if (it.del && c.ui.IconButton("pd#" + it.name, Rect(rc.r() - 36, rc.y, 36, rc.h), IconId::Trash, true, th.red)) {
        ModalSpec ms;
        ms.title = T("Delete this profile?");
        ms.body = Tf("The saved profile \"%s\" will be removed. Your current hardware settings are not changed.", nm.c_str());
        ms.icon = IconId::Trash;
        ms.iconColor = th.red;
        ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Delete"), Btn::Danger, 1}};
        App* ap = &a;
        c.ui.ModalOpen(ms, [ap, nm](int id) {
          if (id == 1) {
            KV args;
            args.Set("name", nm);
            ap->Cmd("profile.delete", args, Tf("Profile %s deleted", nm.c_str()));
          }
        });
      }
    }
    k.Gap(flowH);
    std::string& nm = a.Str("custom.newname");
    const bool valid = !Trim(nm).empty();
    if (k.InputRow("pname", &nm, T("Name for the new profile"), T("Save current values"), valid && !a.Pending("cmd:profile.save"), 24)) {
      KV args;
      args.Set("name", Trim(nm));
      a.Cmd("profile.save", args, Tf("Profile %s saved", Trim(nm).c_str()));
      nm.clear();
    }
  }

  // =================================================================== CPU power limits (embedded controller)
  {
    const std::string oemNote = T("OEM maximum profile on this laptop: PL1 120 W, PL2 120 W, PL4 200 W. Anything above needs your confirmation.");
    const float nh = NoteHeight(c, m, oemNote);
    Card k(c, m, T("CPU power limits"), T("Embedded controller (EC): the limits the laptop firmware enforces"), IconId::Cpu, SliderRows(4) + 9 + nh);
    SliderCfg s1 = Cfg("ec.pl1", T("PL1 - sustained power"), T("Long-term CPU package power. Sets how hot the laptop runs under a lasting load."));
    s1.softMax = 120;
    s1.validate = [&a](int v) {
      const int pl2 = LiveInt(a, "ec.pl2");
      return v > pl2 ? Tf("PL1 cannot be above PL2 (now %d W). Confirm PL2 first, or use Confirm all.", pl2) : std::string();
    };
    k.Slider(s1);
    SliderCfg s2 = Cfg("ec.pl2", T("PL2 - boost power"), T("Short-term boost limit used for the first seconds of a load."));
    s2.softMax = 120;
    s2.validate = [&a](int v) {
      if (v < LiveInt(a, "ec.pl1")) return Tf("PL2 cannot be below PL1 (now %d W). Confirm PL1 first, or use Confirm all.", LiveInt(a, "ec.pl1"));
      if (v > LiveInt(a, "ec.pl4")) return Tf("PL2 cannot be above PL4 (now %d W). Confirm PL4 first, or use Confirm all.", LiveInt(a, "ec.pl4"));
      return std::string();
    };
    k.Slider(s2);
    SliderCfg s4 = Cfg("ec.pl4", T("PL4 - peak power"), T("The highest instantaneous power the EC lets the CPU draw."));
    s4.softMax = 200;
    s4.validate = [&a](int v) {
      if (v < LiveInt(a, "ec.pl2")) return Tf("PL4 cannot be below PL2 (now %d W).", LiveInt(a, "ec.pl2"));
      if (a.I("ec.pl4.scale", 1) == 2 && v % 2) return T("PL4 must be a multiple of 2 W on this firmware.");
      return std::string();
    };
    k.Slider(s4);
    k.Slider(Cfg("ec.tcc", T("Thermal throttle point"), T("Temperature at which the CPU starts slowing down. 100 \xC2\xB0""C is the Intel default.")));
    k.Divider();
    k.Note(oemNote);
  }

  // =================================================================== GPU power (cTGP) + MONSTER GPU POWER integration
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

  // =================================================================== GPU clock offsets
  {
    const std::string note = T("Offsets go to the NVIDIA driver and are lost when it reloads or the PC restarts, unless Keep is on.");
    const float nh = NoteHeight(c, m, note);
    Card k(c, m, T("GPU clocks"), T("NVIDIA driver clock offsets"), IconId::Chip, SliderRows(2) + 9 + nh);
    SliderCfg s1 = Cfg("nv.offset.core", T("Core clock offset"), T("Added to the graphics clock. Raise it slowly and test for stability."));
    s1.softMax = 150;
    k.Slider(s1);
    SliderCfg s2 = Cfg("nv.offset.mem", T("Memory clock offset"), T("Added to the video memory clock. Too high a value causes artifacts or crashes."));
    s2.softMax = 500;
    k.Slider(s2);
    k.Divider();
    k.Note(note);
  }

  // =================================================================== Windows CPU policy
  {
    Card k(c, m, T("Windows CPU policy"), T("Power plan values of the active plan. No firmware involved."), IconId::Performance, SliderRows(5));
    SliderCfg smin = Cfg("os.min", T("Minimum processor state"), T("Lowest CPU speed Windows will use. Keep it low for efficiency."));
    smin.validate = [&a](int v) { return v > LiveInt(a, "os.max") ? Tf("Cannot be above the maximum (now %d%%). Confirm the maximum first, or use Confirm all.", LiveInt(a, "os.max")) : std::string(); };
    k.Slider(smin);
    SliderCfg smax = Cfg("os.max", T("Maximum processor state"), T("Highest CPU speed Windows will use. 100% means no cap."));
    smax.validate = [&a](int v) { return v < LiveInt(a, "os.min") ? Tf("Cannot be below the minimum (now %d%%). Confirm the minimum first, or use Confirm all.", LiveInt(a, "os.min")) : std::string(); };
    k.Slider(smax);
    k.Slider(Cfg("os.epp", T("Energy preference"), T("0 favours speed, 100 favours efficiency (Energy Performance Preference).")));
    k.Slider(Cfg("os.park", T("Core parking"), T("Share of cores Windows keeps awake. 100% disables parking.")));
    k.EnumConfirm("os.boost", T("Boost mode"), T("How eagerly Windows lets the CPU use turbo frequencies."),
                  {T("Disabled"), T("Enabled"), T("Aggressive"), T("Efficient"), T("Efficient aggressive"), T("Aggressive at guaranteed"), T("Efficient aggressive at guaranteed")});
  }
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
