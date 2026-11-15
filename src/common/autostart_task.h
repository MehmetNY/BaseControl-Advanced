// "Start with Windows" as a Task Scheduler task. The program needs administrator rights, and Windows does not start elevated programs from
// the Run registry key, so the sign-in start is a task that runs with the highest available rights of the signed-in user (no prompt at
// sign-in). This file is the pure part - the task's XML, reading the answer of the scheduler, deciding whether the task is really ours and
// active - so it can be tested on any host; src/hw/autostart_win.cpp runs schtasks.exe.
#pragma once
#include <string>
#include <vector>

namespace bc {

inline constexpr char kAutostartTaskName[] = "ControlCenterAdvanced_Autostart";
inline constexpr char kAutostartArgs[] = "--tray";

// UTF-8 text of the task definition (the caller writes it as UTF-16 with a byte-order mark, which is what schtasks /XML wants).
std::string BuildAutostartXml(const std::string& exePath, const std::string& userId);

// What the scheduler reported about the task.
struct TaskObservation {
  bool known = false;     // the query reached a definite answer (task found, or "no such task"); false = scheduler not reachable
  bool exists = false;
  bool enabled = false;
  std::string command;    // the program the task starts
  std::string args;
};
// Reads the XML that `schtasks /Query /TN <name> /XML` prints.
TaskObservation ParseTaskXml(const std::string& xml);

// True only when the task exists, is enabled, starts exactly this program (case-insensitive, quotes ignored) with exactly the tray argument.
bool AutostartIsActive(const TaskObservation& o, const std::string& selfPath);

// True when `path` lies below one of `roots` (whole path components, either kind of slash, ignoring case). A path with a ".." part is never below
// anything, and a root shorter than "C:\\" is ignored. The sign-in task runs with administrator rights, so it may only point at a program in a
// folder that normal users cannot write to: the caller passes Program Files and the Windows folder as the roots.
bool PathIsBelowAny(const std::string& path, const std::vector<std::string>& roots);

}  // namespace bc
