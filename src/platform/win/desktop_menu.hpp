#pragma once

// T-071 / ADR-14 -- StarFish's entry in the desktop's **own** context menu.
//
// The user's decision, after the three things a window of ours over the desktop
// kept costing (the fences' clicks, the icons' clicks and drags, and the shell's
// own menu and rubber band): keep the desktop normal and move the gesture
// somewhere that costs nothing. So F-001 is no longer a right-drag we trap -- it
// is a *static verb* in Explorer's desktop menu, registered per user:
//
//     HKCU\Software\Classes\DesktopBackground\Shell\StarFish
//
// Explorer builds the menu and calls the command we registered; we never take the
// right button, never rebuild the menu, and never host a window. Everything here
// is per-user (no elevation) and reversible with one command.
//
// The command asks a **running** product for a fence through a named event (a
// second process would be turned away by the single-instance guard, T-044, and
// the entry would look broken); when nothing is running, the same process starts
// the product, which honours the request on its way up.

#include <QObject>
#include <QString>

namespace starfish::platform::win {

/// The label Explorer will show.
QString desktopMenuLabel();

/// What the entry runs (for the report, and for the user to see).
QString desktopMenuCommand();

/// Is StarFish's entry registered for this user right now?
bool desktopMenuInstalled();

/// Add or remove the entry. `error` receives why, when it fails.
bool installDesktopMenu(QString* error = nullptr);
bool uninstallDesktopMenu(QString* error = nullptr);

/// Ask the running product for a fence. False when none is running.
bool requestFenceFromRunningInstance();

/// The product's side of that request: a cheap poll on the named event.
class NewFenceWatcher : public QObject
{
    Q_OBJECT

public:
    explicit NewFenceWatcher(QObject* parent = nullptr);
    ~NewFenceWatcher() override;

    /// Starts watching. Harmless if it cannot (the feature simply stays silent).
    void start();
    void stop();

signals:
    void requested();

private:
    struct Impl;
    Impl* d;
};

} // namespace starfish::platform::win
