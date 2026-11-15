// Windows helpers shared by the hardware layer (namespace bc::hw). Windows only.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../mgp/core/interfaces.h"

namespace bc {
namespace hw {

std::wstring W(const std::string& utf8);
std::string U(const std::wstring& wide);

std::string Win32Text(DWORD err);              // "Access is denied (5)" - short, English, no user data
std::wstring KnownFolder(int csidl);           // CSIDL path or empty
std::wstring ProgramFiles();
std::vector<std::string> ProtectedProgramRoots();   // UTF-8: Program Files (both kinds) and the Windows folder - normal users cannot write there
std::wstring SystemDirectory();
std::string ProgramDataDir();                  // UTF-8 "%ProgramData%\ControlCenterAdvanced" (created on demand)
std::string LocalDataDir();                    // UTF-8 "%LOCALAPPDATA%\ControlCenterAdvanced" (created on demand)
std::wstring SelfPath();
std::wstring ExeDir();

bool IsElevated();
bool IsLocalSystem();
bool FileExists(const std::wstring& path);
std::string FileVersion(const std::wstring& path);  // "1.0.2.70" or ""

// Rolling file log: <dir>/<name>.log, rotated at 512 KiB, two generations. Never writes user names or paths
// below the user profile.
class FileLog : public mgp::ILog {
 public:
  FileLog(const std::string& dir, const std::string& name);
  ~FileLog() override;
  void Write(mgp::LogLevel level, const std::string& message) override;
  std::string Path() const { return path_; }

 private:
  std::string path_;
  CRITICAL_SECTION cs_;
};

class SystemClock : public mgp::IClock {
 public:
  uint64_t NowMs() override { return GetTickCount64(); }
  void SleepMs(uint32_t ms) override { ::Sleep(ms); }
};

}  // namespace hw
}  // namespace bc
