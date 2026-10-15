#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStandardPaths>
#include <optional>

#include "core/jsonstore.hpp"

namespace starfish::core {

// A named copy of the whole document (F-027). A snapshot is the layout plus the
// metadata that says when and why it was taken; the layout inside is the same
// schema-1 document the application persists, so a snapshot is a layout that
// happens to be tagged. The two are deliberately the same shape on purpose:
// restoring a snapshot is applying a layout, and nothing else.
struct Snapshot
{
    QString id;         // user-facing name; also the file name (<id>.json)
    QString createdAt;  // ISO 8601, the moment the snapshot was taken
    QString note;       // optional, free form
    Layout layout;
};

// Save, restore, enumerate and delete named snapshots, and export/import a
// layout as a plain file (F-028). Qt Core only -- no widget, no Win32 -- so every
// behaviour is unit-tested, and the probe runs headless.
//
// Where snapshots live: next to the layout document, in a `snapshots/` subdir.
// That keeps one user's snapshots with the layout they describe, and makes
// `--layout FILE` move both together. When the layout uses its default path
// (%APPDATA%/StarFish/layout.json) the dir is %APPDATA%/StarFish/snapshots.
class SnapshotManager
{
public:
    static QString snapshotsDirFor(const QString& layoutPath);
    static QString defaultSnapshotsDir();

    // An id becomes a file name, so it has to be filesystem-safe: no
    // separators, no "..", nothing that escapes the snapshots directory.
    // Empty, "." and ".." are refused; a trailing dot or space is refused too,
    // because Windows strips them and two ids would collide on disk.
    static bool isValidSnapshotId(const QString& id);

    static QString snapshotFilePath(const QString& dir, const QString& id);

    // Writes a snapshot atomically (QSaveFile). Returns false with an error.
    bool saveSnapshot(const QString& dir, const Layout& layout,
                      const QString& id, const QString& note = {},
                      QString* error = nullptr);

    // Reads a snapshot. Returns false when the id does not exist or the file is
    // not a snapshot. A *plain layout file* (schema 1, no `snapshot` key) is
    // refused rather than silently accepted: it is a disposition, not a
    // snapshot, and confusing the two would make restore load the wrong thing.
    bool loadSnapshot(const QString& dir, const QString& id, Snapshot& out,
                      QString* error = nullptr);

    // Removes a snapshot. Returns false when it does not exist.
    bool removeSnapshot(const QString& dir, const QString& id);

    // Snapshot ids, sorted for display.
    QStringList listSnapshots(const QString& dir) const;

    // F-028: write the layout as a plain schema-1 document to `destPath`, and
    // read one back. These are the only two operations an export/import needs;
    // the format is the same one the application already writes, so there is no
    // second format to keep in sync. Importing a *snapshot* file is refused by
    // JsonStore (it has no `schema` key), which is the right answer: a snapshot
    // is restored through loadSnapshot, not through importLayout.
    bool exportLayout(const Layout& layout, const QString& destPath,
                      QString* error = nullptr) const;
    std::optional<Layout> importLayout(const QString& sourcePath,
                                       QString* error = nullptr) const;

private:
    static QByteArray toJson(const Snapshot& snapshot);
    static std::optional<Snapshot> fromJson(const QByteArray& data, QString* error);
};

} // namespace starfish::core