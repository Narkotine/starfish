#include "core/snapshot.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace starfish::core {

namespace {

// A snapshot file is a layout document with an extra envelope, so it can be
// told apart from a plain layout file (F-028 export). The envelope is the
// metadata that makes a disposition a *snapshot*: when it was taken, and why.
// Keeping the envelope inside the same schema-1 document means fromJson never
// has to know about snapshots at all -- a snapshot is a layout that happens to
// carry extra keys, and the application can ignore them.
static constexpr char kSnapshotKey[] = "snapshot";
static constexpr char kCreatedAtKey[] = "createdAt";
static constexpr char kNoteKey[] = "note";

bool isSafeFileName(const QString& name)
{
    if (name.isEmpty() || name == "." || name == "..")
        return false;
    // No path separators, no drive letters, no wildcards: the id becomes a file
    // name inside the snapshots directory, and a hostile id must not be able to
    // write outside it.
    if (name.contains('/') || name.contains('\\'))
        return false;
    if (name.contains(':'))
        return false;
    if (name.contains('?') || name.contains('*') || name.contains('<') || name.contains('>'))
        return false;
    if (name.contains('|'))
        return false;
    // Windows strips a trailing dot or space, so two ids would collide on disk.
    // It strips a leading space too, so refuse one rather than accept an id
    // that would silently become another id on disk.
    if (name.front().isSpace() || name.back().isSpace())
        return false;
    if (name.back() ==QLatin1Char('.'))
        return false;
    return true;
}

// Tells a snapshot file from a plain layout file by *parsing* it, never by
// scanning its first bytes. The `snapshot` key sorts last in a QJsonObject, so a
// size-limited byte scan silently misses it on a layout bigger than the window
// -- which would let export overwrite a snapshot and let import swallow one.
bool hasSnapshotEnvelope(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() && doc.object().value(kSnapshotKey).isObject();
}

} // namespace

bool SnapshotManager::isValidSnapshotId(const QString& id)
{
    return isSafeFileName(id);
}

QString SnapshotManager::snapshotsDirFor(const QString& layoutPath)
{
    if (layoutPath.isEmpty())
        return defaultSnapshotsDir();
    return QFileInfo(layoutPath).absolutePath() +QLatin1String("/snapshots");
}

QString SnapshotManager::defaultSnapshotsDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return QString();
    return dir +QLatin1String("/snapshots");
}

QString SnapshotManager::snapshotFilePath(const QString& dir, const QString& id)
{
    return dir +QLatin1Char('/') + id +QLatin1String(".json");
}

QByteArray SnapshotManager::toJson(const Snapshot& snapshot)
{
    // The layout is serialised first, then its metadata is merged in: the
    // document keeps its schema-1 shape, so a snapshot is a layout the
    // application already understands, with a few extra keys it ignores.
    QJsonObject root = QJsonDocument::fromJson(JsonStore::toJson(snapshot.layout)).object();
    QJsonObject meta;
    meta[kCreatedAtKey] = snapshot.createdAt;
    if (!snapshot.note.isEmpty())
        meta[kNoteKey] = snapshot.note;
    root[kSnapshotKey] = meta;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<Snapshot> SnapshotManager::fromJson(const QByteArray& data, QString* error)
{
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = parseError.errorString();
        return std::nullopt;
    }
    if (!doc.isObject()) {
        if (error)
            *error =QLatin1String("not a JSON object");
        return std::nullopt;
    }

    const QJsonObject root = doc.object();
    if (!root.contains(kSnapshotKey) || !root[kSnapshotKey].isObject()) {
        // Not a snapshot file: a plain layout, or garbage. The caller decides
        // what to do; loadSnapshot refuses, importLayout passes it to JsonStore.
        if (error)
            *error =QLatin1String("not a snapshot (no \"snapshot\" envelope)");
        return std::nullopt;
    }

    // The layout is the whole document minus the envelope. Strip the envelope
    // and parse what is left as a schema-1 layout, so a snapshot benefits from
    // every validation JsonStore already does.
    QJsonObject layoutObj = root;
    layoutObj.remove(kSnapshotKey);
    const QByteArray layoutBytes = QJsonDocument(layoutObj).toJson(QJsonDocument::Indented);

    Snapshot snapshot;
    const auto layout = JsonStore::fromJson(layoutBytes, error);
    if (!layout)
        return std::nullopt;
    snapshot.layout = *layout;

    const QJsonObject meta = root[kSnapshotKey].toObject();
    // The id is the file name: it is supplied by the caller, not stored. A
    // snapshot file is named <id>.json, so the id is known without being
    // duplicated inside the document.
    snapshot.createdAt = meta.value(kCreatedAtKey).toString();
    snapshot.note = meta.value(kNoteKey).toString();
    return snapshot;
}

bool SnapshotManager::saveSnapshot(const QString& dir, const Layout& layout,
                                   const QString& id, const QString& note,
                                   QString* error)
{
    if (!isValidSnapshotId(id)) {
        if (error)
            *error =QLatin1String("invalid snapshot id: \"") + id +QLatin1String("\"");
        return false;
    }

    Snapshot snapshot;
    snapshot.id = id;
    snapshot.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    snapshot.note = note;
    snapshot.layout = layout;

    const QString path = snapshotFilePath(dir, id);
    const QByteArray payload = toJson(snapshot);

    // The snapshots directory does not exist on a fresh profile: create it
    // rather than failing the first snapshot.
    if (!QDir().mkpath(dir)) {
        if (error)
            *error =QLatin1String("could not create %1").arg(dir);
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(payload) != payload.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool SnapshotManager::loadSnapshot(const QString& dir, const QString& id, Snapshot& out,
                                   QString* error)
{
    if (!isValidSnapshotId(id)) {
        if (error)
            *error =QLatin1String("invalid snapshot id: \"") + id +QLatin1String("\"");
        return false;
    }

    const QString path = snapshotFilePath(dir, id);
    if (!QFileInfo::exists(path)) {
        if (error)
            *error =QLatin1String("no such snapshot: ") + id;
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const auto snapshot = fromJson(file.readAll(), error);
    if (!snapshot)
        return false;
    out = *snapshot;
    // The id is the file name, not a field inside the document: restore it
    // from the id the caller asked for, so a loaded snapshot carries the id
    // it was saved under.
    out.id = id;
    return true;
}

bool SnapshotManager::removeSnapshot(const QString& dir, const QString& id)
{
    if (!isValidSnapshotId(id))
        return false;
    const QString path = snapshotFilePath(dir, id);
    return QFileInfo::exists(path) && QFile::remove(path);
}

QStringList SnapshotManager::listSnapshots(const QString& dir) const
{
    QStringList ids;
    if (!QDir().exists(dir))
        return ids;
    const QFileInfoList entries =QDir(dir).entryInfoList({QLatin1String("*.json")},
                                                   QDir::Files, QDir::Name);
    for (const QFileInfo& entry : entries) {
        const QString stem = entry.completeBaseName();
        if (!stem.isEmpty())
            ids.append(stem);
    }
    ids.sort(Qt::CaseInsensitive);
    return ids;
}

bool SnapshotManager::exportLayout(const Layout& layout, const QString& destPath,
                                   QString* error) const
{
    // F-028: a disposition is exported as the same schema-1 document the
    // application writes, so the export has no second format to keep in sync.
    // But a snapshot file is not a disposition: it carries an envelope the
    // export does not, and overwriting it would destroy the metadata. Refuse.
    if (QFileInfo::exists(destPath) && hasSnapshotEnvelope(destPath)) {
        if (error)
            *error =QLatin1String("refusing to overwrite a snapshot file: ") + destPath;
        return false;
    }
    return JsonStore::save(layout, destPath, error);
}

std::optional<Layout> SnapshotManager::importLayout(const QString& sourcePath,
                                                    QString* error) const
{
    // F-028 imports a *layout* file. A snapshot file is not a layout file: it
    // carries a `snapshot` envelope that a layout does not, and importing it
    // would silently drop the metadata. Restore a snapshot through
    // loadSnapshot instead. (A snapshot is still a layout the application
    // understands -- JsonStore::load can read it -- but import is for the
    // plain disposition format, which is what export writes.)
    if (QFileInfo::exists(sourcePath) && hasSnapshotEnvelope(sourcePath)) {
        if (error)
            *error =QLatin1String("not a layout file (it is a snapshot): ") + sourcePath;
        return std::nullopt;
    }
    return JsonStore::load(sourcePath, error);
}

} // namespace starfish::core