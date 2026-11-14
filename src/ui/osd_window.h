// The real in-game overlay: a click-through, always-on-top, per-pixel-alpha window that never takes focus. It is drawn with the
// same painter as the preview on the Overlay page. Works with windowed and borderless games (a game in exclusive full screen
// covers any overlay window).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "../common/osdcfg.h"
#include "gfx.h"

namespace bc {
namespace ui {

class OsdWindow {
 public:
  OsdWindow() = default;
  ~OsdWindow();
  OsdWindow(const OsdWindow&) = delete;
  OsdWindow& operator=(const OsdWindow&) = delete;

  // Shows or refreshes the overlay; `visible` false hides it. `font` is the family the main window picked.
  void Update(const OsdConfig& cfg, const OsdData& data, bool visible, const std::wstring& font);
  bool Shown() const { return shown_; }

 private:
  bool Create();
  void EnsureDib(int w, int h);
  void Free();

  HWND hwnd_ = nullptr;
  Gfx gfx_;
  bool fontsReady_ = false, shown_ = false;
  HDC dc_ = nullptr;
  HBITMAP bmp_ = nullptr, old_ = nullptr;
  void* bits_ = nullptr;
  int bw_ = 0, bh_ = 0;
};

}  // namespace ui
}  // namespace bc
