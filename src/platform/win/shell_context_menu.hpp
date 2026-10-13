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
    // Show the full shell context menu for a single file or directory. Blocks
    // until the user dismisses the popup, then synchronously invokes the
    // selected verb (e.g. opens the file, deletes it). `globalPos` is in
    // screen coordinates (where the user right-clicked). `ownerHwnd` is the
    // HWND that owns the popup (passed to TrackPopupMenuEx / InvokeCommand);
    // pass the desktop-hosted surface's HWND so the menu is attached to the
    // real on-screen window, not to a hidden helper.
    static void showForPath(const QString& path, const QPoint& globalPos, void* ownerHwnd);

    // Non-interactive diagnostic: enumerate the verbs the shell would put in
    // the menu for `path`, without showing any popup. Used by the
    // --context-menu-verbs CLI to prove the IContextMenu pipeline works in
    // headless CI (no user, no menu, no input loop).
    static QStringList menuVerbs(const QString& path);
};

} // namespace starfish::platform::win
