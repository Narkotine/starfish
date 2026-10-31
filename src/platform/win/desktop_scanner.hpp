#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/desktop_item.hpp"

namespace starfish::platform::win {

// Reads the desktop folders the Shell merges into the desktop view (T-004).
//
// The folders come from SHGetKnownFolderPath rather than QStandardPaths: the
// Shell is the authority, and the location can be redirected by the user (or
// by group policy) without the environment reflecting it.
//
// Hidden and system entries are returned but flagged
// (DesktopItem::isHidden), so the caller decides what to draw -- the Shell's
// own view hides them unless "show hidden files" is on.
class DesktopScanner
{
public:
    // Per-user desktop + machine-wide public desktop, duplicates merged (the
    // per-user entry wins). `error` receives a message when the Shell lookup
    // fails; the returned list is then empty.
    static QList<core::DesktopItem> scan(QString* error = nullptr);

    // The folders that were scanned, in scan order, for diagnostics.
    static QStringList desktopFolders();

    // Resolves a single dropped file/folder path to a DesktopItem, using the
    // same Shell resolution as the desktop scan (display name, shortcut
    // target, hidden attribute). Returns an empty item when the path is empty
    // or COM is unavailable. Used by the Explorer drop target (T-006-3) so a
    // dropped item and a scanned desktop entry resolve identically.
    static core::DesktopItem itemForPath(const QString& path);
};

// Opens a desktop entry the way its double-click does. A virtual item (This
// PC, the Recycle Bin, the user's folder -- a parsing name starting with
// "::") is no file: it is opened through its item id list (ShellExecuteEx,
// SEE_MASK_IDLIST), which is what lets a fence hold one (ADR-21). True when
// the shell accepted it.
bool openShellItem(const QString& path);

// Session 74: a file's identity -- "volume serial-file index" (hex), which a
// rename or a move on the same volume keeps. Empty when it cannot be read.
QString fileIdOf(const QString& path);
} // namespace starfish::platform::win
