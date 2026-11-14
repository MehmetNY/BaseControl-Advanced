#include "winutil.h"

#include <sddl.h>

#include <shlobj.h>
#include <winver.h>

#include <cstdio>

#include "../common/fsutil.h"
#include "../common/util.h"

namespace bc {
namespace hw {

std::wstring W(const std::string& utf8) {
  std::u16string u = Utf8ToUtf16(utf8);
  std::wstring w;
  w.reserve(u.size());
  for (char16_t c : u) w.push_back(static_cast<wchar_t>(c));
  return w;
}

std::string U(const std::wstring& wide) {
  std::u16string u;
  u.reserve(wide.size());
  for (wchar_t c : wide) u.push_back(static_cast<char16_t>(c));
  return Utf16ToUtf8(u);
}

std::string Win32Text(DWORD err) {
  wchar_t buf[256] = {0};
  DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                           buf, 255, nullptr);
  std::string s = n ? U(std::wstring(buf, n)) : std::string("error");
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s + Fmt(" (%lu)", static_cast<unsigned long>(err));
}

std::wstring KnownFolder(int csidl) {
  wchar_t p[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, p))) return L"";
  return p;
}

std::wstring ProgramFiles() {
  std::wstring p = KnownFolder(CSIDL_PROGRAM_FILES);
  return p.empty() ? L"C:\\Program Files" : p;
}

std::wstring SystemDirectory() {
  wchar_t p[MAX_PATH] = {0};
  UINT n = GetSystemDirectoryW(p, MAX_PATH);
  return (n > 0 && n < MAX_PATH) ? std::wstring(p, n) : L"C:\\Windows\\System32";
}

std::string ProgramDataDir() {
  std::wstring base = KnownFolder(CSIDL_COMMON_APPDATA);
  if (base.empty()) base = L"C:\\ProgramData";
  const std::string d = U(base) + "\\ControlCenterAdvanced";
  if (!fs::IsDir(d) && IsElevated()) {
    // the service reads its configuration from here: only SYSTEM and Administrators may write, everybody may read
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1, &sd, nullptr)) {
      SECURITY_ATTRIBUTES sa = {sizeof sa, sd, FALSE};
      CreateDirectoryW(W(d).c_str(), &sa);
      LocalFree(sd);
    }
  }
  fs::MakeDirs(d);
  return d;
}

std::string LocalDataDir() {
  std::wstring base = KnownFolder(CSIDL_LOCAL_APPDATA);
  if (base.empty()) return ProgramDataDir();
  const std::string d = U(base) + "\\ControlCenterAdvanced";
  fs::MakeDirs(d);
  return d;
}

std::wstring SelfPath() {
  wchar_t p[MAX_PATH * 2] = {0};
  DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
  return std::wstring(p, n);
}

std::wstring ExeDir() {
  std::wstring p = SelfPath();
  size_t k = p.find_last_of(L"\\/");
  return k == std::wstring::npos ? L"." : p.substr(0, k);
}

bool IsElevated() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION e = {0};
  DWORD n = 0;
  const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
  CloseHandle(tok);
  return ok;
}

bool IsLocalSystem() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  BYTE buf[256];
  DWORD n = 0;
  bool res = false;
  if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n)) {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID sys = nullptr;
    if (AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys)) {
      res = EqualSid(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, sys) != 0;
      FreeSid(sys);
    }
  }
  CloseHandle(tok);
  return res;
}

bool FileExists(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string FileVersion(const std::wstring& path) {
  DWORD h = 0;
  const DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &h);
  if (!sz) return "";
  std::vector<BYTE> buf(sz);
  if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return "";
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) || !fi || len < sizeof(VS_FIXEDFILEINFO)) return "";
  return Fmt("%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}

// ------------------------------------------------------------------------------------------------ log
FileLog::FileLog(const std::string& dir, const std::string& name) {
  InitializeCriticalSection(&cs_);
  fs::MakeDirs(dir);
  path_ = fs::Join(dir, name + ".log");
}

FileLog::~FileLog() { DeleteCriticalSection(&cs_); }

void FileLog::Write(mgp::LogLevel level, const std::string& message) {
  static const char* kName[] = {"DBG", "INF", "WRN", "ERR"};
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::string line = Fmt("%04d-%02d-%02d %02d:%02d:%02d.%03d %s ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, kName[static_cast<int>(level) & 3]);
  // one line per record; control characters are flattened so a log can never be forged by a crafted value
  for (char c : message) line.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
  line += "\r\n";
  EnterCriticalSection(&cs_);
  if (fs::FileSize(path_) > (512u << 10)) {
    fs::Remove(path_ + ".2");
    fs::Rename(path_ + ".1", path_ + ".2");
    fs::Rename(path_, path_ + ".1");
  }
  fs::Append(path_, line);
  LeaveCriticalSection(&cs_);
}

}  // namespace hw
}  // namespace bc
