#pragma once

class QApplication;

namespace starfish::app {

// `--ui-probe` (session 59): StarFish's own windows, photographed and checked
// in-process -- the tray icon and its menu, the settings window's pages, the
// right-click menu editor working on a scratch copy of the configuration. No
// real click or key: the menu is popped and the editor driven by calls. The
// desktop is not touched (no ProductApp, no shell view). Captures in
// build/captures/ui_*.png, report in build/ui-probe.txt.
int runUiProbe(QApplication& app);

} // namespace starfish::app
