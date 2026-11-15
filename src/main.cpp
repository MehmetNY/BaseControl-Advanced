// Control Center Advanced - one executable, one elevated process:
//   (no arguments)          the window (and the controller inside it; it talks to the vendor's Control Center service)
//   --tray                  start hidden in the notification area
//   --sim                   the window on a simulated laptop (no hardware is touched)
//   --probe                 print what this machine reports, then exit
//   --autostart on|off      create or remove the sign-in task ("Start with Windows"), then exit; the installer uses it
//   --lang en|tr            interface language
//   --shot <page> <out.png> [--size WxH] [--scale S] [--font F] [--lang en|tr]   documentation screenshots (simulated)
//   --script <file>         scripted UI test on the simulator
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/controller.h"
#include "common/util.h"
#include "common/version.h"
#include "hw/autostart_win.h"
#include "hw/win_world.h"
#include "hw/winutil.h"
#include "ui/app.h"

namespace {

using bc::Split;

std::vector<std::string> Args() {
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::string> out;
  for (int i = 1; i < n; i++) out.push_back(bc::hw::U(w[i]));
  if (w) LocalFree(w);
  return out;
}

// A GUI-subsystem program has no console of its own. Reach for the parent's console only for the output that is not already going
// somewhere: `ControlCenterAdvanced.exe --probe > probe.txt` and `| more` keep working, a plain run from a prompt prints to the prompt.
void AttachParentConsole() {
  auto usable = [](DWORD which) {
    HANDLE h = GetStdHandle(which);
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
  };
  const bool haveOut = usable(STD_OUTPUT_HANDLE), haveErr = usable(STD_ERROR_HANDLE);
  if (haveOut && haveErr) return;
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* f = nullptr;
  if (!haveOut) freopen_s(&f, "CONOUT$", "w", stdout);
  if (!haveErr) freopen_s(&f, "CONOUT$", "w", stderr);
}

int Help() {
  AttachParentConsole();
  std::printf(
      "Control Center Advanced %s\n\n"
      "  ControlCenterAdvanced.exe                 open the window (administrator rights are needed for the hardware)\n"
      "  --tray                          start hidden in the tray\n"
      "  --sim [--sim-model exact|family|unknown] [--sim-version X.Y.Z]   window on a simulated laptop\n"
      "  --lang en|tr                    interface language\n"
      "  --probe                         print the hardware report and exit\n"
      "  --autostart on|off              create or remove the sign-in task (Start with Windows) and exit\n"
      "  --shot <page> <out.png> [--size WxH] [--scale S] [--font F]\n"
      "  --script <file>                 scripted UI test on the simulator\n",
      BC_VERSION);
  return 0;
}

// "Start with Windows" without opening the window: what the installer and the uninstaller call. Exit code 0 = done, 1 = it did not work, 2 = usage.
int AutostartCommand(const std::string& what) {
  AttachParentConsole();
  if (what != "on" && what != "off") {
    std::printf("usage: ControlCenterAdvanced.exe --autostart on|off\n");
    return 2;
  }
  std::string why;
  if (!bc::hw::SetAutostart(what == "on", &why)) {
    std::printf("Start with Windows could not be turned %s: %s\n", what.c_str(), why.c_str());
    return 1;
  }
  std::printf("Start with Windows is %s.\n", what.c_str());
  return 0;
}

int Probe() {
  AttachParentConsole();
  bc::hw::WinWorld world(bc::hw::ProgramDataDir(), "probe");
  bc::Controller::Options co;
  co.mode = "local";
  co.dataDir = "";
  co.version = BC_VERSION;
  bc::Controller ctl(world.hw(), co);
  ctl.Start();
  // the link to the vendor's service needs a moment: wait until it has reported, at most a few seconds
  bc::KV s = ctl.Snapshot();
  for (int i = 0; i < 24 && !(s.Bool("oem.up") && s.Has("perf.mode")); i++) {
    Sleep(250);
    s = ctl.Snapshot();
  }
  std::printf("Control Center Advanced %s probe (elevated: %s)\n", BC_VERSION, bc::hw::IsElevated() ? "yes" : "no");
  for (const auto& p : s.All()) std::printf("%s=%s\n", p.first.c_str(), p.second.c_str());
  ctl.Stop();
  return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show) {
  const std::vector<std::string> a = Args();
  bc::ui::Options o;
  bool gui = true;
  for (size_t i = 0; i < a.size(); i++) {
    const std::string& s = a[i];
    if (s == "--help" || s == "-h" || s == "/?") return Help();
    if (s == "--version") {
      AttachParentConsole();
      std::printf("%s\n", BC_VERSION);
      return 0;
    }
    if (s == "--probe") return Probe();
    if (s == "--autostart") return AutostartCommand(i + 1 < a.size() ? a[i + 1] : "");
    if (s == "--local") o.mode = "local";   // the default; accepted so older shortcuts keep working
    else if (s == "--sim") o.mode = "sim";
    else if (s == "--sim-model" && i + 1 < a.size()) o.simModel = a[++i];
    else if (s == "--sim-version" && i + 1 < a.size()) o.simVersion = a[++i];
    else if (s == "--tray" || s == "/minimized") o.tray = true;   // "/minimized" is what the autostart task of the Monster GPU Power app used
    else if (s == "--lang" && i + 1 < a.size()) o.lang = a[++i];
    else if (s == "--shot" && i + 2 < a.size()) {
      o.shotPage = a[++i];
      o.shotOut = a[++i];
      o.mode = "sim";
      gui = false;
    } else if (s == "--size" && i + 1 < a.size()) {
      const std::vector<std::string> p = Split(a[++i], 'x');
      if (p.size() == 2) {
        o.shotW = std::atoi(p[0].c_str());
        o.shotH = std::atoi(p[1].c_str());
      }
    } else if (s == "--scale" && i + 1 < a.size()) o.shotScale = static_cast<float>(std::atof(a[++i].c_str()));
    else if (s == "--font" && i + 1 < a.size()) o.shotFont = a[++i];
    else if (s == "--drafts") o.shotDrafts = true;
    else if (s == "--script" && i + 1 < a.size()) {
      o.script = a[++i];
      o.mode = "sim";
      o.shotOut = "";
      gui = false;
    }
  }
  if (o.mode == "local" && !bc::hw::IsElevated()) {
    // The manifest asks for administrator rights, so this only happens when it was bypassed: restart elevated with the same arguments.
    std::wstring args;
    for (const std::string& x : a) args += (args.empty() ? L"\"" : L" \"") + bc::hw::W(x) + L"\"";
    ShellExecuteW(nullptr, L"runas", bc::hw::SelfPath().c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
    return 0;
  }
  if (!gui) {
    AttachParentConsole();
    bc::ui::App app(o);
    return app.RunShot();
  }
  // one window per user session
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ControlCenterAdvancedUI");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND w = FindWindowW(L"ControlCenterAdvancedMain", nullptr);
    if (w) {
      ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(w);
    }
    return 0;
  }
  bc::ui::App app(o);
  const int rc = app.RunWindow(hi, show);
  if (mutex) CloseHandle(mutex);
  return rc;
}
