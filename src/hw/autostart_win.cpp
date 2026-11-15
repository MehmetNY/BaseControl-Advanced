#include "autostart_win.h"

#include <cstdio>
#include <vector>

#include "winutil.h"

namespace bc {
namespace hw {
namespace {

// Runs schtasks.exe with fixed arguments and returns its exit code and what it printed. No window, at most `timeoutMs`.
bool RunSchtasks(const std::wstring& args, std::string* out, DWORD* exitCode, DWORD timeoutMs = 20000) {
  const std::wstring exe = SystemDirectory() + L"\\schtasks.exe";
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    return false;
  }
  std::string text;
  const ULONGLONG t0 = GetTickCount64();
  for (;;) {
    DWORD avail = 0;
    while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      char tmp[4096];
      DWORD got = 0;
      if (!ReadFile(rd, tmp, sizeof tmp, &got, nullptr) || got == 0) break;
      if (text.size() < (1u << 20)) text.append(tmp, got);
    }
    if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
    if (GetTickCount64() - t0 > timeoutMs) {
      TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      CloseHandle(rd);
      return false;
    }
  }
  DWORD avail = 0;
  while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {   // what it printed just before it ended
    char tmp[4096];
    DWORD got = 0;
    if (!ReadFile(rd, tmp, sizeof tmp, &got, nullptr) || got == 0) break;
    if (text.size() < (1u << 20)) text.append(tmp, got);
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(rd);
  if (out) *out = text;
  if (exitCode) *exitCode = code;
  return true;
}

std::wstring TaskArg() { return L"/TN \"" + W(kAutostartTaskName) + L"\""; }

std::string CurrentUserId() {
  wchar_t dom[256] = {0}, usr[256] = {0};
  GetEnvironmentVariableW(L"USERDOMAIN", dom, 255);
  GetEnvironmentVariableW(L"USERNAME", usr, 255);
  if (!usr[0]) return "";
  return dom[0] ? U(std::wstring(dom) + L"\\" + usr) : U(usr);
}

void RemoveRunKeyEntry() {
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
    RegDeleteValueW(k, L"ControlCenterAdvanced");
    RegCloseKey(k);
  }
}

}  // namespace

TaskObservation QueryAutostart() {
  TaskObservation o;
  std::string out;
  DWORD code = 1;
  if (!RunSchtasks(L"/Query " + TaskArg() + L" /XML", &out, &code)) return o;   // the scheduler could not be run: unknown
  if (code != 0) {
    o.known = true;   // "no such task" (the message is localised, so the exit code is what counts)
    return o;
  }
  return ParseTaskXml(out);
}

bool AutostartAllowedHere(std::string* why) {
  if (PathIsBelowAny(U(SelfPath()), ProtectedProgramRoots())) return true;
  if (why) *why = "this copy of the program is not in Program Files, where only administrators can change files";
  return false;
}

bool SetAutostart(bool on, std::string* why) {
  RemoveRunKeyEntry();
  if (on && !AutostartAllowedHere(why)) return false;   // a task with administrator rights must not run a file that any user could replace
  if (!on) {
    std::string out;
    DWORD code = 1;
    RunSchtasks(L"/Delete " + TaskArg() + L" /F", &out, &code);
    const TaskObservation o = QueryAutostart();
    if (o.known && !o.exists) return true;
    if (why) *why = "the task could not be removed: " + out.substr(0, 200);
    return false;
  }
  const std::wstring tmpDir = [] {
    wchar_t t[MAX_PATH + 2] = {0};
    const DWORD n = GetTempPathW(MAX_PATH, t);
    return std::wstring(t, n);
  }();
  const std::wstring file = tmpDir + L"cca_autostart_" + std::to_wstring(GetCurrentProcessId()) + L".xml";
  const std::wstring xml = W(BuildAutostartXml(U(SelfPath()), CurrentUserId()));
  HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (why) *why = "cannot write the task definition: " + Win32Text(GetLastError());
    return false;
  }
  const wchar_t bom = 0xFEFF;
  DWORD wrote = 0;
  WriteFile(h, &bom, sizeof bom, &wrote, nullptr);
  WriteFile(h, xml.data(), static_cast<DWORD>(xml.size() * sizeof(wchar_t)), &wrote, nullptr);
  CloseHandle(h);
  std::string out;
  DWORD code = 1;
  const bool ran = RunSchtasks(L"/Create " + TaskArg() + L" /XML \"" + file + L"\" /F", &out, &code);
  DeleteFileW(file.c_str());
  if (!ran || code != 0) {
    if (why) *why = ran ? "the scheduler refused the task: " + out.substr(0, 200) : std::string("the task scheduler could not be started");
    return false;
  }
  if (!AutostartIsActive(QueryAutostart(), U(SelfPath()))) {
    if (why) *why = "the task was created but the scheduler does not show it as active";
    return false;
  }
  return true;
}

}  // namespace hw
}  // namespace bc
