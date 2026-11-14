#include "service.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "../common/controller.h"
#include "../common/fsutil.h"
#include "../common/util.h"
#include "../common/version.h"
#include "../hw/power_guids.h"
#include "../hw/win_world.h"
#include "../hw/winutil.h"
#include "pipe_server.h"

namespace bc {
namespace svc {
namespace {

const wchar_t kName[] = L"BaseControl";
const wchar_t kDisplay[] = L"BaseControl Advanced";
const wchar_t kDescription[] =
    L"Hardware access for BaseControl Advanced (embedded controller, power limits, fans, GPU, lighting). The user interface talks to it over a local named pipe.";

const GUID& kConsoleDisplayState = hw::kGuidConsoleDisplayState;
const GUID& kAcDcPowerSource = hw::kGuidAcDcPowerSource;

SERVICE_STATUS_HANDLE g_ss = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stop = nullptr;
std::atomic<Controller*> g_ctl{nullptr};
HPOWERNOTIFY g_pn[2] = {nullptr, nullptr};
bool g_console = false;

void SetState(DWORD state, DWORD waitHintMs = 0, DWORD win32Exit = 0) {
  if (!g_ss) return;
  static DWORD check = 1;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = win32Exit;
  g_status.dwWaitHint = waitHintMs;
  g_status.dwControlsAccepted =
      (state == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT);
  g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : check++;
  SetServiceStatus(g_ss, &g_status);
}

void Notify(const char* ev) {
  if (Controller* c = g_ctl.load()) c->Notify(ev);
}

DWORD WINAPI Handler(DWORD ctl, DWORD type, LPVOID data, LPVOID) {
  switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      SetState(SERVICE_STOP_PENDING, 15000);
      if (g_stop) SetEvent(g_stop);
      return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
      if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
        Notify("resume");
      } else if (type == PBT_APMSUSPEND) {
        Notify("suspend");
      } else if (type == PBT_POWERSETTINGCHANGE && data) {
        const POWERBROADCAST_SETTING* p = static_cast<const POWERBROADCAST_SETTING*>(data);
        if (p->DataLength >= 4) {
          DWORD v = 0;
          std::memcpy(&v, p->Data, 4);
          if (IsEqualGUID(p->PowerSetting, kConsoleDisplayState)) {
            if (v == 1) Notify("display_on");     // 0 = off, 1 = on, 2 = dimmed
            else if (v == 0) Notify("display_off");
          } else if (IsEqualGUID(p->PowerSetting, kAcDcPowerSource)) {
            Notify("ac_change");
          }
        }
      }
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

BOOL WINAPI ConsoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    if (g_stop) SetEvent(g_stop);
    return TRUE;
  }
  return FALSE;
}

// The actual work, shared by the SCM entry and the console mode.
DWORD RunCore() {
  const std::string dir = hw::ProgramDataDir();
  hw::WinWorld world(hw::WorldKind::Service, dir, "service");
  mgp::ILog* log = world.log();
  log->Write(mgp::LogLevel::Info, std::string("service starting, version ") + BC_VERSION);

  Controller::Options o;
  o.dataDir = dir;
  o.mode = "service";
  o.version = BC_VERSION;
  Controller ctl(world.hw(), o);
  ctl.Start();
  g_ctl = &ctl;

  PipeServer srv(ctl, *log, "service", BC_VERSION);
  std::string why;
  if (!srv.Start(&why)) {
    log->Write(mgp::LogLevel::Error, "cannot create the named pipe: " + why);
    g_ctl = nullptr;
    ctl.Stop();
    return ERROR_PIPE_BUSY;
  }
  if (g_ss) {
    g_pn[0] = RegisterPowerSettingNotification(g_ss, &kConsoleDisplayState, DEVICE_NOTIFY_SERVICE_HANDLE);
    g_pn[1] = RegisterPowerSettingNotification(g_ss, &kAcDcPowerSource, DEVICE_NOTIFY_SERVICE_HANDLE);
  }
  SetState(SERVICE_RUNNING);
  log->Write(mgp::LogLevel::Info, "service running");
  WaitForSingleObject(g_stop, INFINITE);
  log->Write(mgp::LogLevel::Info, "service stopping");
  for (HPOWERNOTIFY& h : g_pn)
    if (h) {
      UnregisterPowerSettingNotification(h);
      h = nullptr;
    }
  g_ctl = nullptr;
  srv.Stop();
  ctl.Stop();
  log->Write(mgp::LogLevel::Info, "service stopped");
  return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  g_ss = RegisterServiceCtrlHandlerExW(kName, Handler, nullptr);
  if (!g_ss) return;
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  SetState(SERVICE_START_PENDING, 20000);
  const DWORD rc = RunCore();
  SetState(SERVICE_STOPPED, 0, rc);
  CloseHandle(g_stop);
  g_stop = nullptr;
}

// ---- SCM helpers
struct ScHandle {
  SC_HANDLE h = nullptr;
  explicit ScHandle(SC_HANDLE x = nullptr) : h(x) {}
  ~ScHandle() { if (h) CloseServiceHandle(h); }
  ScHandle(const ScHandle&) = delete;
  ScHandle& operator=(const ScHandle&) = delete;
  explicit operator bool() const { return h != nullptr; }
};

std::string Err(const char* what) { return std::string(what) + ": " + hw::Win32Text(GetLastError()); }

bool InProtectedFolder(const std::wstring& path) {
  auto under = [&](const std::wstring& root) {
    if (root.empty() || path.size() <= root.size()) return false;
    return _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 && path[root.size()] == L'\\';
  };
  wchar_t pf[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH) && under(pf)) return true;
  if (GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH) && under(pf)) return true;
  return under(hw::KnownFolder(0x0026 /*CSIDL_PROGRAM_FILES*/));
}

bool WaitState(SC_HANDLE s, DWORD want, DWORD timeoutMs) {
  const ULONGLONG t0 = GetTickCount64();
  for (;;) {
    SERVICE_STATUS st = {};
    if (!QueryServiceStatus(s, &st)) return false;
    if (st.dwCurrentState == want) return true;
    if (GetTickCount64() - t0 > timeoutMs) return false;
    Sleep(200);
  }
}

}  // namespace

const char* StateName(State s) {
  switch (s) {
    case State::NotInstalled: return "not-installed";
    case State::Stopped: return "stopped";
    case State::Starting: return "starting";
    case State::Running: return "running";
    case State::Stopping: return "stopping";
    default: return "unknown";
  }
}

State Query() {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) return State::Unknown;
  ScHandle s(OpenServiceW(scm.h, kName, SERVICE_QUERY_STATUS));
  if (!s) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? State::NotInstalled : State::Unknown;
  SERVICE_STATUS st = {};
  if (!QueryServiceStatus(s.h, &st)) return State::Unknown;
  switch (st.dwCurrentState) {
    case SERVICE_RUNNING: return State::Running;
    case SERVICE_START_PENDING: return State::Starting;
    case SERVICE_STOP_PENDING: return State::Stopping;
    case SERVICE_STOPPED: return State::Stopped;
    default: return State::Running;  // paused / continue pending: treat as present
  }
}

int RunAsService() {
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kName), ServiceMain}, {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(table)) {
    // started from a console by mistake
    return static_cast<int>(GetLastError());
  }
  return 0;
}

int RunInConsole() {
  g_console = true;
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  std::printf("BaseControl service (console mode) - Ctrl+C stops it\n");
  const DWORD rc = RunCore();
  CloseHandle(g_stop);
  g_stop = nullptr;
  return static_cast<int>(rc);
}

bool Install(std::string* why) {
  auto fail = [&](const std::string& m) {
    if (why) *why = m;
    return false;
  };
  if (!hw::IsElevated()) return fail("administrator rights are required");
  std::wstring exe = hw::SelfPath();
  if (!InProtectedFolder(exe)) {
    // portable copy somewhere user-writable: install a private copy below Program Files
    std::wstring dir = hw::ProgramFiles() + L"\\" + L"BaseControl Advanced";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring target = dir + L"\\BaseControl.exe";
    // a running copy would be locked: stop the service first (ignore failures, the copy reports its own)
    {
      std::string ignore;
      StopNow(&ignore);
    }
    if (!CopyFileW(exe.c_str(), target.c_str(), FALSE)) return fail(Err("cannot copy the program to Program Files"));
    exe = target;
  }
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (!scm) return fail(Err("cannot open the service manager"));
  const std::wstring cmd = L"\"" + exe + L"\" --service";
  ScHandle s(CreateServiceW(scm.h, kName, kDisplay, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                            nullptr, nullptr, nullptr, nullptr, nullptr));  // LocalSystem
  if (!s) {
    if (GetLastError() != ERROR_SERVICE_EXISTS) return fail(Err("cannot create the service"));
    s.h = OpenServiceW(scm.h, kName, SERVICE_ALL_ACCESS);
    if (!s) return fail(Err("cannot open the existing service"));
    if (!ChangeServiceConfigW(s.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr, nullptr,
                              nullptr, kDisplay))
      return fail(Err("cannot update the service"));
  }
  SERVICE_DESCRIPTIONW d = {const_cast<LPWSTR>(kDescription)};
  ChangeServiceConfig2W(s.h, SERVICE_CONFIG_DESCRIPTION, &d);
  SERVICE_DELAYED_AUTO_START_INFO delayed = {TRUE};
  ChangeServiceConfig2W(s.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
  SC_ACTION acts[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 15000}, {SC_ACTION_RESTART, 60000}};
  SERVICE_FAILURE_ACTIONSW fa = {};
  fa.dwResetPeriod = 24 * 3600;
  fa.cActions = 3;
  fa.lpsaActions = acts;
  ChangeServiceConfig2W(s.h, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
  return true;
}

bool Uninstall(std::string* why) {
  auto fail = [&](const std::string& m) {
    if (why) *why = m;
    return false;
  };
  if (!hw::IsElevated()) return fail("administrator rights are required");
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) return fail(Err("cannot open the service manager"));
  ScHandle s(OpenServiceW(scm.h, kName, SERVICE_ALL_ACCESS));
  if (!s) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? true : fail(Err("cannot open the service"));
  SERVICE_STATUS st = {};
  ControlService(s.h, SERVICE_CONTROL_STOP, &st);
  WaitState(s.h, SERVICE_STOPPED, 20000);
  if (!DeleteService(s.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) return fail(Err("cannot delete the service"));
  return true;
}

bool StartNow(std::string* why) {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) {
    if (why) *why = Err("cannot open the service manager");
    return false;
  }
  ScHandle s(OpenServiceW(scm.h, kName, SERVICE_START | SERVICE_QUERY_STATUS));
  if (!s) {
    if (why) *why = Err("cannot open the service");
    return false;
  }
  if (!StartServiceW(s.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
    if (why) *why = Err("cannot start the service");
    return false;
  }
  if (!WaitState(s.h, SERVICE_RUNNING, 20000)) {
    if (why) *why = "the service did not reach the running state in time";
    return false;
  }
  return true;
}

bool StopNow(std::string* why) {
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) {
    if (why) *why = Err("cannot open the service manager");
    return false;
  }
  ScHandle s(OpenServiceW(scm.h, kName, SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (!s) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
  SERVICE_STATUS st = {};
  if (!ControlService(s.h, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
    if (why) *why = Err("cannot stop the service");
    return false;
  }
  return WaitState(s.h, SERVICE_STOPPED, 20000);
}

}  // namespace svc
}  // namespace bc
