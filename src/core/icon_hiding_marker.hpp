#pragma once

#include <QString>

namespace starfish::core {

// The note left behind when StarFish hides the native desktop icon layer (T-046).
//
// Hiding SHELLDLL_DefView is a change to the whole session -- the desktop has no
// icons at all until it is undone -- and the only process that would have undone
// it is the one that made the change. A crash, a kill or a power cut would
// therefore leave the user with an empty desktop and nothing on screen to explain
// it.
//
// This marker is the memory that outlives the process: it is written *before*
// the layer is hidden and removed only once the layer is really back (write-ahead
// on the way in, write-behind on the way out). A later run that finds it restores
// the icons without needing anything from the dead process, and a marker that
// cannot be written means the layer must not be hidden at all.
//
// Deliberately not part of the layout document: the icon layer belongs to the
// session, not to one document, and two documents describing different fences
// still share one desktop.
class IconHidingMarker
{
public:
    // %APPDATA%/StarFish/icons-hidden.marker -- the same directory as the layout
    // document (LayoutStore::defaultFilePath), because it is the same product
    // state.
    static QString defaultPath();

    explicit IconHidingMarker(const QString& path = defaultPath());

    QString path() const { return m_path; }

    // True when a marker is on disk -- i.e. some run hid the icon layer and never
    // reported having restored it. Decided by the file's *presence*, never by its
    // content: a marker we cannot parse still counts (see description()).
    bool isPresent() const;

    // What the marker says, for a log line or a report: "pid 1234, hidden at
    // 2026-10-15T22:41:03". Empty when there is no marker, "unreadable marker"
    // when the file is there but its content is not the JSON we wrote.
    QString description() const;

    // Writes the marker, atomically (QSaveFile), creating the parent directory.
    // Returns false when it could not be written -- and the caller must then
    // refuse to hide the icons: a hiding that was never recorded is the one state
    // nothing can repair.
    bool write();

    // Removes the marker. True when no marker is left afterwards, so removing an
    // absent marker is a success, not a failure.
    bool remove();

private:
    QString m_path;
};

} // namespace starfish::core
