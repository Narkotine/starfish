#pragma once

#include <QDateTime>
#include <QList>
#include <QSize>
#include <QVariantMap>
#include <QString>

#include "core/rules.hpp"

namespace starfish::core {

// One entry of the desktop, as the Shell presents it (T-004).
//
// Plain data on purpose: the Win32/Shell lookup lives in
// platform/win/desktop_scanner, so ordering, filtering and rule evaluation
// stay testable without Windows.
//
// Scope note: only real files and folders are covered. Namespace extensions
// that the Shell also draws on the desktop (This PC, Recycle Bin, ...) are not
// files on disk and are not enumerated here.
struct DesktopItem {
    QString name;       // display name, exactly as the Shell would draw it
    QString fileName;   // on-disk name, extension included
    QString path;       // absolute path
    QString targetPath; // resolved .lnk / .url target; empty when not a link
    bool isDir = false;
    bool isLink = false;
    bool isHidden = false;          // FILE_ATTRIBUTE_HIDDEN/SYSTEM, or a ".name"
    bool fromPublicDesktop = false; // C:\Users\Public\Desktop (FOLDERID_PublicDesktop)
    qint64 sizeBytes = -1;          // -1 = unknown (folders, unreadable entries)
    QDateTime modified;
    // Session 74: the file's identity (volume serial + NTFS file index), which a
    // rename keeps -- how a tile finds its file again after a rename made while
    // StarFish was not running. Empty when unknown.
    QString fileId;
    // Session 74: the file is not where the tile says (renamed, moved or deleted
    // elsewhere and not found again). Shown as such, never dropped silently.
    bool missing = false;
    // Session 75: a missing shortcut StarFish kept a copy of (it can put it back).
    bool restorable = false;
    // Session 79: a StarFish widget, not a file (path "widget:<uuid>"; never
    // handed to the shell or the disk). See ui/fence_widgets.hpp.
    QString widgetType;
    QSize span{1, 1};          // cells: columns x rows
    QVariantMap widgetSettings;
    bool isWidget() const { return !widgetType.isEmpty(); }
};

// Session 79: a widget's path -- never a file.
inline bool isWidgetPath(const QString& path)
{
    return path.startsWith(QLatin1String("widget:"));
}

// Session 75: the entry itself is on disk. Not QFileInfo::exists(), which on
// Windows follows a .lnk to its target: a Store-app shortcut (no file target,
// e.g. StoreApp) or a shortcut whose program is gone read as "missing".
bool entryExists(const QString& path);

// ---------------------------------------------------------------------------
// Pure helpers: no Win32, no I/O, unit-tested in tests/test_desktop_item.cpp.
// ---------------------------------------------------------------------------

// True for a ".name" file. The scanner already flags hidden entries from their
// file attributes; this covers the dot convention for callers that only have a
// file name (e.g. a folder portal listing).
bool isDotHiddenName(const QString& fileName);

// Sort key for F-008: case-insensitive, with digit runs zero-padded so that
// "shot9.png" sorts before "shot10.png" instead of after it. Digit runs are
// prefixed with U+0001 so that numbers order before letters.
QString naturalSortKey(const QString& text);

// Display ordering inside a fence. `by` is one of name|type|size|date, matching
// Fence::sortBy in core/jsonstore.hpp; anything else falls back to name.
// Ties always break on the name in ascending order, so the result is stable
// regardless of the direction chosen.
bool lessThanForDisplay(const DesktopItem& a, const DesktopItem& b,
                        const QString& by, bool ascending);

// In-place sort using lessThanForDisplay.
void sortForDisplay(QList<DesktopItem>& items, const QString& by, bool ascending);

// The Shell merges two folders into the single desktop view (the per-user one
// and the machine-wide public one). A name present in both is drawn once: the
// per-user entry wins.
QList<DesktopItem> mergeDesktopItems(const QList<DesktopItem>& items);

// A DesktopItem built from a path alone (T-008): display name from the file
// name, directory/hidden flags from the local filesystem, nothing from the
// Shell. Used when a persisted layout is restored without a Shell resolver --
// tests and headless probes -- so the restore path does not need Windows.
// The entry is kept even when the file is gone: the layout is the authority on
// *what a fence shows*, and a temporarily missing file must not silently empty
// a fence.
DesktopItem itemFromPathOnly(const QString& path);

// Adapter for the rule engine (F-017): the engine only needs a name, a path, a
// folder flag, a size and a modification time, so it does not have to know
// about the desktop model.
//
// The *on-disk* name is handed over (`fileName`, extension included), not the
// Shell display name: the Shell strips the extension of known types, and an
// extension criterion that cannot see the extension would never match.
FileMeta fileMetaFromItem(const DesktopItem& item);

} // namespace starfish::core
