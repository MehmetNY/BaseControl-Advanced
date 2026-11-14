// Safety and service: the switches that decide what BaseControl may change, the OEM Control Center's service, BaseControl's own
// background service, the confirmations the user has given, and diagnostics for a bug report.
//
// Nothing on this page touches the laptop directly. The write-protection switches and the confirmations are stored settings of the
// controller (cfg.*); the service buttons start a short-lived elevated helper (the same executable with --install-service and so on),
// which is the only way Windows lets a normal window change a service.
#include <algorithm>

#include "../common/util.h"
#include "../svc/service.h"
#include "page.h"

namespace bc {
namespace ui {
namespace {

std::string Dash() { return "\xE2\x80\x94"; }

enum class Svc { Missing, Stopped, Starting, Running, Stopping, Unknown };

// The state of the BaseControl Windows service. Asking Windows costs a system call, so the answer is reused for a moment. Offscreen
// renders and scripts never ask: they report what the script said (running by default).
Svc ServiceState(App& a) {
  if (a.shotMode()) {
    const std::string f = a.Str("svc.state.fake");
    if (f == "missing") return Svc::Missing;
    if (f == "stopped") return Svc::Stopped;
    if (f == "starting") return Svc::Starting;
    if (f == "stopping") return Svc::Stopping;
    return Svc::Running;
  }
  static double lastQ = -10;
  static svc::State st = svc::State::Unknown;
  if (a.ui().now - lastQ > 1.5) {
    lastQ = a.ui().now;
    st = svc::Query();
  }
  switch (st) {
    case svc::State::NotInstalled: return Svc::Missing;
    case svc::State::Stopped: return Svc::Stopped;
    case svc::State::Starting: return Svc::Starting;
    case svc::State::Running: return Svc::Running;
    case svc::State::Stopping: return Svc::Stopping;
    default: return Svc::Unknown;
  }
}

float WrapH(Ctx& c, float w, const std::string& text) { return c.ui.gfx.Wrapped(Rect(0, 0, w, 3000), text, FontId::Small, 0, false); }

// ---- modal helpers -------------------------------------------------------------------------------------------------------------
// Asks first; `go` runs only when the user presses the confirming button.
void Ask(App& a, const std::string& title, const std::string& body, const std::string& confirm, Btn style, std::function<void()> go) {
  ModalSpec ms;
  ms.title = title;
  ms.body = body;
  ms.icon = IconId::Warn;
  ms.iconColor = a.ui().th.amber;
  ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {confirm, style, 1}};
  a.ui().ModalOpen(ms, [go](int id) {
    if (id == 1) go();
  });
}

void CfgToast(App& a, const std::string& key, const std::string& value, const std::string& text) {
  App* ap = &a;
  a.Cfg(key, value, [ap, text](const Result& r) {
    if (r.ok && !text.empty()) ap->ui().Toast(text, ToastKind::Ok);
  });
}

// ---- write protection ----------------------------------------------------------------------------------------------------------
void CardProtection(Ctx& c, Masonry& m) {
  App& a = c.app;
  const bool readonly = a.B("cfg.readonly");
  const bool experimental = a.B("cfg.experimental");
  const bool unknownModelOn = a.B("cfg.unknownModel");
  const bool adminOnly = a.B("cfg.adminOnly");
  const bool keep = a.B("cfg.keep", true);
  const bool onStart = a.B("cfg.applyOnStart", true);
  const std::string cls = a.S("dev.class");

  Card k(c, m, T("Write protection"), T("What BaseControl is allowed to change"), IconId::Shield, 6 * kRowToggle);

  if (k.Toggle("sp#readonly", T("Read-only mode"), T("Blocks every hardware write. Readings keep working."), readonly, true,
               T("The kill switch. While it is on, BaseControl refuses every change to the embedded controller, the CPU registers and the graphics card."))) {
    CfgToast(a, "cfg.readonly", readonly ? "0" : "1", readonly ? T("Writes are allowed again") : T("Read-only mode is on"));
  }

  if (k.Toggle("sp#experimental", T("Allow experimental writes"), T("Settings marked Experimental stay locked without it."), experimental, true,
               T("Experimental settings are plausible but unproven on this laptop, or they bypass a firmware protection."))) {
    if (experimental) {
      CfgToast(a, "cfg.experimental", "0", T("Experimental writes are off"));
    } else {
      App* ap = &a;
      Ask(a, T("Allow experimental writes"),
          T("Settings marked Experimental are plausible but unproven on this laptop, or they bypass a firmware protection. A wrong value can make the laptop unstable until you restart it.\n\n"
            "Every change is still read back and undone when the hardware does not keep it."),
          T("Allow"), Btn::Primary, [ap]() { CfgToast(*ap, "cfg.experimental", "1", T("Experimental writes are allowed")); });
    }
  }

  const bool unknownCls = cls == "unknown";
  if (k.Toggle("sp#unknown", T("Allow writes on an unrecognised model"),
               unknownCls ? T("This laptop is not recognised: hardware writes are off until you allow them.") : T("Only needed when the laptop is not recognised."),
               unknownModelOn, unknownCls || unknownModelOn,
               T("BaseControl only writes to hardware it has identified. On another model the same registers may mean something else."))) {
    if (unknownModelOn) {
      CfgToast(a, "cfg.unknownModel", "0", T("Hardware writes are off for unrecognised models"));
    } else {
      App* ap = &a;
      Ask(a, T("Allow writes on an unrecognised model"),
          T("BaseControl has not identified this laptop. The registers it writes were verified on one specific model; on another model they can have a different meaning.\n\n"
            "Only continue if you know this laptop shares the embedded controller of the supported one."),
          T("Allow"), Btn::Primary, [ap]() { CfgToast(*ap, "cfg.unknownModel", "1", T("Hardware writes are allowed on this model")); });
    }
  }

  if (k.Toggle("sp#adminonly", T("Only administrators can change settings"), T("This window then has to run as administrator."), adminOnly, true,
               T("The service refuses changes from a window that is not elevated. Useful on a computer that several people use."))) {
    if (adminOnly) {
      CfgToast(a, "cfg.adminOnly", "0", T("Everyone who can open the window can change settings again"));
    } else {
      App* ap = &a;
      Ask(a, T("Only administrators can change settings"),
          T("From now on the service accepts changes only from a window that runs as administrator. A normal window can still show the readings.\n\n"
            "To change this switch again you have to start BaseControl with \"Run as administrator\"."),
          T("Turn on"), Btn::Primary, [ap]() { CfgToast(*ap, "cfg.adminOnly", "1", T("Only administrators can change settings")); });
    }
  }

  if (k.Toggle("sp#keep", T("Keep my settings applied"), T("Applies them again after sleep, a charger change or display wake."), keep, true,
               T("The firmware resets some values at those moments. BaseControl puts back the ones you set."))) {
    CfgToast(a, "cfg.keep", keep ? "0" : "1", "");
  }

  if (k.Toggle("sp#onstart", T("Apply my settings when the service starts"), keep ? T("After a restart of Windows or of the service.") : T("Needs \"Keep my settings applied\"."), onStart,
               keep)) {
    CfgToast(a, "cfg.applyOnStart", onStart ? "0" : "1", "");
  }
}

// ---- confirmations -------------------------------------------------------------------------------------------------------------
void CardConsents(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  std::vector<std::pair<std::string, std::string>> rows = {{"ec_pl_high", T("CPU power above the OEM maximum")},
                                                          {"msr_write", T("Direct CPU register writes")},
                                                          {"fan_fixed", T("Fixed fan speed")},
                                                          {"ctgp_over_oem", T("GPU power above the OEM maximum")},
                                                          {"ctgp_unverified", T("Unverified GPU power steps")},
                                                          {"ctgp_model", T("GPU power on an unlisted model")}};
  int given = 0;
  for (auto& r : rows)
    if (a.B("cfg.consent." + r.first)) given++;
  const std::string note = T("When you confirm a risky change, BaseControl remembers it so it does not ask again for the same kind of change. Reset to be asked again.");
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

// ---- OEM Control Center --------------------------------------------------------------------------------------------------------
void CardOem(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  const bool installed = a.B("oem.installed");
  const bool running = a.B("oem.running");
  const bool takeover = a.B("cfg.takeover");
  const std::string version = a.S("oem.version");

  std::string note;
  uint32_t noteCol = 0;
  if (!installed) {
    note = T("The OEM Control Center is not installed on this computer, so nothing competes with BaseControl.");
  } else if (takeover) {
    note = T("BaseControl is in charge. The OEM service is stopped and disabled, so the OEM Control Center app does not work until you give the service back.");
  } else if (running) {
    note = T("The OEM service is running. In the background it can change power and fan settings and undo yours. Take over to stop and disable it; you can give it back here at any time.");
  } else {
    note = T("The OEM service is not running right now, so it is not in the way. Take over keeps it from starting again with Windows.");
  }
  std::vector<BtnSpec> btns;
  if (installed && !takeover) btns.push_back({"oem#takeover", T("Take over from the OEM service"), Btn::Primary, IconId::Bolt, true,
                                              T("Stops the OEM service and sets it to Disabled. Reversible from this page.")});
  if (installed && takeover) btns.push_back({"oem#restore", T("Give the OEM service back"), Btn::Normal, IconId::Undo, true,
                                             T("Sets the OEM service to start with Windows again and starts it.")});
  const float contentH = 3 * kRowInfo + 8 + NoteHeight(c, m, note) + static_cast<float>(BtnRows(c, m.InnerW(), btns)) * kRowButtons;
  Card k(c, m, T("OEM Control Center"), installed ? (version.empty() ? T("Installed") : Tf("Version %s", version.c_str())) : T("Not installed"), IconId::Service,
         contentH);
  k.Info(T("OEM Control Center"), installed ? T("Installed") : T("Not installed"));
  k.Info(T("OEM service"), !installed ? Dash() : running ? T("Running") : T("Stopped"), running ? th.amber : 0,
         T("The OEM Control Center's background service (GCUBridge). While it runs it may write to the embedded controller."));
  k.Info(T("BaseControl in charge"), takeover ? T("Yes") : T("No"), takeover ? th.green : 0);
  k.Gap(8);
  k.Note(note, noteCol);
  const int hit = BtnFlow(k, btns);
  if (hit >= 0) {
    if (btns[static_cast<size_t>(hit)].id == "oem#takeover") {
      App* ap = &a;
      Ask(a, T("Take over from the OEM service"),
          T("BaseControl will stop the OEM service (GCUBridge) and set it to Disabled so it does not start again with Windows. The OEM Control Center app stops working meanwhile.\n\n"
            "You can give the service back here at any time, and removing BaseControl's service does it automatically."),
          T("Take over"), Btn::Primary, [ap]() { ap->Cmd("oem.takeover", KV(), T("OEM service stopped; BaseControl is in charge")); });
    } else {
      a.Cmd("oem.restore", KV(), T("OEM service restored"));
    }
  }
}

// ---- BaseControl's service -----------------------------------------------------------------------------------------------------
void CardService(Ctx& c, Masonry& m) {
  App& a = c.app;
  const Theme& th = c.ui.th;
  const std::string mode = a.mode();
  const Svc st = ServiceState(a);
  const bool local = mode == "local";

  std::string stText;
  uint32_t stCol = 0;
  switch (st) {
    case Svc::Running: stText = T("Running"); stCol = th.green; break;
    case Svc::Stopped: stText = T("Stopped"); stCol = th.amber; break;
    case Svc::Starting: stText = T("Starting"); stCol = th.amber; break;
    case Svc::Stopping: stText = T("Stopping"); stCol = th.amber; break;
    case Svc::Missing: stText = T("Not installed"); break;
    default: stText = Dash(); break;
  }
  if (a.shotMode() && st != Svc::Missing && st != Svc::Unknown) stText += " (" + T("simulated") + ")";

  std::vector<BtnSpec> btns;
  const std::string localTip = T("This window controls the hardware itself. Close it and open a normal window first, so only one of them talks to the hardware.");
  if (st == Svc::Missing) {
    btns.push_back({"sv#install", T("Install and start the service"), Btn::Primary, IconId::Shield, !local, local ? localTip : ""});
  } else {
    if (st == Svc::Stopped) btns.push_back({"sv#start", T("Start the service"), Btn::Primary, IconId::Power, !local, local ? localTip : ""});
    if (st == Svc::Running) btns.push_back({"sv#stop", T("Stop the service"), Btn::Normal, IconId::Power, true, ""});
    if (st == Svc::Running || st == Svc::Stopped)
      btns.push_back({"sv#remove", T("Remove the service"), Btn::Danger, IconId::Trash, true,
                      T("Stops and removes the service and gives the OEM service back. Your saved profiles stay.")});
  }
  const std::string note = T("Installing copies BaseControl into Program Files first, so the service never runs a file that a normal user could replace.");
  const float contentH = 4 * kRowInfo + 8 + NoteHeight(c, m, note) + static_cast<float>(BtnRows(c, m.InnerW(), btns)) * kRowButtons;
  const std::string sub = mode == "service" ? T("This window talks to it") : mode == "sim" ? T("Not used in the simulation") : T("Not used by this window");
  Card k(c, m, T("Background service"), sub, IconId::Chip, contentH);
  k.Info(T("Running as"), mode == "service" ? T("Background service") : mode == "sim" ? T("Simulation") : T("This window, elevated"), mode == "sim" ? th.amber : 0,
         T("The BaseControl service does the privileged work, so this window never needs administrator rights."));
  k.Info(T("Windows service"), stText, stCol);
  const std::string link = a.S("ec.link");
  k.Info(T("Hardware link"), link == "connected" ? T("Connected") : link == "absent" ? T("Not found") : Dash(), link == "connected" ? th.green : (link == "absent" ? th.amber : 0),
         T("Whether the laptop's embedded controller answers."));
  k.Info(T("Version"), a.S("app.version").empty() ? Dash() : a.S("app.version"));
  k.Gap(8);
  k.Note(note);
  const int hit = BtnFlow(k, btns);
  if (hit < 0) return;
  App* ap = &a;
  const std::string id = btns[static_cast<size_t>(hit)].id;
  if (id == "sv#install") {
    a.RunElevated(L"--install-service", T("Service installed and started"));
  } else if (id == "sv#start") {
    a.RunElevated(L"--start-service", T("Service started"));
  } else if (id == "sv#stop") {
    Ask(a, T("Stop the service"),
        T("While the service is stopped, BaseControl cannot change or watch anything. Values already in the laptop stay as they are, but the fan watchdog and \"Keep my settings applied\" do nothing until you start it again."),
        T("Stop the service"), Btn::Primary, [ap]() { ap->RunElevated(L"--stop-service", T("Service stopped")); });
  } else if (id == "sv#remove") {
    Ask(a, T("Remove the service"),
        T("The service is stopped and removed from Windows, and the OEM service is given back. Your profiles and settings are kept; installing the service again brings everything back."),
        T("Remove"), Btn::Danger, [ap]() { ap->RunElevated(L"--uninstall-service", T("Service removed")); });
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
  const bool tripped = a.S("fan.guard") == "tripped";
  const float logH = kLogLines * kLogLineH + 16;
  const float contentH = 4 * kRowInfo + 8 + kRowButtons + 30 + logH;
  Card k(c, m, T("Diagnostics"), T("For a bug report"), IconId::Log, contentH);
  k.Info(T("Writes to the hardware"), a.Has("ec.writes") ? Tf("%d", static_cast<int>(a.I("ec.writes"))) : Dash(), 0,
         T("Embedded-controller registers BaseControl has written since it started."));
  k.Info(T("Writes refused by the guard"), a.Has("ec.refused") ? Tf("%d", static_cast<int>(a.I("ec.refused"))) : Dash(), a.I("ec.refused") > 0 ? th.amber : 0,
         T("A write outside the allow-list or its value limits is refused before it reaches the hardware."));
  k.Info(T("Fan watchdog"), tripped ? T("Forced full speed") : T("Not triggered"), tripped ? th.red : 0,
         tripped ? a.S("fan.guard.why") : T("It forces full fan speed when a fixed or custom fan setting lets the CPU or GPU get dangerously hot."));
  k.Info(T("Last error"), err.empty() ? T("None") : g.Fit(err, FontId::Body, k.w * 0.55f), err.empty() ? 0 : th.amber, err);
  k.Gap(8);
  const int hit = BtnFlow(k, {{"dg#probe", T("Detect hardware again"), Btn::Normal, IconId::Refresh, true, T("Runs the start-up hardware check once more.")},
                              {"dg#copy", T("Copy diagnostics"), Btn::Normal, IconId::Copy, true,
                               T("Copies the model, firmware, detected features, the last error and the recent log as text. It contains no serial numbers and no user name.")}});
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
  const Ev evs[] = {Ev::Verified, Ev::Family, Ev::Experimental, Ev::Os};
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
      T("BaseControl never writes to the BIOS, the flash chip or UEFI variables."),
      T("Embedded-controller writes use an allow-list of registers with limits on each value. Anything else is refused, and registers that identify the charger or bypass the firmware's power limits are on a never-write list."),
      T("Every change is read back. When it did not stick, the previous value is put back."),
      T("While a custom or fixed fan setting is active, a watchdog forces full fan speed when the CPU reaches 97 \xC2\xB0""C or the graphics card 92 \xC2\xB0""C."),
      T("The OEM profile file is backed up before it is changed, and the OEM service can be handed back at any time."),
  };
  const float textW = m.InnerW() - 28;
  float contentH = 0;
  for (const std::string& s : items) contentH += WrapH(c, textW, s) + 14;
  Card k(c, m, T("What BaseControl never does"), T("Hard limits that no switch changes"), IconId::Lock, contentH);
  for (const std::string& s : items) {
    const float h = WrapH(c, textW, s) + 14;
    const Rect r = k.Row(h);
    g.Icon(IconId::Check, Rect(r.x, r.y + 2, 16, 16), th.green, 2.0f);
    g.Wrapped(Rect(r.x + 28, r.y, textW, h), s, FontId::Small, th.muted);
  }
}

}  // namespace

float PageSafety(App& a, const Rect& r) {
  Ctx c(a, r.w);
  Masonry m(r.w, 430, 3);
  CardProtection(c, m);
  CardService(c, m);
  CardOem(c, m);
  CardConsents(c, m);
  CardDiagnostics(c, m);
  CardLabels(c, m);
  CardLimits(c, m);
  return m.Bottom();
}

}  // namespace ui
}  // namespace bc
