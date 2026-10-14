#pragma once

#include <QList>
#include <QString>

#include "core/desktop_item.hpp"

namespace starfish::core {

// A folder mirrored into a fence (F-024 / T-011).
//
// The document only ever stores the *folder path* -- the entry is
// `{"kind": "portal", "path": "..."}` in a fence's `items`. The tiles a portal
// contributes are the folder's contents, listed from disk, so the mirror
// follows the folder instead of freezing a snapshot of it (ADR-7 applies to
// what a fence *shows*, and a portal shows whatever the folder holds today).
struct Portal {
    // The path exactly as the document declares it: never cleaned, never
    // rewritten, because it is what gets serialised back. QDir::cleanPath()
    // normalises the separators ("C:\x" -> "C:/x"), which would make a
    // document stop round-tripping byte for byte.
    QString path;
    // The folder's immediate entries, as last listed. Empty is a legitimate
    // state: an empty folder, or a folder that is not there right now.
    QList<DesktopItem> items;
    // Non-empty when the folder could not be listed. The portal is still kept
    // (a network share that is offline must not silently vanish from the
    // document); it simply contributes no tile until it comes back.
    QString error;
};

// True for the `kind` of a document entry that designates a folder to mirror.
// Anything else -- including a missing `kind`, as in a hand-written document --
// is a plain file reference, so documents written before T-011 keep loading.
bool isPortalKind(const QString& kind);

// Lists a folder for a portal: its immediate entries only (same scope as
// F-018's watcher), naturally sorted, dot-prefixed names skipped.
//
// The dot convention is the one filter applied, for the same reason the
// desktop model has it (see isDotHiddenName): a portal is a view of a folder,
// not an inventory of it. Attribute-hidden entries are kept, exactly as the
// desktop scanner keeps them -- the Shell draws those too.
//
// Qt Core + the local filesystem only: no Shell, no Win32, so it runs headless
// and in CI, just like itemFromPathOnly().
Portal listPortal(const QString& folderPath);

// True when two strings designate the same folder, whatever the separator
// style, the relative segments or the case (Windows paths are
// case-insensitive). Used to match a watched folder against a portal, and to
// refuse the same folder twice.
bool sameFolder(const QString& a, const QString& b);

} // namespace starfish::core
