// BaseControl Advanced - one executable, several roles:
//   (no arguments)          the window; talks to the BaseControl service (or offers to set it up)
//   --local                 the window with an in-process controller (needs administrator rights)
//   --sim                   the window on a simulated laptop (no hardware is touched)
//   --tray                  start hidden in the notification area
//   --service               run as the Windows service (started by the service manager)
//   --service-console       the same service logic in a console (debugging)
//   --install-service       (elevated) install + start the service            --uninstall-service (elevated) remove it
//   --start-service / --stop-service / --restore-oem
//   --probe                 print what this machine reports, then exit
//   --shot <page> <out.png> [--size WxH] [--scale S] [--advanced] [--drafts] [--font F] [--lang en|tr]   documentation screenshots (simulated)
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
#include "common/fsutil.h"
#include "common/util.h"
#include "common/version.h"
#include "hw/oem_services.h"
#include "hw/win_world.h"
#include "hw/winutil.h"
#include "svc/service.h"
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

void AttachParentConsole() {
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
}

void Log(const std::string& line) {
  const std::string dir = bc::hw::ProgramDataDir();
  bc::fs::Append(bc::fs::Join(dir, "setup.log"), line + "\n");
  std::printf("%s\n", line.c_str());
}

int Help() {
  AttachParentConsole();
  std::printf(
      "BaseControl Advanced %s\n\n"
      "  BaseControl.exe                 open the window (service mode)\n"
      "  --local                         window + in-process controller (administrator)\n"
      "  --sim [--sim-model exact|family|unknown]   window on a simulated laptop\n"
      "  --tray                          start hidden in the tray\n"
      "  --lang en|tr                    interface language\n"
      "  --service-console               run the service in this console\n"
      "  --install-service               install and start the service (administrator)\n"
      "  --uninstall-service             remove the service (administrator)\n"
      "  --start-service | --stop-service | --restore-oem\n"
      "  --probe                         print the hardware report and exit\n"
      "  --shot <page> <out.png> [--size WxH] [--scale S] [--advanced] [--drafts] [--font F]\n"
      "  --script <file>                 scripted UI test on the simulator\n",
      BC_VERSION);
  return 0;
}

int Probe() {
  AttachParentConsole();
  bc::hw::WinWorld world(bc::hw::WorldKind::Local, bc::hw::ProgramDataDir(), "probe");
  bc::Controller::Options co;
  co.mode = "local";
  co.dataDir = "";
  co.version = BC_VERSION;
  bc::Controller ctl(world.hw(), co);
  ctl.Start();
  Sleep(2500);
  const bc::KV s = ctl.Snapshot();
  std::printf("BaseControl %s probe (elevated: %s)\n", BC_VERSION, bc::hw::IsElevated() ? "yes" : "no");
  for (const auto& p : s.All()) std::printf("%s=%s\n", p.first.c_str(), p.second.c_str());
  ctl.Stop();
  return 0;
}

int ServiceVerb(const std::string& verb) {
  std::string why;
  if (!bc::hw::IsElevated()) {
    Log("this action needs administrator rights");
    return 5;
  }
  bool ok = false;
  if (verb == "--install-service") {
    ok = bc::svc::Install(&why);
    if (ok) ok = bc::svc::StartNow(&why);
  } else if (verb == "--uninstall-service") {
    bc::hw::OemServices oem(bc::hw::ProgramDataDir());
    std::string w2;
    oem.EnableAndStart(&w2);   // give the OEM Control Center its services back (no-op when nothing was taken over)
    bc::svc::StopNow(&w2);
    ok = bc::svc::Uninstall(&why);
  } else if (verb == "--start-service") {
    ok = bc::svc::StartNow(&why);
  } else if (verb == "--stop-service") {
    ok = bc::svc::StopNow(&why);
  } else if (verb == "--restore-oem") {
    bc::hw::OemServices oem(bc::hw::ProgramDataDir());
    ok = oem.EnableAndStart(&why);
  }
  Log(verb + (ok ? ": ok" : ": failed - " + why));
  return ok ? 0 : 1;
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
    if (s == "--service") return bc::svc::RunAsService();
    if (s == "--service-console") {
      AttachParentConsole();
      return bc::svc::RunInConsole();
    }
    if (s == "--install-service" || s == "--uninstall-service" || s == "--start-service" || s == "--stop-service" || s == "--restore-oem") return ServiceVerb(s);
    if (s == "--probe") return Probe();
    if (s == "--local") o.mode = "local";
    else if (s == "--sim") o.mode = "sim";
    else if (s == "--sim-model" && i + 1 < a.size()) o.simModel = a[++i];
    else if (s == "--tray") o.tray = true;
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
    else if (s == "--advanced") o.shotAdvanced = true;
    else if (s == "--drafts") o.shotDrafts = true;
    else if (s == "--script" && i + 1 < a.size()) {
      o.script = a[++i];
      o.mode = "sim";
      o.shotOut = "";
      gui = false;
    }
  }
  if (o.mode == "local" && !bc::hw::IsElevated()) {
    // restart elevated; the elevated copy owns the hardware
    ShellExecuteW(nullptr, L"runas", bc::hw::SelfPath().c_str(), L"--local", nullptr, SW_SHOWNORMAL);
    return 0;
  }
  if (!gui) {
    AttachParentConsole();
    bc::ui::App app(o);
    return app.RunShot();
  }
  // one window per user session
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\BaseControlAdvancedUI");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND w = FindWindowW(L"BaseControlAdvancedMain", nullptr);
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
