// The Win32 shell: window class, message loop, input mapping, DPI, double-buffered GDI+ painting, tray icon.
#include <windowsx.h>

#include <algorithm>
#include <cstring>

#include "../common/keymap.h"
#include "../common/util.h"
#include "../common/version.h"
#include "app_internal.h"
#include "page.h"

namespace bc {
namespace ui {
namespace {
constexpr wchar_t kClass[] = L"BaseControlAdvancedMain";
constexpr int kBaseW = 1360, kBaseH = 860, kMinW = 980, kMinH = 660;

template <class Fn>
Fn Proc(HMODULE m, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

const wchar_t* kArrow = MAKEINTRESOURCEW(32512);
const wchar_t* kIBeam = MAKEINTRESOURCEW(32513);
const wchar_t* kHand = MAKEINTRESOURCEW(32649);

float DpiOf(HWND h) {
  typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
  static GetDpiForWindowFn fn = Proc<GetDpiForWindowFn>(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  if (fn && h) {
    const UINT d = fn(h);
    if (d) return static_cast<float>(d) / 96.0f;
  }
  HDC dc = GetDC(h);
  const int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc) ReleaseDC(h, dc);
  return static_cast<float>(d) / 96.0f;
}

void DarkMenus() {
  HMODULE ux = LoadLibraryW(L"uxtheme.dll");
  if (!ux) return;
  typedef int(WINAPI * SetPreferredAppModeFn)(int);
  typedef void(WINAPI * FlushMenuThemesFn)();
  SetPreferredAppModeFn setMode = Proc<SetPreferredAppModeFn>(ux, MAKEINTRESOURCEA(135));
  FlushMenuThemesFn flush = Proc<FlushMenuThemesFn>(ux, MAKEINTRESOURCEA(136));
  if (setMode) setMode(2);  // ForceDark
  if (flush) flush();
}
}  // namespace

// ------------------------------------------------------------------------------------------------ window creation
bool App::Impl::CreateMainWindow(int show) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = &Impl::WndProc;
  wc.hInstance = hinst;
  wc.hCursor = LoadCursorW(nullptr, kArrow);
  wc.hIcon = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
  wc.hIconSm = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_SHARED));
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClass;
  if (!RegisterClassExW(&wc)) return false;

  DarkMenus();
  taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  const int w = static_cast<int>(app->Pref("win.w", "0").empty() ? 0 : std::atoi(app->Pref("win.w", "0").c_str()));
  const int h = static_cast<int>(app->Pref("win.h", "0").empty() ? 0 : std::atoi(app->Pref("win.h", "0").c_str()));
  const float sysDpi = DpiOf(nullptr);
  int cw = w >= kMinW ? w : kBaseW, ch = h >= kMinH ? h : kBaseH;
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  cw = std::min(cw, static_cast<int>((work.right - work.left) / sysDpi) - 40);
  ch = std::min(ch, static_cast<int>((work.bottom - work.top) / sysDpi) - 40);
  cw = std::max(cw, kMinW);
  ch = std::max(ch, kMinH);
  RECT rc = {0, 0, static_cast<LONG>(cw * sysDpi), static_cast<LONG>(ch * sysDpi)};
  const DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&rc, style, FALSE);
  const std::wstring title = L"BaseControl Advanced";
  hwnd = CreateWindowExW(0, kClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, this);
  if (!hwnd) return false;
  dpi = DpiOf(hwnd);
  ApplyDwm();
  hideOnClose = app->PrefB("tray", true);
  if (app->PrefB("tray.icon", true) || app->opt_.tray) AddTray();
  app->ApplySession();   // keyboard hook, shortcuts and overlay as the preferences ask
  // display on/off and charger changes reach the in-process controller (local mode); the service gets its own from Windows
  powerNotify[0] = RegisterPowerSettingNotification(hwnd, &hw::kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
  powerNotify[1] = RegisterPowerSettingNotification(hwnd, &hw::kGuidAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
  SetTimer(hwnd, kTimerTick, 100, nullptr);
  if (app->opt_.tray && trayAdded) {
    windowVisible = false;
    ShowWindow(hwnd, SW_HIDE);
  } else {
    ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
    UpdateWindow(hwnd);
  }
  return true;
}

void App::Impl::ApplyDwm() {
  HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
  if (!dwm) return;
  typedef HRESULT(WINAPI * SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
  SetAttrFn set = Proc<SetAttrFn>(dwm, "DwmSetWindowAttribute");
  if (set) {
    const BOOL dark = TRUE;
    set(hwnd, 20, &dark, sizeof dark);   // DWMWA_USE_IMMERSIVE_DARK_MODE (19 on early Windows 10 builds)
    set(hwnd, 19, &dark, sizeof dark);
    const COLORREF cap = RGB(0x19, 0x1A, 0x19), text = RGB(0xF1, 0xEE, 0xE8), border = RGB(0x34, 0x35, 0x32);
    set(hwnd, 35, &cap, sizeof cap);      // DWMWA_CAPTION_COLOR (Windows 11)
    set(hwnd, 36, &text, sizeof text);    // DWMWA_TEXT_COLOR
    set(hwnd, 34, &border, sizeof border);  // DWMWA_BORDER_COLOR
  }
  FreeLibrary(dwm);
}

// ------------------------------------------------------------------------------------------------ painting
void App::Impl::FreeDib() {
  if (memDc) {
    if (oldBmp) SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    memDc = nullptr;
  }
  if (dib) {
    DeleteObject(dib);
    dib = nullptr;
  }
  bits = nullptr;
  dibW = dibH = 0;
  oldBmp = nullptr;
}

void App::Impl::EnsureDib(int w, int h) {
  if (memDc && dibW == w && dibH == h) return;
  FreeDib();
  HDC screen = GetDC(hwnd);
  memDc = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(hwnd, screen);
  oldBmp = static_cast<HBITMAP>(SelectObject(memDc, dib));
  dibW = w;
  dibH = h;
}

void App::Impl::UpdateTimers(bool animating) {
  if (animating && windowVisible && !animTimer) {
    SetTimer(hwnd, kTimerAnim, 16, nullptr);
    animTimer = true;
  } else if ((!animating || !windowVisible) && animTimer) {
    KillTimer(hwnd, kTimerAnim);
    animTimer = false;
  }
}

void App::Impl::Paint() {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int w = std::max<LONG>(1, rc.right), h = std::max<LONG>(1, rc.bottom);
  EnsureDib(w, h);
  {
    Gdiplus::Graphics g(memDc);
    app->Frame(&g, dpi, Rect(0, 0, static_cast<float>(w) / dpi, static_cast<float>(h) / dpi));
  }
  BitBlt(dc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
  EndPaint(hwnd, &ps);
  UpdateTimers(app->ui_.Animating());
}

// ------------------------------------------------------------------------------------------------ tray
void App::Impl::AddTray() {
  if (trayAdded) return;
  std::memset(&nid, 0, sizeof nid);
  nid.cbSize = sizeof nid;
  nid.hWnd = hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = kMsgTray;
  nid.hIcon = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  wcsncpy(nid.szTip, L"BaseControl Advanced", ARRAYSIZE(nid.szTip) - 1);
  trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void App::Impl::RemoveTray() {
  if (trayAdded) {
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded = false;
  }
}

void App::Impl::UpdateTray() {
  if (!trayAdded) return;
  std::string tip = "BaseControl";
  const std::string prof = app->S("profile.active");
  if (!prof.empty()) tip += " - " + prof;
  if (app->Has("cpu.temp")) tip += "\nCPU " + Num(app->N("cpu.temp")) + " \xC2\xB0""C  " + Num(app->N("cpu.pkg.w")) + " W";
  if (app->Has("gpu.temp")) tip += "\nGPU " + Num(app->N("gpu.temp")) + " \xC2\xB0""C  " + Num(app->N("gpu.power")) + " W";
  std::wstring w = Wide(tip);
  if (w.size() >= ARRAYSIZE(nid.szTip)) w.resize(ARRAYSIZE(nid.szTip) - 1);
  if (w != nid.szTip) {
    wcsncpy(nid.szTip, w.c_str(), ARRAYSIZE(nid.szTip) - 1);
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  }
}

void App::Impl::SetVisible(bool v) {
  if (windowVisible == v) return;
  windowVisible = v;
  app->UpdateInterest();
  if (!v) app->CancelKeyCapture();
  SetTimer(hwnd, kTimerTick, v ? 100 : 1000, nullptr);
  if (v) InvalidateRect(hwnd, nullptr, FALSE);
}

void App::Impl::TrayMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1, Wide(T("Open BaseControl")).c_str());
  const std::string list = app->S("profile.list");
  std::vector<std::string> names;
  if (!list.empty()) names = Split(list, ',');
  if (!names.empty() && app->connected()) {
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const std::string active = app->S("profile.active");
    for (size_t i = 0; i < names.size() && i < 24; i++)
      AppendMenuW(m, MF_STRING | (names[i] == active ? MF_CHECKED : 0), static_cast<UINT_PTR>(100 + i), Wide(names[i]).c_str());
  }
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 2, Wide(T("Exit")).c_str());
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  if (cmd == 1) {
    SetVisible(true);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
  } else if (cmd == 2) {
    app->Quit();
  } else if (cmd >= 100 && static_cast<size_t>(cmd - 100) < names.size()) {
    KV a;
    a.Set("name", names[static_cast<size_t>(cmd - 100)]);
    app->Cmd("profile.apply", a, names[static_cast<size_t>(cmd - 100)]);
  }
}

// ------------------------------------------------------------------------------------------------ messages
LRESULT CALLBACK App::Impl::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Impl* self = nullptr;
  if (m == WM_NCCREATE) {
    self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) self->hwnd = h;
  } else {
    self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  }
  if (!self) return DefWindowProcW(h, m, w, l);
  return self->Handle(h, m, w, l);
}

LRESULT App::Impl::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
  Ui& ui = app->ui_;
  auto invalidate = [&] { InvalidateRect(h, nullptr, FALSE); };
  auto setMouse = [&](LPARAM lp) {
    ui.in.mouse = {static_cast<float>(GET_X_LPARAM(lp)) / dpi, static_cast<float>(GET_Y_LPARAM(lp)) / dpi};
    ui.in.inWindow = true;
  };
  if (taskbarCreated && m == taskbarCreated) {
    trayAdded = false;
    if (app->PrefB("tray.icon", true) || app->opt_.tray) AddTray();
    return 0;
  }
  switch (m) {
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      if (w == SIZE_MINIMIZED) {
        SetVisible(false);
      } else {
        SetVisible(true);
        invalidate();
      }
      return 0;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(l);
      RECT rc = {0, 0, static_cast<LONG>(kMinW * dpi), static_cast<LONG>(kMinH * dpi)};
      AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
      mm->ptMinTrackSize.x = rc.right - rc.left;
      mm->ptMinTrackSize.y = rc.bottom - rc.top;
      return 0;
    }
    case WM_DPICHANGED: {
      dpi = static_cast<float>(HIWORD(w)) / 96.0f;
      const RECT* r = reinterpret_cast<const RECT*>(l);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      invalidate();
      return 0;
    }
    case WM_EXITSIZEMOVE: {
      RECT rc;
      GetClientRect(h, &rc);
      if (!IsIconic(h) && !IsZoomed(h)) {
        app->SetPref("win.w", std::to_string(static_cast<int>(rc.right / dpi)));
        app->SetPref("win.h", std::to_string(static_cast<int>(rc.bottom / dpi)));
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      setMouse(l);
      if (!trackLeave) {
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        trackLeave = true;
      }
      invalidate();
      return 0;
    case WM_MOUSELEAVE:
      trackLeave = false;
      ui.in.inWindow = false;
      invalidate();
      return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
      setMouse(l);
      SetCapture(h);
      SetFocus(h);
      ui.in.down = true;
      ui.in.pressed = true;
      if (m == WM_LBUTTONDBLCLK) ui.in.dbl = true;
      invalidate();
      return 0;
    case WM_LBUTTONUP:
      setMouse(l);
      ui.in.down = false;
      ui.in.released = true;
      if (GetCapture() == h) ReleaseCapture();
      invalidate();
      return 0;
    case WM_RBUTTONDOWN:
      setMouse(l);
      ui.in.rpressed = true;
      invalidate();
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE) app->CancelKeyCapture();   // a key pressed in another program is not meant for the Keys page
      break;
    case kMsgAction:
      if (l == 1) {          // a key was captured for the Keys page
        app->dirty_ = true;
        invalidate();
      } else if (static_cast<size_t>(w) < KeyActions().size()) {
        app->RunAction(KeyActions()[static_cast<size_t>(w)].id);
        invalidate();
      }
      return 0;
    case WM_HOTKEY:
      if (w == static_cast<WPARAM>(Session::kHotkeyId)) app->RunAction("osd_toggle");
      return 0;
    case WM_INPUT:
      if (session) session->OnRawInput(reinterpret_cast<HRAWINPUT>(l));
      break;   // the system needs DefWindowProc for its own cleanup of the input record
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
      if (ui.in.down) {
        ui.in.down = false;
        ui.in.released = true;
      }
      invalidate();
      return 0;
    case WM_MOUSEWHEEL:
      ui.in.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / 120.0f;
      invalidate();
      return 0;
    case WM_CHAR:
      if (w >= 32 && w != 127) ui.in.chars.push_back(static_cast<wchar_t>(w));
      invalidate();
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      KeyEvent k;
      k.vk = static_cast<int>(w);
      k.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      k.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      k.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
      ui.in.keys.push_back(k);
      invalidate();
      if (m == WM_SYSKEYDOWN && w != VK_F4 && w != VK_MENU) return 0;
      break;
    }
    case WM_POWERBROADCAST: {
      // The in-process controller needs these events: in the elevated "local" mode it does everything, in service mode it is the user-session
      // half that puts the display colours back after sleep (the service handles the hardware side itself).
      Controller* target = ctl ? ctl.get() : userCtl.get();
      if (target) {
        if (w == PBT_APMSUSPEND) {
          target->Notify("suspend");
        } else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
          target->Notify("resume");
        } else if (w == PBT_POWERSETTINGCHANGE && l) {
          const POWERBROADCAST_SETTING* p = reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
          DWORD v = 0;
          if (p->DataLength >= 4) std::memcpy(&v, p->Data, 4);
          if (IsEqualGUID(p->PowerSetting, hw::kGuidConsoleDisplayState)) {
            if (v == 1) target->Notify("display_on");
            else if (v == 0) target->Notify("display_off");
          } else if (IsEqualGUID(p->PowerSetting, hw::kGuidAcDcPowerSource)) {
            target->Notify("ac_change");
          }
        }
      }
      return TRUE;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, ui.WantIBeam() ? kIBeam : (ui.WantHand() ? kHand : kArrow)));
        return TRUE;
      }
      break;
    case WM_TIMER:
      if (w == kTimerTick) {
        app->Tick();
        app->SessionTick();
        static double lastTray = 0;
        const double now = NowSeconds();
        if (trayAdded && now - lastTray > 3) {
          lastTray = now;
          UpdateTray();
        }
        if (app->dirty_ && windowVisible) invalidate();
        if (!backendStarted) {
          backendStarted = true;
          PostMessageW(h, WM_APP + 2, 0, 0);
        }
      } else if (w == kTimerAnim) {
        invalidate();
      }
      return 0;
    case WM_APP + 2:   // first paint is on screen: now the (possibly slow) hardware start
      if (!app->backend_) {
        app->MakeBackend();
        app->dirty_ = true;
        invalidate();
      }
      return 0;
    case kMsgTray:
      if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == NIN_SELECT) {
        if (!windowVisible || IsIconic(h)) {
          SetVisible(true);
          ShowWindow(h, SW_RESTORE);
          SetForegroundWindow(h);
        } else {
          SetForegroundWindow(h);
        }
      } else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
        TrayMenu();
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((w & 0xFFF0) == SC_MINIMIZE && trayAdded && app->PrefB("tray.min", false)) {
        SetVisible(false);
        ShowWindow(h, SW_HIDE);
        return 0;
      }
      break;
    case WM_CLOSE:
      if (!quitting && trayAdded && hideOnClose) {
        SetVisible(false);
        ShowWindow(h, SW_HIDE);
        return 0;
      }
      break;
    case WM_DESTROY:
      for (HPOWERNOTIFY& pn : powerNotify)
        if (pn) {
          UnregisterPowerSettingNotification(pn);
          pn = nullptr;
        }
      KillTimer(h, kTimerTick);
      KillTimer(h, kTimerAnim);
      app->FlushPrefs();
      session.reset();   // releases the keyboard hook and the shortcut
      osd.reset();
      RemoveTray();
      PostQuitMessage(0);
      return 0;
    default: break;
  }
  return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------------------------------------ entry point
int App::RunWindow(HINSTANCE hi, int show) {
  d_->hinst = hi;
  if (!d_->CreateMainWindow(show)) return 1;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

}  // namespace ui
}  // namespace bc
