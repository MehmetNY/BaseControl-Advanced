// The BaseControl Windows service ("BaseControl", LocalSystem): hosts the Controller (all privileged hardware access)
// and the named-pipe server the UI talks to. Also: install / uninstall / start / stop helpers used by the installer
// and by the UI's "set up the service" button (both run them elevated).
#pragma once
#include <string>

namespace bc {
namespace svc {

enum class State { NotInstalled, Stopped, Starting, Running, Stopping, Unknown };

State Query();                      // works without elevation
const char* StateName(State s);

int RunAsService();                 // SCM dispatcher; the return value is the process exit code
int RunInConsole();                 // same logic in the foreground (debugging); Ctrl+C stops it

// All of these need an elevated process. They return false + a short English reason.
// Install copies the running exe into %ProgramFiles%\BaseControl Advanced\ when it is not already inside a
// protected folder (a SYSTEM service must never run a binary a normal user could replace).
bool Install(std::string* why);
bool Uninstall(std::string* why);
bool StartNow(std::string* why);
bool StopNow(std::string* why);

}  // namespace svc
}  // namespace bc
