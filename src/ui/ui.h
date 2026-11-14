// Immediate-mode widget toolkit on top of Gfx. One Ui instance per window. Widgets draw and handle input in the same call;
// state that must survive between frames (hover animation, focus, scroll offsets, open popups) lives in small maps keyed
// by a hash of the widget id string. Coordinates are logical pixels; scroll areas translate the origin.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"

namespace bc {
namespace ui {

struct KeyEvent {
  int vk = 0;
  bool ctrl = false, shift = false, alt = false;
};

struct Input {
  V2 mouse{-100, -100};
  bool inWindow = false;
  bool down = false, pressed = false, released = false, rpressed = false, dbl = false;
  float wheel = 0;  // notches, + = away from the user (scroll up)
  std::vector<wchar_t> chars;
  std::vector<KeyEvent> keys;
};

enum class Btn { Normal, Primary, Ghost, Danger };
enum class ToastKind { Info, Ok, Warn, Error };

struct ModalButton {
  std::string label;
  Btn style = Btn::Normal;
  int id = 0;
};

struct ModalSpec {
  std::string title, body;
  std::vector<ModalButton> buttons;
  IconId icon = IconId::Info;
  uint32_t iconColor = 0;  // 0 = accent
};

class Ui {
 public:
  Gfx gfx;
  Theme th;
  Input in;
  double now = 0;
  float dt = 0.016f;
  Rect window;
  // Test support (--script): remember where every clickable widget was drawn, in window coordinates.
  bool recordHits = false;
  std::map<std::string, Rect> hits;

  void Begin(double nowSec, const Rect& windowRect);
  void End();                                  // overlays (popup, modal, tooltip, toasts) + clears transient input
  bool Animating() const { return animating_; }
  bool WantHand() const { return wantHand_; }
  bool WantIBeam() const { return ibeam_; }
  bool TextFocused() const { return textFocus_ != 0; }
  void ClearFocus() { textFocus_ = 0; focus_ = 0; }

  // ---- behaviour primitives
  static uint32_t Hash(const std::string& s);
  bool Hover(const Rect& r) const;
  // The mouse in the coordinates widgets are drawn in right now (scroll offsets and Offset() applied), for dragging on custom surfaces.
  V2 LocalMouse() const { return V2{in.mouse.x - origin_.x, in.mouse.y - origin_.y}; }
  bool Focused(const std::string& id) const { return focus_ == Hash(id); }   // the widget was the last one pressed
  bool Click(const std::string& id, const Rect& r, bool enabled = true, bool* hover = nullptr, bool* held = nullptr);
  float Anim(const std::string& id, float target, float speed = 12.0f);
  bool Dragging(const std::string& id) const { return active_ == Hash(id) && in.down; }

  // ---- widgets
  bool Button(const std::string& id, const Rect& r, const std::string& label, Btn style = Btn::Normal, IconId icon = IconId::None, bool enabled = true);
  bool IconButton(const std::string& id, const Rect& r, IconId icon, bool enabled = true, uint32_t color = 0);
  bool Switch(const std::string& id, const Rect& r, bool on, bool enabled = true);   // true = the user clicked it (caller flips the value)
  int Segmented(const std::string& id, const Rect& r, const std::vector<std::string>& items, int cur, bool enabled = true);  // clicked index or -1
  int Combo(const std::string& id, const Rect& r, const std::vector<std::string>& items, int cur, bool enabled = true);      // chosen index or -1
  bool TextInput(const std::string& id, const Rect& r, std::string* text, const std::string& hint = "", size_t maxLen = 48);   // true = Enter pressed
  // `live` is what the hardware reports right now (drawn as a marker); *v is the user's draft. Returns true when *v changed.
  bool Slider(const std::string& id, const Rect& r, int vmin, int vmax, int step, int* v, int live, int softMax = -1, bool enabled = true, uint32_t col = 0);
  void Chip(const Rect& r, const std::string& text, uint32_t fg, uint32_t bg = 0);
  float ChipW(const std::string& text);
  void Panel(const Rect& r, uint32_t fill = 0, uint32_t border = 0, float radius = -1);
  void Spinner(const Rect& r, uint32_t col = 0);
  void Bar(const Rect& r, float frac, uint32_t col, float radius = 3);

  // ---- containers
  void BeginScroll(const std::string& id, const Rect& view);
  float EndScroll(float contentHeight);   // returns the current scroll offset

  // ---- feedback
  void Tip(const Rect& r, const std::string& text);
  void Toast(const std::string& text, ToastKind kind = ToastKind::Info, const std::string& detail = "");
  void ModalOpen(const ModalSpec& spec, std::function<void(int)> done);
  bool ModalOpen() const { return modal_.open; }
  void RequestFrame() { animating_ = true; }
  // Shifts the drawing/hit-test origin until the enclosing scroll area (or the frame) ends.
  void Offset(float dx, float dy) {
    origin_.x += dx;
    origin_.y += dy;
    gfx.Translate(dx, dy);
  }

 private:
  struct SState { float value = 0; bool seen = false; };
  struct ScrollSt { float y = 0, contentH = 0, viewH = 0; };
  struct ToastSt { std::string text, detail; ToastKind kind; double born, expire; };
  struct ModalSt { bool open = false; ModalSpec spec; std::function<void(int)> done; double born = 0; };
  struct ComboSt { bool open = false; uint32_t id = 0; Rect anchor; std::vector<std::string> items; int cur = -1; float scroll = 0; };

  void DrawComboPopup();
  void DrawModal();
  void DrawToasts();
  void DrawTooltip();
  void BtnColors(Btn style, bool enabled, float hov, bool held, uint32_t* bg, uint32_t* fg, uint32_t* border);

  bool animating_ = false, wantHand_ = false, ibeam_ = false, tipHovered_ = false, textClaimed_ = false, selAll_ = false;
  uint32_t active_ = 0, focus_ = 0, textFocus_ = 0;
  int layer_ = 0, topLayer_ = 0;
  V2 origin_{0, 0};
  Rect clip_;
  std::unordered_map<uint32_t, SState> anim_;
  std::unordered_map<uint32_t, ScrollSt> scroll_;
  std::unordered_map<uint32_t, int> comboResult_;
  std::vector<ToastSt> toasts_;
  ModalSt modal_;
  ComboSt combo_;
  uint32_t curScroll_ = 0;
  Rect scrollView_;
  V2 savedOrigin_{0, 0};
  Rect savedClip_;
  // tooltip dwell
  uint32_t tipKey_ = 0;
  double tipSince_ = 0;
  std::string tipText_;
  V2 tipPos_;
  // text caret
  double caretT0_ = 0;
  std::string textDraft_;
};

}  // namespace ui
}  // namespace bc
