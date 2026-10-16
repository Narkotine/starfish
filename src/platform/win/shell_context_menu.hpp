#pragma once

#include <QPoint>
#include <QString>
#include <QStringList>

namespace starfish::platform::win {

// Thin wrapper around the Windows shell context menu (IContextMenu) for a
// file-system path. Used to give a fence icon the same right-click menu the
// Explorer shows for the same path -- this is what T-006 expects from the
// "menu contextuel shell" item, and the only way to surface "Open with...",
// "Run as administrator", "Pin to Start", SendTo, the security tab, etc.
//
// The implementation lives in the .cpp because the COM headers are not safe
// to leak into shared headers.
class ShellContextMenu
{
public:
    // What the user picked from the desktop's own background menu.
    enum class BackgroundChoice {
        Dismissed,    // closed without choosing anything
        StarfishVerb, // our own entry, the one at the top
        ShellVerb,    // one of the shell's own entries (already invoked)
    };

    // Show the full shell context menu for a single file or directory. Blocks
    // until the user dismisses the popup, then synchronously invokes the
    // selected verb (e.g. opens the file, deletes it). `globalPos` is in
    // screen coordinates (where the user right-clicked). `ownerHwnd` is the
    // HWND that owns the popup (passed to TrackPopupMenuEx / InvokeCommand);
    // pass the desktop-hosted surface's HWND so the menu is attached to the
    // real on-screen window, not to a hidden helper.
    static void showForPath(const QString& path, const QPoint& globalPos, void* ownerHwnd);

    // The desktop's **background** context menu -- the one SysListView32 answered
    // before ADR-9 hid it, and whose loss was the most visible cost of owning the
    // icon layer (T-054). It is built from the desktop namespace folder's *view
    // object* (there is no item: `CreateViewObject(IID_IContextMenu)`), and one
    // StarFish entry is inserted at the top (T-055), so the same popup both
    // restores the desktop menu and offers "create a fence here".
    //
    // Blocks until the popup is dismissed. `globalPos` is in screen pixels;
    // `ownerHwnd` is the hosted surface's HWND so the popup belongs to the real
    // on-screen window. When the shell's view object cannot be created the menu
    // still opens with our entry alone: the creation gesture must never silently
    // do nothing.
    static BackgroundChoice showForDesktopBackground(const QPoint& globalPos, void* ownerHwnd,
                                                     const QString& starfishLabel);

    // Non-interactive diagnostic: build the desktop background menu and report
    // what it contains. Returns the shell's own verbs (as `menuVerbs()` does for
    // a path) and, through `starfishLabel`, the text of the entry we inserted --
    // empty when the menu could not be built at all. No popup, no input loop, so
    // it runs headless.
    static QStringList desktopBackgroundVerbs(QString* starfishLabel = nullptr);

    // Non-interactive diagnostic: enumerate the verbs the shell would put in
    // the menu for `path`, without showing any popup. Used by the
    // --context-menu-verbs CLI to prove the IContextMenu pipeline works in
    // headless CI (no user, no menu, no input loop).
    static QStringList menuVerbs(const QString& path);
};

} // namespace starfish::platform::win
