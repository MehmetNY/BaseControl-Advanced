// Helpers of the Application profiles page that need the signed-in user's desktop: which programs have a window open, what a
// program calls itself, and a "pick a program" dialog. They run in the UI process on purpose: the service lives in session 0
// and cannot see the user's windows.
#pragma once
#include <string>
#include <vector>

namespace bc {
namespace ui {

struct RunningApp {
  std::string exe;       // lower-case image name, e.g. "chrome.exe"
  std::string product;   // what the program calls itself ("Google Chrome"), may be empty
  std::string title;     // title of its main window, may be empty
};

// Programs of the signed-in user that have a visible top-level window, one entry per image name, sorted by name.
// `simulated` returns a fixed sample list (screenshots, tests).
std::vector<RunningApp> ListRunningApps(bool simulated);

// Product name from the file's version resource ("" when it has none or the file cannot be read).
std::string ProductNameOf(const std::string& utf8Path);

// Windows "open file" dialog limited to programs. Returns the chosen path as UTF-8, "" when cancelled.
std::string BrowseForProgram(void* ownerWindow);

// "C:\Games\Cyberpunk 2077\Cyberpunk2077.exe" -> "cyberpunk2077.exe"; "Game" -> "game.exe". Same rules the service applies.
std::string NormalizeProgramName(const std::string& typed);

}  // namespace ui
}  // namespace bc
