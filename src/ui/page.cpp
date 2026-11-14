#include "page.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../common/settings_reg.h"
#include "../common/util.h"

namespace bc {
namespace ui {

// ------------------------------------------------------------------------------------------------ formatting
std::string Num(double v, int dec) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", dec, v);
  return b;
}
std::string UnitText(const std::string& unit) { return unit == "C" ? std::string("\xC2\xB0""C") : unit; }
std::string With(double v, int dec, const char* unit) { return Num(v, dec) + (unit && *unit ? std::string(" ") + unit : std::string()); }
std::string Watts(double v, int dec) { return With(v, dec, "W"); }

uint32_t EvColor(const Theme& t, Ev e) {
  switch (e) {
    case Ev::Verified: return t.green;
    case Ev::Family: return t.blue;
    case Ev::Experimental: return t.amber;
    default: return t.muted;
  }
}
std::string EvLabel(Ev e) {
  switch (e) {
    case Ev::Verified: return T("Verified");
    case Ev::Family: return T("Family");
    case Ev::Experimental: return T("Experimental");
    default: return T("OS");
  }
}
std::string EvTip(Ev e) {
  switch (e) {
    case Ev::Verified: return T("Measured on a GM5TG7Y by the device owner.");
    case Ev::Family: return T("Documented for the same chassis family / EC project, but not measured on this exact unit.");
    case Ev::Experimental: return T("Plausible but unproven, or it bypasses a firmware protection. Needs Safety > Allow experimental writes.");
    default: return T("Plain Windows API; no firmware involved.");
  }
}

// ------------------------------------------------------------------------------------------------ masonry
Masonry::Masonry(float width, float minCol, int maxCols, float gap) : width_(width), gap_(gap) {
  n_ = std::max(1, std::min(maxCols, static_cast<int>((width + gap) / (minCol + gap))));
  colW_ = (width - gap * (n_ - 1)) / n_;
  y_.assign(static_cast<size_t>(n_), 0.0f);
}

Rect Masonry::Alloc(float h, bool wide) {
  if (wide || n_ == 1) {
    const float y = *std::max_element(y_.begin(), y_.end());
    for (float& v : y_) v = y + h + gap_;
    return Rect(0, y, width_, h);
  }
  size_t k = 0;
  for (size_t i = 1; i < y_.size(); i++)
    if (y_[i] < y_[k] - 0.5f) k = i;
  Rect r(static_cast<float>(k) * (colW_ + gap_), y_[k], colW_, h);
  y_[k] += h + gap_;
  return r;
}

float Masonry::Bottom() const {
  const float m = *std::max_element(y_.begin(), y_.end());
  return std::max(0.0f, m - gap_);
}

float NoteHeight(Ctx& c, const Masonry& m, const std::string& text, bool wide) {
  return c.ui.gfx.Wrapped(Rect(0, 0, m.InnerW(wide), 3000), text, FontId::Small, 0, false) + 10;
}

// ------------------------------------------------------------------------------------------------ card
Card::Card(Ctx& cc, Masonry& m, const std::string& title, const std::string& sub, IconId icon, float contentH, bool wide) : c(cc) {
  const bool head = !title.empty();
  const float h = (head ? 64.0f : kPad) + contentH + kPad;
  box = m.Alloc(h, wide);
  c.ui.Panel(box);
  x0 = box.x + kPad;
  w = box.w - 2 * kPad;
  y = box.y + kPad;
  if (head) {
    Gfx& g = c.ui.gfx;
    const Theme& th = c.ui.th;
    float tx = x0;
    if (icon != IconId::None) {
      g.FillRound(Rect(x0, y, 34, 34), 10, WithAlpha(th.accent, 30));
      g.Icon(icon, Rect(x0 + 6, y + 6, 22, 22), th.accent, 1.8f);
      tx = x0 + 46;
    }
    if (sub.empty()) {
      g.Text(Rect(tx, y, box.r() - kPad - tx, 34), title, FontId::H3, th.text);
    } else {
      g.Text(Rect(tx, y - 1, box.r() - kPad - tx, 20), title, FontId::H3, th.text);
      g.Text(Rect(tx, y + 19, box.r() - kPad - tx, 16), sub, FontId::Small, th.muted);
      c.ui.Tip(Rect(tx, y + 19, box.r() - kPad - tx, 16), c.ui.gfx.TextW(sub, FontId::Small) > box.r() - kPad - tx ? sub : "");
    }
    y = box.y + 64;
  }
}

void Card::Divider() {
  const Rect r = Row(9);
  c.ui.gfx.Line({r.x, r.cy()}, {r.r(), r.cy()}, WithAlpha(c.ui.th.line, 150), 1);
}

void Card::SubHead(const std::string& text) {
  const Rect r = Row(30);
  c.ui.gfx.Text(Rect(r.x, r.y + 6, r.w, 20), text, FontId::Label, c.ui.th.faint);
}

void Card::Info(const std::string& label, const std::string& value, uint32_t col, const std::string& tip) {
  const Rect r = Row(kRowInfo);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const float vw = std::min(g.TextW(value, FontId::Body) + 2, r.w * 0.62f);
  g.Text(Rect(r.x, r.y, r.w - vw - 10, r.h), label, FontId::Body, th.muted);
  g.Text(Rect(r.r() - vw, r.y, vw, r.h), value, FontId::Body, col ? col : th.text, Align::Right);
  c.ui.Tip(r, tip);
}

float Card::NoteH(const std::string& text) const { return c.ui.gfx.Wrapped(Rect(0, 0, w, 3000), text, FontId::Small, 0, false) + 10; }

void Card::Note(const std::string& text, uint32_t col) {
  const float h = c.ui.gfx.Wrapped(Rect(x0, y, w, 3000), text, FontId::Small, col ? col : c.ui.th.muted, true);
  y += h + 10;
}

bool Card::Button(const std::string& id, const std::string& label, Btn style, IconId icon, bool enabled, float width) {
  const Rect r = Row(kRowButtons);
  const float bw = width > 0 ? width : std::max(110.0f, c.ui.gfx.TextW(label, FontId::Label) + (icon != IconId::None ? 58.0f : 40.0f));
  return c.ui.Button(id, Rect(r.x, r.y + 5, std::min(bw, r.w), 36), label, style, icon, enabled);
}

bool Card::Toggle(const std::string& id, const std::string& label, const std::string& desc, bool on, bool enabled, const std::string& tip) {
  const Rect r = Row(desc.empty() ? kRowToggleLean : kRowToggle);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const Rect sw(r.r() - 44, r.cy() - 11, 44, 22);
  const float tw = r.w - 60;
  if (desc.empty()) {
    g.Text(Rect(r.x, r.y, tw, r.h), label, FontId::Body, enabled ? th.text : th.faint);
  } else {
    g.Text(Rect(r.x, r.y + 6, tw, 22), label, FontId::Body, enabled ? th.text : th.faint);
    g.Text(Rect(r.x, r.y + 28, tw, 18), desc, FontId::Small, th.muted);
    if (g.TextW(desc, FontId::Small) > tw) c.ui.Tip(Rect(r.x, r.y + 28, tw, 18), desc);
  }
  c.ui.Tip(Rect(r.x, r.y, tw, r.h), tip);
  return c.ui.Switch(id, sw, on, enabled);
}

int Card::Segment(const std::string& id, const std::string& label, const std::string& desc, const std::vector<std::string>& items, int cur, bool enabled) {
  const Rect r = Row(desc.empty() ? kRowSegLean : kRowSeg);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  g.Text(Rect(r.x, r.y + 2, r.w, 22), label, FontId::Body, enabled ? th.text : th.faint);
  float sy = r.y + 26;
  if (!desc.empty()) {
    g.Text(Rect(r.x, r.y + 24, r.w, 18), desc, FontId::Small, th.muted);
    if (g.TextW(desc, FontId::Small) > r.w) c.ui.Tip(Rect(r.x, r.y + 24, r.w, 18), desc);
    sy = r.y + 46;
  }
  return c.ui.Segmented(id, Rect(r.x, sy, r.w, 34), items, cur, enabled);
}

bool Card::PrefRow(const std::string& key, const std::string& label, const std::string& desc, bool def, bool enabled, const std::string& tip) {
  const bool on = c.app.PrefB(key, def);
  if (!Toggle("pr#" + key, label, desc, on, enabled, tip)) return false;
  c.app.SetPref(key, on ? "0" : "1");
  return true;
}

bool Card::PrefSlider(const std::string& id, const std::string& label, int* v, int vmin, int vmax, int step, const std::string& unit, bool enabled) {
  const Rect r = Row(kRowPrefSlider);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const std::string val = std::to_string(*v) + (unit.empty() ? std::string() : " " + UnitText(unit));
  const float cw = std::max(54.0f, c.ui.ChipW(val));
  g.Text(Rect(r.x, r.y + 2, r.w - cw - 10, 22), label, FontId::Body, enabled ? th.text : th.faint);
  c.ui.Chip(Rect(r.r() - cw, r.y + 3, cw, 20), val, enabled ? th.text : th.faint, th.elev);
  return c.ui.Slider(id, Rect(r.x - 6, r.y + 28, r.w + 12, 28), vmin, vmax, step, v, INT_MIN, -1, enabled, 0);
}

int Card::Swatches(const std::string& id, const std::string& label, const std::vector<uint32_t>& cols, int cur, bool enabled) {
  const Rect r = Row(kRowSwatch);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  g.Text(Rect(r.x, r.y + 2, r.w, 22), label, FontId::Body, enabled ? th.text : th.faint);
  const float d = 26, gap = 12;
  int pick = -1;
  float x = r.x + 2;
  for (size_t i = 0; i < cols.size(); i++) {
    const Rect cell(x, r.y + 28, d, d);
    bool hov = false;
    if (c.ui.Click(id + "#" + std::to_string(i), cell.Inset(-3), enabled, &hov)) pick = static_cast<int>(i);
    g.Dot({cell.cx(), cell.cy()}, d * 0.5f, enabled ? cols[i] : WithAlpha(cols[i], 90));
    if (static_cast<int>(i) == cur) {
      g.Circle({cell.cx(), cell.cy()}, d * 0.5f + 4.5f, th.accent, 2.0f);
    } else if (hov) {
      g.Circle({cell.cx(), cell.cy()}, d * 0.5f + 4.5f, WithAlpha(th.text, 90), 1.5f);
    }
    x += d + gap;
  }
  return pick;
}

int Card::ComboRow(const std::string& id, const std::string& label, const std::string& desc, const std::vector<std::string>& items, int cur, bool enabled,
                   float comboW) {
  const Rect r = Row(desc.empty() ? kRowToggleLean : kRowCombo);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const float cw = std::min(comboW, r.w * 0.5f);
  const float tw = r.w - cw - 14;
  const Rect cbox(r.r() - cw, r.cy() - 17, cw, 34);
  if (desc.empty()) {
    g.Text(Rect(r.x, r.y, tw, r.h), label, FontId::Body, enabled ? th.text : th.faint);
  } else {
    g.Text(Rect(r.x, r.y + 6, tw, 22), label, FontId::Body, enabled ? th.text : th.faint);
    g.Text(Rect(r.x, r.y + 28, tw, 18), desc, FontId::Small, th.muted);
    if (g.TextW(desc, FontId::Small) > tw) c.ui.Tip(Rect(r.x, r.y + 28, tw, 18), desc);
  }
  return c.ui.Combo(id, cbox, items, cur, enabled);
}

void Card::BoolRow(const std::string& key, const std::string& label, const std::string& desc, const std::string& warnOff, const std::string& warnOn) {
  const SettingDef* d = FindSetting(key);
  const bool capOk = d ? c.app.CapOk(d->cap) : false;
  const bool pending = c.app.Pending(key);
  const bool on = c.app.B(key);
  const Rect r = Row(desc.empty() ? kRowToggleLean : kRowToggle);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  const Rect sw(r.r() - 44, r.cy() - 11, 44, 22);
  const float tw = r.w - 70;
  float lw = g.TextW(label, FontId::Body);
  const float ty = desc.empty() ? r.y : r.y + 6;
  g.Text(Rect(r.x, ty, tw, desc.empty() ? r.h : 22), label, FontId::Body, capOk ? th.text : th.faint);
  if (d && d->ev != Ev::Os && lw + 90 < tw) {
    const std::string ev = EvLabel(d->ev);
    const float cw = c.ui.ChipW(ev);
    const Rect chip(r.x + lw + 10, (desc.empty() ? r.cy() : ty + 11) - 9, cw, 18);
    c.ui.Chip(chip, ev, EvColor(th, d->ev));
    c.ui.Tip(chip, EvTip(d->ev));
  }
  if (!desc.empty()) {
    const std::string shown = capOk ? desc : (c.app.CapWhy(d ? d->cap : "").empty() ? T("Not available on this system.") : c.app.CapWhy(d->cap));
    g.Text(Rect(r.x, r.y + 28, tw, 18), shown, FontId::Small, capOk ? th.muted : th.amber);
    if (g.TextW(shown, FontId::Small) > tw) c.ui.Tip(Rect(r.x, r.y + 28, tw, 18), shown);
  }
  if (pending) c.ui.Spinner(Rect(sw.x - 26, r.cy() - 9, 18, 18));
  if (c.ui.Switch("sw#" + key, sw, on, capOk && !pending)) {
    const std::string& warn = on ? warnOff : warnOn;
    const std::string nextVal = on ? "0" : "1";
    const std::string doneText = label + ": " + (on ? T("off") : T("on"));
    if (warn.empty()) {
      c.app.Set(key, nextVal, doneText);
    } else {
      ModalSpec ms;
      ms.title = label;
      ms.body = warn;
      ms.icon = IconId::Warn;
      ms.iconColor = th.amber;
      ms.buttons = {{T("Cancel"), Btn::Ghost, 0}, {on ? T("Turn off") : T("Turn on"), Btn::Primary, 1}};
      App* ap = &c.app;
      c.ui.ModalOpen(ms, [ap, key, nextVal, doneText](int id) {
        if (id == 1) ap->Set(key, nextVal, doneText);
      });
    }
  }
  if (!capOk && d) c.ui.Tip(r, T("Unavailable") + ": " + c.app.CapState(d->cap) + (c.app.CapWhy(d->cap).empty() ? "" : " - " + c.app.CapWhy(d->cap)));
}

void Card::EnumRow(const std::string& key, const std::string& label, const std::string& desc, const std::vector<std::string>& shown) {
  const SettingDef* d = FindSetting(key);
  if (!d) return;
  const bool capOk = c.app.CapOk(d->cap);
  const bool pending = c.app.Pending(key);
  const int cur = EnumIndex(*d, c.app.S(key));
  const Rect r = Row(desc.empty() ? kRowSegLean : kRowSeg);
  Gfx& g = c.ui.gfx;
  const Theme& th = c.ui.th;
  g.Text(Rect(r.x, r.y + 2, r.w - 120, 22), label, FontId::Body, capOk ? th.text : th.faint);
  if (d->ev != Ev::Os) {
    const std::string ev = EvLabel(d->ev);
    const float lw = g.TextW(label, FontId::Body);
    const float cw = c.ui.ChipW(ev);
    if (lw + cw + 20 < r.w) {
      const Rect chip(r.x + lw + 10, r.y + 4, cw, 18);
      c.ui.Chip(chip, ev, EvColor(th, d->ev));
      c.ui.Tip(chip, EvTip(d->ev));
    }
  }
  float sy = r.y + 26;
  if (!desc.empty()) {
    const std::string text = capOk ? desc : (c.app.CapWhy(d->cap).empty() ? T("Not available on this system.") : c.app.CapWhy(d->cap));
    g.Text(Rect(r.x, r.y + 24, r.w, 18), text, FontId::Small, capOk ? th.muted : th.amber);
    if (g.TextW(text, FontId::Small) > r.w) c.ui.Tip(Rect(r.x, r.y + 24, r.w, 18), text);
    sy = r.y + 46;
  }
  if (pending) c.ui.Spinner(Rect(r.r() - 20, r.y + 4, 18, 18));
  const int n = EnumCount(*d);
  std::vector<std::string> items;
  for (int i = 0; i < n; i++) items.push_back(static_cast<size_t>(i) < shown.size() ? shown[static_cast<size_t>(i)] : EnumName(*d, i));
  const bool seg = n <= 4 || (n <= 6 && r.w > 560);
  int pick = -1;
  if (seg) pick = c.ui.Segmented("seg#" + key, Rect(r.x, sy, r.w, 34), items, cur, capOk && !pending);
  else pick = c.ui.Combo("cmb#" + key, Rect(r.x, sy, r.w, 34), items, cur, capOk && !pending);
  if (pick >= 0 && pick != cur) c.app.Set(key, EnumName(*d, pick), label + ": " + items[static_cast<size_t>(pick)]);
}

void Card::Slider(const SliderCfg& cfg) {
  const Rect row = Row(kRowSlider);
  const SettingDef* def = FindSetting(cfg.key);
  if (!def) return;
  Ui& ui = c.ui;
  App& app = c.app;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const std::string liveKey = cfg.liveKey.empty() ? cfg.key : cfg.liveKey;
  const bool capOk = app.CapOk(def->cap);
  const int vmin = std::max(def->min, cfg.vmin), vmax = std::min(def->max, cfg.vmax);
  const int step = std::max(1, def->step);
  const std::string unit = UnitText(cfg.unit.empty() ? std::string(def->unit) : cfg.unit);
  auto fmt = [&](int v) {
    if (cfg.fmt) return cfg.fmt(v);
    return unit.empty() ? std::to_string(v) : std::to_string(v) + " " + unit;
  };
  const bool haveLive = app.Has(liveKey) || cfg.fallback != INT_MIN;
  const int live = app.Has(liveKey) ? static_cast<int>(std::lround(app.N(liveKey))) : (haveLive ? cfg.fallback : INT_MIN);
  const bool storedTag = !cfg.tag.empty();
  const std::string tagText = storedTag ? cfg.tag : T("LIVE");
  Draft& d = app.GetDraft(cfg.key);
  const bool pending = app.Pending(cfg.key);
  if (!d.active) {
    d.v = haveLive ? std::min(vmax, std::max(vmin, live)) : vmin;
    d.liveAtStart = live;
  }

  // ---- geometry
  const float btnW = 104, discW = 34;
  const Rect confirm(row.r() - btnW, row.y + 26, btnW, 32);
  const Rect discard(confirm.x - discW - 6, row.y + 26, discW, 32);
  const Rect track(row.x, row.y + 26, discard.x - 14 - row.x, 32);

  // ---- slider + state
  int v = d.v;
  const bool moved = ui.Slider("sl#" + cfg.key, track, vmin, vmax, step, &v, live, cfg.softMax, capOk && !pending, EvColor(th, def->ev) == th.amber ? th.amber : th.accent);
  if (moved) {
    d.active = true;
    d.v = v;
  }
  if (d.active && haveLive && d.v == live && !moved) d.active = false;   // dragged back onto the live value: nothing pending
  const bool changed = d.active && (!haveLive || d.v != live);
  const std::string err = (changed && cfg.validate) ? cfg.validate(d.v) : std::string();
  const bool over = cfg.softMax >= 0 && d.v > cfg.softMax;

  // ---- title line
  const std::string liveS = haveLive ? fmt(live) : std::string("\xE2\x80\x94");
  const std::string draftS = fmt(d.v);
  const float wl = g.TextW(liveS, FontId::Value), wd = g.TextW(draftS, FontId::Value);
  float xr = row.r();
  if (changed) {
    g.Text(Rect(xr - wd - 2, row.y, wd + 2, 24), draftS, FontId::Value, over ? th.amber : th.accent, Align::Right);
    xr -= wd + 8;
    g.Icon(IconId::ChevronR, Rect(xr - 12, row.y + 6, 12, 12), th.faint, 2.0f);
    xr -= 18;
  }
  g.Text(Rect(xr - wl - 2, row.y, wl + 2, 24), liveS, FontId::Value, changed ? th.muted : th.text, Align::Right);
  xr -= wl + 10;
  const float livTag = g.TextW(tagText, FontId::Tiny);
  if (haveLive) {
    g.Text(Rect(xr - livTag - 2, row.y, livTag + 2, 24), tagText, FontId::Tiny, th.faint, Align::Right);
    g.Dot({xr - livTag - 9, row.y + 12}, 3.2f, storedTag ? th.faint : th.green);
  }
  ui.Tip(Rect(xr - livTag - 16, row.y, row.r() - xr + livTag + 16, 24),
         storedTag ? T("The value BaseControl has stored. The hardware does not use it until the matching mode is switched on.")
                   : T("The value the hardware reports right now. It follows changes made by other programs."));
  const float rightReserved = (row.r() - xr) + livTag + 22;
  float tx = row.x;
  const float tw = std::min(g.TextW(cfg.title, FontId::Value) + 2, row.w - rightReserved - 4);
  g.Text(Rect(tx, row.y, tw, 24), cfg.title, FontId::Value, capOk ? th.text : th.faint);
  tx += tw + 8;
  if (def->ev != Ev::Os) {
    const std::string ev = EvLabel(def->ev);
    const float cw = ui.ChipW(ev);
    if (tx + cw < row.r() - rightReserved) {
      const Rect chip(tx, row.y + 3, cw, 18);
      ui.Chip(chip, ev, EvColor(th, def->ev));
      ui.Tip(chip, EvTip(def->ev));
    }
  }
  ui.Tip(track, Tf("Range %s to %s, step %d. Setting key: %s", fmt(vmin).c_str(), fmt(vmax).c_str(), step, cfg.key.c_str()));

  // ---- buttons
  if (changed && ui.IconButton("dc#" + cfg.key, discard, IconId::Undo, true)) app.DiscardDraft(cfg.key);
  if (changed) ui.Tip(discard, T("Discard the draft"));
  const bool canConfirm = capOk && changed && !pending && err.empty();
  const bool clicked = ui.Button("ok#" + cfg.key, confirm, pending ? T("Applying") : T("Confirm"), over ? Btn::Primary : Btn::Primary,
                                 pending ? IconId::None : IconId::Check, canConfirm);
  if (pending) ui.Spinner(Rect(confirm.x + 10, confirm.cy() - 8, 16, 16), th.muted);
  if (changed) {
    ui.Tip(confirm, canConfirm ? Tf("Send %s to the hardware now. Nothing is sent until you press this.", draftS.c_str())
                               : (err.empty() ? std::string() : err));
  } else {
    ui.Tip(confirm, T("Move the slider, then press Confirm. Nothing is sent to the hardware until then."));
  }
  if (clicked) app.Set(cfg.key, std::to_string(d.v), cfg.title + " \xE2\x86\x92 " + draftS);

  // ---- caption line
  const Rect cap(row.x, row.y + 62, row.w, 34);
  std::string text = cfg.desc;
  uint32_t col = th.muted;
  if (!capOk) {
    text = app.CapWhy(def->cap).empty() ? T("Not available on this system.") : app.CapWhy(def->cap);
    col = th.amber;
  } else if (!err.empty()) {
    text = err;
    col = th.amber;
  } else if (changed && haveLive && d.liveAtStart != INT_MIN && d.liveAtStart != live) {
    text = Tf("Changed outside BaseControl: the hardware now reports %s.", liveS.c_str());
    col = th.amber;
  } else if (changed && over) {
    text = T("Above the OEM maximum. BaseControl asks you to confirm unless you agreed before.");
    col = th.amber;
  } else if (app.RecentOk(cfg.key) < 3.0) {
    text = T("Applied and verified by reading the value back.");
    col = th.green;
  }
  bool cut = false;
  g.WrappedClamped(cap, text, FontId::Small, col, 2, 2.0f, &cut);
  if (cut) ui.Tip(cap, text);
}

void Card::EnumConfirm(const std::string& key, const std::string& label, const std::string& desc, const std::vector<std::string>& shown) {
  const Rect row = Row(kRowSlider);
  const SettingDef* def = FindSetting(key);
  if (!def) return;
  Ui& ui = c.ui;
  App& app = c.app;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const bool capOk = app.CapOk(def->cap);
  const bool pending = app.Pending(key);
  const int n = EnumCount(*def);
  auto name = [&](int i) { return static_cast<size_t>(i) < shown.size() ? shown[static_cast<size_t>(i)] : EnumName(*def, i); };
  const int liveIdx = app.Has(key) ? EnumIndex(*def, app.S(key)) : -1;
  int& draft = app.Int("ed#" + key, -1);
  if (draft >= n) draft = -1;
  if (draft >= 0 && draft == liveIdx) draft = -1;
  const bool changed = draft >= 0;
  std::vector<std::string> items;
  for (int i = 0; i < n; i++) items.push_back(name(i));

  const float btnW = 104, discW = 34;
  const Rect confirm(row.r() - btnW, row.y + 26, btnW, 32);
  const Rect discard(confirm.x - discW - 6, row.y + 26, discW, 32);
  const Rect combo(row.x, row.y + 26, discard.x - 14 - row.x, 32);
  const int pick = ui.Combo("cmb#" + key, combo, items, changed ? draft : liveIdx, capOk && !pending);
  if (pick >= 0) draft = pick == liveIdx ? -1 : pick;

  const std::string liveS = liveIdx >= 0 ? name(liveIdx) : std::string("\xE2\x80\x94");
  const std::string draftS = changed ? name(draft) : std::string();
  float xr = row.r();
  if (changed) {
    const float wd = g.TextW(draftS, FontId::Value);
    g.Text(Rect(xr - wd - 2, row.y, wd + 2, 24), draftS, FontId::Value, th.accent, Align::Right);
    xr -= wd + 8;
    g.Icon(IconId::ChevronR, Rect(xr - 12, row.y + 6, 12, 12), th.faint, 2.0f);
    xr -= 18;
  }
  const float wl = g.TextW(liveS, FontId::Value);
  g.Text(Rect(xr - wl - 2, row.y, wl + 2, 24), liveS, FontId::Value, changed ? th.muted : th.text, Align::Right);
  xr -= wl + 10;
  const float livTag = g.TextW(T("LIVE"), FontId::Tiny);
  if (liveIdx >= 0) {
    g.Text(Rect(xr - livTag - 2, row.y, livTag + 2, 24), T("LIVE"), FontId::Tiny, th.faint, Align::Right);
    g.Dot({xr - livTag - 9, row.y + 12}, 3.2f, th.green);
  }
  const float rightReserved = (row.r() - xr) + livTag + 22;
  const float tw = std::min(g.TextW(label, FontId::Value) + 2, row.w - rightReserved - 4);
  g.Text(Rect(row.x, row.y, tw, 24), label, FontId::Value, capOk ? th.text : th.faint);
  float tx = row.x + tw + 8;
  if (def->ev != Ev::Os) {
    const std::string ev = EvLabel(def->ev);
    const float cw = ui.ChipW(ev);
    if (tx + cw < row.r() - rightReserved) {
      const Rect chip(tx, row.y + 3, cw, 18);
      ui.Chip(chip, ev, EvColor(th, def->ev));
      ui.Tip(chip, EvTip(def->ev));
    }
  }
  if (changed && ui.IconButton("dc#" + key, discard, IconId::Undo, true)) draft = -1;
  const bool clicked = ui.Button("ok#" + key, confirm, pending ? T("Applying") : T("Confirm"), Btn::Primary, pending ? IconId::None : IconId::Check, capOk && changed && !pending);
  if (pending) ui.Spinner(Rect(confirm.x + 10, confirm.cy() - 8, 16, 16), th.muted);
  ui.Tip(confirm, changed ? T("Send the chosen value to the hardware now. Nothing is sent until you press this.") : T("Pick a value, then press Confirm. Nothing is sent until then."));
  if (clicked && draft >= 0) app.Set(key, EnumName(*def, draft), label + " \xE2\x86\x92 " + draftS);

  std::string text = desc;
  uint32_t col = th.muted;
  if (!capOk) {
    text = app.CapWhy(def->cap).empty() ? T("Not available on this system.") : app.CapWhy(def->cap);
    col = th.amber;
  } else if (!changed && app.RecentOk(key) < 3.0) {
    text = T("Applied and verified by reading the value back.");
    col = th.green;
  }
  bool cut = false;
  g.WrappedClamped(Rect(row.x, row.y + 62, row.w, 34), text, FontId::Small, col, 2, 2.0f, &cut);
  if (cut) ui.Tip(Rect(row.x, row.y + 62, row.w, 34), text);
}

void Card::ListConfirm(const std::string& key, const std::string& label, const std::string& desc, const std::vector<int>& values, const std::string& unit) {
  const Rect row = Row(kRowSlider);
  const SettingDef* def = FindSetting(key);
  if (!def) return;
  Ui& ui = c.ui;
  App& app = c.app;
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const bool capOk = app.CapOk(def->cap) && !values.empty();
  const bool pending = app.Pending(key);
  const std::string u = UnitText(unit.empty() ? std::string(def->unit) : unit);
  auto name = [&](int v) { return std::to_string(v) + (u.empty() ? std::string() : " " + u); };
  const bool haveLive = app.Has(key);
  const int live = haveLive ? static_cast<int>(std::lround(app.N(key))) : INT_MIN;
  int liveIdx = -1;
  for (size_t i = 0; i < values.size(); i++)
    if (values[i] == live) liveIdx = static_cast<int>(i);
  int& draft = app.Int("lc#" + key, -1);
  if (draft >= static_cast<int>(values.size())) draft = -1;
  if (draft >= 0 && draft == liveIdx) draft = -1;
  const bool changed = draft >= 0;
  std::vector<std::string> items;
  for (int v : values) items.push_back(name(v));

  const float btnW = 104, discW = 34;
  const Rect confirm(row.r() - btnW, row.y + 26, btnW, 32);
  const Rect discard(confirm.x - discW - 6, row.y + 26, discW, 32);
  const Rect combo(row.x, row.y + 26, discard.x - 14 - row.x, 32);
  const int pick = ui.Combo("cmb#" + key, combo, items, changed ? draft : liveIdx, capOk && !pending);
  if (pick >= 0) draft = pick == liveIdx ? -1 : pick;

  const std::string liveS = haveLive ? name(live) : std::string("\xE2\x80\x94");
  float xr = row.r();
  if (changed) {
    const std::string draftS = name(values[static_cast<size_t>(draft)]);
    const float wd = g.TextW(draftS, FontId::Value);
    g.Text(Rect(xr - wd - 2, row.y, wd + 2, 24), draftS, FontId::Value, th.accent, Align::Right);
    xr -= wd + 8;
    g.Icon(IconId::ChevronR, Rect(xr - 12, row.y + 6, 12, 12), th.faint, 2.0f);
    xr -= 18;
  }
  const float wl = g.TextW(liveS, FontId::Value);
  g.Text(Rect(xr - wl - 2, row.y, wl + 2, 24), liveS, FontId::Value, changed ? th.muted : th.text, Align::Right);
  xr -= wl + 10;
  const float livTag = g.TextW(T("LIVE"), FontId::Tiny);
  if (haveLive) {
    g.Text(Rect(xr - livTag - 2, row.y, livTag + 2, 24), T("LIVE"), FontId::Tiny, th.faint, Align::Right);
    g.Dot({xr - livTag - 9, row.y + 12}, 3.2f, th.green);
  }
  g.Text(Rect(row.x, row.y, std::max(40.0f, xr - livTag - 20 - row.x), 24), label, FontId::Value, capOk ? th.text : th.faint);

  if (changed && ui.IconButton("dc#" + key, discard, IconId::Undo, true)) draft = -1;
  const bool clicked = ui.Button("ok#" + key, confirm, pending ? T("Applying") : T("Confirm"), Btn::Primary, pending ? IconId::None : IconId::Check, capOk && changed && !pending);
  if (pending) ui.Spinner(Rect(confirm.x + 10, confirm.cy() - 8, 16, 16), th.muted);
  ui.Tip(confirm, changed ? T("Send the chosen value to the hardware now. Nothing is sent until you press this.") : T("Pick a value, then press Confirm. Nothing is sent until then."));
  if (clicked && draft >= 0) app.Set(key, std::to_string(values[static_cast<size_t>(draft)]), label + " \xE2\x86\x92 " + name(values[static_cast<size_t>(draft)]));

  std::string text = desc;
  uint32_t col = th.muted;
  if (!capOk) {
    text = app.CapWhy(def->cap).empty() ? T("Not available on this system.") : app.CapWhy(def->cap);
    col = th.amber;
  } else if (!changed && app.RecentOk(key) < 3.0) {
    text = T("Applied and verified by reading the value back.");
    col = th.green;
  }
  bool cut = false;
  g.WrappedClamped(Rect(row.x, row.y + 62, row.w, 34), text, FontId::Small, col, 2, 2.0f, &cut);
  if (cut) ui.Tip(Rect(row.x, row.y + 62, row.w, 34), text);
}

bool Card::InputRow(const std::string& id, std::string* text, const std::string& hint, const std::string& button, bool enabled, size_t maxLen, bool selectOnClick) {
  const Rect r = Row(50);
  const float bw = std::max(88.0f, c.ui.gfx.TextW(button, FontId::Label) + 36);
  const bool enter = c.ui.TextInput("in#" + id, Rect(r.x, r.y + 5, r.w - bw - 10, 38), text, hint, maxLen, selectOnClick);
  const bool click = c.ui.Button("bt#" + id, Rect(r.r() - bw, r.y + 5, bw, 38), button, Btn::Normal, IconId::None, enabled);
  return (click || enter) && enabled;
}

// ------------------------------------------------------------------------------------------------ shared widgets
void StatusDot(Ui& ui, V2 c, uint32_t col, bool) { ui.gfx.Dot(c, 3.5f, col); }

void Sparkline(Ui& ui, const Rect& r, const std::deque<float>& s, float ymin, float ymax, uint32_t col) {
  Gfx& g = ui.gfx;
  if (s.size() < 2) return;
  const float cap = 120;
  std::vector<V2> pts;
  pts.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    const float x = r.r() - r.w * static_cast<float>(s.size() - 1 - i) / (cap - 1);
    const float t = (std::min(ymax, std::max(ymin, s[i])) - ymin) / std::max(0.001f, ymax - ymin);
    pts.push_back({x, r.b() - r.h * t});
  }
  std::vector<V2> poly = pts;
  poly.push_back({pts.back().x, r.b()});
  poly.push_back({pts.front().x, r.b()});
  g.PolyFill(poly, WithAlpha(col, 30));
  g.Poly(pts, col, 1.6f);
}

void TimeAxis(Ui& ui, const Rect& plot, int windowSeconds) {
  Gfx& g = ui.gfx;
  for (int i = 0; i <= 4; i++) {
    const float x = plot.x + plot.w * static_cast<float>(i) / 4.0f;
    const int sec = windowSeconds - windowSeconds * i / 4;
    const std::string t = sec == 0 ? T("now") : Tf("-%d s", sec);
    const float tw = g.TextW(t, FontId::Tiny);
    const float tx = std::min(plot.r() - tw, std::max(plot.x, x - tw * 0.5f));
    g.Text(Rect(tx, plot.b() + 3, tw + 2, 14), t, FontId::Tiny, ui.th.faint);
  }
}

void Gauge(Ui& ui, const Rect& r, float frac, uint32_t col, const std::string& big, const std::string& small) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const float rad = std::min(r.w, r.h) * 0.5f - 7;
  const V2 c{r.cx(), r.cy() + 4};
  frac = std::min(1.0f, std::max(0.0f, frac));
  g.Arc(c, rad, 135, 270, th.elev, 9);
  if (frac > 0.003f) g.Arc(c, rad, 135, 270 * frac, col, 9);
  g.Text(Rect(r.x, c.y - 24, r.w, 30), big, FontId::H2, th.text, Align::Center);
  g.Text(Rect(r.x, c.y + 6, r.w, 16), small, FontId::Tiny, th.muted, Align::Center);
}

void LineChart(Ui& ui, const Rect& r, const std::vector<const std::deque<float>*>& series, const std::vector<uint32_t>& cols, float ymin, float ymax,
               const std::string& unit) {
  Gfx& g = ui.gfx;
  const Theme& th = ui.th;
  const Rect plot(r.x + 34, r.y + 6, r.w - 40, r.h - 16);
  for (int i = 0; i <= 3; i++) {
    const float y = plot.y + plot.h * static_cast<float>(i) / 3.0f;
    g.Line({plot.x, y}, {plot.r(), y}, WithAlpha(th.line, i == 3 ? 200 : 110), 1);
    const float v = ymax - (ymax - ymin) * static_cast<float>(i) / 3.0f;
    g.Text(Rect(r.x, y - 8, 30, 16), Num(v, 0), FontId::Tiny, th.faint, Align::Right);
  }
  if (!unit.empty()) g.Text(Rect(plot.r() - 60, plot.y - 4, 60, 14), unit, FontId::Tiny, th.faint, Align::Right);
  const float cap = 120;
  for (size_t si = 0; si < series.size(); si++) {
    const std::deque<float>& s = *series[si];
    if (s.size() < 2) continue;
    std::vector<V2> pts;
    pts.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
      const float x = plot.r() - plot.w * static_cast<float>(s.size() - 1 - i) / (cap - 1);
      const float t = (std::min(ymax, std::max(ymin, s[i])) - ymin) / std::max(0.001f, ymax - ymin);
      pts.push_back({x, plot.b() - plot.h * t});
    }
    if (si == 0) {
      std::vector<V2> poly = pts;
      poly.push_back({pts.back().x, plot.b()});
      poly.push_back({pts.front().x, plot.b()});
      g.PolyFill(poly, WithAlpha(cols[si], 34));
    }
    g.Poly(pts, cols[si], 2.0f);
  }
}

}  // namespace ui
}  // namespace bc
