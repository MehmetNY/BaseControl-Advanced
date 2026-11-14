// Input features that only make sense in the signed-in user's session, so they live in the UI process and not in the service:
//  * a low-level keyboard hook: Windows-key lock, Num Lock lock, key remapping and "press the key you want to pick",
//  * the global hotkey of the overlay,
//  * the corner double-tap of a precision touchpad (raw HID input).
// Nothing here talks to the firmware; the actions it triggers go back to the App (App::RunAction).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "../common/keymap.h"

namespace bc {
namespace ui {

struct SessionConfig {
  bool winLock = false, numLock = false, remap = false, touchpadCorner = false;
  std::vector<KeyBind> binds;
  unsigned hotMods = 0, hotVk = 0;   // global hotkey of the overlay (MOD_* / VK_*; 0 = none)
};

class Session {
 public:
  static constexpr int kHotkeyId = 0x4243;
  // The owner receives `msgAction` (WPARAM = index into KeyActions(), LPARAM 0) when a mapped key or the touchpad corner fires, and
  // LPARAM 1 when a key was captured for the Keys page. WM_INPUT / WM_HOTKEY are forwarded by the window procedure.
  Session(HWND owner, UINT msgAction);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void Configure(const SessionConfig& c);
  bool HookActive() const { return hook_ != nullptr; }
  bool TouchpadPresent();

  void BeginCapture();
  void EndCapture();
  bool Capturing() const { return capture_; }
  bool TakeCaptured(int* id);

  void OnRawInput(HRAWINPUT h);   // WM_INPUT

 private:
  struct Pad;
  static LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp);
  bool OnKey(const KBDLLHOOKSTRUCT& k, WPARAM wp);
  void UpdateHook();
  void UpdateRawInput();
  void Post(int actionIndex, LPARAM kind);
  Pad* PadFor(HANDLE dev);
  void Finger(int link, bool down, long x, long y, const Pad& pad);

  HWND owner_;
  UINT msg_;
  SessionConfig cfg_;
  HHOOK hook_ = nullptr;
  bool capture_ = false;
  int captured_ = 0;
  bool haveCaptured_ = false;
  std::set<int> held_;          // swallowed keys that are down right now (auto-repeat / key-up handling)
  bool hotkeyOn_ = false;
  bool rawOn_ = false;
  int touchpadCache_ = -1;
  std::map<HANDLE, std::unique_ptr<Pad>> pads_;
  // corner double-tap state
  struct Track {
    bool down = false, inCorner = false;
    DWORD t = 0;
    long sx = 0, sy = 0;
    bool moved = false;
  };
  std::map<int, Track> tracks_;
  DWORD lastTap_ = 0, lastFire_ = 0;
};

}  // namespace ui
}  // namespace bc
