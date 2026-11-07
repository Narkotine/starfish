#include <algorithm>
#include "core/layout_store.hpp"
#include <QFile>
#include <QRegularExpression>
#include <QDateTime>

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

namespace starfish::core {

namespace {
QLoggingCategory lc("starfish.layout");
}

LayoutStore::LayoutStore(QObject* parent)
    : QObject(parent)
    , m_filePath(defaultFilePath())
    , m_fingerprint(JsonStore::toJson(m_layout))
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(m_debounceMs);
    connect(&m_timer, &QTimer::timeout, this, [this] { saveNow(); });
}

QString LayoutStore::defaultFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.starfish");
    return dir + QStringLiteral("/layout.json");
}

void LayoutStore::setFilePath(const QString& path)
{
    if (path.isEmpty() || path == m_filePath)
        return;
    // A pending write belongs to the previous document.
    cancelPendingSave();
    m_filePath = path;
}

void LayoutStore::setDebounceInterval(int ms)
{
    m_debounceMs = qMax(0, ms);
    m_timer.setInterval(m_debounceMs);
}

bool LayoutStore::load(QString* error)
{
    m_readOnly = false;
    m_readOnlyReason.clear();
    m_recoveredFrom.clear();
    if (!QFileInfo::exists(m_filePath)) {
        // First run: an empty layout is the correct state, not a failure.
        qCInfo(lc).noquote()
            << QStringLiteral("no layout file at %1, starting empty").arg(m_filePath);
        emit layoutLoaded(m_layout);
        return true;
    }

    QFile file(m_filePath);
    QString localError;
    if (!file.open(QIODevice::ReadOnly)) {
        localError = file.errorString();
    } else {
        const QByteArray data = file.readAll();
        file.close();
        if (loadFromBytes(data, &localError)) {
            emit layoutLoaded(m_layout);
            return true;
        }
    }

    // Session 85: the file is broken. Its copy goes to the backups (never
    // deleted), then the newest backup that still reads is used -- and saved
    // over the broken file at the next write.
    const QString broken = backupNow(QStringLiteral("corrupt"));
    for (const QString& candidate : backups()) {
        if (candidate == broken || candidate.contains(QLatin1String(".corrupt.")))
            continue;
        QFile backup(candidate);
        if (!backup.open(QIODevice::ReadOnly))
            continue;
        QString ignored;
        if (loadFromBytes(backup.readAll(), &ignored) && !m_readOnly) {
            m_recoveredFrom = candidate;
            m_dirty = true;
            qCWarning(lc).noquote() << QStringLiteral("%1 unreadable (%2): recovered from %3")
                                           .arg(m_filePath, localError, candidate);
            emit layoutLoaded(m_layout);
            return true;
        }
    }
    // Nothing to recover from: keep the layout, and never write over the file.
    m_readOnly = true;
    m_readOnlyReason = QStringLiteral("unreadable document, no usable backup: %1").arg(localError);
    if (error)
        *error = localError;
    qCWarning(lc).noquote()
        << QStringLiteral("could not load %1: %2 (layout kept, file protected)").arg(m_filePath, localError);
    return false;
}

bool LayoutStore::loadFromBytes(const QByteArray& data, QString* error)
{
    const JsonStore::Info info = JsonStore::peek(data, error);
    if (!info.valid)
        return false;
    if (info.schema > JsonStore::kCurrentSchema) {
        // A newer StarFish wrote it: show what this build understands, write nothing.
        m_readOnly = true;
        m_readOnlyReason = QStringLiteral("written by a newer StarFish (schema %1, %2)")
                               .arg(info.schema)
                               .arg(info.writtenBy.isEmpty() ? QStringLiteral("?") : info.writtenBy);
    } else if (info.schema >= 1 && info.schema < JsonStore::kCurrentSchema) {
        backupNow(QStringLiteral("pre-migration"));
    } else if (!m_appVersion.isEmpty() && info.writtenBy != m_appVersion) {
        // Another version wrote it last (an update, a downgrade, or the first
        // run of a build that stamps): keep that state before touching it.
        backupNow(info.writtenBy.isEmpty() ? QStringLiteral("unversioned")
                                           : QStringLiteral("from-") + info.writtenBy);
    }
    const auto loaded = JsonStore::fromJson(data, error);
    if (!loaded)
        return false;
    m_layout = *loaded;
    m_raw = info.root;
    m_fingerprint = JsonStore::toJson(m_layout);
    m_dirty = false;
    qCInfo(lc).noquote() << QStringLiteral("loaded %1 fence(s) from %2%3")
                                .arg(m_layout.fences.size())
                                .arg(m_filePath, m_readOnly ? QStringLiteral(" (read-only: ") + m_readOnlyReason + QLatin1Char(')') : QString());
    return true;
}

QString LayoutStore::backupDir() const
{
    return QFileInfo(m_filePath).absolutePath() + QStringLiteral("/backups");
}

QStringList LayoutStore::backups() const
{
    QDir dir(backupDir());
    // Newest first by the timestamp in the name: a copy keeps the source's
    // mtime, so file times cannot order backups.
    static const QRegularExpression stamp(QStringLiteral("\\.(\\d{8}-\\d{6}-\\d{3})\\."));
    QList<QPair<QString, QString>> keyed;
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("layout.*.bak.json")}, QDir::Files)) {
        const auto m = stamp.match(fi.fileName());
        keyed.append({m.hasMatch() ? m.captured(1) : QString(), fi.absoluteFilePath()});
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    QStringList list;
    for (const auto& k : keyed)
        list << k.second;
    return list;
}

QString LayoutStore::backupNow(const QString& reason)
{
    if (!QFileInfo::exists(m_filePath))
        return {};
    QDir().mkpath(backupDir());
    QFile file(m_filePath);
    int schema = 0;
    if (file.open(QIODevice::ReadOnly))
        schema = JsonStore::peek(file.readAll()).schema;
    file.close();
    QString safeReason = reason;
    safeReason.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    const QString target = QStringLiteral("%1/layout.v%2.%3.%4.bak.json")
                               .arg(backupDir())
                               .arg(schema)
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")), safeReason);
    if (!QFile::copy(m_filePath, target)) {
        qCWarning(lc).noquote() << QStringLiteral("could not back %1 up to %2").arg(m_filePath, target);
        return {};
    }
    // Keep the newest kMaxBackups; a broken file's copy counts like the rest.
    const QStringList all = backups();
    for (int i = kMaxBackups; i < all.size(); ++i)
        QFile::remove(all.at(i));
    qCInfo(lc).noquote() << QStringLiteral("backed up %1 (%2)").arg(target, reason);
    return target;
}

bool LayoutStore::setLayout(const Layout& layout)
{
    const QByteArray candidate = JsonStore::toJson(layout);
    if (candidate == m_fingerprint)
        return false;

    m_layout = layout;
    m_fingerprint = candidate;
    m_dirty = true;
    emit layoutChanged();
    return true;
}

void LayoutStore::scheduleSave()
{
    if (!m_dirty)
        return;
    m_timer.start();
}

void LayoutStore::cancelPendingSave()
{
    m_timer.stop();
}

bool LayoutStore::saveNow(QString* error)
{
    cancelPendingSave();
    if (!m_dirty) {
        // Nothing changed since the last write: an identical file would only
        // cost I/O and disk wear.
        return true;
    }

    QString localError;
    if (m_readOnly) {
        // Session 85: a newer StarFish's document, or a broken one: kept as it is.
        localError = QStringLiteral("not written: ") + m_readOnlyReason;
        if (error)
            *error = localError;
        qCWarning(lc).noquote() << QStringLiteral("%1 %2").arg(m_filePath, localError);
        emit saveFailed(m_filePath, localError);
        return false;
    }
    const QByteArray payload = JsonStore::toJsonPreserving(m_layout, m_raw, m_appVersion);
    if (!JsonStore::saveBytes(payload, m_filePath, &localError)) {
        if (error)
            *error = localError;
        // Stays dirty: the next change (or an explicit save) retries.
        qCWarning(lc).noquote()
            << QStringLiteral("could not save %1: %2").arg(m_filePath, localError);
        emit saveFailed(m_filePath, localError);
        return false;
    }

    m_dirty = false;
    m_raw = JsonStore::peek(payload).root;
    qCInfo(lc).noquote()
        << QStringLiteral("saved %1 fence(s) to %2").arg(m_layout.fences.size()).arg(m_filePath);
    emit saved(m_filePath);
    return true;
}

} // namespace starfish::core
