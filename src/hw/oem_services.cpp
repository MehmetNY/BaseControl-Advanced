#include "oem_services.h"

#include <tlhelp32.h>

#include <algorithm>

#include "../common/fsutil.h"
#include "../common/kv.h"
#include "../common/util.h"

namespace bc {
namespace hw {
namespace {

std::wstring LowerW(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

// "C:\Program Files\OEM\Control Center\UniwillService\GCUBridge.exe" -args  ->  the bare image path
std::wstring StripImage(const std::wstring& cmd) {
  std::wstring s = cmd;
  size_t b = 0;
  while (b < s.size() && s[b] == L' ') ++b;
  s = s.substr(b);
  if (!s.empty() && s[0] == L'"') {
    size_t e = s.find(L'"', 1);
    return e == std::wstring::npos ? s.substr(1) : s.substr(1, e - 1);
  }
  const std::wstring l = LowerW(s);
  size_t e = l.find(L".exe");
  return e == std::wstring::npos ? s : s.substr(0, e + 4);
}

bool WaitState(SC_HANDLE svc, DWORD want, DWORD ms) {
  const ULONGLONG t0 = GetTickCount64();
  for (;;) {
    SERVICE_STATUS_PROCESS ssp;
    DWORD need = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof ssp, &need)) return false;
    if (ssp.dwCurrentState == want) return true;
    if (GetTickCount64() - t0 > ms) return false;
    Sleep(200);
  }
}

struct ScHandle {
  SC_HANDLE h;
  explicit ScHandle(SC_HANDLE x = nullptr) : h(x) {}
  ~ScHandle() { if (h) CloseServiceHandle(h); }
  ScHandle(const ScHandle&) = delete;
  ScHandle& operator=(const ScHandle&) = delete;
  operator SC_HANDLE() const { return h; }
};

}  // namespace

OemProbeWin::OemProbeWin(const std::string& stateDir) : stateFile_(stateDir.empty() ? "" : fs::Join(stateDir, "oem_takeover.kv")) {
  knownNames_ = {"GCUBridge", "GCUService"};  // evidence: GCUBridge is registered on the reference device
}

std::wstring OemProbeWin::OemRoot() const { return ProgramFiles() + L"\\OEM\\Control Center\\"; }

bool OemProbeWin::IsOemImage(const std::wstring& path) const {
  const std::wstring l = LowerW(path), root = LowerW(OemRoot());
  return l.size() > root.size() && l.compare(0, root.size(), root) == 0 && l.find(L"..") == std::wstring::npos;
}

std::wstring OemProbeWin::ServiceImagePath(SC_HANDLE svc) const {
  DWORD need = 0;
  QueryServiceConfigW(svc, nullptr, 0, &need);
  if (need == 0 || need > (1u << 16)) return L"";
  std::vector<BYTE> buf(need);
  QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
  if (!QueryServiceConfigW(svc, cfg, need, &need) || !cfg->lpBinaryPathName) return L"";
  return StripImage(cfg->lpBinaryPathName);
}

std::vector<OemProbeWin::Svc> OemProbeWin::Discover(bool full) {
  std::vector<Svc> out;
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
  if (!scm) return out;
  auto add = [&](const std::wstring& wname) {
    for (const Svc& s : out)
      if (s.wname == wname) return;
    ScHandle svc(OpenServiceW(scm, wname.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!svc) return;
    if (!IsOemImage(ServiceImagePath(svc))) return;  // only services that really live in the OEM folder
    Svc s;
    s.name = U(wname);
    s.wname = wname;
    DWORD need = 0;
    QueryServiceConfigW(svc, nullptr, 0, &need);
    if (need && need < (1u << 16)) {
      std::vector<BYTE> buf(need);
      QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
      if (QueryServiceConfigW(svc, cfg, need, &need)) s.startType = cfg->dwStartType;
    }
    SERVICE_STATUS_PROCESS ssp;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof ssp, &need)) s.state = ssp.dwCurrentState;
    out.push_back(s);
  };
  for (const std::string& n : knownNames_) add(W(n));
  if (full && !enumerated_) {
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &need, &count, &resume, nullptr);
    if (need > 0 && need < (4u << 20)) {
      std::vector<BYTE> buf(need + 4096);
      resume = 0;
      if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), static_cast<DWORD>(buf.size()), &need, &count,
                                &resume, nullptr)) {
        ENUM_SERVICE_STATUS_PROCESSW* e = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
          const std::wstring nm = e[i].lpServiceName;
          add(nm);
          for (const Svc& s : out)
            if (s.wname == nm && std::find(knownNames_.begin(), knownNames_.end(), s.name) == knownNames_.end()) knownNames_.push_back(s.name);
        }
      }
    }
    enumerated_ = true;
  }
  return out;
}

std::wstring OemProbeWin::FindGcuServiceExe() const {
  const std::wstring p = OemRoot() + L"UniwillService\\MyControlCenter\\GCUService.exe";
  return FileExists(p) ? p : L"";
}

bool OemProbeWin::ProcessRunning() {
  bool any = false;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) {
    do {
      const std::wstring n = LowerW(pe.szExeFile);
      if (n != L"gcuservice.exe" && n != L"gcubridge.exe") continue;
      HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
      if (!p) continue;
      wchar_t path[MAX_PATH * 2] = {0};
      DWORD len = MAX_PATH * 2;
      const bool ok = QueryFullProcessImageNameW(p, 0, path, &len) != 0;
      CloseHandle(p);
      if (ok && IsOemImage(path)) any = true;   // a same-named binary elsewhere is NOT the OEM service
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return any;
}

OemSvcState OemProbeWin::Probe() {
  const ULONGLONG now = GetTickCount64();
  if (cacheAt_ && now - cacheAt_ < 2000) return cache_;
  OemSvcState st;
  std::vector<Svc> svcs = Discover(true);
  bool svcRunning = false;
  for (const Svc& s : svcs) {
    st.services.push_back(s.name);
    if (s.state != SERVICE_STOPPED) svcRunning = true;
  }
  const bool procRunning = ProcessRunning();
  const std::wstring exe = FindGcuServiceExe();
  st.installed = !svcs.empty() || !exe.empty() || procRunning;
  st.running = svcRunning || procRunning;
  if (!exe.empty()) st.version = FileVersion(exe);
  cache_ = st;
  cacheAt_ = now;
  return st;
}

bool OemProbeWin::StartServices(std::string* why) {
  cacheAt_ = 0;
  std::vector<Svc> svcs = Discover(true);
  KV rec;
  if (!stateFile_.empty()) {
    std::string t;
    if (fs::ReadAll(stateFile_, &t, 1u << 16)) rec = KV::Parse(t);
  }
  ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm) {
    if (why) *why = "cannot open the service manager: " + Win32Text(GetLastError());
    return false;
  }
  if (svcs.empty()) {
    if (why) *why = "no OEM service is installed";
    return false;
  }
  bool ok = true;
  std::stable_sort(svcs.begin(), svcs.end(), [](const Svc& a, const Svc& b) { return a.name != "GCUBridge" && b.name == "GCUBridge"; });  // helpers first
  for (const Svc& s : svcs) {
    ScHandle svc(OpenServiceW(scm, s.wname.c_str(), SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG | SERVICE_START));
    if (!svc) {
      if (why) *why = "cannot open " + s.name + ": " + Win32Text(GetLastError());
      ok = false;
      continue;
    }
    DWORD start = static_cast<DWORD>(rec.Int("svc." + s.name, SERVICE_AUTO_START));
    if (start != SERVICE_AUTO_START && start != SERVICE_DEMAND_START && start != SERVICE_BOOT_START && start != SERVICE_SYSTEM_START) start = SERVICE_AUTO_START;
    if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
      if (why) *why = "cannot re-enable " + s.name + ": " + Win32Text(GetLastError());
      ok = false;
      continue;
    }
    if (!StartServiceW(svc, 0, nullptr)) {
      const DWORD e = GetLastError();
      if (e != ERROR_SERVICE_ALREADY_RUNNING) {
        if (why) *why = "cannot start " + s.name + ": " + Win32Text(e);
        ok = false;
        continue;
      }
    }
    if (!WaitState(svc, SERVICE_RUNNING, 20000)) {
      if (why) *why = s.name + " did not reach the running state";
      ok = false;
    } else {
      rec.Erase("svc." + s.name);
    }
  }
  if (!stateFile_.empty()) {
    if (rec.Size() == 0) fs::Remove(stateFile_);
    else fs::WriteAtomic(stateFile_, rec.Serialize());
  }
  cacheAt_ = 0;
  return ok;
}

}  // namespace hw
}  // namespace bc
