#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/autostart_task.h"
#include "../common/fsutil.h"
#include "../hw/autostart_win.h"
#include "../common/hostdata.h"
#include "../common/util.h"
#include "../common/version.h"
#include "app_internal.h"
#include "page.h"

namespace bc {
namespace ui {

using hw::LocalDataDir;

// ------------------------------------------------------------------------------------------------ helpers
double NowSeconds() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

namespace {
struct PageName {
  Page p;
  const char* key;
};
const PageName kPageNames[] = {
    {Page::Overview, "overview"}, {Page::Prefs, "prefs"},       {Page::Devices, "devices"},   {Page::GpuInfo, "gpu"},
    {Page::Osd, "osd"},           {Page::Keys, "keys"},         {Page::Profiles, "profiles"}, {Page::Fan, "fan"},
    {Page::Custom, "custom"},     {Page::Apps, "apps"},         {Page::Lighting, "lighting"}, {Page::Battery, "battery"},
    {Page::Display, "display"},   {Page::DeviceInfo, "deviceinfo"}, {Page::Service, "service"}, {Page::About, "about"},
};
}  // namespace

Page PageFromName(const std::string& name, bool* ok) {
  for (const PageName& n : kPageNames)
    if (name == n.key) {
      if (ok) *ok = true;
      return n.p;
    }
  if (ok) *ok = false;
  return Page::Overview;
}

const char* PageKey(Page p) {
  for (const PageName& n : kPageNames)
    if (n.p == p) return n.key;
  return "overview";
}

App::Impl::~Impl() {
  FreeDib();
  RemoveTray();
  // the controller first (it stops its threads and the link), then the hardware it uses; GDI+ goes last.
  ctl.reset();
  world.reset();
  simWorld.reset();
  if (gdipStarted) Gdiplus::GdiplusShutdown(gdipToken);
}

// ------------------------------------------------------------------------------------------------ construction
static std::string PrefsPath() { return fs::Join(LocalDataDir(), "ui.kv"); }

App::App(const Options& o) : d_(new Impl), opt_(o) {
  d_->app = this;
  d_->t0 = NowSeconds();
  shot_ = !o.shotOut.empty() || !o.script.empty();   // offscreen: no preference file, no keyboard hook
  Gdiplus::GdiplusStartupInput in;
  if (Gdiplus::GdiplusStartup(&d_->gdipToken, &in, nullptr) == Gdiplus::Ok) d_->gdipStarted = true;
  ui_.gfx.ChooseFonts(Wide(o.shotFont));
  if (!shot_) {
    std::string txt;
    if (fs::ReadAll(PrefsPath(), &txt)) prefs_ = KV::Parse(txt);
  }
  ApplyLanguage();
  ApplyAccent();
}

App::~App() {
  FlushPrefs();
  backend_.reset();
  // d_ (declared first in the class, so destroyed last) tears down the controller, the hardware and GDI+.
}

void App::ApplyLanguage() {
  std::string name = opt_.lang.empty() ? Pref("lang", "auto") : opt_.lang;
  SetLang(LangFromName(name, Lang::En));
  dirty_ = true;
}

const AccentDef* Accents(int* count) {
  static const AccentDef kAcc[] = {
      {"ember", 0xFFFF7A2F},    // orange: the default, close to the vendor's own colour
      {"cyan", 0xFF22D3EE},
      {"lime", 0xFF7BE35B},
      {"violet", 0xFFA78BFA},
      {"rose", 0xFFFF4D8D},
      {"gold", 0xFFFFC53D},
  };
  if (count) *count = static_cast<int>(sizeof kAcc / sizeof kAcc[0]);
  return kAcc;
}

std::string NormalAccent(const std::string& stored) {
  std::string a = stored;
  // names the first build stored
  if (a == "copper") a = "ember";
  else if (a == "sky") a = "cyan";
  else if (a == "sage") a = "lime";
  else if (a == "sand") a = "gold";
  int n = 0;
  const AccentDef* acc = Accents(&n);
  for (int i = 0; i < n; i++)
    if (a == acc[i].key) return a;
  return kDefaultAccent;
}

void App::ApplyAccent() {
  int n = 0;
  const AccentDef* acc = Accents(&n);
  const std::string a = NormalAccent(Pref("accent", kDefaultAccent));
  ui_.th.accent = acc[0].col;
  for (int i = 0; i < n; i++)
    if (a == acc[i].key) ui_.th.accent = acc[i].col;
  dirty_ = true;
}

// ------------------------------------------------------------------------------------------------ backend
void App::MakeBackend() {
  Impl& d = *d_;
  if (opt_.mode == "sim") {
    const sim::Model sm = opt_.simModel == "unknown" ? sim::Model::Unknown : opt_.simModel == "family" ? sim::Model::Family : sim::Model::Exact;
    d.simWorld.reset(new sim::World(sm, true));
    Controller::Options co;
    co.mode = "sim";
    co.version = opt_.simVersion.empty() ? std::string(BC_VERSION) : opt_.simVersion;
    d.ctl.reset(new Controller(d.simWorld->hw(), co));
    d.ctl->Start();
    backend_.reset(new LocalBackend(*d.ctl, "sim"));
  } else {
    // The one process does everything: it reads the hardware itself and talks to the vendor's service over the loopback link.
    d.world.reset(new hw::WinWorld(hw::ProgramDataDir(), "local"));
    Controller::Options co;
    co.mode = "local";
    co.dataDir = hw::ProgramDataDir();
    co.version = BC_VERSION;
    d.ctl.reset(new Controller(d.world->hw(), co));
    d.ctl->Start();
    backend_.reset(new LocalBackend(*d.ctl, "local"));
  }
  mode_ = backend_->Mode();
  backend_->SetInterest(true);
}

void App::Tick() {
  if (!backend_) return;
  const double now = NowSeconds();
  // ---- results of requests
  for (size_t i = 0; i < ops_.size();) {
    Result r;
    if (backend_->TakeResult(ops_[i].id, &r)) {
      const Op op = ops_[i];
      ops_.erase(ops_.begin() + static_cast<long>(i));
      OnResult(op, r);
    } else {
      i++;
    }
  }
  // ---- state
  KV s = backend_->Snapshot();
  if (s.All() != snap_.All()) {
    snap_ = std::move(s);
    dirty_ = true;
  }
  const std::string m = backend_->Mode();
  if (m != mode_) {
    mode_ = m;
    dirty_ = true;
  }
  const double every = shot_ ? 0.1 : 1.0;
  if (backend_->Connected() && now - lastHistT_ >= every) {
    lastHistT_ = now;
    PushHistory();
  }
  AutoRefresh();
  if (prefsDirty_ && now - prefsT_ > 0.4) FlushPrefs();
  lastTickT_ = now;
}

void App::PushHistory() {
  auto push = [](std::deque<float>& d, float v) {
    d.push_back(v);
    while (d.size() > 120) d.pop_front();
  };
  push(hist_.cpuW, static_cast<float>(N("cpu.pkg.w")));
  push(hist_.gpuW, static_cast<float>(N("gpu.power")));
  push(hist_.cpuT, static_cast<float>(N("cpu.temp")));
  push(hist_.gpuT, static_cast<float>(N("gpu.temp")));
  push(hist_.cpuRpm, static_cast<float>(N("fan.cpu.rpm")));
  push(hist_.gpuRpm, static_cast<float>(N("fan.gpu.rpm")));
  push(hist_.cpuLoad, static_cast<float>(N("os.cpu.pct")));
  push(hist_.gpuLoad, static_cast<float>(N("gpu.util")));
  dirty_ = true;
}

std::string App::Fmt1(const std::string& k, int dec, const char* unit) const {
  if (!snap_.Has(k) || snap_.Str(k).empty()) return "\xE2\x80\x94";
  return With(snap_.Num(k), dec, unit);
}

bool App::Pending(const std::string& key) const {
  auto it = pendingKeys_.find(key);
  return it != pendingKeys_.end() && it->second > 0;
}

double App::RecentOk(const std::string& key) const {
  auto it = okAt_.find(key);
  return it == okAt_.end() ? 1e9 : std::max(0.0, ui_.now - it->second);
}

int& App::Int(const std::string& k, int def) {
  auto it = intState_.find(k);
  if (it == intState_.end()) it = intState_.emplace(k, def).first;
  return it->second;
}

bool& App::Flag(const std::string& k, bool def) {
  auto it = flagState_.find(k);
  if (it == flagState_.end()) it = flagState_.emplace(k, def).first;
  return it->second;
}

void App::Go(Page p) {
  if (p == page_) return;
  CancelKeyCapture();   // the Keys page is the only one that waits for a key press
  page_ = p;
  ui_.ClearFocus();
  dirty_ = true;
}

// ------------------------------------------------------------------------------------------------ requests
uint64_t App::Submit(const Request& r, const std::string& label, std::function<void(const Result&)> done, bool quiet) {
  if (!backend_ || !backend_->Connected()) {
    if (!quiet) ui_.Toast(T("The hardware layer is not running."), ToastKind::Warn, T("Restart Control Center Advanced."));
    Result nr;
    nr.code = "unsupported";
    nr.msg = "not connected";
    if (done) done(nr);
    return 0;
  }
  const uint64_t id = backend_->Submit(r);
  if (id == 0) {
    Result nr;
    nr.code = "busy";
    nr.msg = "request rejected by the backend";
    if (!quiet) ui_.Toast(T("The request was rejected."), ToastKind::Error);
    if (done) done(nr);
    return 0;
  }
  Op op;
  op.id = id;
  op.req = r;
  op.label = label;
  op.done = std::move(done);
  op.quiet = quiet;
  ops_.push_back(op);
  pendingKeys_[r.op == "set" ? r.key : r.op + ":" + r.key]++;
  dirty_ = true;
  return id;
}

uint64_t App::Set(const std::string& key, const std::string& value, const std::string& label, const KV& args, std::function<void(const Result&)> done) {
  Request r;
  r.op = "set";
  r.key = key;
  r.value = value;
  r.args = args;
  return Submit(r, label.empty() ? key : label, std::move(done));
}

uint64_t App::Cmd(const std::string& name, const KV& args, const std::string& label, std::function<void(const Result&)> done) {
  Request r;
  r.op = "cmd";
  r.key = name;
  r.args = args;
  return Submit(r, label, std::move(done));
}

uint64_t App::CmdQuiet(const std::string& name, const KV& args) {
  Request r;
  r.op = "cmd";
  r.key = name;
  r.args = args;
  return Submit(r, "", nullptr, true);
}

uint64_t App::Cfg(const std::string& key, const std::string& value, std::function<void(const Result&)> done) {
  Request r;
  r.op = "cfg";
  r.key = key;
  r.value = value;
  return Submit(r, "", std::move(done));
}

struct App::ChainSt {
  std::vector<Request> reqs;
  std::vector<std::string> labels;
  size_t i = 0;
};

void App::Chain(std::vector<Request> reqs, std::vector<std::string> labels) {
  auto st = std::make_shared<ChainSt>();
  st->reqs = std::move(reqs);
  st->labels = std::move(labels);
  ChainNext(st);
}

void App::ChainNext(const std::shared_ptr<ChainSt>& st) {
  if (st->i >= st->reqs.size()) return;
  const size_t k = st->i++;
  Submit(st->reqs[k], k < st->labels.size() ? st->labels[k] : std::string(), [this, st](const Result& r) {
    if (r.ok) ChainNext(st);
  });
}

std::string App::CapStateText(const std::string& cap) const {
  static const struct {
    const char* state;
    const char* words;
  } kStates[] = {
      {"ok", N_("available")},           {"absent", N_("not present")},      {"denied", N_("access denied")},
      {"error", N_("failed to start")},  {"locked", N_("locked")},           {"implausible", N_("implausible readings")},
      {"unsupported", N_("not supported")}, {"off", N_("switched off")},     {"blocked", N_("blocked by a safety check")},
      {"offline", N_("service not reachable")}, {"pending", N_("waiting for the service")},
  };
  const std::string s = CapState(cap);
  for (const auto& e : kStates)
    if (s == e.state) return T(e.words);
  return T("unknown");
}

std::string App::CodeText(const std::string& code) const {
  if (code == "range") return T("The value is outside the allowed range.");
  if (code == "gate.readonly") return T("Read-only mode is on (Service page).");
  if (code == "gate.mode") return T("The vendor's ready-made modes keep their own values. Switch to the Custom mode first.");
  if (code == "gate.thermal") return T("The CPU is too hot to raise power limits right now.");
  if (code == "gate.oem") return T("The vendor's profile file is locked by another program right now.");
  if (code == "needs.service") return T("The vendor's service is not reachable. Open the Service page.");
  if (code == "failed.service") return T("The vendor's service did not confirm the change.");
  if (code == "busy") return T("Another program is changing this right now. Try again in a moment.");
  if (code == "failed.hw") return T("The hardware refused the change.");
  if (code == "failed.verify") return T("The change could not be verified by reading it back.");
  if (code == "reverted") return T("The value did not stick, so the previous value was restored.");
  if (code == "partial") return T("Some of the settings were applied, others were not.");
  if (code == "unsupported") return T("Not available on this system.");
  if (code == "unknown") return T("Unknown setting or command.");
  return T("Something went wrong inside Control Center Advanced.");
}

void App::OnResult(const Op& op, const Result& r) {
  const std::string pkey = op.req.op == "set" ? op.req.key : op.req.op + ":" + op.req.key;
  auto it = pendingKeys_.find(pkey);
  if (it != pendingKeys_.end() && --it->second <= 0) pendingKeys_.erase(it);
  dirty_ = true;
  if (backend_) {
    snap_ = backend_->Snapshot();   // the controller refreshed it before answering: show the new live value right away
  }

  if (r.ok) {
    if (op.req.op == "set") {
      drafts_.erase(op.req.key);
      okAt_[op.req.key] = ui_.now;
    }
    if (!op.label.empty()) ui_.Toast(op.label, ToastKind::Ok, op.req.op == "set" ? T("Applied and verified.") : std::string());
    if (op.done) op.done(r);
    return;
  }

  if (op.quiet) {   // a background report: its failure is not the user's business
    if (op.done) op.done(r);
    return;
  }
  // ---- recoverable "ask first" outcomes: the operation continues (or not) after the dialog
  if (r.code == "gate.consent") return AskConsent(op, r.data.Str("consent"), r);
  if (r.code == "needs.approval") return AskApproval(op, r);

  const std::string what = op.label.empty() ? op.req.key : op.label;
  ui_.Toast(Tf("Could not apply: %s", what.c_str()), ToastKind::Error, CodeText(r.code) + (r.msg.empty() ? std::string() : "\n" + TMsg(r.msg)));
  if (op.done) op.done(r);
}

namespace {
struct ConsentInfo {
  std::string title, body;
};
ConsentInfo ConsentText(const std::string& name, const std::string& value) {
  ConsentInfo c;
  if (name == "ctgp_over_oem") {
    c.title = T("Above the OEM maximum GPU power");
    c.body = Tf("You are about to set the GPU's dynamic power target (cTGP) to %s W, above the highest value the OEM Control Center ever applies (140 W).\n\n"
                "The 230 W adapter and the cooling were sized for the OEM range. Long CPU + GPU loads at this level can overheat the laptop or push the adapter to its limit.\n\n"
                "Control Center Advanced writes the value, reads it back, and restores the previous one if the firmware does not keep it. It cannot protect the hardware from heat.",
                value.c_str());
  } else if (name == "ctgp_unverified") {
    c.title = T("This GPU power step was not verified");
    c.body = Tf("%s W is one of the cTGP steps that has not been verified on the reference laptop (125, 140, 145 and 150 W were measured).\n\n"
                "Control Center Advanced writes it, reads it back, and restores the previous value if the driver does not show it.",
                value.c_str());
  } else if (name == "ctgp_model") {
    c.title = T("GPU power on a model that is not on the verified list");
    c.body = T("The GPU power engine was only verified on the Monster Tulpar T5 V21.7 (GM5TG7Y). On any other model the same embedded-controller registers are assumed to behave the same way, but that is not proven.\n\n"
               "Control Center Advanced reads every value back and restores the previous one if it does not stick.");
  } else {
    c.title = T("Confirmation required");
    c.body = T("This change needs your confirmation.");
  }
  return c;
}
}  // namespace

void App::AskConsent(const Op& op, const std::string& name, const Result& r) {
  std::string shown = op.req.value;
  if (shown.empty() && op.req.op == "set") shown = op.req.value;
  const ConsentInfo ci = ConsentText(name, shown);
  ModalSpec m;
  m.title = ci.title;
  m.body = ci.body;
  m.icon = IconId::Warn;
  m.iconColor = ui_.th.amber;
  m.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("I understand, apply"), Btn::Primary, 1}};
  const Op copy = op;
  ui_.ModalOpen(m, [this, copy, name, r](int id) {
    if (id == 1) {
      Request rq = copy.req;
      rq.args.Set("consent." + name, "1");
      Submit(rq, copy.label, copy.done);
    } else if (copy.done) {
      copy.done(r);
    }
  });
}

void App::AskApproval(const Op& op, const Result& r) {
  ModalSpec m;
  m.title = T("The OEM profile file holds a value Control Center Advanced did not write");
  m.body = Tf("Something else changed the OEM profile file (it currently stores %d W). Applying your value replaces it; Control Center Advanced keeps a backup of the original file next to it.\n\n"
              "Fingerprint of the file: %s",
              static_cast<int>(r.data.Int("oem.target")), r.data.Str("sha").substr(0, 16).c_str());
  m.icon = IconId::Warn;
  m.iconColor = ui_.th.amber;
  m.buttons = {{T("Cancel"), Btn::Ghost, 0}, {T("Replace and apply"), Btn::Primary, 1}};
  const Op copy = op;
  const std::string sha = r.data.Str("sha");
  ui_.ModalOpen(m, [this, copy, sha, r](int id) {
    if (id == 1) {
      Request rq = copy.req;
      rq.args.Set("approve.sha", sha);
      rq.args.Set("approve.foreign", "1");
      Submit(rq, copy.label, copy.done);
    } else if (copy.done) {
      copy.done(r);
    }
  });
}

// ------------------------------------------------------------------------------------------------ OS helpers
void* App::WindowHandle() const { return d_ ? static_cast<void*>(d_->hwnd) : nullptr; }

void App::OpenUrl(const std::string& url) {
  if (url.compare(0, 8, "https://") != 0) return;
  Str("url.last") = url;
  if (shot_) return;
  ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::OpenWindowsTool(const std::string& id) {
  std::wstring target;
  if (id == "display") {
    target = L"ms-settings:display";
  } else if (id == "nightlight") {
    target = L"ms-settings:nightlight";
  } else if (id == "colorcpl") {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);   // the system copy, not whatever the search path finds first
    if (n == 0 || n >= MAX_PATH) return;
    target = std::wstring(sys) + L"\\colorcpl.exe";
  } else {
    return;
  }
  Str("tool.last") = id;
  if (shot_) return;
  ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::CopyText(const std::string& text) {
  if (shot_) {   // offscreen renders and scripts never touch the real clipboard; the text is kept for the script to check
    Str("clip.last") = text;
    ui_.Toast(T("Copied to the clipboard"), ToastKind::Ok);
    return;
  }
  const std::wstring w = Wide(text);
  if (!OpenClipboard(d_->hwnd)) return;
  EmptyClipboard();
  const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (h) {
    void* p = GlobalLock(h);
    if (p) {
      std::memcpy(p, w.c_str(), bytes);
      GlobalUnlock(h);
      if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
  ui_.Toast(T("Copied to the clipboard"), ToastKind::Ok);
}

bool App::Autostart() const {
  if (mode_ == "sim") return PrefB("sim.autostart", false);   // the simulator never touches the scheduler
  return AutostartIsActive(hw::QueryAutostart(), Narrow(hw::SelfPath()));
}

bool App::SetAutostart(bool on) {
  if (mode_ == "sim") {
    SetPref("sim.autostart", on ? "1" : "0");
    return true;
  }
  std::string why;
  if (on && !hw::AutostartAllowedHere(&why)) {
    ui_.Toast(T("\"Start with Windows\" needs the installed copy"), ToastKind::Error,
              T("The task starts this program with administrator rights and without a prompt, so it is only created for a copy in Program Files, where only administrators can change files. Install Control Center Advanced with the setup program (or copy it to Program Files) and turn this on there."));
    return false;
  }
  if (hw::SetAutostart(on, &why)) return true;
  ui_.Toast(T("Could not change \"Start with Windows\""), ToastKind::Error, why);
  return false;
}

std::string App::ProbeReport() const {
  // The same plain-text description the Device info page copies (model, firmware, processor, graphics, memory, drives: no serial numbers,
  // no computer or user name), then what the controller detected and what it logged.
  std::string o = "Control Center Advanced diagnostics\n\n";
  o += DeviceSummary(snap_);
  o += Fmt("Window mode: %s | EC project: %s | elevated: %s\n", mode_.c_str(), S("dev.project").c_str(), S("host.elevated").c_str());
  o += "\nCapabilities:\n";
  for (const auto& p : snap_.All()) {
    if (p.first.compare(0, 4, "cap.") != 0) continue;
    const std::string name = p.first.substr(4);
    const std::string why = S("capwhy." + name);
    o += Fmt("  %-14s %s%s%s\n", name.c_str(), p.second.c_str(), why.empty() ? "" : "  - ", why.c_str());
  }
  const std::string err = S("last.error");
  if (!err.empty()) o += "\nLast error: " + err + "\n";
  const std::string tail = S("log.tail");
  if (!tail.empty()) o += "\nRecent log:\n" + tail + "\n";
  return o;
}

void App::Quit() {
  d_->quitting = true;
  if (d_->hwnd) PostMessageW(d_->hwnd, WM_CLOSE, 0, 0);
}

}  // namespace ui
}  // namespace bc
