#pragma once

#include <QDateTime>
#include <QList>
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
};

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
