// Private state of the App (Win32 handles, backends, offscreen renderer). Shared by app*.cpp only.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <memory>
#include <string>

#include "../hw/power_guids.h"
#include "../hw/win_world.h"
#include "../hw/winutil.h"
#include "../sim/sim.h"
#include "../svc/pipe_client.h"
#include "../svc/service.h"
#include "app.h"
#include "osd_window.h"
#include "session.h"

namespace bc {
namespace ui {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgAction = WM_APP + 3;   // from the keyboard hook / touchpad: WPARAM = action index, LPARAM 1 = a key was captured
constexpr UINT_PTR kTimerTick = 1, kTimerAnim = 2;

struct App::Impl {
  ~Impl();
  App* app = nullptr;
  ULONG_PTR gdipToken = 0;
  bool gdipStarted = false;

  // ---- window / renderer
  HWND hwnd = nullptr;
  HINSTANCE hinst = nullptr;
  float dpi = 1.0f;
  int pxW = 0, pxH = 0;
  HDC memDc = nullptr;
  HBITMAP dib = nullptr, oldBmp = nullptr;
  void* bits = nullptr;
  int dibW = 0, dibH = 0;
  bool trackLeave = false, windowVisible = true, quitting = false, animTimer = false, backendStarted = false, hideOnClose = false;
  UINT taskbarCreated = 0;
  NOTIFYICONDATAW nid{};
  bool trayAdded = false;
  HPOWERNOTIFY powerNotify[2] = {nullptr, nullptr};
  double t0 = 0;

  // ---- session layer (app_session.cpp)
  std::unique_ptr<Session> session;
  std::unique_ptr<OsdWindow> osd;
  bool capturing = false;         // the Keys page waits for a key press
  int injectedKey = -1;           // a key "pressed" by a test script
  double captureT = 0;
  bool osdVisible = true;         // runtime show/hide of the overlay (the hotkey); "enabled" is the stored preference
  bool overlayWanted = false;     // the overlay is on screen: keep the data fresh even with the window hidden
  double lastOverlayT = 0;
  bool lightIdleSent = false;     // the controller was told the user is idle (and has not been told otherwise since)
  double lightIdleBeat = 0, lightIdleT = 0;
  void Balloon(const std::string& title, const std::string& text);

  // ---- elevated helper process (service install, ...)
  HANDLE elevProc = nullptr;
  std::string elevToast;

  // ---- data sources. Declaration order = reverse destruction order: controllers go before the hardware they use.
  std::unique_ptr<sim::World> simWorld;
  std::unique_ptr<hw::WinWorld> world, userWorld;
  std::unique_ptr<Controller> ctl, userCtl;
  std::unique_ptr<PipeBackend> pipe;

  // ---- window procedure helpers (app_window.cpp)
  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  LRESULT Handle(HWND h, UINT m, WPARAM w, LPARAM l);
  bool CreateMainWindow(int show);
  void EnsureDib(int w, int h);
  void Paint();
  void UpdateTimers(bool animating);
  void AddTray();
  void RemoveTray();
  void UpdateTray();
  void TrayMenu();
  void SetVisible(bool v);
  void ApplyDwm();
  void FreeDib();
};

double NowSeconds();
Page PageFromName(const std::string& name, bool* ok);
const char* PageKey(Page p);

}  // namespace ui
}  // namespace bc
