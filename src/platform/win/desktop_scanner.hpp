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
};

} // namespace starfish::platform::win
