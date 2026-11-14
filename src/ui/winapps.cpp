#include "winapps.h"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <map>

#include "../common/util.h"
#include "gfx.h"

namespace bc {
namespace ui {
namespace {

std::string Base(const std::string& path) {
  const size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? path : path.substr(p + 1);
}

// Shell and system hosts that always have a window: mapping them would switch the profile permanently.
bool Ignored(const std::string& exe) {
  static const char* kSkip[] = {"explorer.exe", "applicationframehost.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe",
                                "searchhost.exe", "textinputhost.exe", "lockapp.exe", "systemsettings.exe", "dwm.exe"};
  for (const char* s : kSkip)
    if (exe == s) return true;
  return false;
}

struct EnumState {
  std::map<std::string, RunningApp> found;
  std::map<std::string, std::string> paths;
  DWORD self = 0;
};

BOOL CALLBACK OnWindow(HWND h, LPARAM lp) {
  EnumState* st = reinterpret_cast<EnumState*>(lp);
  if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) return TRUE;
  if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;   // suspended store apps
  wchar_t title[256] = {};
  if (GetWindowTextW(h, title, 255) <= 0) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (!pid || pid == st->self) return TRUE;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return TRUE;
  wchar_t path[MAX_PATH * 2] = {};
  DWORD n = MAX_PATH * 2;
  const BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
  CloseHandle(p);
  if (!ok) return TRUE;
  const std::string full = Narrow(path);
  const std::string exe = Lower(Base(full));
  if (exe.empty() || Ignored(exe) || st->found.count(exe)) return TRUE;
  RunningApp a;
  a.exe = exe;
  a.title = Narrow(title);
  st->found[exe] = a;
  st->paths[exe] = full;
  return TRUE;
}

}  // namespace

std::string ProductNameOf(const std::string& utf8Path) {
  const std::wstring wp = Wide(utf8Path);
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(wp.c_str(), &ignored);
  if (!size) return std::string();
  std::vector<char> buf(size);
  if (!GetFileVersionInfoW(wp.c_str(), 0, size, buf.data())) return std::string();
  struct Translation {
    WORD lang, codepage;
  };
  Translation* tr = nullptr;
  UINT len = 0;
  std::vector<Translation> tries;
  if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &len) && tr)
    for (UINT i = 0; i < len / sizeof(Translation) && i < 4; i++) tries.push_back(tr[i]);
  tries.push_back({0x0409, 0x04B0});
  tries.push_back({0x0409, 0x04E4});
  for (const wchar_t* field : {L"ProductName", L"FileDescription"}) {
    for (const Translation& t : tries) {
      wchar_t q[96];
      swprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\%ls", t.lang, t.codepage, field);
      wchar_t* v = nullptr;
      UINT vl = 0;
      if (VerQueryValueW(buf.data(), q, reinterpret_cast<LPVOID*>(&v), &vl) && v && vl > 1) {
        std::string s = Trim(Narrow(std::wstring(v)));
        if (!s.empty() && s.size() <= 48) return s;
      }
    }
  }
  return std::string();
}

std::vector<RunningApp> ListRunningApps(bool simulated) {
  std::vector<RunningApp> out;
  if (simulated) {
    out = {{"blender.exe", "Blender", "Blender"},
           {"chrome.exe", "Google Chrome", "New Tab - Google Chrome"},
           {"cs2.exe", "Counter-Strike 2", "Counter-Strike 2"},
           {"cyberpunk2077.exe", "Cyberpunk 2077", "Cyberpunk 2077"},
           {"discord.exe", "Discord", "Discord"},
           {"obs64.exe", "OBS Studio", "OBS 31.0"}};
    return out;
  }
  EnumState st;
  st.self = GetCurrentProcessId();
  EnumWindows(OnWindow, reinterpret_cast<LPARAM>(&st));
  static std::map<std::string, std::string> productCache;   // path -> product name; reading version resources is slow
  for (auto& kv : st.found) {
    RunningApp a = kv.second;
    const std::string& path = st.paths[a.exe];
    auto it = productCache.find(path);
    if (it == productCache.end()) it = productCache.emplace(path, ProductNameOf(path)).first;
    a.product = it->second;
    out.push_back(a);
  }
  return out;
}

std::string BrowseForProgram(void* ownerWindow) {
  wchar_t file[MAX_PATH * 2] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = static_cast<HWND>(ownerWindow);
  static const wchar_t kFilter[] = L"Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
  ofn.lpstrFilter = kFilter;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH * 2;
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
  if (!GetOpenFileNameW(&ofn)) return std::string();
  return Narrow(std::wstring(file));
}

std::string NormalizeProgramName(const std::string& typed) {
  std::string e = Lower(Trim(Base(Trim(typed))));
  if (e.empty()) return e;
  if (e.find('.') == std::string::npos) e += ".exe";
  return e;
}

}  // namespace ui
}  // namespace bc
