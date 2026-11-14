#include "gfx.h"

#include <cmath>
#include <cstring>

namespace bc {
namespace ui {

using namespace Gdiplus;

namespace {
constexpr float kPi = 3.14159265358979f;
inline Color C(uint32_t c) { return Color(static_cast<ARGB>(c)); }
bool Avail(const wchar_t* name) {
  FontFamily f(name);
  return f.IsAvailable();
}
}  // namespace

Rect Rect::Intersect(const Rect& o) const {
  const float x0 = max(x, o.x), y0 = max(y, o.y), x1 = min(r(), o.r()), y1 = min(b(), o.b());
  return Rect(x0, y0, max(0.0f, x1 - x0), max(0.0f, y1 - y0));
}

uint32_t Mix(uint32_t a, uint32_t b, float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  auto ch = [&](int sh) {
    const float va = static_cast<float>((a >> sh) & 0xFF), vb = static_cast<float>((b >> sh) & 0xFF);
    return static_cast<uint32_t>(va + (vb - va) * t + 0.5f) & 0xFF;
  };
  return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

Gfx::Gfx() {}
Gfx::~Gfx() {}

void Gfx::ChooseFonts(const std::wstring& forced) {
  const wchar_t* sans[] = {L"Segoe UI Variable Text", L"Segoe UI", L"Inter", L"Noto Sans", L"Tahoma", L"Liberation Sans", L"DejaVu Sans", L"Arial"};
  const wchar_t* mono[] = {L"Cascadia Mono", L"Consolas", L"DejaVu Sans Mono", L"Liberation Mono", L"Courier New"};
  family_.clear();
  if (!forced.empty() && Avail(forced.c_str())) family_ = forced;
  for (const wchar_t* n : sans)
    if (family_.empty() && Avail(n)) family_ = n;
  if (family_.empty()) family_ = L"Arial";
  fam_.reset(new FontFamily(family_.c_str()));
  famSemi_.reset();
  std::wstring semi = family_;
  if (semi == L"Segoe UI Variable Text") semi = L"Segoe UI Semibold";
  else semi += L" SemiBold";
  if (Avail(semi.c_str())) famSemi_.reset(new FontFamily(semi.c_str()));
  famMono_.reset();
  for (const wchar_t* n : mono)
    if (!famMono_ && Avail(n)) famMono_.reset(new FontFamily(n));
  if (!famMono_) famMono_.reset(new FontFamily(family_.c_str()));

  struct Spec { float px; bool semi; bool mono; };
  static const Spec specs[static_cast<int>(FontId::Count)] = {
      {13.5f, false, false}, {12.0f, false, false}, {11.0f, false, false}, {12.0f, true, false}, {14.0f, true, false},
      {16.0f, true, false},  {20.0f, true, false},  {27.0f, true, false},  {38.0f, true, false}, {12.0f, false, true}};
  for (int i = 0; i < static_cast<int>(FontId::Count); i++) {
    const Spec& s = specs[i];
    if (s.mono) fonts_[i].reset(new Font(famMono_.get(), s.px, FontStyleRegular, UnitPixel));
    else if (s.semi && famSemi_) fonts_[i].reset(new Font(famSemi_.get(), s.px, FontStyleRegular, UnitPixel));
    else fonts_[i].reset(new Font(fam_.get(), s.px, s.semi ? FontStyleBold : FontStyleRegular, UnitPixel));
  }
  widthCache_.clear();
  wrapCache_.clear();
}

Font* Gfx::Fnt(FontId f) { return fonts_[static_cast<int>(f)].get(); }

void Gfx::Begin(Graphics* g, float scale) {
  g_ = g;
  scale_ = scale;
  g_->SetSmoothingMode(SmoothingModeAntiAlias);
  g_->SetPixelOffsetMode(PixelOffsetModeHalf);
  g_->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g_->SetInterpolationMode(InterpolationModeHighQualityBicubic);
  g_->ResetTransform();
  g_->ScaleTransform(scale, scale);
  clips_.clear();
}

void Gfx::End() {
  while (!clips_.empty()) PopClip();
  g_ = nullptr;
}

namespace {
std::vector<GraphicsState>& States() {
  static std::vector<GraphicsState> s;
  return s;
}
}  // namespace

void Gfx::PushClip(const Rect& r) {
  States().push_back(g_->Save());
  g_->SetClip(RectF(r.x, r.y, r.w, r.h), CombineModeIntersect);
  clips_.push_back(r);
}

void Gfx::PopClip() {
  if (clips_.empty()) return;
  clips_.pop_back();
  g_->Restore(States().back());
  States().pop_back();
}

void Gfx::Translate(float dx, float dy) { g_->TranslateTransform(dx, dy); }

void Gfx::Scale(float s) { g_->ScaleTransform(s, s); }

GraphicsPath* Gfx::RoundPath(const Rect& r, float rad) {
  GraphicsPath* p = new GraphicsPath();
  rad = std::max(0.0f, std::min(rad, std::min(r.w, r.h) * 0.5f));
  if (rad < 0.5f) {
    p->AddRectangle(RectF(r.x, r.y, r.w, r.h));
    return p;
  }
  const float d = rad * 2;
  p->AddArc(r.x, r.y, d, d, 180, 90);
  p->AddArc(r.r() - d, r.y, d, d, 270, 90);
  p->AddArc(r.r() - d, r.b() - d, d, d, 0, 90);
  p->AddArc(r.x, r.b() - d, d, d, 90, 90);
  p->CloseFigure();
  return p;
}

void Gfx::Fill(const Rect& r, uint32_t c) {
  SolidBrush b(C(c));
  g_->FillRectangle(&b, RectF(r.x, r.y, r.w, r.h));
}

void Gfx::FillRound(const Rect& r, float rad, uint32_t c) {
  if (r.w <= 0 || r.h <= 0) return;
  std::unique_ptr<GraphicsPath> p(RoundPath(r, rad));
  SolidBrush b(C(c));
  g_->FillPath(&b, p.get());
}

void Gfx::StrokeRound(const Rect& r, float rad, uint32_t c, float w) {
  if (r.w <= 0 || r.h <= 0) return;
  // inset by half the pen width so the stroke stays inside the rect
  Rect in = r.Inset(w * 0.5f);
  std::unique_ptr<GraphicsPath> p(RoundPath(in, rad - w * 0.5f));
  Pen pen(C(c), w);
  g_->DrawPath(&pen, p.get());
}

void Gfx::FillGrad(const Rect& r, float rad, uint32_t c1, uint32_t c2, float angleDeg) {
  if (r.w <= 0 || r.h <= 0) return;
  std::unique_ptr<GraphicsPath> p(RoundPath(r, rad));
  LinearGradientBrush b(RectF(r.x, r.y, r.w, r.h), C(c1), C(c2), angleDeg, FALSE);
  g_->FillPath(&b, p.get());
}

void Gfx::Line(V2 a, V2 b, uint32_t c, float w) {
  Pen pen(C(c), w);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  g_->DrawLine(&pen, a.x, a.y, b.x, b.y);
}

void Gfx::Dot(V2 c, float rad, uint32_t col) {
  SolidBrush b(C(col));
  g_->FillEllipse(&b, c.x - rad, c.y - rad, rad * 2, rad * 2);
}

void Gfx::Circle(V2 c, float rad, uint32_t col, float w) {
  Pen pen(C(col), w);
  g_->DrawEllipse(&pen, c.x - rad, c.y - rad, rad * 2, rad * 2);
}

void Gfx::Arc(V2 c, float rad, float startDeg, float sweepDeg, uint32_t col, float w) {
  Pen pen(C(col), w);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  g_->DrawArc(&pen, c.x - rad, c.y - rad, rad * 2, rad * 2, startDeg, sweepDeg);
}

void Gfx::Poly(const std::vector<V2>& pts, uint32_t col, float w) {
  if (pts.size() < 2) return;
  std::vector<PointF> p;
  p.reserve(pts.size());
  for (auto& q : pts) p.push_back(PointF(q.x, q.y));
  Pen pen(C(col), w);
  pen.SetLineJoin(LineJoinRound);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  g_->DrawLines(&pen, p.data(), static_cast<INT>(p.size()));
}

void Gfx::Image(const Rect& box, const uint32_t* argb, int w, int h, float rad) {
  if (!g_ || !argb || w <= 0 || h <= 0 || box.w <= 0 || box.h <= 0) return;
  Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(argb)));
  ImageAttributes edge;
  edge.SetWrapMode(WrapModeTileFlipXY);   // no half-transparent fringe where the scaling samples past the border
  const GraphicsState st = g_->Save();
  std::unique_ptr<GraphicsPath> path(RoundPath(box, rad));
  g_->SetClip(path.get(), CombineModeIntersect);
  g_->SetInterpolationMode(InterpolationModeHighQualityBilinear);
  g_->DrawImage(&bmp, RectF(box.x, box.y, box.w, box.h), 0.0f, 0.0f, static_cast<REAL>(w), static_cast<REAL>(h), UnitPixel, &edge);
  g_->Restore(st);
}

void Gfx::PolyFill(const std::vector<V2>& pts, uint32_t col) {
  if (pts.size() < 3) return;
  std::vector<PointF> p;
  p.reserve(pts.size());
  for (auto& q : pts) p.push_back(PointF(q.x, q.y));
  SolidBrush b(C(col));
  g_->FillPolygon(&b, p.data(), static_cast<INT>(p.size()));
}

// ------------------------------------------------------------------------------------------------ text
float Gfx::LineH(FontId f) const {
  switch (f) {
    case FontId::Tiny: return 14;
    case FontId::Small: return 16;
    case FontId::Body: return 19;
    case FontId::Label: return 16;
    case FontId::Value: return 20;
    case FontId::H3: return 22;
    case FontId::H2: return 26;
    case FontId::H1: return 34;
    case FontId::Mega: return 46;
    default: return 16;
  }
}

float Gfx::TextW(const std::string& s, FontId f) {
  if (s.empty()) return 0;
  const std::string key = std::to_string(static_cast<int>(f)) + ":" + s;
  auto it = widthCache_.find(key);
  if (it != widthCache_.end()) return it->second;
  const std::wstring w = Wide(s);
  StringFormat fmt(StringFormat::GenericTypographic());
  fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
  RectF out;
  g_->MeasureString(w.c_str(), static_cast<INT>(w.size()), Fnt(f), PointF(0, 0), &fmt, &out);
  if (widthCache_.size() > 6000) widthCache_.clear();
  widthCache_[key] = out.Width;
  return out.Width;
}

std::string Gfx::Fit(const std::string& s, FontId f, float maxW) {
  if (TextW(s, f) <= maxW) return s;
  const std::string dots = "\xE2\x80\xA6";
  size_t n = s.size();
  while (n > 0) {
    n--;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;  // do not cut a UTF-8 sequence
    std::string t = s.substr(0, n) + dots;
    if (TextW(t, f) <= maxW) return t;
  }
  return dots;
}

void Gfx::Text(const Rect& box, const std::string& s, FontId f, uint32_t c, Align a, bool vcenter, bool ellipsis) {
  if (s.empty() || box.w <= 0 || box.h <= 0) return;
  const std::string shown = ellipsis ? Fit(s, f, box.w) : s;
  const std::wstring w = Wide(shown);
  StringFormat fmt(StringFormat::GenericTypographic());
  fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
  fmt.SetAlignment(a == Align::Left ? StringAlignmentNear : a == Align::Center ? StringAlignmentCenter : StringAlignmentFar);
  fmt.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
  fmt.SetTrimming(StringTrimmingNone);
  SolidBrush br(C(c));
  g_->DrawString(w.c_str(), static_cast<INT>(w.size()), Fnt(f), RectF(box.x, box.y, box.w, box.h), &fmt, &br);
}

std::vector<std::string> Gfx::WrapLines(const std::string& s, FontId f, float maxW) {
  const std::string key = std::to_string(static_cast<int>(f)) + ":" + std::to_string(static_cast<int>(maxW)) + ":" + s;
  auto it = wrapCache_.find(key);
  if (it != wrapCache_.end()) return it->second;
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t nl = s.find('\n', pos);
    const std::string para = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    std::string cur;
    size_t i = 0;
    while (i < para.size()) {
      size_t j = para.find(' ', i);
      std::string word = para.substr(i, j == std::string::npos ? std::string::npos : j - i);
      i = j == std::string::npos ? para.size() : j + 1;
      std::string trial = cur.empty() ? word : cur + " " + word;
      if (TextW(trial, f) <= maxW || cur.empty()) {
        cur = trial;
        // a single word wider than the box: break it at character boundaries
        while (TextW(cur, f) > maxW && cur.size() > 1) {
          size_t n = cur.size() - 1;
          while (n > 1 && (static_cast<unsigned char>(cur[n]) & 0xC0) == 0x80) n--;
          size_t cut = n;
          while (cut > 1 && TextW(cur.substr(0, cut), f) > maxW) {
            cut--;
            while (cut > 1 && (static_cast<unsigned char>(cur[cut]) & 0xC0) == 0x80) cut--;
          }
          lines.push_back(cur.substr(0, cut));
          cur = cur.substr(cut);
        }
      } else {
        lines.push_back(cur);
        cur = word;
      }
    }
    lines.push_back(cur);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (wrapCache_.size() > 800) wrapCache_.clear();
  wrapCache_[key] = lines;
  return lines;
}

float Gfx::Wrapped(const Rect& box, const std::string& s, FontId f, uint32_t c, bool draw, float lineGap) {
  if (s.empty() || box.w <= 8) return 0;
  const std::vector<std::string> lines = WrapLines(s, f, box.w);
  const float lh = LineH(f) + lineGap;
  float y = box.y;
  for (const std::string& l : lines) {
    if (draw && !l.empty()) Text(Rect(box.x, y, box.w, LineH(f)), l, f, c, Align::Left, true, false);
    y += lh;
  }
  return y - box.y - lineGap;
}

float Gfx::WrappedClamped(const Rect& box, const std::string& s, FontId f, uint32_t c, int maxLines, float lineGap, bool* truncated) {
  if (truncated) *truncated = false;
  if (s.empty() || box.w <= 8 || maxLines < 1) return 0;
  std::vector<std::string> lines = WrapLines(s, f, box.w);
  if (static_cast<int>(lines.size()) > maxLines) {
    std::string tail;
    for (size_t i = static_cast<size_t>(maxLines) - 1; i < lines.size(); i++) tail += (tail.empty() ? "" : " ") + lines[i];
    lines.resize(static_cast<size_t>(maxLines) - 1);
    lines.push_back(Fit(tail, f, box.w - 2));
    if (truncated) *truncated = true;
  }
  const float lh = LineH(f) + lineGap;
  float y = box.y;
  for (const std::string& l : lines) {
    if (c && !l.empty()) Text(Rect(box.x, y, box.w, LineH(f)), l, f, c, Align::Left, true, false);
    y += lh;
  }
  return y - box.y - lineGap;
}

// ------------------------------------------------------------------------------------------------ icons (24 x 24 grid)
namespace {
struct Ink {
  Graphics* g;
  float ox, oy, s;
  Pen pen;
  SolidBrush br;
  Ink(Graphics* gg, const Rect& box, uint32_t col, float stroke) : g(gg), pen(C(col), 1), br(C(col)) {
    s = std::min(box.w, box.h) / 24.0f;
    ox = box.x + (box.w - 24 * s) * 0.5f;
    oy = box.y + (box.h - 24 * s) * 0.5f;
    pen.SetWidth(stroke * s);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
  }
  PointF P(float x, float y) const { return PointF(ox + x * s, oy + y * s); }
  void L(float x1, float y1, float x2, float y2) { g->DrawLine(&pen, P(x1, y1), P(x2, y2)); }
  void Pl(std::initializer_list<float> v, bool close = false, bool fill = false) {
    std::vector<PointF> p;
    for (auto it = v.begin(); it != v.end(); it += 2) p.push_back(P(*it, *(it + 1)));
    if (fill) g->FillPolygon(&br, p.data(), static_cast<INT>(p.size()));
    if (close) g->DrawPolygon(&pen, p.data(), static_cast<INT>(p.size()));
    else g->DrawLines(&pen, p.data(), static_cast<INT>(p.size()));
  }
  void RR(float x, float y, float w, float h, float r) {
    GraphicsPath p;
    const float d = r * 2 * s;
    const PointF a = P(x, y), b = P(x + w, y + h);
    p.AddArc(a.X, a.Y, d, d, 180, 90);
    p.AddArc(b.X - d, a.Y, d, d, 270, 90);
    p.AddArc(b.X - d, b.Y - d, d, d, 0, 90);
    p.AddArc(a.X, b.Y - d, d, d, 90, 90);
    p.CloseFigure();
    g->DrawPath(&pen, &p);
  }
  void C2(float cx, float cy, float r) { const PointF c = P(cx, cy); g->DrawEllipse(&pen, c.X - r * s, c.Y - r * s, 2 * r * s, 2 * r * s); }
  void A(float cx, float cy, float r, float a0, float sw) { const PointF c = P(cx, cy); g->DrawArc(&pen, c.X - r * s, c.Y - r * s, 2 * r * s, 2 * r * s, a0, sw); }
  void D(float cx, float cy, float r) { const PointF c = P(cx, cy); g->FillEllipse(&br, c.X - r * s, c.Y - r * s, 2 * r * s, 2 * r * s); }
  void Bz(float x1, float y1, float c1x, float c1y, float c2x, float c2y, float x2, float y2) { g->DrawBezier(&pen, P(x1, y1), P(c1x, c1y), P(c2x, c2y), P(x2, y2)); }
};
}  // namespace

void Gfx::Icon(IconId id, const Rect& box, uint32_t c, float stroke) {
  if (id == IconId::None || box.w <= 0) return;
  Ink k(g_, box, c, stroke);
  switch (id) {
    case IconId::Overview: k.RR(3, 3, 7.5f, 7.5f, 2); k.RR(13.5f, 3, 7.5f, 7.5f, 2); k.RR(3, 13.5f, 7.5f, 7.5f, 2); k.RR(13.5f, 13.5f, 7.5f, 7.5f, 2); break;
    case IconId::Settings:
      k.C2(12, 12, 3); k.C2(12, 12, 6.8f);
      for (int i = 0; i < 8; i++) { const float a = i * kPi / 4; k.L(12 + 6.8f * std::cos(a), 12 + 6.8f * std::sin(a), 12 + 9.6f * std::cos(a), 12 + 9.6f * std::sin(a)); }
      break;
    case IconId::Performance: k.A(12, 13.5f, 9, 150, 240); k.L(12, 13.5f, 16.6f, 8.6f); k.D(12, 13.5f, 1.5f); break;
    case IconId::Profiles:
    case IconId::Sliders:
      k.L(3, 7, 21, 7); k.L(3, 12, 21, 12); k.L(3, 17, 21, 17);
      k.D(8, 7, 2.2f); k.D(16, 12, 2.2f); k.D(10, 17, 2.2f);
      break;
    case IconId::Fan: {
      k.C2(12, 12, 9.5f); k.D(12, 12, 1.6f);
      for (int i = 0; i < 3; i++) {
        const float a = i * 2 * kPi / 3 - kPi / 2, ca = std::cos(a), sa = std::sin(a);
        auto rot = [&](float x, float y) { return PointF(k.ox + (12 + x * ca - y * sa) * k.s, k.oy + (12 + x * sa + y * ca) * k.s); };
        PointF pts[3] = {rot(0, 0), rot(4.6f, -3.4f), rot(7.2f, 1.0f)};
        k.g->DrawCurve(&k.pen, pts, 3, 0.9f);
      }
      break;
    }
    case IconId::Custom: k.L(6, 4, 6, 20); k.L(12, 4, 12, 20); k.L(18, 4, 18, 20); k.D(6, 14, 2.2f); k.D(12, 8, 2.2f); k.D(18, 15, 2.2f); break;
    case IconId::Apps: k.RR(3, 4.5f, 18, 15, 2.5f); k.L(3, 9.5f, 21, 9.5f); k.D(6.2f, 7, 0.7f); k.D(8.7f, 7, 0.7f); break;
    case IconId::Lighting: k.Pl({12, 3, 14.2f, 9.8f, 21, 12, 14.2f, 14.2f, 12, 21, 9.8f, 14.2f, 3, 12, 9.8f, 9.8f}, true); break;
    case IconId::Battery: k.RR(2.5f, 7, 16.5f, 10, 2.5f); k.L(21.3f, 10.5f, 21.3f, 13.5f); k.Pl({5.5f, 10, 12.5f, 10, 12.5f, 14, 5.5f, 14}, true, true); break;
    case IconId::Display: k.RR(3, 4, 18, 12.5f, 2); k.L(9, 20, 15, 20); k.L(12, 16.5f, 12, 20); break;
    case IconId::Info: k.C2(12, 12, 9); k.L(12, 11, 12, 16.5f); k.D(12, 7.8f, 1.1f); break;
    case IconId::Shield: k.Pl({12, 3, 19.5f, 6, 19.5f, 12, 12, 21, 4.5f, 12, 4.5f, 6}, true); k.Pl({8.7f, 11.6f, 11.2f, 14, 15.5f, 9.4f}); break;
    case IconId::Bolt: k.Pl({13.2f, 3, 5, 13.6f, 11, 13.6f, 9.8f, 21, 19, 10.2f, 13, 10.2f}, true); break;
    case IconId::Plug: k.L(9, 3, 9, 8); k.L(15, 3, 15, 8); k.RR(6, 8, 12, 5.5f, 2); k.A(12, 13.5f, 3.2f, 0, 180); k.L(12, 16.7f, 12, 21); break;
    case IconId::Cpu:
    case IconId::Chip:
      k.RR(6, 6, 12, 12, 2); k.RR(9.7f, 9.7f, 4.6f, 4.6f, 1);
      for (float v : {9.0f, 12.0f, 15.0f}) { k.L(v, 3, v, 6); k.L(v, 18, v, 21); k.L(3, v, 6, v); k.L(18, v, 21, v); }
      break;
    case IconId::Gpu: k.RR(2.5f, 6.5f, 19, 10.5f, 2); k.C2(9, 11.7f, 2.8f); k.C2(16, 11.7f, 2.8f); k.L(5.5f, 17, 5.5f, 20); break;
    case IconId::Ram: k.RR(2.5f, 7.5f, 19, 9, 1.5f); for (float v : {7.0f, 11.0f, 15.0f}) k.L(v, 10.5f, v, 13.5f); for (float v : {6.0f, 10.0f, 14.0f, 18.0f}) k.L(v, 16.5f, v, 19.5f); break;
    case IconId::Thermo: k.RR(9.8f, 3, 4.4f, 12.5f, 2.2f); k.C2(12, 17.6f, 3.4f); k.D(12, 17.6f, 1.6f); k.L(12, 8.5f, 12, 16); break;
    case IconId::Wifi: k.A(12, 19, 14, 232, 76); k.A(12, 19, 9.5f, 230, 80); k.A(12, 19, 5, 226, 88); k.D(12, 19.2f, 1.3f); break;
    case IconId::Bluetooth: k.Pl({7, 8, 17, 16, 12, 21, 12, 3, 17, 8, 7, 16}); break;
    case IconId::Camera: k.RR(3, 8, 18, 12, 2.5f); k.C2(12, 14, 3.4f); k.Pl({8.5f, 8, 10, 5, 14, 5, 15.5f, 8}); break;
    case IconId::Touchpad: k.RR(3, 5, 18, 14, 2.5f); k.L(3, 15, 21, 15); k.L(12, 15, 12, 19); break;
    case IconId::Keyboard:
      k.RR(2.5f, 6, 19, 12, 2.5f);
      for (float v : {6.5f, 10.0f, 13.5f, 17.0f}) { k.D(v, 10, 0.8f); }
      k.D(8, 13.4f, 0.8f); k.D(12, 13.4f, 0.8f); k.D(16, 13.4f, 0.8f); k.L(8.5f, 15.8f, 15.5f, 15.8f);
      break;
    case IconId::Leaf: {
      // two curved edges from the stem (5, 19) to the tip (19.5, 4.5) and the midrib
      auto pt = [&](float x, float y) { return PointF(k.ox + x * k.s, k.oy + y * k.s); };
      PointF up[4] = {pt(5, 19), pt(4.6f, 11.2f), pt(10.5f, 5.6f), pt(19.5f, 4.5f)};
      PointF lo[4] = {pt(5, 19), pt(12.8f, 19.4f), pt(18.6f, 13.6f), pt(19.5f, 4.5f)};
      k.g->DrawCurve(&k.pen, up, 4, 0.8f);
      k.g->DrawCurve(&k.pen, lo, 4, 0.8f);
      k.L(5, 19, 13.5f, 10.5f);
      break;
    }
    case IconId::Check: k.Pl({5, 12.5f, 10, 17.5f, 19, 7}); break;
    case IconId::Cross: k.L(6, 6, 18, 18); k.L(18, 6, 6, 18); break;
    case IconId::Warn: k.Pl({12, 3.8f, 21.5f, 20, 2.5f, 20}, true); k.L(12, 9.8f, 12, 14.6f); k.D(12, 17.3f, 1); break;
    case IconId::Lock: k.RR(5, 11, 14, 10, 2.5f); k.A(12, 11, 4.5f, 180, 180); k.D(12, 16, 1.3f); break;
    case IconId::ChevronR: k.Pl({9, 5, 16, 12, 9, 19}); break;
    case IconId::ChevronD: k.Pl({5, 9, 12, 16, 19, 9}); break;
    case IconId::Plus: k.L(12, 5, 12, 19); k.L(5, 12, 19, 12); break;
    case IconId::Trash: k.L(4, 7, 20, 7); k.Pl({6.2f, 7, 7.2f, 20, 16.8f, 20, 17.8f, 7}); k.Pl({9, 7, 9, 4, 15, 4, 15, 7}); k.L(10.3f, 11, 10.3f, 16.5f); k.L(13.7f, 11, 13.7f, 16.5f); break;
    case IconId::Refresh: k.A(12, 12, 7.5f, 20, 290); k.Pl({17.4f, 2.8f, 17.4f, 6.8f, 13.4f, 6.8f}); break;
    case IconId::Undo: k.L(5, 9.5f, 14, 9.5f); k.A(14, 15, 5.5f, 270, 180); k.L(14, 20.5f, 8, 20.5f); k.Pl({8.5f, 5.5f, 4.5f, 9.5f, 8.5f, 13.5f}); break;
    case IconId::External: k.Pl({10, 5, 5, 5, 5, 19.5f, 19, 19.5f, 19, 14}); k.L(11.5f, 12.5f, 20, 4); k.Pl({14.5f, 4, 20, 4, 20, 9.5f}); break;
    case IconId::Copy: k.RR(4, 4, 11, 11, 2); k.RR(9, 9, 11, 11, 2); break;
    case IconId::Power: k.A(12, 13, 7.5f, -50, 280); k.L(12, 3.5f, 12, 12); break;
    case IconId::Eye: k.Bz(2.5f, 12, 7, 5, 17, 5, 21.5f, 12); k.Bz(2.5f, 12, 7, 19, 17, 19, 21.5f, 12); k.C2(12, 12, 3); break;
    case IconId::Log: k.RR(5, 3.5f, 14, 17, 2); k.L(8.5f, 8.5f, 15.5f, 8.5f); k.L(8.5f, 12, 15.5f, 12); k.L(8.5f, 15.5f, 13, 15.5f); break;
    case IconId::Gamepad: k.RR(2.5f, 8, 19, 10.5f, 5); k.L(7, 10.8f, 7, 15.8f); k.L(4.5f, 13.3f, 9.5f, 13.3f); k.D(15.8f, 11.8f, 1); k.D(18.2f, 14.6f, 1); break;
    case IconId::Sun: k.C2(12, 12, 4); for (int i = 0; i < 8; i++) { const float a = i * kPi / 4; k.L(12 + 6.8f * std::cos(a), 12 + 6.8f * std::sin(a), 12 + 9.4f * std::cos(a), 12 + 9.4f * std::sin(a)); } break;
    case IconId::Moon: k.A(12, 12, 8.5f, 40, 280); k.A(16.5f, 8.5f, 6, 150, 200); break;
    case IconId::Key: k.C2(7.5f, 12, 4); k.L(11.5f, 12, 21, 12); k.L(17.5f, 12, 17.5f, 15.8f); k.L(21, 12, 21, 14.8f); break;
    case IconId::Folder: k.Pl({3, 6, 9.5f, 6, 11.5f, 8.5f, 21, 8.5f, 21, 19.5f, 3, 19.5f}, true); break;
    case IconId::Download: k.L(12, 4, 12, 15); k.Pl({7.5f, 10.5f, 12, 15, 16.5f, 10.5f}); k.L(5, 20, 19, 20); break;
    case IconId::Drive:   // a drive enclosure: body, a split line, the activity light and a label strip
      k.RR(3, 5.5f, 18, 13, 2.6f);
      k.L(3, 13, 21, 13);
      k.D(7.4f, 16, 1);
      k.L(11.5f, 16, 17, 16);
      k.L(7, 8.6f, 9.5f, 8.6f);
      break;
    case IconId::Service: k.RR(4, 4, 16, 6.5f, 1.8f); k.RR(4, 13.5f, 16, 6.5f, 1.8f); k.D(7.6f, 7.2f, 1); k.D(7.6f, 16.7f, 1); break;
    default: break;
  }
}

}  // namespace ui
}  // namespace bc
