#pragma once

#include "ui/fence_geometry.hpp"

namespace starfish::ui {

// Session 60 -- a fence's spacing, named, in one place, so a future "Design
// avancé des fences" page (TODO T-097, with T-093) can expose them instead of
// hunting constants. Logical pixels.
//
//   +-------------------------------------------+  -
//   |  Title                                    |  | titleBarHeight
//   +-------------------------------------------+  -
//   |   (titleGap)                              |
//   |  inset | gridPadding [icon] cellGap [icon]|
//   |        |            label                 |
//   |                                  (inset)  |
//   +-------------------------------------------+
//
// The title bar's height is also the height of a rolled-up fence, which the
// geometry (FenceGeometry::kTitleBarHeight) owns: it is not free yet.
struct FenceMetrics {
    int titleBarHeight = FenceGeometry::kTitleBarHeight;
    int titleInset = 12;  // the title text from the left edge
    int titleGap = 0;     // between the title band and the content area
    int inset = 12;       // panel edge -> content area, sides and bottom
    int gridPadding = 12; // the grid's own margin inside the content area
    int cellGap = 8;      // between two cells
    int labelHeight = 34; // a caption's two lines

    // Before session 60 the content area began `inset` under the band, and the
    // grid added its padding on top: 24 px between the band and the first icon.
    // Now the band is followed by the grid's padding alone (12 px).
    static FenceMetrics defaults() { return FenceMetrics{}; }
};

} // namespace starfish::ui
