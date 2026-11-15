// Service: the link to the vendor's Control Center service, the switches that decide what Control Center Advanced may send, the confirmations the
// user has given, and diagnostics for a bug report.
//
// Nothing on this page touches the laptop directly. The write-protection switches and the confirmations are stored settings of the controller
// (cfg.*); "Reconnect" and "Start the vendor's services" are commands of the controller, which owns the link and the (read-only) service probe.
#include <algorithm>

#include "../common/util.h"
#include "page.h"

namespace bc {
namespace ui {
namespace {

std::string Dash() { return "\xE2\x80\x94"; }

float WrapH(Ctx& c, float w, const std::string& text) { return c.ui.gfx.Wrapped(Rect(0, 0, w, 3000), text, FontId::Small, 0, false); }

void CfgToast(App& a, const std::string& key, const std::string& value, const std::string& text) {
  App* ap = &a;
  a.Cfg(key, value, [ap, text](const Result& r) {
    if (r.ok && !text.empty()) ap->ui().Toast(text, ToastKind::Ok);
  });
}

// ---- the link to the vendor's service ------------------------------------------------------------------------------------------
struct LinkLook {
  std::string state;     // normalised: connected | connecting | refused | offline | none
  std::string text;      // in words
  uint32_t col = 0;
};

LinkLook LinkNow(App& a, const Theme& th) {
  LinkLook l;
  l.state = a.S("oem.link.state", "none");
  if (l.state == "connected") {
    l.text = T("Connected");
    l.col = th.green;
  } else if (l.state == "connecting") {
    l.text = T("Connecting...");
    l.col = th.amber;
  } else if (l.state == "refused") {
    l.text = T("Refused by the service");
    l.col = th.red;
  } else if (l.state == "offline") {
    l.text = T("Not reachable");
    l.col = th.red;
  } else {
    l.state = "none";
    l.text = Dash();
  }
  return l;
}

std::string LinkNote(App& a, const std::string& state) {
  const bool installed = a.B("oem.installed");
  const bool running = a.B("oem.running");
  if (state == "connected")
    return T("Connected to the vendor's Control Center service on this computer. Power limits, fan modes and the keyboard light are sent through it, the way the "
             "vendor's own window does it. The live values on the other pages are read straight from the embedded controller.");
  if (state == "connecting") return T("Trying to reach the vendor's Control Center service. This normally takes a second or two.");
  if (state == "refused")
    return T("The vendor's service answered but did not accept this program. Press Reconnect to try the other client names the service knows. If it keeps "
             "refusing, copy the diagnostics below and attach them to a bug report.");
  if (state == "none") return T("This build has no link to the vendor's service.");
  if (!installed)
    return T("The vendor's Control Center is not installed on this computer. Everything that the vendor's service applies (power limits, fan modes, keyboard "
             "light) needs it. Install the Control Center that came with the laptop; Control Center Advanced does not ship or copy its files.");
  if (!running)
    return T("The vendor's Control Center service is installed but not running. Start it below; Control Center Advanced connects as soon as it is up.");
  return T("The vendor's service is running but does not accept the connection yet. Press Reconnect. If it keeps failing, copy the diagnostics below.");
}

void CardLink(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  const Theme& th = ui.th;
  const LinkLook l = LinkNow(a, th);
  const std::string detail = a.S("oem.link.detail");
  const std::string note = LinkNote(a, l.state);
  const bool showDetail = !detail.empty() && l.state != "connected";
  const float contentH = (showDetail ? 7 : 6) * kRowInfo + 8 + NoteHeight(c, m, note) + kRowButtons;
  Card k(c, m, T("Link to the vendor's service"), l.text, IconId::Service, contentH);
  k.Info(T("State"), l.text, l.col);
  k.Info(T("Address"), a.Has("oem.link.endpoint") ? a.S("oem.link.endpoint") : Dash(), 0,
         T("The vendor's service listens on this computer only. Control Center Advanced never connects to another machine."));
  k.Info(T("Client name"), a.S("oem.link.class").empty() ? Dash() : a.S("oem.link.class"), 0,
         T("The name this program is registered under at the vendor's service. The service tells its own windows apart by it."));
  k.Info(T("Client slot"), a.Has("oem.link.slot") ? Tf("%d", static_cast<int>(a.I("oem.link.slot"))) : Dash());
  k.Info(T("Connections made"), a.Has("oem.link.sessions") ? Tf("%d", static_cast<int>(a.I("oem.link.sessions"))) : Dash(), 0,
         T("How many times the link has been opened since this program started. A high number means the service keeps dropping it."));
  k.Info(T("Messages received / sent"),
         a.Has("oem.link.rx") ? Tf("%d / %d", static_cast<int>(a.I("oem.link.rx")), static_cast<int>(a.I("oem.link.tx"))) : Dash());
  if (showDetail) k.Info(T("Detail"), ui.gfx.Fit(detail, FontId::Body, k.w * 0.6f), 0, detail);
  k.Gap(8);
  k.Note(note);
  const bool ok = l.state != "none";
  if (k.Button("lk#reconnect", T("Reconnect"), Btn::Primary, IconId::Refresh, ok, 0)) {
    a.Cmd("link.reconnect", KV(), T("Reconnecting to the vendor's service"));
  }
}

// ---- the vendor's Control Center ------------------------------------------------------------------------------------------------
void CardVendor(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  const Theme& th = ui.th;
  const bool installed = a.B("oem.installed");
  const bool running = a.B("oem.running");
  const std::string version = a.S("oem.version");
  const std::string services = a.S("oem.services");

  std::string note;
  if (!installed)
    note = T("Nothing of the vendor's Control Center was found. Without it only what the embedded controller shows can be read and the GPU power (cTGP) can be set.");
  else if (running)
    note = T("The vendor's services are running. Control Center Advanced does not stop, disable or replace them: it works with them.");
  else
    note = T("The vendor's services are installed but stopped. Starting them is the same as opening the vendor's Control Center; it does not change any setting.");

  std::vector<BtnSpec> btns;
  if (installed && !running)
    btns.push_back({"vn#start", T("Start the vendor's services"), Btn::Primary, IconId::Power, !a.B("cfg.readonly"),
                    a.B("cfg.readonly") ? T("Read-only mode is on.") : T("Starts the vendor's Windows services (GCUBridge and GCUService). Needs administrator rights, which this program has.")});

  const float contentH = (services.empty() ? 3 : 4) * kRowInfo + 8 + NoteHeight(c, m, note) + static_cast<float>(BtnRows(c, m.InnerW(), btns)) * kRowButtons;
  Card k(c, m, T("Vendor's Control Center"), !installed ? T("Not installed") : (version.empty() ? T("Installed") : Tf("Version %s", version.c_str())), IconId::Chip,
         contentH);
  k.Info(T("Installed"), installed ? T("Yes") : T("No"));
  k.Info(T("Services"), !installed ? Dash() : running ? T("Running") : T("Stopped"), running ? th.green : (installed ? th.amber : 0),
         T("The vendor's background services (GCUBridge and GCUService). They do the privileged work that Control Center Advanced asks for."));
  k.Info(T("Version"), version.empty() ? Dash() : version);
  if (!services.empty()) k.Info(T("Service names"), ui.gfx.Fit(services, FontId::Body, k.w * 0.6f), 0, services);
  k.Gap(8);
  k.Note(note);
  if (!btns.empty()) {
    const int hit = BtnFlow(k, btns);
    if (hit == 0) a.Cmd("oem.start", KV(), T("The vendor's services were started"));
  }
}

// ---- write protection ----------------------------------------------------------------------------------------------------------
void CardProtection(Ctx& c, Masonry& m) {
  App& a = c.app;
  const bool readonly = a.B("cfg.readonly");
  const bool keep = a.B("cfg.keep", true);
  const bool onStart = a.B("cfg.applyOnStart", true);

  Card k(c, m, T("Write protection"), T("What Control Center Advanced is allowed to send"), IconId::Shield, 3 * kRowToggle);

  if (k.Toggle("sp#readonly", T("Read-only mode"), T("Blocks every change. Readings keep working."), readonly, true,
               T("The kill switch. While it is on, nothing is sent to the vendor's service and nothing is written to the embedded controller."))) {
    CfgToast(a, "cfg.readonly", readonly ? "0" : "1", readonly ? T("Changes are allowed again") : T("Read-only mode is on"));
  }

  if (k.Toggle("sp#keep", T("Keep my settings applied"), T("Applies them again after sleep, a charger change or display wake."), keep, true,
               T("The firmware resets some values at those moments. Control Center Advanced puts back the ones you set."))) {
    CfgToast(a, "cfg.keep", keep ? "0" : "1", "");
  }

  if (k.Toggle("sp#onstart", T("Apply my settings when the program starts"), keep ? T("After a restart of Windows or of the program.") : T("Needs \"Keep my settings applied\"."),
               onStart, keep)) {
    CfgToast(a, "cfg.applyOnStart", onStart ? "0" : "1", "");
  }
}

// ---- confirmations -------------------------------------------------------------------------------------------------------------
void CardConsents(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  const std::vector<std::pair<std::string, std::string>> rows = {{"ctgp_over_oem", T("GPU power above the vendor's maximum")},
                                                                {"ctgp_unverified", T("Unverified GPU power steps")},
                                                                {"ctgp_model", T("GPU power on an unlisted model")}};
  int given = 0;
  for (auto& r : rows)
    if (a.B("cfg.consent." + r.first)) given++;
  const std::string note = T("When you confirm a risky change, Control Center Advanced remembers it so it does not ask again for the same kind of change. Reset to be asked again.");
  Card k(c, m, T("Confirmations"), given == 0 ? T("None given") : Tf("%d given", given), IconId::Check,
         static_cast<float>(rows.size()) * kRowInfo + 8 + NoteHeight(c, m, note) + kRowButtons);
  for (auto& r : rows) {
    const bool on = a.B("cfg.consent." + r.first);
    k.Info(r.second, on ? T("Allowed") : T("Asked each time"), on ? th.amber : 0);
  }
  k.Gap(8);
  k.Note(note);
  if (k.Button("cs#reset", T("Reset confirmations"), Btn::Normal, IconId::Undo, given > 0)) {
    a.Cmd("consent.reset", KV(), T("Confirmations reset"));
  }
}

// ---- diagnostics ---------------------------------------------------------------------------------------------------------------
constexpr int kLogLines = 6;
constexpr float kLogLineH = 17;

void CardDiagnostics(Ctx& c, Masonry& m) {
  App& a = c.app;
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::string err = a.S("last.error");
  const bool capture = a.B("oem.link.capture");
  const float logH = kLogLines * kLogLineH + 16;
  const float contentH = 3 * kRowInfo + 8 + kRowToggle + kRowButtons + 30 + logH;
  Card k(c, m, T("Diagnostics"), T("For a bug report"), IconId::Log, contentH);
  k.Info(T("Writes to the embedded controller"), a.Has("ec.writes") ? Tf("%d", static_cast<int>(a.I("ec.writes"))) : Dash(), 0,
         T("Registers Control Center Advanced has written since it started. Only the GPU power (cTGP) registers are ever written."));
  k.Info(T("Writes refused by the guard"), a.Has("ec.refused") ? Tf("%d", static_cast<int>(a.I("ec.refused"))) : Dash(), a.I("ec.refused") > 0 ? th.amber : 0,
         T("A write outside the allow-list or its value limits is refused before it reaches the hardware."));
  k.Info(T("Last error"), err.empty() ? T("None") : g.Fit(err, FontId::Body, k.w * 0.55f), err.empty() ? 0 : th.amber, err);
  k.Gap(8);
  if (k.Toggle("dg#capture", T("Record all link messages"), T("Also keeps what the other windows send to the service. Off by default."), capture, a.B("oem.up"),
               T("Adds the messages of the vendor's own window to the log below, so a bug report can show what the vendor's program does. Nothing is sent anywhere."))) {
    KV args;
    args.SetB("on", !capture);
    a.Cmd("link.capture", args);
  }
  const int hit = BtnFlow(k, {{"dg#probe", T("Detect hardware again"), Btn::Normal, IconId::Refresh, true, T("Runs the start-up hardware check once more.")},
                              {"dg#copy", T("Copy diagnostics"), Btn::Normal, IconId::Copy, true,
                               T("Copies the model, firmware, detected features, the state of the link, the last error and the recent log as text. It contains no serial numbers and no user name.")}});
  k.SubHead(T("Recent log"));
  const Rect box = k.Row(logH);
  g.FillRound(box, 10, th.inset);
  g.StrokeRound(box, 10, th.line, 1.0f);
  std::vector<std::string> lines = Split(a.S("log.tail"), '\n');
  if (lines.size() > static_cast<size_t>(kLogLines)) lines.erase(lines.begin(), lines.end() - kLogLines);
  if (lines.empty()) {
    g.Text(Rect(box.x + 12, box.y + 8, box.w - 24, kLogLineH), T("Nothing logged yet."), FontId::Small, th.faint);
  } else {
    for (size_t i = 0; i < lines.size(); i++) {
      const std::string& ln = lines[i];
      const bool bad = ln.find("ERROR") != std::string::npos || ln.find("failed") != std::string::npos;
      g.Text(Rect(box.x + 12, box.y + 8 + static_cast<float>(i) * kLogLineH, box.w - 24, kLogLineH), g.Fit(ln, FontId::Mono, box.w - 24), FontId::Mono,
             bad ? th.amber : th.muted);
    }
  }
  if (hit == 0) {
    a.Cmd("probe", KV(), T("Hardware detected again"));
  } else if (hit == 1) {
    a.CopyText(a.ProbeReport());
  }
}

// ---- labels and hard limits ----------------------------------------------------------------------------------------------------
void CardLabels(Ctx& c, Masonry& m) {
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const Ev evs[] = {Ev::Verified, Ev::Service, Ev::Os};
  float chipW = 0;
  for (Ev e : evs) chipW = std::max(chipW, ui.ChipW(EvLabel(e)));
  const float textW = m.InnerW() - chipW - 14;
  float contentH = 0;
  for (Ev e : evs) contentH += std::max(30.0f, WrapH(c, textW, EvTip(e)) + 12);
  Card k(c, m, T("Evidence labels"), T("What the badges next to settings mean"), IconId::Info, contentH);
  for (Ev e : evs) {
    const std::string tip = EvTip(e);
    const float h = std::max(30.0f, WrapH(c, textW, tip) + 12);
    const Rect r = k.Row(h);
    ui.Chip(Rect(r.x, r.y + 4, chipW, 18), EvLabel(e), EvColor(th, e));
    g.Wrapped(Rect(r.x + chipW + 14, r.y + 3, textW, h - 3), tip, FontId::Small, th.muted);
  }
}

void CardLimits(Ctx& c, Masonry& m) {
  Ui& ui = c.ui;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::vector<std::string> items = {
      T("Control Center Advanced never writes to the BIOS, the flash chip or UEFI variables."),
      T("It installs no driver and no service of its own, and it does not use the processor's model-specific registers or talk to the keyboard controller. "
        "The vendor's service does the privileged work."),
      T("Power limits, fan modes and the keyboard light are sent to the vendor's service, the way the vendor's own window sends them, and only for the values on an "
        "allow-list. The ranges come from the service."),
      T("The vendor's ready-made modes (Office, Gaming and Turbo) keep their own values: only the Custom mode is ever edited."),
      T("The only thing written to the embedded controller directly is the GPU power (cTGP) pair of registers, through an allow-list with limits on every "
        "value. Registers that identify the charger or bypass the firmware's power limits are on a never-write list."),
      T("The vendor's own profile file is written only if you switch on \"Also write the OEM profile file\" (Custom mode). A backup of the original is kept next to "
        "it, and you are asked first when something else changed it."),
      T("Every change is checked afterwards. A setting that did not arrive is reported, and a GPU power change that the hardware did not keep is undone."),
  };
  const float textW = m.InnerW() - 28;
  float contentH = 0;
  for (const std::string& s : items) contentH += WrapH(c, textW, s) + 14;
  Card k(c, m, T("What Control Center Advanced never does"), T("Hard limits that no switch changes"), IconId::Lock, contentH);
  for (const std::string& s : items) {
    const float h = WrapH(c, textW, s) + 14;
    const Rect r = k.Row(h);
    g.Icon(IconId::Check, Rect(r.x, r.y + 2, 16, 16), th.green, 2.0f);
    g.Wrapped(Rect(r.x + 28, r.y, textW, h), s, FontId::Small, th.muted);
  }
}

}  // namespace

float PageService(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 430, 3);
  CardLink(c, m);
  CardVendor(c, m);
  CardProtection(c, m);
  CardConsents(c, m);
  CardDiagnostics(c, m);
  CardLabels(c, m);
  CardLimits(c, m);
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
