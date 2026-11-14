#include "osd_window.h"

#include <algorithm>
#include <cmath>

#include "gdip.h"
#include "osd_paint.h"

namespace bc {
namespace ui {
namespace {
constexpr wchar_t kOsdClass[] = L"BaseControlAdvancedOverlay";

float SystemScale() {
  HDC dc = GetDC(nullptr);
  const int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc) ReleaseDC(nullptr, dc);
  return std::max(1.0f, static_cast<float>(d) / 96.0f);
}
}  // namespace

OsdWindow::~OsdWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
  Free();
}

void OsdWindow::Free() {
  if (dc_) {
    if (old_) SelectObject(dc_, old_);
    DeleteDC(dc_);
    dc_ = nullptr;
  }
  if (bmp_) {
    DeleteObject(bmp_);
    bmp_ = nullptr;
  }
  bits_ = nullptr;
  bw_ = bh_ = 0;
  old_ = nullptr;
}

bool OsdWindow::Create() {
  if (hwnd_) return true;
  HINSTANCE hi = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = hi;
  wc.lpszClassName = kOsdClass;
  RegisterClassExW(&wc);   // fails harmlessly when it already exists
  hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kOsdClass, L"BaseControl overlay", WS_POPUP, 0, 0, 10, 10,
                          nullptr, nullptr, hi, nullptr);
  return hwnd_ != nullptr;
}

void OsdWindow::EnsureDib(int w, int h) {
  if (dc_ && bw_ == w && bh_ == h) return;
  Free();
  HDC screen = GetDC(nullptr);
  dc_ = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
  ReleaseDC(nullptr, screen);
  old_ = static_cast<HBITMAP>(SelectObject(dc_, bmp_));
  bw_ = w;
  bh_ = h;
}

void OsdWindow::Update(const OsdConfig& cfg, const OsdData& data, bool visible, const std::wstring& font) {
  const std::vector<OsdLine> lines = OsdLines(cfg, data);
  if (!visible || lines.empty()) {
    if (hwnd_ && shown_) ShowWindow(hwnd_, SW_HIDE);
    shown_ = false;
    return;
  }
  if (!Create()) return;
  if (!fontsReady_) {
    gfx_.ChooseFonts(font);
    fontsReady_ = true;
  }
  const float scale = SystemScale();

  // measure with a scratch bitmap, then draw into the DIB the window shows
  V2 logical;
  {
    Gdiplus::Bitmap scratch(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&scratch);
    gfx_.Begin(&g, scale);
    logical = OsdMeasure(gfx_, cfg, lines);
    gfx_.End();
  }
  const int w = std::max(8, static_cast<int>(std::ceil(logical.x * scale))), h = std::max(8, static_cast<int>(std::ceil(logical.y * scale)));
  EnsureDib(w, h);
  if (!bits_) return;
  {
    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits_));
    Gdiplus::Graphics g(&bmp);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    gfx_.Begin(&g, scale);
    OsdPaint(gfx_, {0, 0}, cfg, lines);
    gfx_.End();
  }

  // the screen the game is on
  HWND fg = GetForegroundWindow();
  HMONITOR mon = MonitorFromWindow(fg ? fg : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(mon, &mi);
  const RECT& m = mi.rcMonitor;
  POINT pos;
  pos.x = (cfg.corner == 1 || cfg.corner == 3) ? m.right - cfg.mx - w : m.left + cfg.mx;
  pos.y = (cfg.corner == 2 || cfg.corner == 3) ? m.bottom - cfg.my - h : m.top + cfg.my;
  SIZE size = {w, h};
  POINT src = {0, 0};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(hwnd_, nullptr, &pos, &size, dc_, &src, 0, &bf, ULW_ALPHA);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  shown_ = true;
}

}  // namespace ui
}  // namespace bc
