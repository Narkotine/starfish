#pragma once

namespace starfish::core {

// Pure z-order helpers: no Win32 dependency, unit-testable anywhere.
//
// Ordering convention: a sibling list is given TOP-FIRST, i.e. in the order
// EnumChildWindows yields it (topmost window first). This is the ordering the
// T-003 spike used to diagnose the desktop placement, and the one that exposed
// the session-4 false positive (see docs/technique.md ADR-4).

enum class PlacementVerdict {
    BelowIconLayer, // good: our window is drawn under the native icons
    AboveIconLayer, // bad: our window would cover the icons
    Unknown,        // our window and/or the icon layer was not found
};

// Compares the index of our window with the index of SHELLDLL_DefView in a
// top-first sibling list. Indices of -1 mean "not found".
PlacementVerdict evaluatePlacement(int ourIndex, int defViewIndex);

// Short human-readable form, for logs and devlog entries.
const char* placementVerdictText(PlacementVerdict verdict);

} // namespace starfish::core
