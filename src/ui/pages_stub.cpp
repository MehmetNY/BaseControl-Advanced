// Temporary placeholders while the real pages are being written.
#include "page.h"
namespace bc { namespace ui {
#define STUB(N) float N(App& a, const Rect& r) { Ctx c(a, r.w); Masonry m(r.w); Card k(c, m, #N, "stub", IconId::Info, 40); k.Info("x", "y"); return m.Bottom(); }
STUB(PageAbout)
}}
