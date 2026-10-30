#pragma once

class QApplication;

namespace starfish::app {

// `--context-menu-probe` (T-092): a diagnostic, not a test. Dumps the
// desktop's real menus as StarFish reads them (labels, state, the verb as it
// survives the proxy -- its first letter -- and the command offsets) for the
// background, a test file of its own, the Recycle Bin and one shortcut, to
// build/context-menu-probe.txt. The checks are in --drawn-desktop-probe and
// tests/test_context_menu.cpp.
int runContextMenuProbe(QApplication& app);

} // namespace starfish::app