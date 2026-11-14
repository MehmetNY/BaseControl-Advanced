#include "ui.h"

#include <cmath>

namespace bc {
namespace ui {

namespace {
constexpr uint32_t kInk = 0xFF1B1A18;  // text on accent fills

std::string Utf8Of(wchar_t hi, wchar_t lo) {
  unsigned cp = hi;
  if (lo) cp = 0x10000 + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
  std::string s;
  if (cp < 0x80) s += static_cast<char>(cp);
  else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
  return s;
}

void PopUtf8(std::string* s) {
  if (s->empty()) return;
  size_t n = s->size() - 1;
  while (n > 0 && (static_cast<unsigned char>((*s)[n]) & 0xC0) == 0x80) n--;
  s->resize(n);
}

size_t CountUtf8(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
  return n;
}

std::string ClipboardText() {
  std::string out;
  if (!OpenClipboard(nullptr)) return out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
    if (w) {
      out = Narrow(w);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}
}  // namespace

uint32_t Ui::Hash(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h ? h : 1u;
}

void Ui::Begin(double nowSec, const Rect& windowRect) {
  dt = now > 0 ? static_cast<float>(std::min(0.1, std::max(0.001, nowSec - now))) : 0.016f;
  now = nowSec;
  window = windowRect;
  animating_ = false;
  wantHand_ = false;
  ibeam_ = false;
  layer_ = 0;
  topLayer_ = modal_.open ? 2 : (combo_.open ? 1 : 0);
  origin_ = {0, 0};
  clip_ = windowRect;
  tipText_.clear();
  tipHovered_ = false;
  if (recordHits) hits.clear();
}

bool Ui::Hover(const Rect& r) const {
  if (!in.inWindow || layer_ < topLayer_) return false;
  if (!clip_.Has(in.mouse)) return false;
  const V2 m{in.mouse.x - origin_.x, in.mouse.y - origin_.y};
  return r.Has(m);
}

bool Ui::Click(const std::string& id, const Rect& r, bool enabled, bool* hover, bool* held) {
  if (recordHits) hits[id] = Rect(r.x + origin_.x, r.y + origin_.y, r.w, r.h);
  const uint32_t h = Hash(id);
  const bool hov = enabled && Hover(r);
  if (hov) wantHand_ = true;
  bool clicked = false;
  if (hov && in.pressed) {
    active_ = h;
    focus_ = h;
  }
  if (active_ == h) {
    if (in.released) {
      clicked = hov;
      active_ = 0;
    } else if (!in.down) {
      active_ = 0;
    }
  }
  if (hover) *hover = hov;
  if (held) *held = (active_ == h) && in.down;
  return clicked;
}

float Ui::Anim(const std::string& id, float target, float speed) {
  SState& s = anim_[Hash(id)];
  if (!s.seen) {
    s.seen = true;
    s.value = target;
    return target;
  }
  const float d = target - s.value;
  if (std::fabs(d) < 0.003f) {
    s.value = target;
  } else {
    s.value += d * std::min(1.0f, dt * speed);
    animating_ = true;
  }
  return s.value;
}

// ------------------------------------------------------------------------------------------------ buttons
void Ui::BtnColors(Btn style, bool enabled, float hov, bool held, uint32_t* bg, uint32_t* fg, uint32_t* border) {
  *border = 0;
  if (!enabled) {
    *bg = style == Btn::Ghost ? 0 : th.inset;
    *fg = th.faint;
    *border = style == Btn::Ghost ? 0 : WithAlpha(th.line, 140);
    return;
  }
  switch (style) {
    case Btn::Primary:
      *bg = Mix(Mix(th.accent, 0xFFFFFFFF, 0.14f * hov), 0xFF000000, held ? 0.14f : 0.0f);
      *fg = kInk;
      break;
    case Btn::Ghost:
      *bg = WithAlpha(th.hover, static_cast<int>(255 * hov));
      *fg = Mix(th.muted, th.text, hov);
      break;
    case Btn::Danger:
      *bg = WithAlpha(th.red, static_cast<int>(26 + 40 * hov + (held ? 20 : 0)));
      *fg = th.red;
      *border = WithAlpha(th.red, 120);
      break;
    default:
      *bg = Mix(Mix(th.elev, th.hover, hov), 0xFF000000, held ? 0.2f : 0.0f);
      *fg = th.text;
      *border = th.line;
      break;
  }
}

bool Ui::Button(const std::string& id, const Rect& r, const std::string& label, Btn style, IconId icon, bool enabled) {
  bool hov = false, held = false;
  const bool clicked = Click(id, r, enabled, &hov, &held);
  const float ha = Anim(id + "#h", hov ? 1.0f : 0.0f, 14);
  uint32_t bg, fg, bd;
  BtnColors(style, enabled, ha, held, &bg, &fg, &bd);
  gfx.FillRound(r, 9, bg);
  if (bd) gfx.StrokeRound(r, 9, bd, 1);
  const float iw = icon != IconId::None ? 16.0f : 0.0f, gap = (icon != IconId::None && !label.empty()) ? 7.0f : 0.0f;
  const float tw = std::min(gfx.TextW(label, FontId::Label), r.w - 20 - iw - gap);
  float x = r.x + (r.w - (iw + gap + tw)) * 0.5f;
  if (icon != IconId::None) {
    gfx.Icon(icon, Rect(x, r.cy() - 8, 16, 16), fg, 1.8f);
    x += iw + gap;
  }
  gfx.Text(Rect(x, r.y, tw + 2, r.h), label, FontId::Label, fg, Align::Left);
  return clicked;
}

bool Ui::IconButton(const std::string& id, const Rect& r, IconId icon, bool enabled, uint32_t color) {
  bool hov = false, held = false;
  const bool clicked = Click(id, r, enabled, &hov, &held);
  const float ha = Anim(id + "#h", hov ? 1.0f : 0.0f, 14);
  gfx.FillRound(r, 9, WithAlpha(th.hover, static_cast<int>(255 * ha)));
  const uint32_t base = color ? color : th.muted;
  const uint32_t c = enabled ? Mix(base, th.text, ha * 0.8f) : th.faint;
  gfx.Icon(icon, Rect(r.cx() - 9, r.cy() - 9, 18, 18), c, 1.7f);
  return clicked;
}

bool Ui::Switch(const std::string& id, const Rect& r, bool on, bool enabled) {
  bool hov = false;
  const bool clicked = Click(id, r, enabled, &hov);
  const float t = Anim(id + "#t", on ? 1.0f : 0.0f, 16);
  uint32_t track = Mix(th.elev, th.accent, t);
  if (!enabled) track = Mix(track, th.base, 0.45f);
  gfx.FillRound(r, r.h * 0.5f, track);
  if (t < 0.99f) gfx.StrokeRound(r, r.h * 0.5f, WithAlpha(hov ? th.muted : th.line, static_cast<int>(255 * (1 - t))), 1);
  const float kr = r.h * 0.5f - 3.5f;
  const float kx = r.x + r.h * 0.5f + t * (r.w - r.h);
  uint32_t knob = Mix(th.muted, kInk, t);
  if (!enabled) knob = th.faint;
  gfx.Dot({kx, r.cy()}, kr, knob);
  return clicked;
}

int Ui::Segmented(const std::string& id, const Rect& r, const std::vector<std::string>& items, int cur, bool enabled) {
  gfx.FillRound(r, 10, th.inset);
  gfx.StrokeRound(r, 10, th.line, 1);
  const int n = static_cast<int>(items.size());
  if (n == 0) return -1;
  const float w = r.w / n;
  int res = -1;
  for (int i = 0; i < n; i++) {
    const Rect seg = Rect(r.x + i * w, r.y, w, r.h).Inset(3);
    bool hov = false;
    const bool c = Click(id + "#" + std::to_string(i), seg, enabled, &hov);
    const float sel = Anim(id + "#s" + std::to_string(i), i == cur ? 1.0f : 0.0f, 16);
    if (sel > 0.01f) {
      gfx.FillRound(seg, 8, WithAlpha(th.elev, static_cast<int>(255 * sel)));
      gfx.StrokeRound(seg, 8, WithAlpha(th.line, static_cast<int>(255 * sel)), 1);
    } else if (hov) {
      gfx.FillRound(seg, 8, WithAlpha(th.hover, 140));
    }
    uint32_t fg = Mix(th.muted, th.text, std::max(sel, hov ? 0.7f : 0.0f));
    if (!enabled) fg = th.faint;
    gfx.Text(seg.Inset(6, 0), items[static_cast<size_t>(i)], FontId::Label, fg, Align::Center);
    if (c && i != cur) res = i;
  }
  return res;
}

int Ui::Combo(const std::string& id, const Rect& r, const std::vector<std::string>& items, int cur, bool enabled) {
  const uint32_t h = Hash(id);
  int result = -1;
  auto it = comboResult_.find(h);
  if (it != comboResult_.end()) {
    result = it->second;
    comboResult_.erase(it);
  }
  bool hov = false;
  const bool clicked = Click(id, r, enabled, &hov);
  const bool open = combo_.open && combo_.id == h;
  if (clicked) {
    if (open) {
      combo_.open = false;
    } else {
      combo_.open = true;
      combo_.id = h;
      combo_.anchor = r.Offset(origin_.x, origin_.y);
      combo_.items = items;
      combo_.cur = cur;
      combo_.scroll = 0;
      animating_ = true;
    }
  }
  const float ha = Anim(id + "#h", hov || open ? 1.0f : 0.0f, 14);
  gfx.FillRound(r, 9, enabled ? Mix(th.inset, th.hover, ha * 0.5f) : th.inset);
  gfx.StrokeRound(r, 9, open ? th.accent : Mix(th.line, th.muted, ha * 0.4f), 1);
  const std::string label = cur >= 0 && cur < static_cast<int>(items.size()) ? items[static_cast<size_t>(cur)] : "";
  gfx.Text(Rect(r.x + 12, r.y, r.w - 38, r.h), label, FontId::Body, enabled ? th.text : th.faint);
  gfx.Icon(IconId::ChevronD, Rect(r.r() - 26, r.cy() - 7, 14, 14), enabled ? th.muted : th.faint, 1.9f);
  return result;
}

bool Ui::TextInput(const std::string& id, const Rect& r, std::string* text, const std::string& hint, size_t maxLen, bool selectOnClick) {
  const uint32_t h = Hash(id);
  bool hov = false;
  Click(id, r, true, &hov);
  if (hov && in.pressed) {
    textFocus_ = h;
    caretT0_ = now;
    selAll_ = selectOnClick && !text->empty();   // a short value the user will most likely replace: typing overwrites it
  }
  if (hov) wantHand_ = false, ibeam_ = true;
  if (hov && in.pressed) textClaimed_ = true;
  bool submitted = false;
  const bool focused = textFocus_ == h;
  if (focused) {
    for (size_t i = 0; i < in.chars.size(); i++) {
      wchar_t c = in.chars[i];
      if (c < 32 || c == 127) continue;
      wchar_t lo = 0;
      if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.chars.size()) lo = in.chars[++i];
      if (selAll_) { text->clear(); selAll_ = false; }
      if (CountUtf8(*text) < maxLen) *text += Utf8Of(c, lo);
      caretT0_ = now;
    }
    for (auto& k : in.keys) {
      if (k.vk == VK_BACK) { if (selAll_) { text->clear(); selAll_ = false; } else PopUtf8(text); k.vk = 0; caretT0_ = now; }
      else if (k.vk == VK_DELETE) { text->clear(); selAll_ = false; k.vk = 0; }
      else if (k.vk == VK_RETURN) { submitted = true; k.vk = 0; }
      else if (k.vk == VK_ESCAPE) { textFocus_ = 0; k.vk = 0; }
      else if (k.vk == 'A' && k.ctrl) { selAll_ = true; k.vk = 0; }
      else if (k.vk == 'V' && k.ctrl) {
        std::string clip = ClipboardText();
        std::string clean;
        for (char c : clip) if (static_cast<unsigned char>(c) >= 32 && c != 127) clean += c;
        if (selAll_) { text->clear(); selAll_ = false; }
        for (size_t p = 0; p < clean.size() && CountUtf8(*text) < maxLen; ) {
          size_t n = 1;
          while (p + n < clean.size() && (static_cast<unsigned char>(clean[p + n]) & 0xC0) == 0x80) n++;
          *text += clean.substr(p, n);
          p += n;
        }
        k.vk = 0;
      }
    }
    animating_ = true;  // caret blink
  }
  gfx.FillRound(r, 9, th.inset);
  gfx.StrokeRound(r, 9, focused ? th.accent : Mix(th.line, th.muted, hov ? 0.4f : 0.0f), 1);
  const Rect tr(r.x + 12, r.y, r.w - 24, r.h);
  if (text->empty() && !focused) {
    gfx.Text(tr, hint, FontId::Body, th.faint);
  } else {
    const std::string shown = *text;
    const float tw = gfx.TextW(shown, FontId::Body);
    const float off = std::max(0.0f, tw - tr.w + 4);
    gfx.PushClip(tr);
    if (focused && selAll_ && !shown.empty()) gfx.FillRound(Rect(tr.x - 2, tr.cy() - 10, tw + 4, 20), 4, WithAlpha(th.accent, 90));
    gfx.Text(Rect(tr.x - off, tr.y, tr.w + off + 200, tr.h), shown, FontId::Body, th.text, Align::Left, true, false);
    if (focused && std::fmod(now - caretT0_, 1.0) < 0.55) gfx.Line({tr.x - off + tw + 1, tr.cy() - 9}, {tr.x - off + tw + 1, tr.cy() + 9}, th.accent, 1.6f);
    gfx.PopClip();
  }
  return submitted;
}

bool Ui::Slider(const std::string& id, const Rect& r, int vmin, int vmax, int step, int* v, int live, int softMax, bool enabled, uint32_t col) {
  const uint32_t h = Hash(id);
  if (col == 0) col = th.accent;
  step = std::max(1, step);
  const float trackH = 6;
  const Rect track(r.x + 10, r.cy() - trackH * 0.5f, r.w - 20, trackH);
  const float span = static_cast<float>(std::max(1, vmax - vmin));
  auto posOf = [&](int val) { return track.x + track.w * static_cast<float>(std::min(std::max(val, vmin), vmax) - vmin) / span; };
  bool hov = false, held = false;
  Click(id, r, enabled, &hov, &held);
  int nv = *v;
  if (enabled && active_ == h && in.down) {
    float t = (in.mouse.x - origin_.x - track.x) / track.w;
    t = std::min(1.0f, std::max(0.0f, t));
    const int steps = static_cast<int>(std::lround(t * span / step));
    nv = std::min(vmax, std::max(vmin, vmin + steps * step));
  }
  if (enabled && focus_ == h) {
    for (auto& k : in.keys) {
      int d = 0;
      if (k.vk == VK_LEFT || k.vk == VK_DOWN) d = -step;
      else if (k.vk == VK_RIGHT || k.vk == VK_UP) d = step;
      else if (k.vk == VK_PRIOR) d = step * 10;
      else if (k.vk == VK_NEXT) d = -step * 10;
      else if (k.vk == VK_HOME) { nv = vmin; k.vk = 0; continue; }
      else if (k.vk == VK_END) { nv = vmax; k.vk = 0; continue; }
      if (d) { nv = std::min(vmax, std::max(vmin, nv + d)); k.vk = 0; }
    }
  }
  const bool changed = nv != *v;
  *v = nv;

  const float lx = posOf(live), dx = posOf(nv);
  gfx.FillRound(track, 3, enabled ? th.elev : Mix(th.elev, th.base, 0.4f));
  if (softMax >= 0 && softMax < vmax) {
    const float sx = posOf(softMax);
    gfx.FillRound(Rect(sx, track.y, track.r() - sx, trackH), 3, WithAlpha(th.amber, 62));
  }
  if (live >= vmin && lx > track.x) gfx.FillRound(Rect(track.x, track.y, lx - track.x, trackH), 3, Mix(th.elev, th.muted, enabled ? 0.55f : 0.25f));
  if (nv != live) {
    const bool over = softMax >= 0 && nv > softMax;
    const uint32_t seg = over ? th.amber : col;
    const float a = std::min(lx, dx), b = std::max(lx, dx);
    gfx.FillRound(Rect(a, track.y, std::max(2.0f, b - a), trackH), 3, WithAlpha(seg, 210));
  }
  if (live >= vmin) gfx.Line({lx, r.cy() - 8.5f}, {lx, r.cy() + 8.5f}, WithAlpha(th.text, enabled ? 210 : 90), 2.0f);
  const float hr = held ? 10.0f : (hov ? 9.5f : 8.5f);
  if (enabled && (hov || held)) gfx.Dot({dx, r.cy()}, hr + 4, WithAlpha(col, 46));
  gfx.Dot({dx, r.cy()}, hr, th.panel);
  gfx.Circle({dx, r.cy()}, hr - 1.25f, enabled ? col : th.faint, 2.5f);
  if (enabled && focus_ == h && !held) gfx.Circle({dx, r.cy()}, hr + 3.2f, WithAlpha(col, 120), 1.2f);
  return changed;
}

void Ui::Chip(const Rect& r, const std::string& text, uint32_t fg, uint32_t bg) {
  if (!bg) bg = WithAlpha(fg, 36);
  gfx.FillRound(r, r.h * 0.5f, bg);
  gfx.Text(r.Inset(6, 0), text, FontId::Label, fg, Align::Center);
}

float Ui::ChipW(const std::string& text) { return gfx.TextW(text, FontId::Label) + 18; }

void Ui::Panel(const Rect& r, uint32_t fill, uint32_t border, float radius) {
  if (radius < 0) radius = th.radius;
  gfx.FillRound(r, radius, fill ? fill : th.panel);
  gfx.StrokeRound(r, radius, border ? border : th.line, 1);
}

void Ui::Spinner(const Rect& r, uint32_t col) {
  const float ang = static_cast<float>(std::fmod(now * 420.0, 360.0));
  const float rad = std::min(r.w, r.h) * 0.5f - 1.5f;
  gfx.Arc({r.cx(), r.cy()}, rad, 0, 360, WithAlpha(col ? col : th.accent, 50), 2.2f);
  gfx.Arc({r.cx(), r.cy()}, rad, ang, 95, col ? col : th.accent, 2.2f);
  animating_ = true;
}

void Ui::Bar(const Rect& r, float frac, uint32_t col, float radius) {
  gfx.FillRound(r, radius, th.elev);
  frac = std::min(1.0f, std::max(0.0f, frac));
  if (frac > 0.001f) gfx.FillRound(Rect(r.x, r.y, std::max(r.h, r.w * frac), r.h), radius, col);
}

// ------------------------------------------------------------------------------------------------ scrolling
void Ui::BeginScroll(const std::string& id, const Rect& view) {
  curScroll_ = Hash(id);
  ScrollSt& s = scroll_[curScroll_];
  s.viewH = view.h;
  if (Hover(view) && in.wheel != 0) {
    s.y -= in.wheel * 70.0f;
    in.wheel = 0;
  }
  const float maxY = std::max(0.0f, s.contentH - view.h);
  s.y = std::min(maxY, std::max(0.0f, s.y));
  scrollView_ = view;
  savedOrigin_ = origin_;
  savedClip_ = clip_;
  clip_ = clip_.Intersect(view.Offset(origin_.x, origin_.y));
  origin_ = {origin_.x + view.x, origin_.y + view.y - s.y};
  gfx.PushClip(view);
  gfx.Translate(view.x, view.y - s.y);
}

float Ui::EndScroll(float contentHeight) {
  ScrollSt& s = scroll_[curScroll_];
  const float prevH = s.contentH;
  s.contentH = contentHeight;
  const float maxY = std::max(0.0f, contentHeight - s.viewH);
  if (s.y > maxY) s.y = maxY;
  if (std::fabs(prevH - contentHeight) > 0.5f) animating_ = true;  // layout changed: one more frame settles the scrollbar
  gfx.PopClip();
  origin_ = savedOrigin_;
  clip_ = savedClip_;
  if (maxY > 0.5f) {
    const Rect view = scrollView_;
    const Rect trackR(view.r() - 9, view.y + 3, 6, view.h - 6);
    const float thumbH = std::max(34.0f, trackR.h * view.h / contentHeight);
    float thumbY = trackR.y + (trackR.h - thumbH) * (s.y / maxY);
    Rect thumb(trackR.x - 3, thumbY, 12, thumbH);
    bool hov = false, held = false;
    Click("scrollbar#" + std::to_string(curScroll_), thumb, true, &hov, &held);
    if (held) {
      const float t = (in.mouse.y - origin_.y - trackR.y - thumbH * 0.5f) / std::max(1.0f, trackR.h - thumbH);
      s.y = std::min(maxY, std::max(0.0f, t * maxY));
      thumbY = trackR.y + (trackR.h - thumbH) * (s.y / maxY);
    }
    const float a = Anim("scrollbar#a" + std::to_string(curScroll_), (hov || held) ? 1.0f : 0.0f, 12);
    gfx.FillRound(Rect(trackR.x + 1.5f - a * 1.5f, thumbY, 3.0f + a * 3.0f, thumbH), 3, WithAlpha(th.muted, static_cast<int>(70 + 90 * a)));
  }
  return s.y;
}

// ------------------------------------------------------------------------------------------------ feedback
void Ui::Tip(const Rect& r, const std::string& text) {
  if (text.empty() || !Hover(r)) return;
  tipHovered_ = true;
  const uint32_t key = Hash(text) ^ static_cast<uint32_t>(static_cast<int>(r.x) * 7 + static_cast<int>(r.y) * 13);
  if (key != tipKey_) {
    tipKey_ = key;
    tipSince_ = now;
  }
  if (now - tipSince_ > 0.45) {
    tipText_ = text;
    tipPos_ = in.mouse;
  } else {
    animating_ = true;
  }
}

void Ui::Toast(const std::string& text, ToastKind kind, const std::string& detail) {
  ToastSt t;
  t.text = text;
  t.detail = detail;
  t.kind = kind;
  t.born = now;
  t.expire = now + (kind == ToastKind::Error ? 9.0 : kind == ToastKind::Warn ? 6.5 : 4.0);
  toasts_.push_back(t);
  if (toasts_.size() > 5) toasts_.erase(toasts_.begin());
  animating_ = true;
}

void Ui::ModalOpen(const ModalSpec& spec, std::function<void(int)> done) {
  modal_.open = true;
  modal_.spec = spec;
  modal_.done = std::move(done);
  modal_.born = now;
  animating_ = true;
}

void Ui::DrawComboPopup() {
  const float rowH = 30;
  const int n = static_cast<int>(combo_.items.size());
  const int vis = std::min(n, 8);
  const float H = vis * rowH + 8;
  Rect a = combo_.anchor;
  float y = a.b() + 4;
  if (y + H > window.b() - 8) y = a.y - 4 - H;
  const Rect pop(a.x, y, std::max(a.w, 160.0f), H);
  gfx.FillRound(pop.Offset(0, 3).Inset(-1), 12, WithAlpha(0xFF000000, 60));
  gfx.FillRound(pop, 11, th.elev);
  gfx.StrokeRound(pop, 11, th.line, 1);
  bool inside = pop.Has(in.mouse) || a.Has(in.mouse);
  if (n > 8 && pop.Has(in.mouse) && in.wheel != 0) {
    combo_.scroll = std::min(static_cast<float>(n - 8), std::max(0.0f, combo_.scroll - in.wheel));
    in.wheel = 0;
  }
  const int first = static_cast<int>(combo_.scroll);
  gfx.PushClip(pop.Inset(0, 4));
  for (int i = 0; i < vis && first + i < n; i++) {
    const int idx = first + i;
    const Rect row(pop.x + 4, pop.y + 4 + i * rowH, pop.w - 8, rowH);
    if (recordHits) hits["opt#" + std::to_string(idx)] = row;   // lets the script driver pick an entry
    const bool hov = Hover(row);
    if (hov) wantHand_ = true;
    if (hov || idx == combo_.cur) gfx.FillRound(row, 7, hov ? th.hover : WithAlpha(th.accent, 34));
    gfx.Text(row.Inset(10, 0), combo_.items[static_cast<size_t>(idx)], FontId::Body, idx == combo_.cur ? th.accent : th.text);
    if (hov && in.pressed) {
      comboResult_[combo_.id] = idx;
      combo_.open = false;
      animating_ = true;
      in.pressed = false;
      break;
    }
  }
  gfx.PopClip();
  if (in.pressed && !inside) combo_.open = false;
  for (auto& k : in.keys)
    if (k.vk == VK_ESCAPE) {
      combo_.open = false;
      k.vk = 0;
    }
}

void Ui::DrawModal() {
  const float age = static_cast<float>(std::min(1.0, (now - modal_.born) / 0.14));
  if (age < 1.0f) animating_ = true;
  gfx.Fill(window, WithAlpha(0xFF000000, static_cast<int>(170 * age)));
  const ModalSpec& sp = modal_.spec;
  const float W = std::min(500.0f, window.w - 40);
  const float bodyH = gfx.Wrapped(Rect(0, 0, W - 56, 2000), sp.body, FontId::Body, th.muted, false);
  const float H = 28 + 30 + 14 + bodyH + 26 + 38 + 24;
  const Rect d((window.w - W) * 0.5f, std::max(20.0f, (window.h - H) * 0.42f + (1 - age) * 10), W, H);
  gfx.FillRound(d.Offset(0, 6).Inset(-2), 18, WithAlpha(0xFF000000, 70));
  gfx.FillRound(d, 16, th.elev);
  gfx.StrokeRound(d, 16, th.line, 1);
  const uint32_t ic = sp.iconColor ? sp.iconColor : th.accent;
  gfx.Dot({d.x + 44, d.y + 43}, 17, WithAlpha(ic, 40));
  gfx.Icon(sp.icon, Rect(d.x + 32, d.y + 31, 24, 24), ic, 1.8f);
  gfx.Text(Rect(d.x + 70, d.y + 24, d.w - 90, 38), sp.title, FontId::H3, th.text);
  gfx.Wrapped(Rect(d.x + 28, d.y + 80, W - 56, bodyH + 4), sp.body, FontId::Body, th.muted, true);
  // buttons, right aligned
  float bx = d.r() - 24;
  int chosen = -2;
  const float by = d.b() - 24 - 38;
  for (int i = static_cast<int>(sp.buttons.size()) - 1; i >= 0; i--) {
    const ModalButton& b = sp.buttons[static_cast<size_t>(i)];
    const float bw = std::max(96.0f, gfx.TextW(b.label, FontId::Label) + 38);
    const Rect br(bx - bw, by, bw, 38);
    if (Button("modal#" + std::to_string(i), br, b.label, b.style)) chosen = b.id;
    bx -= bw + 10;
  }
  for (auto& k : in.keys) {
    if (k.vk == VK_ESCAPE) { chosen = 0; k.vk = 0; }
    else if (k.vk == VK_RETURN && !sp.buttons.empty()) { chosen = sp.buttons.back().id; k.vk = 0; }
  }
  if (chosen != -2) {
    auto cb = std::move(modal_.done);
    modal_.open = false;
    modal_.done = nullptr;
    animating_ = true;
    if (cb) cb(chosen);
  }
}

void Ui::DrawToasts() {
  const float W = std::min(380.0f, window.w - 32);
  float y = window.b() - 18;
  for (size_t i = toasts_.size(); i-- > 0;) {
    ToastSt& t = toasts_[i];
    if (now >= t.expire) {
      toasts_.erase(toasts_.begin() + static_cast<long>(i));
      continue;
    }
    const float fin = static_cast<float>(std::min(1.0, (now - t.born) / 0.18)), fout = static_cast<float>(std::min(1.0, (t.expire - now) / 0.35));
    const float a = std::min(fin, fout);
    if (a < 1.0f) animating_ = true;
    const float th1 = gfx.Wrapped(Rect(0, 0, W - 70, 1000), t.text, FontId::Body, th.text, false);
    const float th2 = t.detail.empty() ? 0.0f : gfx.Wrapped(Rect(0, 0, W - 70, 1000), t.detail, FontId::Small, th.muted, false) + 4;
    const float H = 16 + th1 + th2 + 16;
    y -= H;
    const Rect r(window.r() - 18 - W + (1 - a) * 24, y, W, H);
    const uint32_t col = t.kind == ToastKind::Ok ? th.green : t.kind == ToastKind::Warn ? th.amber : t.kind == ToastKind::Error ? th.red : th.blue;
    const int al = static_cast<int>(255 * a);
    gfx.FillRound(r.Offset(0, 3).Inset(-1), 14, WithAlpha(0xFF000000, static_cast<int>(60 * a)));
    gfx.FillRound(r, 13, WithAlpha(th.elev, al));
    gfx.StrokeRound(r, 13, WithAlpha(th.line, al), 1);
    gfx.FillRound(Rect(r.x + 10, r.y + 12, 3, H - 24), 1.5f, WithAlpha(col, al));
    const IconId ic = t.kind == ToastKind::Ok ? IconId::Check : t.kind == ToastKind::Warn ? IconId::Warn : t.kind == ToastKind::Error ? IconId::Cross : IconId::Info;
    gfx.Icon(ic, Rect(r.x + 22, r.y + 14, 18, 18), WithAlpha(col, al), 2.0f);
    gfx.Wrapped(Rect(r.x + 50, r.y + 14, W - 70, th1), t.text, FontId::Body, WithAlpha(th.text, al), true);
    if (!t.detail.empty()) gfx.Wrapped(Rect(r.x + 50, r.y + 14 + th1 + 4, W - 70, th2), t.detail, FontId::Small, WithAlpha(th.muted, al), true);
    if (Click("toast#" + std::to_string(static_cast<long long>(t.born * 1000)) + t.text, r, true)) t.expire = now;
    y -= 10;
  }
}

void Ui::DrawTooltip() {
  if (tipText_.empty()) return;
  const float W = std::min(340.0f, window.w - 24);
  const float tw = std::min(W - 24, std::max(60.0f, gfx.TextW(tipText_, FontId::Small) + 2));
  const float bw = gfx.Wrapped(Rect(0, 0, tw, 2000), tipText_, FontId::Small, th.text, false);
  const Rect r(std::min(tipPos_.x + 14, window.r() - tw - 28), std::min(tipPos_.y + 20, window.b() - bw - 24), tw + 24, bw + 18);
  Rect rr = r;
  if (rr.b() > window.b() - 6) rr.y = tipPos_.y - rr.h - 10;
  gfx.FillRound(rr.Offset(0, 3).Inset(-1), 10, WithAlpha(0xFF000000, 60));
  gfx.FillRound(rr, 9, th.elev);
  gfx.StrokeRound(rr, 9, th.line, 1);
  gfx.Wrapped(Rect(rr.x + 12, rr.y + 9, tw, bw), tipText_, FontId::Small, th.text, true);
}

void Ui::End() {
  layer_ = 1;
  if (combo_.open) DrawComboPopup();
  layer_ = 2;
  if (modal_.open) DrawModal();
  layer_ = 3;
  DrawToasts();
  DrawTooltip();
  if (!tipHovered_) tipKey_ = 0;
  if (in.pressed && !textClaimed_) textFocus_ = 0;
  textClaimed_ = false;
  in.pressed = in.released = in.rpressed = in.dbl = false;
  in.wheel = 0;
  in.chars.clear();
  in.keys.clear();
  layer_ = 0;
}

}  // namespace ui
}  // namespace bc
