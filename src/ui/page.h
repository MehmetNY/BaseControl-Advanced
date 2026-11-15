// Page building blocks: a masonry layout, cards with a row cursor, and the composite rows every page uses
// (info line, switch, segmented enum, and the "live value + draft + Confirm" slider the Performance pages are built on).
#pragma once
#include <climits>
#include <functional>
#include <string>
#include <vector>

#include "app.h"

namespace bc {
namespace ui {

constexpr float kGap = 16.0f;
constexpr float kPad = 18.0f;
constexpr float kRowInfo = 30, kRowToggle = 58, kRowToggleLean = 44, kRowSlider = 108, kRowSeg = 86, kRowSegLean = 60, kRowButtons = 50;
constexpr float kRowPrefSlider = 62, kRowSwatch = 60, kRowCombo = 58;

struct Ctx {
  App& app;
  Ui& ui;
  const KV& s;
  float W;   // content width
  Ctx(App& a, float w) : app(a), ui(a.ui()), s(a.snap()), W(w) {}
};

class Masonry {
 public:
  Masonry(float width, float minCol = 430, int maxCols = 3, float gap = kGap);
  Rect Alloc(float h, bool wide = false);
  float Bottom() const;
  float colW() const { return colW_; }
  float InnerW(bool wide = false) const { return (wide ? width_ : colW_) - 2 * kPad; }
  int cols() const { return n_; }

 private:
  float width_, colW_, gap_;
  int n_;
  std::vector<float> y_;
};

struct SliderCfg {
  std::string key, title, desc;
  std::string liveKey;            // snapshot key holding the hardware value (default: key)
  int softMax = -1;               // above this the track turns amber (needs an explicit confirmation)
  int vmin = INT_MIN, vmax = INT_MAX;
  std::string unit;               // default: registry unit
  std::function<std::string(int)> validate;   // returns an error text when the draft must not be applied
  std::function<std::string(int)> fmt;        // value formatter (default: "<n> <unit>")
  bool chain = false;             // page handles Confirm itself (not used by the standard rows)
  std::string tag;                // label next to the reading instead of "LIVE" (e.g. "STORED" for a value the hardware is not using yet)
  int fallback = INT_MIN;         // reading to assume when the snapshot has none (a stored default)
  std::string lockWhy;            // non-empty: the value cannot be changed right now (this is the reason); the reading stays live, nothing can be drafted
};

class Card {
 public:
  Card(Ctx& c, Masonry& m, const std::string& title, const std::string& sub, IconId icon, float contentH, bool wide = false);
  Ctx& c;
  Rect box;
  float x0, w, y;
  float InnerW() const { return w; }

  Rect Row(float h) {
    Rect r(x0, y, w, h);
    y += h;
    return r;
  }
  void Gap(float h) { y += h; }
  void Divider();
  void Info(const std::string& label, const std::string& value, uint32_t col = 0, const std::string& tip = "");
  float NoteH(const std::string& text) const;
  void Note(const std::string& text, uint32_t col = 0);
  void SubHead(const std::string& text);
  bool Toggle(const std::string& id, const std::string& label, const std::string& desc, bool on, bool enabled = true, const std::string& tip = "");
  int Segment(const std::string& id, const std::string& label, const std::string& desc, const std::vector<std::string>& items, int cur, bool enabled = true);
  // Settings-bound rows (use the registry for range / capability / evidence and the snapshot for the live value):
  // warnOff / warnOn: when non-empty, switching the setting to that state first asks "Are you sure?" with this text.
  void BoolRow(const std::string& key, const std::string& label, const std::string& desc, const std::string& warnOff = "", const std::string& warnOn = "",
               const std::string& lockWhy = "");   // lockWhy non-empty: shown but cannot be switched (the reason is the tooltip)
  void EnumRow(const std::string& key, const std::string& label, const std::string& desc, const std::vector<std::string>& shown);
  void Slider(const SliderCfg& cfg);
  // Enum setting with a draft + Confirm button (same semantics as Slider; used where a value must not be sent on a click).
  void EnumConfirm(const std::string& key, const std::string& label, const std::string& desc, const std::vector<std::string>& shown);
  // ---- rows for the UI's own preferences (ui.kv): they never touch the hardware, so they apply immediately.
  // On/off row bound to a preference. Returns true when it was toggled (the new value is already stored).
  bool PrefRow(const std::string& key, const std::string& label, const std::string& desc, bool def, bool enabled = true, const std::string& tip = "");
  // Title + value chip + slider (kRowPrefSlider). Returns true when *v changed.
  bool PrefSlider(const std::string& id, const std::string& label, int* v, int vmin, int vmax, int step, const std::string& unit, bool enabled = true);
  // Row of round colour swatches (kRowSwatch). Returns the clicked index or -1.
  int Swatches(const std::string& id, const std::string& label, const std::vector<uint32_t>& cols, int cur, bool enabled = true);
  // Label on the left, drop-down on the right (kRowCombo). Returns the chosen index or -1.
  int ComboRow(const std::string& id, const std::string& label, const std::string& desc, const std::vector<std::string>& items, int cur, bool enabled = true,
               float comboW = 190);
  // Integer setting that is offered as a list of values (refresh rates): drop-down + Confirm, same draft semantics as EnumConfirm.
  void ListConfirm(const std::string& key, const std::string& label, const std::string& desc, const std::vector<int>& values, const std::string& unit = "");
  // One row: text field + button. Returns true when the button (or Enter) was pressed.
  bool InputRow(const std::string& id, std::string* text, const std::string& hint, const std::string& button, bool enabled = true, size_t maxLen = 32,
                bool selectOnClick = false);
  bool Button(const std::string& id, const std::string& label, Btn style = Btn::Normal, IconId icon = IconId::None, bool enabled = true, float width = 0);

  static float Head(const std::string& sub) { return sub.empty() ? 54.0f : 70.0f; }
};

// Buttons side by side in one row, wrapping to further rows when they do not fit the width.
struct BtnSpec {
  std::string id, label;
  Btn style = Btn::Normal;
  IconId icon = IconId::None;
  bool enabled = true;
  std::string tip;
};
int BtnRows(Ctx& c, float w, const std::vector<BtnSpec>& v);   // how many rows BtnFlow uses at this width (for sizing the card)
int BtnFlow(Card& k, const std::vector<BtnSpec>& v);           // draws them; returns the index of the one that was clicked, or -1

// Height a Note() of this text will take inside a card of the masonry's column width.
float NoteHeight(Ctx& c, const Masonry& m, const std::string& text, bool wide = false);
// Sum of card chrome around `contentH` (header + paddings), for pages that place cards by hand.
inline float CardH(float contentH, bool header = true) { return (header ? 64.0f : kPad) + contentH + kPad; }

// formatting helpers
std::string Num(double v, int dec = 0);
std::string UnitText(const std::string& unit);   // registry unit -> display text ("C" -> "\xC2\xB0""C")
std::string With(double v, int dec, const char* unit);        // "123.4 W"
std::string WithUnit(const std::string& num, const std::string& unit);   // number + unit; a percent sign follows the number in English ("76 %") and leads it in Turkish ("%76")
std::string Pct(double v, int dec = 0, bool tight = false);   // WithUnit(Num(v), "%"); tight drops the space in English ("76%")
std::string Watts(double v, int dec = 0);
uint32_t EvColor(const Theme& t, Ev e);
std::string EvLabel(Ev e);
std::string EvTip(Ev e);

// small widgets shared by pages
void Gauge(Ui& ui, const Rect& r, float frac, uint32_t col, const std::string& big, const std::string& small);
void LineChart(Ui& ui, const Rect& r, const std::vector<const std::deque<float>*>& series, const std::vector<uint32_t>& cols, float ymin, float ymax,
               const std::string& unit);
void StatusDot(Ui& ui, V2 c, uint32_t col, bool pulse);
void Sparkline(Ui& ui, const Rect& r, const std::deque<float>& s, float ymin, float ymax, uint32_t col);
// Stacks page sections: shifts the drawing origin down by `y` for the lifetime of the object.
struct Section {
  Ui& ui;
  float y;
  Section(Ui& u, float yy) : ui(u), y(yy) { ui.Offset(0, y); }
  ~Section() { ui.Offset(0, -y); }
  Section(const Section&) = delete;
  Section& operator=(const Section&) = delete;
};
// Draws `s` seconds of the history window as time labels (-120 s ... now) under a chart.
void TimeAxis(Ui& ui, const Rect& plot, int windowSeconds);

}  // namespace ui
}  // namespace bc
