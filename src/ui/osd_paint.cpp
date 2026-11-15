#include "osd_paint.h"

#include <algorithm>

namespace bc {
namespace ui {
namespace {

constexpr float kPad = 9.0f, kGapCols = 14.0f, kLineGap = 3.0f;

struct Metrics {
  float w = 0, h = 0, lineH = 0, labelW = 0;
};

Metrics Measure(Gfx& g, const std::vector<OsdLine>& lines) {
  Metrics m;
  m.lineH = g.LineH(FontId::Mono) + kLineGap;
  float vw = 0;
  for (const OsdLine& l : lines) {
    m.labelW = std::max(m.labelW, g.TextW(l.label, FontId::Mono));
    vw = std::max(vw, g.TextW(l.value, FontId::Mono));
  }
  m.w = kPad * 2 + m.labelW + kGapCols + vw + 2;
  m.h = kPad * 2 + m.lineH * static_cast<float>(lines.size()) - kLineGap;
  return m;
}

}  // namespace

V2 OsdMeasure(Gfx& g, const OsdConfig& c, const std::vector<OsdLine>& lines) {
  const Metrics m = Measure(g, lines);
  const float f = OsdSizeFactor(c.size);
  V2 v;
  v.x = lines.empty() ? 0 : m.w * f;
  v.y = lines.empty() ? 0 : m.h * f;
  return v;
}

void OsdPaint(Gfx& g, V2 topLeft, const OsdConfig& c, const std::vector<OsdLine>& lines) {
  if (lines.empty()) return;
  const Metrics m = Measure(g, lines);
  const float f = OsdSizeFactor(c.size);
  g.Translate(topLeft.x, topLeft.y);
  g.Scale(f);
  const int alpha = std::min(255, std::max(0, c.opacity * 255 / 100));
  g.FillRound(Rect(0, 0, m.w, m.h), 7, Argb(alpha, 14, 15, 14));
  g.StrokeRound(Rect(0, 0, m.w, m.h), 7, WithAlpha(0xFFFFFFFF, std::min(40, alpha / 4)), 1.0f);
  const uint32_t dim = Mix(WithAlpha(c.color, 255), 0xFF8C8C8C, 0.45f);
  float y = kPad;
  for (const OsdLine& l : lines) {
    g.Text(Rect(kPad, y, m.labelW + 2, m.lineH - kLineGap), l.label, FontId::Mono, dim, Align::Left, true, false);
    g.Text(Rect(m.w - kPad - 400, y, 400, m.lineH - kLineGap), l.value, FontId::Mono, c.color, Align::Right, true, false);
    y += m.lineH;
  }
  g.Scale(1.0f / f);
  g.Translate(-topLeft.x, -topLeft.y);
}

void OsdPreviewScene(Gfx& g, const Rect& r) {
  g.PushClip(r);
  g.FillGrad(r, 0, 0xFF1E2A6B, 0xFFFF7E5F, 90);                       // dusk sky
  const float hz = r.y + r.h * 0.58f;                                  // horizon
  g.Dot({r.x + r.w * 0.70f, hz - r.h * 0.17f}, r.h * 0.085f, 0x88FFE9C4);
  std::vector<V2> ridgeFar = {{r.x, hz}, {r.x + r.w * 0.10f, hz - r.h * 0.14f}, {r.x + r.w * 0.24f, hz - r.h * 0.30f}, {r.x + r.w * 0.33f, hz - r.h * 0.20f},
                         {r.x + r.w * 0.46f, hz - r.h * 0.36f}, {r.x + r.w * 0.60f, hz - r.h * 0.22f}, {r.x + r.w * 0.78f, hz - r.h * 0.33f},
                         {r.x + r.w * 0.90f, hz - r.h * 0.16f}, {r.r(), hz - r.h * 0.24f}, {r.r(), hz}};
  g.PolyFill(ridgeFar, 0xFF2D2F6E);
  std::vector<V2> ridgeNear = {{r.x, hz}, {r.x + r.w * 0.18f, hz - r.h * 0.10f}, {r.x + r.w * 0.36f, hz - r.h * 0.05f}, {r.x + r.w * 0.58f, hz - r.h * 0.13f},
                          {r.x + r.w * 0.80f, hz - r.h * 0.06f}, {r.r(), hz - r.h * 0.11f}, {r.r(), hz}};
  g.PolyFill(ridgeNear, 0xFF1B1D4A);
  g.FillGrad(Rect(r.x, hz, r.w, r.b() - hz), 0, 0xFF3A4C9C, 0xFF141A3D, 90);   // lake
  for (int i = 0; i < 5; i++) {                                         // soft ripples
    const float y = hz + (r.b() - hz) * (0.14f + 0.17f * static_cast<float>(i));
    g.Line({r.x + r.w * (0.10f + 0.07f * static_cast<float>(i)), y}, {r.x + r.w * (0.48f + 0.05f * static_cast<float>(i)), y}, 0x22FFFFFF, 1.2f);
  }
  std::vector<V2> shore = {{r.r(), r.b()}, {r.r() - r.w * 0.34f, r.b()}, {r.r() - r.w * 0.26f, r.b() - r.h * 0.10f}, {r.r() - r.w * 0.12f, r.b() - r.h * 0.17f},
                           {r.r(), r.b() - r.h * 0.12f}};
  g.PolyFill(shore, 0xFF2A1F3D);
  g.PopClip();
}

}  // namespace ui
}  // namespace bc
