// Drawing of the in-game overlay panel. One implementation serves the live preview on the Overlay page and the real overlay
// window, so what the user configures is exactly what a game shows.
#pragma once
#include <vector>

#include "../common/osdcfg.h"
#include "gfx.h"

namespace bc {
namespace ui {

// Size of the panel on screen for these rows at the configured text size (logical px).
V2 OsdMeasure(Gfx& g, const OsdConfig& c, const std::vector<OsdLine>& lines);
// Draws the panel with its top-left corner at `topLeft` in the current coordinate system.
void OsdPaint(Gfx& g, V2 topLeft, const OsdConfig& c, const std::vector<OsdLine>& lines);
// An invented calm landscape for the preview (not a screenshot of any game): dusk sky, ridges and a lake.
void OsdPreviewScene(Gfx& g, const Rect& r);

}  // namespace ui
}  // namespace bc
