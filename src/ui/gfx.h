// Drawing layer of the native UI: GDI+ behind a tiny immediate-mode API (logical pixels, DPI via one scale transform).
// Colours are 0xAARRGGBB. Everything the UI draws goes through this class, so the look (and the Wine/Xvfb screenshot
// path used for the README) has exactly one implementation.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gdip.h"

namespace bc {
namespace ui {

struct V2 {
  float x = 0, y = 0;
};

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  Rect() = default;
  Rect(float X, float Y, float W, float H) : x(X), y(Y), w(W), h(H) {}
  float r() const { return x + w; }
  float b() const { return y + h; }
  float cx() const { return x + w * 0.5f; }
  float cy() const { return y + h * 0.5f; }
  bool Has(V2 p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
  Rect Inset(float d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
  Rect Inset(float dx, float dy) const { return Rect(x + dx, y + dy, w - 2 * dx, h - 2 * dy); }
  Rect Top(float hh) const { return Rect(x, y, w, hh); }
  Rect Bottom(float hh) const { return Rect(x, y + h - hh, w, hh); }
  Rect Left(float ww) const { return Rect(x, y, ww, h); }
  Rect Right(float ww) const { return Rect(x + w - ww, y, ww, h); }
  Rect Offset(float dx, float dy) const { return Rect(x + dx, y + dy, w, h); }
  Rect Intersect(const Rect& o) const;
};

constexpr uint32_t Argb(int a, int r, int g, int b) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}
inline uint32_t WithAlpha(uint32_t c, int a) { return (c & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24); }
uint32_t Mix(uint32_t a, uint32_t b, float t);  // linear blend, alpha included

// Warm dark palette (see docs/DESIGN.md). Light surfaces are never pure black or pure white.
struct Theme {
  uint32_t base = 0xFF141515, rail = 0xFF191A19, panel = 0xFF1D1E1D, inset = 0xFF171817, elev = 0xFF252624, hover = 0xFF2B2D2A;
  uint32_t line = 0xFF343532, text = 0xFFF1EEE8, muted = 0xFFABA9A1, faint = 0xFF74736D;
  uint32_t accent = 0xFFE2A387, amber = 0xFFD8BA83, green = 0xFFADBD9A, blue = 0xFFA0B5C0, red = 0xFFD98A84;
  float radius = 13;
};

enum class FontId { Body, Small, Tiny, Label, Value, H3, H2, H1, Mega, Mono, Count };
enum class Align { Left, Center, Right };

enum class IconId {
  None, Overview, Settings, Performance, Profiles, Fan, Custom, Apps, Lighting, Battery, Display, Info, Shield, Bolt, Plug, Cpu, Gpu, Ram,
  Thermo, Wifi, Bluetooth, Camera, Touchpad, Keyboard, Check, Cross, Warn, Lock, ChevronR, ChevronD, Plus, Trash, Refresh, Undo, External,
  Copy, Power, Eye, Log, Gamepad, Chip, Sliders, Sun, Moon, Key, Folder, Download, Service
};

class Gfx {
 public:
  Gfx();
  ~Gfx();
  // Pick the first installed family of each list (call once after GDI+ started).
  void ChooseFonts(const std::wstring& forcedFamily = L"");
  std::wstring Family() const { return family_; }

  void Begin(Gdiplus::Graphics* g, float scale);
  void End();

  void PushClip(const Rect& r);   // intersects with the current clip
  void PopClip();
  void Translate(float dx, float dy);  // content-space origin (scroll areas)
  void Scale(float s);                 // uniform scale of the current transform (the overlay's text size); undo with Scale(1 / s)

  // ---- shapes
  void Fill(const Rect& r, uint32_t c);
  void FillRound(const Rect& r, float rad, uint32_t c);
  void StrokeRound(const Rect& r, float rad, uint32_t c, float w = 1.0f);
  void FillGrad(const Rect& r, float rad, uint32_t c1, uint32_t c2, float angleDeg);
  void Line(V2 a, V2 b, uint32_t c, float w = 1.0f);
  void Dot(V2 c, float rad, uint32_t col);
  void Circle(V2 c, float rad, uint32_t col, float w);
  void Arc(V2 c, float rad, float startDeg, float sweepDeg, uint32_t col, float w);  // 0 deg = east, clockwise
  void Poly(const std::vector<V2>& pts, uint32_t col, float w);
  void PolyFill(const std::vector<V2>& pts, uint32_t col);  // closed polygon

  // ---- text
  void Text(const Rect& box, const std::string& s, FontId f, uint32_t c, Align a = Align::Left, bool vcenter = true, bool ellipsis = true);
  // Wrapped text; returns the height used. `draw` false = measure only.
  float Wrapped(const Rect& box, const std::string& s, FontId f, uint32_t c, bool draw = true, float lineGap = 3.0f);
  // Like Wrapped, but never more than `maxLines` lines: the last visible line ends with an ellipsis. *truncated tells the caller
  // whether text was cut (so it can offer the full text as a tooltip).
  float WrappedClamped(const Rect& box, const std::string& s, FontId f, uint32_t c, int maxLines, float lineGap = 3.0f, bool* truncated = nullptr);
  float TextW(const std::string& s, FontId f);
  float LineH(FontId f) const;
  std::string Fit(const std::string& s, FontId f, float maxW);   // shortened with an ellipsis

  void Icon(IconId id, const Rect& box, uint32_t c, float stroke = 1.7f);

  float scale() const { return scale_; }
  Gdiplus::Graphics* g() { return g_; }

 private:
  Gdiplus::Font* Fnt(FontId f);
  Gdiplus::GraphicsPath* RoundPath(const Rect& r, float rad);
  std::vector<std::string> WrapLines(const std::string& s, FontId f, float maxW);

  Gdiplus::Graphics* g_ = nullptr;
  float scale_ = 1.0f;
  std::wstring family_;
  std::unique_ptr<Gdiplus::FontFamily> fam_, famSemi_, famMono_;
  std::unique_ptr<Gdiplus::Font> fonts_[static_cast<int>(FontId::Count)];
  std::vector<Rect> clips_;
  std::unordered_map<std::string, float> widthCache_;
  std::map<std::string, std::vector<std::string>> wrapCache_;
};

std::wstring Wide(const std::string& utf8);
std::string Narrow(const std::wstring& w);

}  // namespace ui
}  // namespace bc
