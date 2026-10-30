#pragma once

class QApplication;

namespace starfish::app {

// `--drawn-desktop-probe` (ADR-21, session 58): runs the product's drawn desktop
// on the real desktop and checks it from the outside -- the list hidden behind
// the marker, every item of the shell's view drawn where the view keeps it, a
// file of the probe's own appearing, being renamed and leaving, the shell's real
// background and item menus (submenus filled), and the native desktop back at
// the end. Report: build/drawn-desktop-probe.txt, captures in build/captures/.
//
// It touches nothing of the user's: the one file it creates on the Desktop is
// its own ("StarFish probe ... .txt") and is deleted before it ends. It opens two
// menus on screen for about a second each (closed by the probe itself).
int runDrawnDesktopProbe(QApplication& app);

} // namespace starfish::app
