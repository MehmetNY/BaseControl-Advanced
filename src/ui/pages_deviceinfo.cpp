// Device info: what is inside this laptop and how it is doing right now. The processor, graphics, memory and fans each get live gauges,
// every internal drive has its own tile (model, size, interface, power-on count and time, temperature, wear), and a "System" card
// carries the firmware and OS facts plus a button that copies a plain-text description for bug reports.
// Everything on this page is read-only: nothing here can change the hardware.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../common/hostdata.h"
#include "../common/util.h"
#include "page.h"
#include "pages_shared.h"

namespace bc {
namespace ui {
namespace {

constexpr float kGaugeRowH = 130;
constexpr float kTileH = 204;

std::string Dash() { return "\xE2\x80\x94"; }
std::string Deg() { return "\xC2\xB0""C"; }
std::string Dot() { return "  \xC2\xB7  "; }

// Amber normally, going through orange to red as a temperature nears what the part is rated for.
uint32_t HeatColor(const Theme& th, double t, double warn, double crit) {
  if (t >= crit) return th.red;
  if (t >= warn) return Mix(th.amber, th.red, 0.5f);
  return th.amber;
}

// "11th Gen Intel(R) Core(TM) i7-11800H @ 2.30GHz" -> without the clock, which has its own row.
std::string BrandNoClock(const std::string& brand) {
  const size_t at = brand.find(" @ ");
  return at == std::string::npos ? brand : Trim(brand.substr(0, at));
}

std::string GbText(double mb) { return Num(mb / 1024.0, 1) + " GB"; }

// Whole gigabytes when the size is one ("16 GB"), one decimal otherwise ("7.5 GB").
std::string ModuleGb(long long mb) { return mb % 1024 == 0 ? Fmt("%lld GB", mb / 1024) : GbText(static_cast<double>(mb)); }

std::string CpuFreq(double mhz) { return mhz >= 1000 ? Num(mhz / 1000.0, 2) + " GHz" : Num(mhz, 0) + " MHz"; }

// 116 hours -> "4 days 20 hours"
std::string DurationText(long long hours) {
  if (hours < 0) return Dash();
  if (hours < 1) return T("Less than an hour");
  const long long d = hours / 24, h = hours % 24;
  const std::string hs = Tf(h == 1 ? "%lld hour" : "%lld hours", h);
  if (d == 0) return hs;
  const std::string ds = Tf(d == 1 ? "%lld day" : "%lld days", d);
  return h == 0 ? ds : ds + " " + hs;
}

std::string TimesText(long long n) { return n < 0 ? Dash() : Tf(n == 1 ? "%lld time" : "%lld times", n); }

std::string WrittenText(long long gb) {
  if (gb < 0) return Dash();
  if (gb >= 1000) return Num(static_cast<double>(gb) / 1000.0, gb >= 10000 ? 0 : 1) + " TB";
  return Num(static_cast<double>(gb), 0) + " GB";
}

// NVMe "critical warning" bits, in words.
std::string WarnText(int bits) {
  std::string s;
  auto add = [&](int bit, const std::string& t) {
    if (bits & bit) s += (s.empty() ? "" : ", ") + t;
  };
  add(0x01, T("spare capacity is low"));
  add(0x02, T("temperature is out of range"));
  add(0x04, T("reliability is degraded"));
  add(0x08, T("it switched to read-only"));
  add(0x10, T("its memory backup failed"));
  add(0x20, T("its persistent memory is read-only"));
  return Tf("The drive itself reports a problem: %s.", s.empty() ? T("unspecified").c_str() : s.c_str());
}

// A label/value line placed anywhere (the same look as Card::Info).
void InfoAt(Ui& ui, const Rect& r, const std::string& label, const std::string& value, uint32_t col = 0, const std::string& tip = "") {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const float vw = std::min(g.TextW(value, FontId::Body) + 2, r.w * 0.62f);
  g.Text(Rect(r.x, r.y, r.w - vw - 10, r.h), label, FontId::Body, th.muted);
  g.Text(Rect(r.r() - vw, r.y, vw, r.h), value, FontId::Body, col ? col : th.text, Align::Right);
  ui.Tip(r, tip);
}

struct GaugeSpec {
  float frac = 0;
  uint32_t col = 0;
  std::string big, unit, label, tip;
};

void GaugeSlot(Ctx& c, const Rect& slot, const GaugeSpec& s) {
  Gfx& g = c.ui.gfx;
  const Rect gr(slot.cx() - 52, slot.y, 104, 104);
  Gauge(c.ui, gr, s.frac, s.col, s.big, s.unit);
  g.Text(Rect(slot.x, slot.y + 104, slot.w, 20), s.label, FontId::Small, c.ui.th.muted, Align::Center);
  c.ui.Tip(Rect(gr.x, gr.y, gr.w, gr.h + 20), s.tip);
}

void GaugeRow(Card& k, const std::vector<GaugeSpec>& specs) {
  const Rect row = k.Row(kGaugeRowH);
  const float sw = row.w / static_cast<float>(specs.size());
  for (size_t i = 0; i < specs.size(); i++) GaugeSlot(k.c, Rect(row.x + sw * static_cast<float>(i), row.y + 2, sw, 124), specs[i]);
}

// Usage / temperature gauges share their look: frac from the value, "—" when the snapshot has no reading.
GaugeSpec UsageGauge(App& a, const std::string& key, uint32_t col, const std::string& tip = "") {
  GaugeSpec s;
  s.col = col;
  s.unit = "%";
  s.label = T("Usage");
  s.tip = tip;
  if (a.Has(key)) {
    const double v = a.N(key);
    s.frac = static_cast<float>(v / 100.0);
    s.big = Num(v, 0);
  } else {
    s.big = Dash();
    s.unit.clear();
  }
  return s;
}

GaugeSpec TempGauge(App& a, const std::string& key, double warn, double crit, double full, const std::string& tip = "") {
  GaugeSpec s;
  s.unit = Deg();
  s.label = T("Temperature");
  s.tip = tip;
  s.col = a.ui().th.amber;
  if (a.Has(key) && a.N(key) > 0) {
    const double v = a.N(key);
    s.frac = static_cast<float>(v / full);
    s.big = Num(v, 0);
    s.col = HeatColor(a.ui().th, v, warn, crit);
  } else {
    s.big = Dash();
    s.unit.clear();
  }
  return s;
}

// Adapters Windows lists that are not the one NVML reports on (usually the integrated GPU).
std::vector<std::string> OtherAdapters(const std::string& all, const std::string& primary) {
  std::vector<std::string> out;
  const std::string p = Lower(primary);
  for (const std::string& n : Split(all, ';')) {
    const std::string t = Trim(n);
    if (t.empty()) continue;
    if (!p.empty() && (Lower(t) == p || p.find(Lower(t)) != std::string::npos)) continue;
    out.push_back(t);
  }
  return out;
}

// "2 x 16 GB" when the modules match, "16 GB + 8 GB" otherwise.
std::string ModuleSizes(App& a, int n) {
  std::vector<long long> mb;
  for (int i = 0; i < n; i++) mb.push_back(a.I(Fmt("host.mem.%d.mb", i)));
  bool same = true;
  for (long long v : mb) same = same && v == mb[0];
  if (same) return n == 1 ? ModuleGb(mb[0]) : Fmt("%d \xC3\x97 ", n) + ModuleGb(mb[0]);
  std::string s;
  for (long long v : mb) s += (s.empty() ? "" : " + ") + ModuleGb(v);
  return s;
}

// ------------------------------------------------------------------------------------------------ cards
void CardProcessor(Ctx& c, Masonry& m) {
  App& a = c.app;
  Card k(c, m, T("Processor"), BrandNoClock(a.S("host.cpu")), IconId::Cpu, kGaugeRowH + 8 + 3 * kRowInfo);
  const Theme& th = c.ui.th;
  GaugeRow(k, {UsageGauge(a, "os.cpu.pct", th.accent, T("Load across all logical processors.")),
               TempGauge(a, "cpu.temp", 85, 95, 100, T("Package temperature (the hotter of the sensors that are available)."))});
  k.Gap(8);
  k.Info(T("Cores / threads"), a.Has("host.cores") ? Tf("%d / %d", static_cast<int>(a.I("host.cores")), static_cast<int>(a.I("host.threads"))) : Dash());
  k.Info(T("Base frequency"), a.Has("host.cpu.mhz") ? CpuFreq(a.N("host.cpu.mhz")) : Dash());
  k.Info(T("Current frequency"), a.Has("os.cpu.mhz") ? CpuFreq(a.N("os.cpu.mhz")) : Dash(), 0, T("Average over all logical processors, as Windows reports it."));
}

void CardGraphics(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  const bool nv = a.Has("gpu.name");
  const std::vector<std::string> others = OtherAdapters(a.S("host.gpus"), a.S("gpu.name"));
  if (!nv) {
    // no NVIDIA readings: list what Windows knows about, with the reason
    const std::string note = T("Live readings need the NVIDIA driver (NVML), which is not available.");
    const std::vector<std::string> all = OtherAdapters(a.S("host.gpus"), "");
    Card k(c, m, T("Graphics"), all.empty() ? T("Not detected") : all[0], IconId::Gpu, NoteHeight(c, m, note) + all.size() * kRowInfo);
    k.Note(note);
    for (size_t i = 0; i < all.size(); i++) k.Info(Tf("Adapter %d", static_cast<int>(i + 1)), all[i]);
    return;
  }
  const float rows = 5 + (others.empty() ? 0 : 1);
  Card k(c, m, T("Graphics"), a.S("gpu.name"), IconId::Gpu, kGaugeRowH + 8 + rows * kRowInfo);
  GaugeRow(k, {UsageGauge(a, "gpu.util", th.blue, T("Graphics engine load reported by the NVIDIA driver.")), TempGauge(a, "gpu.temp", 80, 90, 100)});
  k.Gap(8);
  if (a.Has("gpu.vram.total")) {
    std::string v = GbText(a.N("gpu.vram.total"));
    if (a.Has("gpu.vram.used")) v = Tf("%.1f of %.0f GB in use", a.N("gpu.vram.used") / 1024.0, a.N("gpu.vram.total") / 1024.0);
    k.Info(T("Video memory"), v);
  } else {
    k.Info(T("Video memory"), Dash());
  }
  k.Info(T("Core clock"), a.Has("gpu.clock") ? Tf("%d MHz", static_cast<int>(a.N("gpu.clock"))) : Dash());
  k.Info(T("Memory clock"), a.Has("gpu.memclock") ? Tf("%d MHz", static_cast<int>(a.N("gpu.memclock"))) : Dash());
  k.Info(T("Performance state"), a.Has("gpu.pstate") ? Fmt("P%d", static_cast<int>(a.I("gpu.pstate"))) : Dash(),
         0, T("P0 is full speed, higher numbers are idle states."));
  k.Info(T("Power draw"), a.Has("gpu.power") ? (a.Has("gpu.limit") && a.N("gpu.limit") > 0 ? Tf("%.0f of %.0f W", a.N("gpu.power"), a.N("gpu.limit")) : Watts(a.N("gpu.power"), 0)) : Dash());
  if (!others.empty()) k.Info(T("Also installed"), Join(others, ", "));
}

void CardMemory(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  const int n = static_cast<int>(std::min<long long>(a.I("host.mem.n"), 8));
  std::string sub;
  if (n > 0) {
    sub = a.S("host.mem.0.type");
    const long long mts = a.I("host.mem.0.run", a.I("host.mem.0.rated"));
    if (mts > 0) sub += (sub.empty() ? "" : Dot()) + Tf("%lld MT/s", mts);
  }
  const float rows = 2 + (n > 0 ? 1 : 0);
  const float mods = n > 0 ? 30 + n * 50.0f : 0;
  Card k(c, m, T("Memory"), sub, IconId::Ram, kGaugeRowH + 8 + rows * kRowInfo + mods);
  GaugeRow(k, {UsageGauge(a, "os.mem.pct", th.green, T("Share of the installed memory that Windows has in use."))});
  k.Gap(8);
  k.Info(T("Total"), a.Has("os.mem.total") ? GbText(a.N("os.mem.total")) : (a.S("host.ram").empty() ? Dash() : a.S("host.ram")),
         0, T("What Windows can use. A little of the installed memory is reserved by the hardware."));
  k.Info(T("In use"), a.Has("os.mem.used") ? GbText(a.N("os.mem.used")) : Dash());
  if (n > 0) {
    k.Info(T("Modules"), ModuleSizes(a, n));
    k.SubHead(T("Installed modules"));
    Gfx& g = c.ui.gfx;
    for (int i = 0; i < n; i++) {
      const std::string p = Fmt("host.mem.%d.", i);
      const Rect r = k.Row(50);
      std::string slot = a.S(p + "slot");
      if (slot.empty()) slot = Tf("Module %d", i + 1);
      std::string right = ModuleGb(a.I(p + "mb"));
      const long long mts = a.I(p + "run", a.I(p + "rated"));
      if (!a.S(p + "type").empty()) right += Dot() + a.S(p + "type") + (mts > 0 ? Fmt("-%lld", mts) : std::string());
      const float rw = g.TextW(right, FontId::Body);
      g.Text(Rect(r.x, r.y, r.w - rw - 10, 24), slot, FontId::Body, th.muted);
      g.Text(Rect(r.r() - rw, r.y, rw, 24), right, FontId::Body, th.text, Align::Right);
      const std::string who = Trim(a.S(p + "maker") + " " + a.S(p + "part"));
      if (!who.empty()) g.Text(Rect(r.x, r.y + 24, r.w, 18), who, FontId::Small, th.faint);
      c.ui.Tip(r, who.empty() ? "" : who);
    }
  }
}

void CardFans(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  Card k(c, m, T("Fans"), "", IconId::Fan, kGaugeRowH + 8 + 2 * kRowInfo);
  auto fan = [&](const char* duty, const std::string& label, uint32_t col) {
    GaugeSpec s;
    s.col = col;
    s.unit = "%";
    s.label = label;
    if (a.Has(duty)) {
      s.frac = static_cast<float>(a.N(duty) / 100.0);
      s.big = Num(a.N(duty), 0);
    } else {
      s.big = Dash();
      s.unit.clear();
    }
    s.tip = T("Fan speed as a share of its top speed.");
    return s;
  };
  GaugeRow(k, {fan("fan.cpu.duty", T("Processor"), th.accent), fan("fan.gpu.duty", T("Graphics card"), th.blue)});
  k.Gap(8);
  k.Info(T("Processor fan"), a.Has("fan.cpu.rpm") ? Tf("%d rpm", static_cast<int>(a.N("fan.cpu.rpm"))) : Dash());
  k.Info(T("Graphics card fan"), a.Has("fan.gpu.rpm") ? Tf("%d rpm", static_cast<int>(a.N("fan.gpu.rpm"))) : Dash());
}

void DriveTile(Ctx& c, const Rect& t, int idx) {
  App& a = c.app;
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::string p = Fmt("storage.%d.", idx);
  g.FillRound(t, 12, th.inset);
  g.StrokeRound(t, 12, th.line, 1.0f);

  const Rect icon(t.x + 16, t.y + 16, 38, 38);
  g.FillRound(icon, 10, WithAlpha(th.accent, 30));
  g.Icon(IconId::Drive, icon.Inset(7), th.accent, 1.8f);

  // title and "2 TB · NVMe · SSD"
  const std::string title = StorageTitle(a.S(p + "model"));
  std::string sub = FormatDriveSize(static_cast<uint64_t>(std::strtoull(a.S(p + "size").c_str(), nullptr, 10)));
  if (!a.S(p + "bus").empty()) sub += (sub.empty() ? "" : Dot()) + a.S(p + "bus");
  sub += (sub.empty() ? "" : Dot()) + (a.B(p + "ssd") ? std::string("SSD") : std::string("HDD"));
  const long long warn = a.I(p + "warn", 0);
  float chipW = 0;
  if (warn > 0) {
    const std::string t2 = T("Check the drive");
    chipW = ui.ChipW(t2);
    ui.Chip(Rect(t.r() - 16 - chipW, t.y + 25, chipW, 20), t2, th.red);
    ui.Tip(Rect(t.r() - 16 - chipW, t.y + 25, chipW, 20), WarnText(static_cast<int>(warn)));
  }
  const float tw = t.w - 70 - 16 - (chipW > 0 ? chipW + 8 : 0);
  g.Text(Rect(t.x + 68, t.y + 14, tw, 24), title, FontId::H3, th.text);
  g.Text(Rect(t.x + 68, t.y + 38, tw, 18), sub, FontId::Small, th.muted);
  ui.Tip(Rect(t.x + 68, t.y + 14, tw, 44), g.TextW(title, FontId::H3) > tw ? a.S(p + "model") : "");

  const bool health = a.Has(p + "temp") || a.Has(p + "cycles") || a.Has(p + "hours") || a.Has(p + "wear") || a.Has(p + "written");
  if (!health) {
    const std::string note = T("Health values are not available. Some drives do not report them, and some drivers refuse the query.");
    g.Wrapped(Rect(t.x + 16, t.y + 70, t.w - 32, t.h - 80), note, FontId::Small, th.muted, true);
    return;
  }
  // temperature gauge on the left, the counters on the right
  GaugeSpec ts = TempGauge(a, p + "temp", 65, 75, 90, T("Drive temperature as the drive reports it."));
  GaugeSlot(c, Rect(t.x + 8, t.y + 66, 116, 130), ts);
  const float rx = t.x + 138, rw = t.w - 138 - 16;
  float y = t.y + 72;
  auto row = [&](const std::string& label, const std::string& value, uint32_t col = 0, const std::string& tip = "") {
    InfoAt(ui, Rect(rx, y, rw, 28), label, value, col, tip);
    y += 30;
  };
  row(T("Power-on count"), a.Has(p + "cycles") ? TimesText(a.I(p + "cycles")) : Dash(), 0, T("How many times the drive has been powered up."));
  row(T("Power-on time"), a.Has(p + "hours") ? DurationText(a.I(p + "hours")) : Dash(), 0, T("Total time the drive has been powered on."));
  if (a.Has(p + "wear")) {
    const long long w = a.I(p + "wear");
    std::string tip = T("Share of the drive's rated write endurance that is used up.");
    if (a.Has(p + "spare")) tip += " " + Tf("Spare capacity left: %d %%.", static_cast<int>(a.I(p + "spare")));
    row(T("Life used"), Tf("%d %%", static_cast<int>(w)), w >= 90 ? th.red : (w >= 70 ? th.amber : 0), tip);
  } else {
    row(T("Life used"), Dash());
  }
  row(T("Data written"), a.Has(p + "written") ? WrittenText(a.I(p + "written")) : Dash(), 0, T("Total amount the computer has written to the drive."));
}

void CardStorage(Ctx& c, Masonry& m) {
  App& a = c.app;
  const bool known = a.Has("storage.n");
  const int n = known ? static_cast<int>(std::min<long long>(a.I("storage.n"), 8)) : 0;
  if (n == 0) {
    const std::string note = known ? T("No internal drives were found.") : T("Drive information is not available right now.");
    Card k(c, m, T("Storage"), "", IconId::Drive, NoteHeight(c, m, note, true), true);
    k.Note(note);
    return;
  }
  const float innerW = m.InnerW(true);
  const int cols = std::max(1, std::min(n, static_cast<int>((innerW + kGap) / (410 + kGap))));
  const int rows = (n + cols - 1) / cols;
  const float contentH = rows * kTileH + (rows - 1) * kGap;
  Card k(c, m, T("Storage"), n == 1 ? T("1 drive") : Tf("%d drives", n), IconId::Drive, contentH, true);
  const float tw = (k.w - (cols - 1) * kGap) / static_cast<float>(cols);
  for (int i = 0; i < n; i++) {
    const int col = i % cols, row = i / cols;
    DriveTile(c, Rect(k.x0 + col * (tw + kGap), k.y + row * (kTileH + kGap), tw, kTileH), i);
  }
}

void CardSystem(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  const Theme& th = ui.th;
  struct Fact {
    std::string label, value, tip;
    uint32_t col = 0;
  };
  std::vector<Fact> f;
  const std::string model = Trim(a.S("host.vendor") + " " + a.S("host.product"));
  f.push_back({T("Model"), model.empty() ? Dash() : model, "", 0});
  f.push_back({T("Mainboard"), a.S("host.board").empty() ? Dash() : a.S("host.board"), "", 0});
  f.push_back({T("BIOS version"), a.S("host.bios").empty() ? Dash() : a.S("host.bios"), "", 0});
  f.push_back({T("Operating system"), a.S("host.os").empty() ? Dash() : a.S("host.os"), "", 0});
  const std::string cls = a.S("dev.class");
  f.push_back({T("Hardware support"),
               cls == "verified" ? T("Verified model") : cls == "family" ? T("Same EC family") : cls == "unknown" ? T("Not recognised") : Dash(),
               cls == "verified" ? T("This exact model and BIOS were tested.")
                                 : cls == "family" ? T("The embedded controller matches the tested one, but the model or BIOS is a sibling.")
                                                   : T("Hardware writes stay disabled until the model is recognised."),
               cls == "verified" ? th.green : (cls == "family" ? th.amber : (cls == "unknown" ? th.red : 0))});
  const std::string mode = a.mode();
  f.push_back({T("Running as"), mode == "sim" ? T("Simulation") : T("This program, as administrator"), "", mode == "sim" ? th.amber : 0});
  const std::string ls = a.S("oem.link.state", "none");
  f.push_back({T("Vendor's service"), ls == "connected" ? T("Connected") : ls == "connecting" ? T("Connecting...") : ls == "refused" ? T("Refused by the service")
                                      : ls == "offline" ? T("Not reachable") : Dash(),
               T("The link to the vendor's Control Center service. See the Service page."),
               ls == "connected" ? th.green : (ls == "connecting" ? th.amber : ((ls == "offline" || ls == "refused") ? th.red : 0))});

  const bool two = m.InnerW(true) >= 700;
  const int perCol = two ? 4 : 7;
  Card k(c, m, T("System"), "", IconId::Info, perCol * kRowInfo + 8 + kRowButtons, true);
  const float gap = 34;
  const float cw = two ? (k.w - gap) / 2 : k.w;
  const float y0 = k.y;
  for (size_t i = 0; i < f.size(); i++) {
    const int col = two ? static_cast<int>(i) / perCol : 0, row = two ? static_cast<int>(i) % perCol : static_cast<int>(i);
    InfoAt(ui, Rect(k.x0 + col * (cw + gap), y0 + row * kRowInfo, cw, kRowInfo), f[i].label, f[i].value, f[i].col, f[i].tip);
  }
  k.Gap(perCol * kRowInfo + 8);
  if (k.Button("di#copy", T("Copy device summary"), Btn::Normal, IconId::Copy)) a.CopyText(DeviceSummary(a.snap()));
  ui.Tip(Rect(k.x0, k.y - kRowButtons, 220, kRowButtons), T("Copies the model, firmware, processor, graphics, memory and drive models as text for a bug report. It contains no serial numbers and no computer or user name."));
}

}  // namespace

float PageDeviceInfo(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 380, 3);
  CardProcessor(c, m);
  CardGraphics(c, m);
  CardMemory(c, m);
  CardFans(c, m);
  CardStorage(c, m);
  CardSystem(c, m);
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
